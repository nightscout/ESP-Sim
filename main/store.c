/*
 * Non-volatile storage: the pod's and the CGM's state, so a reboot is the
 * same pod and sensor, and the Wi-Fi network the panel saved.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
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
    size_t len = sizeof *st;
    return nvs_get_blob(nvs, "cgm", st, &len) == ESP_OK && len == sizeof *st && st->version == CGM_STATE_VERSION;
}

bool store_load_medtrum(medtrum_saved_t *st)
{
    size_t len = sizeof *st;
    return nvs_get_blob(nvs, "medtrum", st, &len) == ESP_OK && len == sizeof *st;
}

void store_save_medtrum(void *ctx, const medtrum_saved_t *st)
{
    esp_err_t err = nvs_set_blob(nvs, "medtrum", st, sizeof *st);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not save the Medtrum state: %s", esp_err_to_name(err));
    }
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
