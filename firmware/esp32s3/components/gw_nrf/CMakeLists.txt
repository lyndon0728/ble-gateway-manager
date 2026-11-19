idf_component_register(SRCS "gw_nrf.c" "gw_scan_config.c" INCLUDE_DIRS "include"
    REQUIRES gw_wire gw_authority esp_driver_uart esp_driver_gpio esp_timer json mbedtls)
