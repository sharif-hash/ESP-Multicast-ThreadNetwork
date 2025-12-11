idf_component_register(SRCS "esp_ot_cli.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_wifi nvs_flash esp_event driver esp_timer mqtt json bt)