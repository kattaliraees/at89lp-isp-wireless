idf_component_register(SRCS "esp-isp-at89lp.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES at89lp_isp wifi_webserver nvs_flash)
