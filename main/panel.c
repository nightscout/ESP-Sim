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
#include <stdlib.h>
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

/* The network's time arrived (at first and about hourly). */
static void on_time_sync(struct timeval *tv)
{
    if (store_load_pump() == 1) {
        ypso_app_clock_changed();
    }
}


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
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (++sta_failures == 5) {
            ESP_LOGW(TAG, "could not join the saved network: starting the access point too");
            start_ap();
        }
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        sta_failures = 0;
        ESP_LOGW(TAG, "control panel at http://" IPSTR "/", IP2STR(&e->ip_info.ip));
        if (!sntp_started) {
            sntp_started = true;
            esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            cfg.sync_cb = on_time_sync;
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
    } else {
        start_ap();
    }
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
    return httpd_resp_send(req, panel_html_start, panel_html_end - panel_html_start);
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
    char json[1400];
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

/* ---- the YpsoPump: GET /ypso, POST /ypso {"command": ...} ----------------------- */

static esp_err_t send_ypso(httpd_req_t *req)
{
    char json[900];
    size_t n;
    if (store_load_pump() == 1) {
        n = ypso_app_status_json(json, sizeof json);
        if (n == 0 || n + 20 > sizeof json) {
            return httpd_resp_send_500(req);
        }
        n--;
        n += (size_t)snprintf(json + n, sizeof json - n, ",\"active\":true}");
    } else {
        n = (size_t)snprintf(json, sizeof json, "{\"active\":false}");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, (ssize_t)n);
}

static void restart(void *arg);
void restart_soon(void);

static esp_err_t post_ypso(httpd_req_t *req)
{
    char body[256];
    if (read_body(req, body, sizeof body) < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad request body");
        return ESP_OK;
    }
    /* Switching the pump: {"command": "pumpSelect", "pump": 0 (pod) | 1 (YpsoPump),
     * "podType": 0 | 1, "fw": 0-4, "fresh": true}. The board restarts. */
    if (strstr(body, "\"pumpSelect\"")) {
        long pump = -1, fw = -1, pod_type = -1, fresh = 0;
        const char *p;
        if ((p = strstr(body, "\"pump\""))) {
            pump = strtol(strchr(p, ':') + 1, NULL, 10);
        }
        if ((p = strstr(body, "\"fw\""))) {
            fw = strtol(strchr(p, ':') + 1, NULL, 10);
        }
        if ((p = strstr(body, "\"podType\""))) {
            pod_type = strtol(strchr(p, ':') + 1, NULL, 10);
        }
        fresh = strstr(body, "\"fresh\":true") != NULL;
        if (pump != 0 && pump != 1) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "pump is 0 (Omnipod pod) or 1 (YpsoPump)");
            return ESP_OK;
        }
        if (pump == 1) {
            if (fw < 0 || fw >= YPSO_FW_COUNT) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "fw is 0 to 4");
                return ESP_OK;
            }
            ypso_state_t *st = malloc(sizeof *st);
            if (!st) {
                return httpd_resp_send_500(req);
            }
            /* The same pump on another firmware keeps its key and history;
             * "fresh" (or none yet) makes a new one. */
            if (fresh || !store_load_ypso(st)) {
                ypso_state_fresh(st, (ypso_fw_t)fw);
                ble_forget_bonds();
            }
            st->fw = (uint8_t)fw;
            store_save_ypso(NULL, st);
            free(st);
        } else if (pod_type == 0 || pod_type == 1) {
            char cmd[64];
            snprintf(cmd, sizeof cmd, "{\"command\":\"setPodType\",\"value\":%ld}", pod_type);
            if (store_load_pump() == 1 || pod_type != the_pod->st.pod_type) {
                dash_pod_panel_command(the_pod, cmd, strlen(cmd));
            }
        }
        store_save_pump((uint8_t)pump);
        httpd_resp_sendstr(req, "{\"restarting\":true}");
        restart_soon();
        return ESP_OK;
    }
    char *reply = malloc(4200);
    if (!reply) {
        return httpd_resp_send_500(req);
    }
    const char *err = store_load_pump() == 1 ? ypso_app_command(body, reply, 4200) : "the board is not a YpsoPump";
    esp_err_t r;
    if (err) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, err);
        r = ESP_OK;
    } else if (reply[0]) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        r = httpd_resp_sendstr(req, reply);
    } else {
        r = send_ypso(req);
    }
    memset(reply, 0, 4200);
    free(reply);
    return r;
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

void restart_soon(void)
{
    static esp_timer_handle_t t;
    const esp_timer_create_args_t args = {.callback = restart, .name = "restart"};
    if (!t) {
        esp_timer_create(&args, &t);
    }
    esp_timer_start_once(t, 1000 * 1000);
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
    restart_soon();
    return ESP_OK;
}

/* GET /tz: the board's zone. POST /tz {"tz": "<POSIX TZ>"}: the panel's
 * browser sends its own, so the board (and the pump's clock) keep local time. */
static esp_err_t get_tz(httpd_req_t *req)
{
    const char *tz = getenv("TZ");
    char out[96];
    snprintf(out, sizeof out, "{\"tz\":\"%s\"}", tz ? tz : "");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, out);
}

static esp_err_t post_tz(httpd_req_t *req)
{
    char body[160] = {0}, tz[64];
    int got = 0;
    while (got < req->content_len && got < (int)sizeof body - 1) {
        int r = httpd_req_recv(req, body + got, sizeof body - 1 - got);
        if (r <= 0) {
            return httpd_resp_send_500(req);
        }
        got += r;
    }
    if (!json_string(body, "tz", tz, sizeof tz) || !tz[0]) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "want {\"tz\": \"<POSIX TZ>\"}");
        return ESP_OK;
    }
    for (const char *c = tz; *c; c++) {
        if (!((*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') ||
              strchr(",./:+-<>", *c))) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "not a POSIX TZ");
            return ESP_OK;
        }
    }
    const char *old = getenv("TZ");
    if (!old || strcmp(old, tz) != 0) {
        store_save_tz(tz);
        setenv("TZ", tz, 1);
        tzset();
        ESP_LOGI(TAG, "time zone now %s", tz); /* the pump follows within a second (ypso_app.c) */
    }
    return get_tz(req);
}

void panel_start(dash_pod_t *pod)
{
    the_pod = pod;
    wifi_start();

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 12288; /* room for the larger handlers */
    cfg.max_uri_handlers = 20;
    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &cfg));
    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = get_index},
        {.uri = "/status", .method = HTTP_GET, .handler = send_status},
        {.uri = "/command", .method = HTTP_POST, .handler = post_command},
        {.uri = "/wifi", .method = HTTP_POST, .handler = post_wifi},
        {.uri = "/cgm", .method = HTTP_GET, .handler = send_cgm},
        {.uri = "/cgm", .method = HTTP_POST, .handler = post_cgm},
        {.uri = "/ypso", .method = HTTP_GET, .handler = send_ypso},
        {.uri = "/ypso", .method = HTTP_POST, .handler = post_ypso},
        {.uri = "/tz", .method = HTTP_GET, .handler = get_tz},
        {.uri = "/tz", .method = HTTP_POST, .handler = post_tz},
    };
    for (size_t i = 0; i < sizeof routes / sizeof routes[0]; i++) {
        httpd_register_uri_handler(server, &routes[i]);
    }
    remote_start(server);
}
