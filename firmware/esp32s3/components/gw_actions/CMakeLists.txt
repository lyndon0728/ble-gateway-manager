idf_component_register(SRCS "gw_actions.c" "gw_ble.c" "gw_ble_bonds.c" "gw_gatt_nrf.c" "gw_ota.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "vendor/monocypher"
    REQUIRES gw_core gw_wire gw_nrf gw_capture gw_http gw_crypto gw_authority json bt app_update
             esp_app_format esp_partition esp_system esp_timer mbedtls nvs_flash)
