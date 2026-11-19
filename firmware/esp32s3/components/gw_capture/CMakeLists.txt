idf_component_register(SRCS "gw_capture.c" "gw_spool.c" "gw_capture_rules.c"
    INCLUDE_DIRS "include"
    REQUIRES gw_wire gw_core gw_nrf gw_http esp_partition esp_timer esp_system bootloader_support json mbedtls)
