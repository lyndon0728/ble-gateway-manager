idf_component_register(SRCS "gw_nrf.c" INCLUDE_DIRS "include"
    REQUIRES gw_wire esp_driver_uart esp_driver_gpio esp_timer json mbedtls)
