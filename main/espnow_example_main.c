/*
   Kentuki: juego para dos placas conectadas por ESP-NOW.

   Las dos placas corren el mismo firmware. Cada una busca su MAC en
   s_device_macs: la primera es el AMO y la segunda el KENTUKI, y la otra
   entrada es su peer (unicast directo, sin broadcast).

   - Los dos se mueven por la casa con el joystick y se avisan la habitación.
   - Si están juntos: el amo acaricia (KEY1) y el kentuki pide atención (KEY1).
   - El kentuki pierde salud con el tiempo; si llega a 0 muere y es GAME OVER.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "espnow_example.h"
#include "board.h"
#include "display.h"
#include "buttons.h"

static const char *TAG = "kentuki";

/* ================================================================
 *  Configuración del juego
 * ================================================================ */

#define SALUD_MAX          100
#define SALUD_CARICIA      15                    // cuánta salud suma cada caricia del amo
#define SALUD_PERIODO      pdMS_TO_TICKS(1000)   // cada cuánto pierde 1 punto de salud el kentuki

#define REACCION_DURACION  pdMS_TO_TICKS(2000)   // cuánto tiempo se ve el ícono de reacción
#define TITILAR_PERIODO    pdMS_TO_TICKS(250)    // medio ciclo del titileo (blanco / negro)

#define TAM_FLECHA         6                     // alto de la flecha en píxeles

/* ================================================================
 *  Tipos
 * ================================================================ */

typedef enum { ROL_AMO = 0, ROL_KENTUKI = 1 } rol_t;

typedef enum {
    MSG_HABITACION = 0,   // valor = habitación en la que estoy
    MSG_ACARICIAR  = 1,   // amo -> kentuki
    MSG_PEDIRATT   = 2,   // kentuki -> amo
    MSG_REACCION   = 3,   // valor = id de reacción
    MSG_GAMEOVER   = 4,   // kentuki -> amo: el kentuki murió
} msg_tipo_t;

typedef struct __attribute__((packed)) {
    msg_tipo_t accion;
    uint8_t valor;
    uint8_t flags;        // para más adelante: escondido, sonido, etc.
} mensaje_t;

enum { REACCION_CORAZON = 0, REACCION_ATENCION = 1, N_REACCIONES };
#define REACCION_NINGUNA   0xFF

/* ================================================================
 *  Datos fijos: MACs, mapa e íconos
 * ================================================================ */

// [0] = AMO, [1] = KENTUKI
static const uint8_t s_device_macs[2][ESP_NOW_ETH_ALEN] = {
    { 0x80, 0x45, 0x6B, 0x36, 0x2F, 0x9C },
    { 0x80, 0x45, 0x6B, 0x36, 0x01, 0x20 },
};

#define N_ROOMS 7

// Vecinos de cada habitación por dirección (0 = arriba, 1 = derecha, 2 = abajo, 3 = izquierda); -1 = pared
static const int8_t rooms[N_ROOMS][4] = {
    { 1,  2,  3,  4},   // 0 Living
    {-1, -1,  0, -1},   // 1 Afuera
    {-1, -1, -1,  0},   // 2 Patio
    { 0, -1,  5,  6},   // 3 Pasillo
    {-1,  0, -1, -1},   // 4 Cocina
    { 3, -1, -1, -1},   // 5 Dormitorio
    {-1,  3, -1, -1},   // 6 Baño
};

// Sin Ñ: la fuente solo tiene ASCII
static const char *room_names[N_ROOMS] = {"LIVING", "AFUERA", "PATIO", "PASILLO", "COCINA", "DORMITORIO", "BANO"};

// Íconos 16x16: cada fila es un uint16_t, el bit más alto es el píxel de la izquierda.
static const uint16_t ICONO_AMO[16] = {
    0x07E0, 0x0FF0, 0x0FF0, 0x0FF0,   // cabeza
    0x07E0, 0x03C0,                   // mentón y cuello
    0x3FFC, 0x7FFE,                   // hombros
    0x6FF6, 0x6FF6, 0x6FF6,           // brazos y torso
    0x0FF0,                           // cintura
    0x0E70, 0x0E70, 0x0E70, 0x1E78,   // piernas
};

static const uint16_t ICONO_KENTUKI[16] = {
    0x700E, 0xF81F,                   // orejas
    0xFFFF, 0x8001,                   // cabeza
    0xBC3D, 0xA425, 0xA425, 0xBC3D,   // ojos-cámara
    0x8001, 0x8181, 0x8001,           // nariz
    0x4002, 0x3FFC,                   // base del cuerpo
    0x1818, 0x3C3C, 0x1818,           // ruedas
};

// Íconos de reacción 8x8: cada fila es un uint8_t, el bit más alto es el píxel de la izquierda.
static const uint8_t ICONOS_REACCION[N_REACCIONES][8] = {
    [REACCION_CORAZON]  = { 0x66, 0xFF, 0xFF, 0xFF, 0x7E, 0x3C, 0x18, 0x00 },
    [REACCION_ATENCION] = { 0x18, 0x18, 0x18, 0x18, 0x18, 0x00, 0x18, 0x18 },   // "!"
};

/* ================================================================
 *  Estado
 * ================================================================ */

// Red
static uint8_t s_peer_mac[ESP_NOW_ETH_ALEN];
static QueueHandle_t s_cola_red = NULL;           // último mensaje recibido del peer (largo 1)
static rol_t MI_ROL = ROL_AMO;                    // se define en select_peer() según la MAC

// Juego
static uint8_t in_room = 1;
static bool otro_aca = false;                     // el peer está en mi habitación
static uint8_t salud = SALUD_MAX;                 // solo la usa el kentuki (0..100)
static TickType_t s_prox_decremento = 0;          // tick del próximo descuento de salud

// Pantalla
static uint8_t s_reaccion = REACCION_NINGUNA;     // reacción que se está mostrando
static bool s_reaccion_sobre_mi = false;          // true: sobre mi ícono, false: sobre el del peer
static TickType_t s_reaccion_hasta = 0;           // tick en el que deja de mostrarse
static TickType_t s_titilar_hasta = 0;            // hasta cuándo titila el fondo (pedido de atención)
static bool s_invertido = false;                  // estado actual de la pantalla

/* ================================================================
 *  Red: WiFi + ESP-NOW
 * ================================================================ */

// WiFi tiene que arrancar antes que ESP-NOW
static void wifi_init(void)
{
    ESP_ERROR_CHECK( esp_netif_init() );
    ESP_ERROR_CHECK( esp_event_loop_create_default() );
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
    ESP_ERROR_CHECK( esp_wifi_set_mode(ESPNOW_WIFI_MODE) );
    ESP_ERROR_CHECK( esp_wifi_start() );
    ESP_ERROR_CHECK( esp_wifi_set_channel(CONFIG_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE) );

#if CONFIG_ESPNOW_ENABLE_LONG_RANGE
    ESP_ERROR_CHECK( esp_wifi_set_protocol(ESPNOW_WIFI_IF, WIFI_PROTOCOL_11B|WIFI_PROTOCOL_11G|WIFI_PROTOCOL_11N|WIFI_PROTOCOL_LR) );
#endif
}

// Según mi MAC elijo rol y peer: s_device_macs[0] es el AMO y s_device_macs[1] el KENTUKI
static esp_err_t select_peer(void)
{
    uint8_t my_mac[ESP_NOW_ETH_ALEN];

    ESP_ERROR_CHECK( esp_wifi_get_mac(ESPNOW_WIFI_IF, my_mac) );
    ESP_LOGI(TAG, "My MAC: "MACSTR"", MAC2STR(my_mac));

    if (memcmp(my_mac, s_device_macs[0], ESP_NOW_ETH_ALEN) == 0) {
        memcpy(s_peer_mac, s_device_macs[1], ESP_NOW_ETH_ALEN);
        MI_ROL = ROL_AMO;
    } else if (memcmp(my_mac, s_device_macs[1], ESP_NOW_ETH_ALEN) == 0) {
        memcpy(s_peer_mac, s_device_macs[0], ESP_NOW_ETH_ALEN);
        MI_ROL = ROL_KENTUKI;
    } else {
        ESP_LOGE(TAG, "My MAC is not in s_device_macs, add it and reflash");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Peer MAC: "MACSTR", mi rol: %s", MAC2STR(s_peer_mac), MI_ROL == ROL_AMO ? "AMO" : "KENTUKI");
    return ESP_OK;
}

/* Los callbacks de ESP-NOW corren en la tarea de WiFi: hacer lo mínimo
 * y pasarle el trabajo al main por la cola. */
static void al_enviar(const esp_now_send_info_t *tx_info, esp_now_send_status_t status)
{
    if (tx_info == NULL) {
        return;
    }
    if (status == ESP_NOW_SEND_SUCCESS) {
        ESP_LOGI(TAG, "Send to "MACSTR" OK", MAC2STR(tx_info->des_addr));
    } else {
        ESP_LOGW(TAG, "Send to "MACSTR" FAIL (peer off or out of range?)", MAC2STR(tx_info->des_addr));
    }
}

static void al_recibir(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (memcmp(info->src_addr, s_peer_mac, ESP_NOW_ETH_ALEN) != 0) return;  // no es nuestro peer
    if (len != sizeof(mensaje_t)) return;                                   // no es un mensaje nuestro

    mensaje_t msg;
    memcpy(&msg, data, sizeof(msg));
    xQueueOverwrite(s_cola_red, &msg);        // guarda solo el último mensaje
}

static esp_err_t espnow_init(void)
{
    if (select_peer() != ESP_OK) {
        return ESP_FAIL;
    }

    s_cola_red = xQueueCreate(1, sizeof(mensaje_t));   // largo 1: lo exige xQueueOverwrite
    if (s_cola_red == NULL) {
        ESP_LOGE(TAG, "Create queue fail");
        return ESP_FAIL;
    }

    ESP_ERROR_CHECK( esp_now_init() );
    ESP_ERROR_CHECK( esp_now_register_send_cb(al_enviar) );
    ESP_ERROR_CHECK( esp_now_register_recv_cb(al_recibir) );
#if CONFIG_ESPNOW_ENABLE_POWER_SAVE
    ESP_ERROR_CHECK( esp_now_set_wake_window(CONFIG_ESPNOW_WAKE_WINDOW) );
    ESP_ERROR_CHECK( esp_wifi_connectionless_module_set_wake_interval(CONFIG_ESPNOW_WAKE_INTERVAL) );
#endif
    ESP_ERROR_CHECK( esp_now_set_pmk((uint8_t *)CONFIG_ESPNOW_PMK) );

    // Peer fijo, encriptado con la LMK
    esp_now_peer_info_t peer = {
        .channel = CONFIG_ESPNOW_CHANNEL,
        .ifidx = ESPNOW_WIFI_IF,
        .encrypt = true,
    };
    memcpy(peer.lmk, CONFIG_ESPNOW_LMK, ESP_NOW_KEY_LEN);
    memcpy(peer.peer_addr, s_peer_mac, ESP_NOW_ETH_ALEN);
    ESP_ERROR_CHECK( esp_now_add_peer(&peer) );

    return ESP_OK;
}

static void enviar(msg_tipo_t accion, uint8_t valor)
{
    mensaje_t msg = { .accion = accion, .valor = valor, .flags = 0 };
    esp_err_t err = esp_now_send(s_peer_mac, (uint8_t *)&msg, sizeof(msg));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_send failed: %s", esp_err_to_name(err));
    }
}

// Le avisa al peer en qué habitación estoy
static void enviar_habitacion(void)
{
    ESP_LOGI(TAG, "Send habitacion: %d (%s)", in_room, room_names[in_room]);
    enviar(MSG_HABITACION, in_room);
}

static void enviar_reaccion(uint8_t id_reaccion)
{
    ESP_LOGI(TAG, "Send reaccion: %d", id_reaccion);
    enviar(MSG_REACCION, id_reaccion);
}

static void acariciar(void)
{
    enviar(MSG_ACARICIAR, 0);
}

static void pedir_atencion(void)
{
    enviar(MSG_PEDIRATT, 0);
}

/* ================================================================
 *  Dibujo
 * ================================================================ */

// Texto centrado horizontalmente usando la fuente del driver
static void texto_centrado(int y, const char *s)
{
    int x = (BOARD_LCD_WIDTH - (int)strlen(s) * DISPLAY_FONT_W) / 2;
    display_text(x, y, s, false);
}

// dir: 0 = arriba, 1 = derecha, 2 = abajo, 3 = izquierda
static void dibujar_flecha(uint8_t dir)
{
    const int W = BOARD_LCD_WIDTH;
    const int H = BOARD_LCD_HEIGHT;
    const int cx = W / 2;
    const int cy = H / 2;

    for (int i = 0; i < TAM_FLECHA; i++) {
        int largo = 2 * i + 1;
        switch (dir) {
            case 0: display_fill_rect(cx - i, 1 + i, largo, 1, true);     break; // arriba
            case 1: display_fill_rect(W - 2 - i, cy - i, 1, largo, true); break; // derecha
            case 2: display_fill_rect(cx - i, H - 2 - i, largo, 1, true); break; // abajo
            case 3: display_fill_rect(1 + i, cy - i, 1, largo, true);     break; // izquierda
            default: return;
        }
    }
}

static const uint16_t *icono_de(rol_t rol)
{
    return rol == ROL_AMO ? ICONO_AMO : ICONO_KENTUKI;
}

static void dibujar_icono(const uint16_t *icono, int x, int y)
{
    for (int fila = 0; fila < 16; fila++) {
        for (int col = 0; col < 16; col++) {
            if (icono[fila] & (0x8000 >> col)) {
                display_set_pixel(x + col, y + fila, true);
            }
        }
    }
}

// Dibuja la reacción 8x8 centrada arriba de un ícono de 16 que está en (x_icono, y_icono)
static void dibujar_reaccion(uint8_t id, int x_icono, int y_icono)
{
    const uint8_t *r = ICONOS_REACCION[id];
    for (int fila = 0; fila < 8; fila++) {
        for (int col = 0; col < 8; col++) {
            if (r[fila] & (0x80 >> col)) {
                display_set_pixel(x_icono + 4 + col, y_icono - 9 + fila, true);
            }
        }
    }
}

// Dibuja la pantalla completa a partir del estado actual
static void dibujar_habitacion(void)
{
    const int y_nombre = 10;
    const int y_iconos = 28;
    const int x_otro = 72;
    int x_mio = otro_aca ? 40 : 56;
    rol_t rol_otro = (MI_ROL == ROL_AMO) ? ROL_KENTUKI : ROL_AMO;

    display_clear();

    texto_centrado(y_nombre, room_names[in_room]);
    if (MI_ROL == ROL_KENTUKI) {
        char txt[8];
        snprintf(txt, sizeof(txt), "%d", salud);
        display_text(0, 0, txt, false);                     // salud en la esquina superior izquierda
    }

    dibujar_icono(icono_de(MI_ROL), x_mio, y_iconos);
    if (otro_aca) {
        dibujar_icono(icono_de(rol_otro), x_otro, y_iconos);
    }

    // Reacción arriba de la cabeza que corresponda, mientras no haya vencido
    if (s_reaccion != REACCION_NINGUNA && (int32_t)(s_reaccion_hasta - xTaskGetTickCount()) > 0) {
        if (s_reaccion_sobre_mi) {
            dibujar_reaccion(s_reaccion, x_mio, y_iconos);
        } else if (otro_aca) {
            dibujar_reaccion(s_reaccion, x_otro, y_iconos);
        }
    }

    for (uint8_t dir = 0; dir < 4; dir++) {
        if (rooms[in_room][dir] != -1) dibujar_flecha(dir);
    }

    display_flush();                                        // recién acá se ve en pantalla
}

// Muestra una reacción arriba de mi cabeza (sobre_mi) o de la del peer durante REACCION_DURACION.
// No dibuja acá: dibujar_habitacion() la pinta en cada vuelta del main.
static void mostrar_reaccion(uint8_t id_reaccion, bool sobre_mi)
{
    if (id_reaccion >= N_REACCIONES) {
        ESP_LOGW(TAG, "Reaccion desconocida: %d", id_reaccion);
        return;
    }
    ESP_LOGI(TAG, "Reaccion %s sobre %s", id_reaccion == REACCION_CORAZON ? "corazon" : "pedido de atencion",
             sobre_mi ? "mi" : "el peer");
    s_reaccion = id_reaccion;
    s_reaccion_sobre_mi = sobre_mi;
    s_reaccion_hasta = xTaskGetTickCount() + REACCION_DURACION;
}

// Titileo del fondo: alterna invertido / normal cada TITILAR_PERIODO mientras dure
static void actualizar_titileo(void)
{
    TickType_t ahora = xTaskGetTickCount();
    bool invertir = false;
    if ((int32_t)(s_titilar_hasta - ahora) > 0) {
        invertir = ((ahora / TITILAR_PERIODO) % 2) == 0;
    }
    if (invertir != s_invertido) {        // solo mando el comando cuando cambia
        display_invert(invertir);
        s_invertido = invertir;
    }
}

/* ================================================================
 *  Lógica del juego
 * ================================================================ */

static int8_t boton_a_direccion(button_id_t id)
{
    // El joystick está rotado en la placa, por eso no coincide 1 a 1
    switch (id) {
        case BTN_UP:    return 2;
        case BTN_RIGHT: return 3;
        case BTN_DOWN:  return 0;
        case BTN_LEFT:  return 1;
        default:        return -1;   // no es una dirección
    }
}

// Fin del juego: muestra GAME OVER y cierra la conexión.
// avisar_peer = true lo usa el kentuki al morir, para que el amo también se entere.
static void game_over(bool avisar_peer)
{
    ESP_LOGW(TAG, "GAME OVER");

    if (avisar_peer) {
        // Lo mando varias veces por si se pierde alguno; el amo lo procesa una sola vez
        for (int i = 0; i < 3; i++) {
            enviar(MSG_GAMEOVER, 0);
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        vTaskDelay(pdMS_TO_TICKS(100));   // dar tiempo a que salga el último antes de cerrar
    }

    esp_now_deinit();
    esp_wifi_stop();

    display_invert(false);                // por si murió en medio de un titileo
    display_clear();
    texto_centrado(20, "GAME OVER");
    texto_centrado(36, "EL KENTUKI MURIO");
    display_flush();
}

// El kentuki pierde salud con el tiempo. Devuelve true si murió.
static bool actualizar_salud(void)
{
    if (MI_ROL != ROL_KENTUKI || (int32_t)(xTaskGetTickCount() - s_prox_decremento) < 0) {
        return false;
    }
    s_prox_decremento += SALUD_PERIODO;
    if (salud > 0) salud--;
    ESP_LOGI(TAG, "Salud: %d", salud);
    return salud == 0;
}

static void manejar_boton(const button_event_t *ev)
{
    // Moverse de habitación
    int8_t dir = boton_a_direccion(ev->id);
    if (dir >= 0 && rooms[in_room][dir] != -1) {
        in_room = rooms[in_room][dir];
        otro_aca = false;            // cambié de habitación: no sé si está, me avisa si sí
        enviar_habitacion();         // siempre aviso, así el otro sabe también que me fui
    }

    // Acciones: solo si estamos en la misma habitación
    if (otro_aca && ev->id == BTN_KEY1) {
        if (MI_ROL == ROL_AMO) {
            acariciar();
        } else {
            pedir_atencion();
            mostrar_reaccion(REACCION_ATENCION, true);   // "!" sobre mí en mi pantalla
        }
    }
}

// Procesa un mensaje del peer. Devuelve true si terminó el juego.
static bool manejar_mensaje(const mensaje_t *msg)
{
    ESP_LOGI(TAG, "Recibido: accion=%d valor=%d", msg->accion, msg->valor);

    switch (msg->accion) {
        case MSG_HABITACION: {
            bool estaba = otro_aca;
            otro_aca = (msg->valor == in_room);
            // Respondo solo si recién llegó a mi habitación (falso -> verdadero),
            // así le aviso que estoy acá sin quedar rebotando mensajes.
            if (otro_aca && !estaba) enviar_habitacion();
            break;
        }
        case MSG_ACARICIAR:
            if (MI_ROL == ROL_KENTUKI) {
                salud = (salud + SALUD_CARICIA > SALUD_MAX) ? SALUD_MAX : salud + SALUD_CARICIA;
                enviar_reaccion(REACCION_CORAZON);            // se manda siempre, aunque la salud esté llena
                mostrar_reaccion(REACCION_CORAZON, true);     // y también lo muestro sobre mí
            }
            break;
        case MSG_PEDIRATT:
            if (MI_ROL == ROL_AMO) {
                mostrar_reaccion(REACCION_ATENCION, false);
                s_titilar_hasta = xTaskGetTickCount() + REACCION_DURACION;   // titila mientras se ve el "!"
            }
            break;
        case MSG_REACCION:
            mostrar_reaccion(msg->valor, false);             // reacción del peer: arriba de su cabeza
            break;
        case MSG_GAMEOVER:
            game_over(false);                                 // el kentuki murió: cierro yo también
            return true;
        default:
            break;
    }
    return false;
}

/* ================================================================
 *  app_main
 * ================================================================ */

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK( nvs_flash_erase() );
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK( ret );

    ESP_ERROR_CHECK( display_init() );
    wifi_init();
    espnow_init();
    buttons_init();
    QueueHandle_t cola_botones = buttons_event_queue();

    dibujar_habitacion();
    s_prox_decremento = xTaskGetTickCount() + SALUD_PERIODO;

    while (1) {
        if (actualizar_salud()) {
            game_over(true);
            return;                   // termina app_main: el juego terminó
        }

        // Espera hasta 50 ms un botón; si no llega, sigue el ciclo
        button_event_t ev;
        if (xQueueReceive(cola_botones, &ev, pdMS_TO_TICKS(50)) == pdTRUE && ev.pressed) {
            manejar_boton(&ev);
        }

        // ¿Llegó algo por la red?
        mensaje_t msg;
        if (xQueueReceive(s_cola_red, &msg, 0) == pdTRUE && manejar_mensaje(&msg)) {
            return;
        }

        actualizar_titileo();
        dibujar_habitacion();
    }
}
