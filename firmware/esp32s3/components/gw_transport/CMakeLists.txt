idf_component_register(SRCS "gw_transport.c" "gw_transport_rules.c" INCLUDE_DIRS "include"
    REQUIRES gw_core gw_http mqtt json freertos)
