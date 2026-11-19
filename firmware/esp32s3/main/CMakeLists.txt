idf_component_register(SRCS "app_main.c" INCLUDE_DIRS "."
    REQUIRES gw_core gw_provision gw_cloud gw_nrf gw_capture gw_actions gw_authority driver app_update)
