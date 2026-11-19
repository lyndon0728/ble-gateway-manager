idf_component_register(SRCS "gw_peripheral.c" "gw_pg.c" INCLUDE_DIRS "include"
    REQUIRES gw_core gw_capture gw_nrf gw_actions gw_transport gw_authority json
    PRIV_REQUIRES protocomm protobuf-c mbedtls esp_timer esp_system)
