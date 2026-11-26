/* The settings page: the board's own network, clock and firmware. The page's
 * data comes from this board, not the fork's assumptions: no mDNS, no serial
 * console, and the AP's name and password are the ones the panel generated.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app.h"
#include "esp_idf_version.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "pages.h"
#include "settings_app.h"

static const char *TAG = "settings";

extern const char settings_html_start[] asm("_binary_settings_html_start");
extern const char settings_html_end[] asm("_binary_settings_html_end");

/* JSON-escape into out, leaving room for the closing quote and terminator. */
static void json_escape(char *out, size_t cap, const char *in)
{
    size_t n = 0;
    if (!in) {
        in = "";
    }
    for (; *in && n + 7 < cap; in++) {
        unsigned char c = (unsigned char)*in;
        switch (c) {
        case '"': out[n++] = '\\'; out[n++] = '"'; break;
        case '\\': out[n++] = '\\'; out[n++] = '\\'; break;
        case '\n': out[n++] = '\\'; out[n++] = 'n'; break;
        case '\r': out[n++] = '\\'; out[n++] = 'r'; break;
        case '\t': out[n++] = '\\'; out[n++] = 't'; break;
        default:
            if (c < 0x20) {
                int w = snprintf(out + n, cap - n, "\\u%04x", c);
                if (w > 0) {
                    n += (size_t)w;
                }
            } else {
                out[n++] = (char)c;
            }
            break;
        }
    }
    out[n] = 0;
}

/* The running app image's length in flash. The partition's capacity is the
 * fallback; the metadata read is cached after the first request. */
static uint32_t app_image_size(void)
{
    static uint32_t cached;
    if (cached == 0) {
        const esp_partition_t *part = esp_ota_get_running_partition();
        if (part) {
            esp_partition_pos_t pos = {.offset = part->address, .size = part->size};
            esp_image_metadata_t meta;
            if (esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &meta) == ESP_OK) {
                cached = meta.image_len;
            } else {
                cached = part->size;
            }
        }
    }
    return cached;
}

static void ip_string(char *out, size_t cap)
{
    out[0] = 0;
    const char *key = panel_sta_joined() ? "WIFI_STA_DEF" : "WIFI_AP_DEF";
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey(key);
    if (!netif) {
        return;
    }
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(netif, &info) == ESP_OK) {
        esp_ip4addr_ntoa(&info.ip, out, (int)cap);
    }
}

static const char *pump_name(uint8_t pump)
{
    switch (pump) {
    case PUMP_YPSO: return "YpsoPump";
    case PUMP_MEDTRUM: return "Medtrum patch";
    case PUMP_POD:
    default: return "Omnipod pod";
    }
}

static esp_err_t get_settings(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, settings_html_start, (ssize_t)(settings_html_end - settings_html_start));
}

static esp_err_t get_settings_status(httpd_req_t *req)
{
    char saved_ssid[33] = {0}, saved_pass[65];
    store_load_wifi(saved_ssid, saved_pass);

    const bool ap = panel_ap_active();
    char ap_ssid[33] = {0}, ap_pass[17] = {0};
    if (ap) {
        panel_ap_credentials(ap_ssid, ap_pass);
    }

    char ip[16];
    ip_string(ip, sizeof ip);

    char clock[32] = {0};
    time_t now = time(NULL);
    struct tm local;
    if (localtime_r(&now, &local)) {
        strftime(clock, sizeof clock, "%Y-%m-%d %H:%M:%S", &local);
    }

    char wifi_ssid_e[96], ap_ssid_e[96], ap_pass_e[64];
    char tz_e[128], built_e[64], idf_e[64], target_e[32], pump_e[48];
    json_escape(wifi_ssid_e, sizeof wifi_ssid_e, saved_ssid);
    json_escape(ap_ssid_e, sizeof ap_ssid_e, ap_ssid);
    json_escape(ap_pass_e, sizeof ap_pass_e, ap_pass);
    json_escape(tz_e, sizeof tz_e, CONFIG_ESPSIM_TZ);
    json_escape(built_e, sizeof built_e, __DATE__ " " __TIME__);
    json_escape(idf_e, sizeof idf_e, esp_get_idf_version());
    json_escape(target_e, sizeof target_e, CONFIG_IDF_TARGET);
    json_escape(pump_e, sizeof pump_e, pump_name(store_load_pump()));

    char json[1536];
    int n = snprintf(json, sizeof json,
                     "{\"wifi\":{\"ssid\":\"%s\",\"joined\":%s},"
                     "\"ap\":{\"active\":%s,\"ssid\":\"%s\",\"password\":\"%s\"},"
                     "\"ip\":\"%s\",\"clock\":\"%s\",\"tz\":\"%s\","
                     "\"firmware\":{\"built\":\"%s\",\"idf\":\"%s\",\"target\":\"%s\",\"size\":%lu},"
                     "\"pump\":%u,\"pumpName\":\"%s\"}",
                     wifi_ssid_e, panel_sta_joined() ? "true" : "false",
                     ap ? "true" : "false", ap_ssid_e, ap_pass_e,
                     ip, clock, tz_e,
                     built_e, idf_e, target_e, (unsigned long)app_image_size(),
                     (unsigned)store_load_pump(), pump_e);
    if (n < 0 || (size_t)n >= sizeof json) {
        return httpd_resp_send_500(req);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, n);
}

static esp_err_t post_settings_wifi(httpd_req_t *req)
{
    return panel_post_wifi(req);
}

static esp_err_t post_settings_forget(httpd_req_t *req)
{
    store_save_wifi("", "");
    httpd_resp_sendstr(req, "forgot; restarting\n");
    panel_restart_soon();
    return ESP_OK;
}

static esp_err_t post_settings_reboot(httpd_req_t *req)
{
    httpd_resp_sendstr(req, "restarting\n");
    panel_restart_soon();
    return ESP_OK;
}

static esp_err_t post_settings_pump(httpd_req_t *req)
{
    /* The same handler as POST /ypso, including pumpSelect. */
    return panel_post_ypso(req);
}

esp_err_t settings_app_register(httpd_handle_t server)
{
    const httpd_uri_t routes[] = {
        {.uri = "/settings", .method = HTTP_GET, .handler = get_settings},
        {.uri = "/settings/status", .method = HTTP_GET, .handler = get_settings_status},
        {.uri = "/settings/wifi", .method = HTTP_POST, .handler = post_settings_wifi},
        {.uri = "/settings/forget", .method = HTTP_POST, .handler = post_settings_forget},
        {.uri = "/settings/reboot", .method = HTTP_POST, .handler = post_settings_reboot},
        {.uri = "/settings/pump", .method = HTTP_POST, .handler = post_settings_pump},
    };
    for (size_t i = 0; i < sizeof routes / sizeof routes[0]; i++) {
        esp_err_t err = httpd_register_uri_handler(server, &routes[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "registering %s: %s", routes[i].uri, esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}
