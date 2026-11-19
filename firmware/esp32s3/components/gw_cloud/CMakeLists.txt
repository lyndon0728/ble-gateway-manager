idf_component_register(SRCS "gw_cloud.c" "gw_signed_time.c" "gw_signed_time_verify.c" "gw_sync.c" INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "../gw_actions/vendor/monocypher"
    REQUIRES gw_core gw_http gw_transport gw_nrf gw_actions gw_capture gw_authority gw_peripheral esp_timer esp_system esp_app_format json)
