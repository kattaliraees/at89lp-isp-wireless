idf_component_register(SRCS "wifi_webserver.c"
                    INCLUDE_DIRS "include"
                    REQUIRES esp_wifi nvs_flash esp_event esp_netif esp_http_server)
