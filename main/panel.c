/*
 * Wi-Fi and the control panel: the Go simulator's page and JSON API
 * (GET /status, POST /command) served from the board, plus POST /wifi.
 *
 * With a saved network the board joins it (and takes the time from NTP);
 * without one, or if joining fails, it runs its own access point. Its
 * password comes from the chip's MAC and is printed on the serial console,
 * so no credential is built into the firmware.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"

static const char *TAG = "panel";
static dash_pod_t *the_pod;

extern const char panel_html_start[] asm("_binary_panel_html_start");
extern const char panel_html_end[] asm("_binary_panel_html_end");

/* ---- Wi-Fi ----------------------------------------------------------------- */

static int sta_failures;
static bool ap_started;
static bool sntp_started;

static void ap_credentials(char ssid[33], char pass[17])
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(ssid, 33, "%s-%02X%02X", CONFIG_ESPSIM_AP_SSID_PREFIX, mac[4], mac[5]);
    snprintf(pass, 17, "pod-%02x%02x%02x%02x", mac[2] ^ 0x5a, mac[3] ^ 0xa5, mac[4] ^ 0x3c, mac[5] ^ 0xc3);
}

static void start_ap(void)
{
    if (ap_started) {
        return;
    }
    ap_started = true;
    char ssid[33], pass[17];
    ap_credentials(ssid, pass);
    wifi_config_t ap = {0};
    memcpy(ap.ap.ssid, ssid, strlen(ssid));
    ap.ap.ssid_len = (uint8_t)strlen(ssid);
    memcpy(ap.ap.password, pass, strlen(pass));
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    /* Broadcast the name: the panel is only useful if you can find it, and a
     * hidden access point is one a phone cannot be captured by either. */
    ap.ap.ssid_hidden = 0;
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    esp_wifi_set_mode(mode == WIFI_MODE_STA ? WIFI_MODE_APSTA : WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    ESP_LOGW(TAG, "access point \"%s\", password \"%s\": control panel at http://192.168.4.1/", ssid, pass);
}

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_START) {
        captive_portal_start();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        /* The access point runs either way, so a station that will not join is
         * only worth a note. */
        if (++sta_failures == 5) {
            ESP_LOGW(TAG, "could not join the saved network; the access point is up regardless");
        }
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        sta_failures = 0;
        ESP_LOGW(TAG, "control panel at http://" IPSTR "/", IP2STR(&e->ip_info.ip));
        if (!sntp_started) {
            sntp_started = true;
            esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            esp_netif_sntp_init(&cfg);
        }
    }
}

static void wifi_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL);

    char ssid[33] = {0}, pass[65] = {0};
    if (store_load_wifi(ssid, pass)) {
        wifi_config_t sta = {0};
        memcpy(sta.sta.ssid, ssid, strlen(ssid));
        memcpy(sta.sta.password, pass, strlen(pass));
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_set_config(WIFI_IF_STA, &sta);
        ESP_LOGI(TAG, "joining \"%s\"", ssid);
    }
    /* The access point runs either way, so the panel is always reachable and
     * always there to be captured. start_ap() promotes the mode to APSTA when
     * a station is configured. */
    start_ap();
    ESP_ERROR_CHECK(esp_wifi_start());
}

/* ---- HTTP -------------------------------------------------------------------- */

static esp_err_t send_status(httpd_req_t *req)
{
    char json[1024];
    int n = dash_pod_status_json(the_pod, json, sizeof json);
    if (n < 0) {
        return httpd_resp_send_500(req);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, n);
}

static esp_err_t get_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, panel_html_start, panel_html_end - panel_html_start);
}

/* Anything the panel does not serve is a phone's captive portal probe: it
 * asked for its connectivity check and belongs at the panel instead. iOS
 * wants a body, not just the redirect, before it calls it a portal. */
static esp_err_t redirect_to_panel(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, "Redirect to the ESP-Sim control panel", HTTPD_RESP_USE_STRLEN);
}

static int read_body(httpd_req_t *req, char *buf, size_t size)
{
    if (req->content_len >= size) {
        return -1;
    }
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r <= 0) {
            return -1;
        }
        got += (size_t)r;
    }
    buf[got] = 0;
    return (int)got;
}

static esp_err_t post_command(httpd_req_t *req)
{
    char body[256];
    int n = read_body(req, body, sizeof body);
    if (n < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad request body");
        return ESP_OK;
    }
    const char *err = dash_pod_panel_command(the_pod, body, (size_t)n);
    if (err) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, err);
        return ESP_OK;
    }
    return send_status(req);
}

/* ---- the CGM: GET /cgm, POST /cgm {"command": ...} ---------------------------- */

static esp_err_t send_cgm(httpd_req_t *req)
{
    char json[768];
    size_t n = cgm_app_status_json(json, sizeof json);
    if (n == 0) {
        return httpd_resp_send_500(req);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, (ssize_t)n);
}

static esp_err_t post_cgm(httpd_req_t *req)
{
    char body[256];
    if (read_body(req, body, sizeof body) < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad request body");
        return ESP_OK;
    }
    const char *err = cgm_app_command(body);
    if (err) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, err);
        return ESP_OK;
    }
    return send_cgm(req);
}

/* Reads a JSON string value ("key": "value") with \" and \\ escapes. */
static bool json_string(const char *j, const char *key, char *out, size_t size)
{
    char pat[24];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(j, pat);
    if (!p) {
        return false;
    }
    p += strlen(pat);
    while (*p == ' ' || *p == ':') {
        p++;
    }
    if (*p++ != '"') {
        return false;
    }
    size_t n = 0;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) {
            p++;
        }
        if (n + 1 >= size) {
            return false;
        }
        out[n++] = *p++;
    }
    out[n] = 0;
    return *p == '"';
}

static void restart(void *arg)
{
    esp_restart();
}

/* Reboots the board after a moment, dropping whatever the phone is doing. */
static void schedule_restart(unsigned ms)
{
    static esp_timer_handle_t t;
    const esp_timer_create_args_t args = {.callback = restart, .name = "restart"};
    if (!t) {
        esp_timer_create(&args, &t);
    }
    esp_timer_start_once(t, (uint64_t)ms * 1000);
}

static esp_err_t post_wifi(httpd_req_t *req)
{
    char body[256], ssid[33], pass[65];
    if (read_body(req, body, sizeof body) < 0 || !json_string(body, "ssid", ssid, sizeof ssid) ||
        !json_string(body, "password", pass, sizeof pass)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "want {\"ssid\": ..., \"password\": ...}");
        return ESP_OK;
    }
    if (ssid[0] && pass[0] && strlen(pass) < 8) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "a WPA2 password has at least 8 characters");
        return ESP_OK;
    }
    store_save_wifi(ssid, pass);
    httpd_resp_sendstr(req, "saved; restarting\n");
    schedule_restart(1000);
    return ESP_OK;
}

/* ---- the Medtrum patch: GET /patch, POST /patch ----------------------------- */

#if CONFIG_ESPSIM_DEVICE_MEDTRUM
static esp_err_t send_patch(httpd_req_t *req)
{
    char json[512];
    size_t n = medtrum_app_status_json(json, sizeof json);
    if (n == 0) {
        return httpd_resp_send_500(req);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, (ssize_t)n);
}

static esp_err_t post_patch(httpd_req_t *req)
{
    char body[256];
    if (read_body(req, body, sizeof body) < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad request body");
        return ESP_OK;
    }
    /* The patch core has no crash hook, so the panel reboots the base itself:
     * the phone loses its in-flight command and finds the base again. */
    char cmd[32];
    if (json_string(body, "command", cmd, sizeof cmd) && strcmp(cmd, "medtrumCrash") == 0) {
        schedule_restart(1500);
        return send_patch(req);
    }
    const char *err = medtrum_app_command(body);
    if (err) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, err);
        return ESP_OK;
    }
    return send_patch(req);
}
#endif

void panel_start(dash_pod_t *pod)
{
    the_pod = pod;
    wifi_start();

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &cfg));
    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = get_index},
        {.uri = "/status", .method = HTTP_GET, .handler = send_status},
        {.uri = "/command", .method = HTTP_POST, .handler = post_command},
        {.uri = "/wifi", .method = HTTP_POST, .handler = post_wifi},
        {.uri = "/cgm", .method = HTTP_GET, .handler = send_cgm},
        {.uri = "/cgm", .method = HTTP_POST, .handler = post_cgm},
#if CONFIG_ESPSIM_DEVICE_MEDTRUM
        {.uri = "/patch", .method = HTTP_GET, .handler = send_patch},
        {.uri = "/patch", .method = HTTP_POST, .handler = post_patch},
#endif
    };
    for (size_t i = 0; i < sizeof routes / sizeof routes[0]; i++) {
        httpd_register_uri_handler(server, &routes[i]);
    }
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, redirect_to_panel);
}
