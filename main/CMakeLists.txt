idf_component_register(
    SRCS main.c ble.c cgm_app.c ypso_app.c panel.c store.c remote.c
    INCLUDE_DIRS .
    EMBED_TXTFILES panel.html
    PRIV_REQUIRES dashpod cgmsim ypsosim bt nvs_flash esp_wifi esp_netif esp_event esp_http_server esp_timer app_update esp_app_format
)
