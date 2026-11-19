idf_component_register(SRCS "gw_cloud.c" INCLUDE_DIRS "include"
    REQUIRES gw_core gw_http gw_nrf gw_actions gw_capture esp_timer esp_system esp_app_format json)
