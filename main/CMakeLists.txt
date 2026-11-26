idf_component_register(
    SRCS main.c ble.c cgm_app.c medtrum_app.c ypso_app.c panel.c store.c
         pages.c settings_app.c pump_app.c captive.c dns.c dns_answer.c
    INCLUDE_DIRS .
    EMBED_TXTFILES panel.html setup.html settings.html pump.html
    PRIV_REQUIRES dashpod cgmsim medtrumpatch ypsosim portalsim bt nvs_flash esp_wifi esp_netif esp_event esp_http_server esp_timer lwip app_update bootloader_support
)
