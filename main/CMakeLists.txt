idf_component_register(SRCS "espnow_example_main.c" "display.c" "buttons.c"
                    PRIV_REQUIRES nvs_flash esp_event esp_netif esp_wifi esp_lcd esp_driver_spi esp_driver_i2s esp_driver_gpio
                    INCLUDE_DIRS ".")
