idf_component_register(
    SRCS main.c ble.c cgm_app.c medtrum_app.c panel.c store.c
    INCLUDE_DIRS .
    EMBED_TXTFILES panel.html
    PRIV_REQUIRES dashpod cgmsim medtrumpatch bt nvs_flash esp_wifi esp_netif esp_event esp_http_server esp_timer
)
