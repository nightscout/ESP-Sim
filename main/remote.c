/*
 * Remote access over Wi-Fi, for developers: the log and new firmware.
 *
 *   GET  /log?since=N  the log, from byte N of everything logged since boot
 *                      (the last 32 KB are kept). The X-Log-Next header is
 *                      the N for the next call; a smaller one than sent means
 *                      the board restarted.
 *   GET  /ypsohistory  the YpsoPump's event log, newest first.
 *   POST /ota          a firmware image (build/esp_sim_dash.bin) as the body:
 *                      written to the other app slot, checked, booted. A new
 *                      image that does not stay up for a minute is rolled
 *                      back by the bootloader.
 *
 * Like the rest of the panel, anyone on the same network can use these: the
 * board is test equipment and delivers nothing.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "remote";

/* ---- the log ring ----------------------------------------------------------------------- */

#define RING (32 * 1024)
static char ring[RING];
static uint32_t total; /* bytes ever logged since boot */
static portMUX_TYPE ring_mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t original;

static int tee(const char *fmt, va_list ap)
{
    char line[256];
    va_list copy;
    va_copy(copy, ap);
    int n = vsnprintf(line, sizeof line, fmt, copy);
    va_end(copy);
    if (n > 0) {
        if (n >= (int)sizeof line) {
            n = sizeof line - 1;
        }
        portENTER_CRITICAL(&ring_mux);
        for (int i = 0; i < n; i++) {
            ring[(total + i) % RING] = line[i];
        }
        total += (uint32_t)n;
        portEXIT_CRITICAL(&ring_mux);
    }
    return original ? original(fmt, ap) : n;
}

void remote_log_init(void)
{
    original = esp_log_set_vprintf(tee);
}

static esp_err_t get_log(httpd_req_t *req)
{
    uint32_t since = 0;
    char q[32], v[16];
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK &&
        httpd_query_key_value(q, "since", v, sizeof v) == ESP_OK) {
        since = (uint32_t)strtoul(v, NULL, 10);
    }
    portENTER_CRITICAL(&ring_mux);
    uint32_t end = total;
    portEXIT_CRITICAL(&ring_mux);
    uint32_t from = since > end ? 0 : since; /* a restart: from the oldest kept */
    if (end - from > RING) {
        from = end - RING;
    }
    char next[16];
    snprintf(next, sizeof next, "%lu", (unsigned long)end);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "X-Log-Next", next);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    /* In small pieces: no 32 KB buffer to spare. Lines logged meanwhile may
     * overwrite the oldest part; that much is lost, nothing else. */
    char piece[512];
    for (uint32_t at = from; at < end;) {
        uint32_t k = end - at < sizeof piece ? end - at : sizeof piece;
        portENTER_CRITICAL(&ring_mux);
        for (uint32_t i = 0; i < k; i++) {
            piece[i] = ring[(at + i) % RING];
        }
        portEXIT_CRITICAL(&ring_mux);
        if (httpd_resp_send_chunk(req, piece, (ssize_t)k) != ESP_OK) {
            return ESP_FAIL;
        }
        at += k;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* GET /ypsohistory: the YpsoPump's event log, newest first. */
static esp_err_t get_history(httpd_req_t *req)
{
    char *buf = malloc(4096);
    if (!buf) {
        return httpd_resp_send_500(req);
    }
    size_t n = store_load_pump() == 1 ? ypso_app_history_text(buf, 4096, 45) : (size_t)snprintf(buf, 4096, "no YpsoPump\n");
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t r = httpd_resp_send(req, buf, (ssize_t)n);
    free(buf);
    return r;
}

/* ---- firmware over Wi-Fi ------------------------------------------------------------------ */

static esp_err_t post_ota(httpd_req_t *req)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "no second app slot: flash once over USB with the OTA partition table\n");
        return ESP_OK;
    }
    if (req->content_len < 1024 || req->content_len > part->size) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "send build/esp_sim_dash.bin as the body\n");
        return ESP_OK;
    }
    ESP_LOGW(TAG, "new firmware over Wi-Fi: %u bytes into %s", (unsigned)req->content_len, part->label);
    esp_ota_handle_t h;
    esp_err_t err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &h);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_OK;
    }
    char *buf = malloc(4096);
    if (!buf) {
        esp_ota_abort(h);
        return httpd_resp_send_500(req);
    }
    size_t got = 0;
    int retries = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, buf, req->content_len - got < 4096 ? req->content_len - got : 4096);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++retries < 5) {
            continue;
        }
        if (r <= 0) {
            err = ESP_FAIL;
            break;
        }
        retries = 0;
        err = esp_ota_write(h, buf, (size_t)r);
        if (err != ESP_OK) {
            break;
        }
        got += (size_t)r;
    }
    free(buf);
    if (err != ESP_OK) {
        esp_ota_abort(h);
        ESP_LOGE(TAG, "firmware upload failed after %u bytes: %s", (unsigned)got, esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "upload failed\n");
        return ESP_OK;
    }
    err = esp_ota_end(h); /* checks the image */
    if (err == ESP_OK) {
        err = esp_ota_set_boot_partition(part);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "firmware refused: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, esp_err_to_name(err));
        return ESP_OK;
    }
    ESP_LOGW(TAG, "firmware written to %s: restarting into it", part->label);
    httpd_resp_sendstr(req, "ok: restarting into the new firmware (rolled back if it does not stay up a minute)\n");
    panel_restart_soon();
    return ESP_OK;
}

/* A new image proves itself by running a minute; until then the bootloader
 * rolls it back on the next restart. */
static void prove(void *arg)
{
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        ESP_LOGI(TAG, "this firmware stayed up a minute: kept");
    }
}

void remote_start(httpd_handle_t server)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    const esp_app_desc_t *d = esp_app_get_description();
    ESP_LOGI(TAG, "firmware %s (%s %s) in %s; log at /log, new firmware at /ota", d->version, d->date, d->time,
             run ? run->label : "?");
    nvs_stats_t ns;
    if (nvs_get_stats(NULL, &ns) == ESP_OK) {
        ESP_LOGI(TAG, "saved settings: %u of %u entries used (%u free)", (unsigned)ns.used_entries,
                 (unsigned)ns.total_entries, (unsigned)ns.free_entries);
    }
    ESP_LOGI(TAG, "restarted: reason %d", (int)esp_reset_reason());
    if (run && esp_ota_get_state_partition(run, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGW(TAG, "new firmware on trial: kept if it stays up a minute");
        const esp_timer_create_args_t t = {.callback = prove, .name = "prove"};
        esp_timer_handle_t timer;
        if (esp_timer_create(&t, &timer) == ESP_OK) {
            esp_timer_start_once(timer, 60 * 1000000LL);
        }
    }
    const httpd_uri_t routes[] = {
        {.uri = "/log", .method = HTTP_GET, .handler = get_log},
        {.uri = "/ota", .method = HTTP_POST, .handler = post_ota},
        {.uri = "/ypsohistory", .method = HTTP_GET, .handler = get_history},
    };
    for (size_t i = 0; i < sizeof routes / sizeof routes[0]; i++) {
        httpd_register_uri_handler(server, &routes[i]);
    }
}
