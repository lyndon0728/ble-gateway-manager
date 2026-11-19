idf_component_register(SRCS "gw_core.c" "gw_store.c" "gw_network.c" "gw_validate.c" "gw_rules.c" "gw_time.c" "gw_time_bounds.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES json esp_wifi esp_event esp_netif esp_timer nvs_flash mbedtls esp_partition esp_eth
    PRIV_REQUIRES esp_hw_support driver lwip spi_flash esp_psram bootloader_support)
