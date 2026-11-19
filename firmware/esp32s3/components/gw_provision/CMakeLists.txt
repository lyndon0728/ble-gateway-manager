idf_component_register(SRCS "gw_provision.c" "gw_provision_guard.c" "gw_provision_io.c"
    INCLUDE_DIRS "include"
    REQUIRES gw_core gw_actions protocomm esp_http_server esp_netif esp_timer esp_wifi json
    PRIV_REQUIRES protobuf-c mbedtls
    EMBED_FILES "assets/index.html" "assets/app.js" "assets/style.css")
