idf_component_register(SRCS "gw_actions.c" "gw_ble.c" "gw_gatt_nrf.c" "gw_ota.c"
    "vendor/monocypher/monocypher.c" "vendor/monocypher/monocypher-ed25519.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "vendor/monocypher"
    REQUIRES gw_core gw_wire gw_nrf gw_capture gw_http json bt app_update
             esp_app_format esp_partition esp_system esp_timer mbedtls)
