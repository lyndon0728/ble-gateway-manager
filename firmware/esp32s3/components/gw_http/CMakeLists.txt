idf_component_register(SRCS "gw_http.c" "gw_http_parser.c" "gw_http_time.c" "gw_http_tls_bounds.c" INCLUDE_DIRS "include"
    REQUIRES gw_core esp-tls esp_netif lwip json esp_timer freertos mbedtls)
