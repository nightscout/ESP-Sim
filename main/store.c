/*
 * Non-volatile storage: the pod's and the CGM's state, so a reboot is the
 * same pod and sensor, and the Wi-Fi network the panel saved.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stddef.h>
#include <string.h>

#include "app.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "store";
static nvs_handle_t nvs;

void store_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(nvs_open("dashpod", NVS_READWRITE, &nvs));
}

bool store_load_state(dash_state_t *st)
{
    size_t len = sizeof *st;
    if (nvs_get_blob(nvs, "state", st, &len) != ESP_OK || len != sizeof *st || !dash_state_valid(st)) {
        return false;
    }
    return true;
}

void store_save_state(void *ctx, const dash_state_t *st)
{
    esp_err_t err = nvs_set_blob(nvs, "state", st, sizeof *st);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not save the pod state: %s", esp_err_to_name(err));
    }
}

/* The board's last known time (Unix seconds), saved now and then: without a
 * network a restart picks up from it instead of from an old save. */
bool store_load_clock(int64_t *t)
{
    return nvs_get_i64(nvs, "clock", t) == ESP_OK;
}

void store_save_clock(int64_t t)
{
    nvs_set_i64(nvs, "clock", t);
    nvs_commit(nvs);
}

/* The board's time zone (POSIX TZ), from the panel's browser. */
bool store_load_tz(char tz[64])
{
    size_t len = 64;
    return nvs_get_str(nvs, "tz", tz, &len) == ESP_OK && tz[0];
}

void store_save_tz(const char *tz)
{
    nvs_set_str(nvs, "tz", tz);
    nvs_commit(nvs);
}

bool store_load_wifi(char ssid[33], char password[65])
{
    size_t l1 = 33, l2 = 65;
    if (nvs_get_str(nvs, "wifi_ssid", ssid, &l1) != ESP_OK || ssid[0] == 0) {
        return false;
    }
    if (nvs_get_str(nvs, "wifi_pass", password, &l2) != ESP_OK) {
        password[0] = 0;
    }
    return true;
}

void store_save_wifi(const char *ssid, const char *password)
{
    if (ssid[0] == 0) {
        nvs_erase_key(nvs, "wifi_ssid");
        nvs_erase_key(nvs, "wifi_pass");
    } else {
        nvs_set_str(nvs, "wifi_ssid", ssid);
        nvs_set_str(nvs, "wifi_pass", password);
    }
    nvs_commit(nvs);
}

bool store_load_cgm(cgm_state_t *st)
{
    /* A save from before the virtual person is shorter: its new fields load
     * as zero (the manual pattern), and the sensor stays the same. */
    memset(st, 0, sizeof *st);
    size_t len = sizeof *st;
    return nvs_get_blob(nvs, "cgm", st, &len) == ESP_OK && len >= offsetof(cgm_state_t, pattern) &&
           len <= sizeof *st && st->version == CGM_STATE_VERSION;
}

void store_save_cgm(void *ctx, const cgm_state_t *st)
{
    esp_err_t err = nvs_set_blob(nvs, "cgm", st, sizeof *st);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not save the CGM state: %s", esp_err_to_name(err));
    }
}

uint8_t store_load_pump(void)
{
    uint8_t v = 0;
    nvs_get_u8(nvs, "pump", &v);
    return v;
}

void store_save_pump(uint8_t pump)
{
    nvs_set_u8(nvs, "pump", pump);
    nvs_commit(nvs);
}

/* The YpsoPump's bonded devices, oldest first (7 bytes each: type, address). */
size_t store_load_ypso_peers(uint8_t *out, size_t cap)
{
    size_t len = cap;
    return nvs_get_blob(nvs, "ypsoPeers", out, &len) == ESP_OK ? len : 0;
}

void store_save_ypso_peers(const uint8_t *p, size_t n)
{
    nvs_set_blob(nvs, "ypsoPeers", p, n);
    nvs_commit(nvs);
}

/* The YpsoPump lets phones in without its password unless told not to. */
bool store_load_ypso_open(void)
{
    uint8_t v = 1;
    nvs_get_u8(nvs, "ypsoOpen", &v);
    return v != 0;
}

void store_save_ypso_open(bool open)
{
    nvs_set_u8(nvs, "ypsoOpen", open);
    nvs_commit(nvs);
}

bool store_load_ypso(ypso_state_t *st)
{
    /* A state saved before the key exchange was simulated is shorter: the
     * fields after it start at zero (ypso_init fills them in). */
    memset(st, 0, sizeof *st);
    size_t len = sizeof *st;
    return nvs_get_blob(nvs, "ypso", st, &len) == ESP_OK && len >= offsetof(ypso_state_t, server_priv) &&
           len <= sizeof *st && st->version == YPSO_STATE_VERSION && st->fw < YPSO_FW_COUNT;
}

void store_save_ypso(void *ctx, const ypso_state_t *st)
{
    esp_err_t err = nvs_set_blob(nvs, "ypso", st, sizeof *st);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not save the YpsoPump state: %s", esp_err_to_name(err));
    }
}
