idf_component_register(SRCS "gw_http.c" INCLUDE_DIRS "include"
    REQUIRES gw_core esp_http_client esp_netif lwip json esp_timer)
