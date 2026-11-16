/*
 * The simulated CGM on the board: wires the portable G6/G7 engine (cgmsim)
 * to the radio, the clock and flash, ticks it once a second, and takes the
 * control panel's commands.
 *
 * TEST EQUIPMENT ONLY: the readings are made up. Never let an app that
 * controls a real pump or pod read them.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "cgm";

static cgm_t cgm;
static SemaphoreHandle_t mu;

static int64_t now_s(void *ctx)
{
    return (int64_t)time(NULL);
}

static void log_line(void *ctx, const char *msg)
{
    ESP_LOGI(TAG, "%s", msg);
}

static const cgm_io_t io = {
    .send = ble_cgm_send,
    .disconnect = ble_cgm_disconnect,
    .advertise = ble_cgm_advertise,
    .now = now_s,
    .save = store_save_cgm,
    .log = log_line,
};

#define LOCKED(stmt)                           \
    do {                                       \
        xSemaphoreTake(mu, portMAX_DELAY);     \
        stmt;                                  \
        xSemaphoreGive(mu);                    \
    } while (0)

static void tick_task(void *arg)
{
    for (;;) {
        LOCKED(cgm_tick(&cgm));
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* The G6 a board with nothing saved comes up as. A G6 has no discovery of its
 * own -- the app has to be told the transmitter id -- so it is a fixed, known
 * one rather than a random one the panel would have to be read to find. */
static const char default_g6_id[] = "81AB7Q";

void cgm_app_start(void)
{
    mu = xSemaphoreCreateMutex();
    cgm_state_t st;
    if (!store_load_cgm(&st)) {
        /* A blank board starts as a G6 rather than with the CGM off, so a
         * fresh flash is usable without reaching the panel first. Only a board
         * with nothing saved gets this: a saved CGM always wins. */
        cgm_state_fresh(&st, CGM_G6, default_g6_id, now_s(NULL));
    }
    cgm_init(&cgm, &st, &io);
    if (st.kind != CGM_OFF) {
        char name[16];
        cgm_name(&st, name);
        ESP_LOGW(TAG, "simulating a Dexcom %s, %s: made-up readings, for test phones only", st.kind == CGM_G6 ? "G6" : "G7",
                 name);
    }
    xTaskCreatePinnedToCore(tick_task, "cgm", 4096, NULL, 4, NULL, tskNO_AFFINITY);
}

void cgm_app_on_connect(void)
{
    LOCKED(cgm_on_connect(&cgm));
}

void cgm_app_on_disconnect(void)
{
    LOCKED(cgm_on_disconnect(&cgm); cgm_tick(&cgm));
}

void cgm_app_on_subscribe(cgm_chr_t chr, bool on)
{
    LOCKED(cgm_on_subscribe(&cgm, chr, on));
}

void cgm_app_on_write(cgm_chr_t chr, const uint8_t *p, size_t n)
{
    LOCKED(cgm_on_write(&cgm, chr, p, n));
}

size_t cgm_app_on_read(cgm_chr_t chr, uint8_t *out, size_t cap)
{
    size_t n;
    LOCKED(n = cgm_on_read(&cgm, chr, out, cap));
    return n;
}

size_t cgm_app_status_json(char *out, size_t cap)
{
    size_t n;
    LOCKED(n = cgm_status_json(&cgm, out, cap));
    return n;
}

/* ---- the panel's commands -------------------------------------------------------- */

/* The number after "key": in a small JSON object, or false. */
static bool json_number(const char *j, const char *key, long *out)
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
    char *end;
    long v = strtol(p, &end, 10);
    if (end == p) {
        if (strncmp(p, "true", 4) == 0) {
            v = 1;
        } else if (strncmp(p, "false", 5) == 0) {
            v = 0;
        } else {
            return false;
        }
    }
    *out = v;
    return true;
}

static bool json_str(const char *j, const char *key, char *out, size_t cap)
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
    while (*p && *p != '"' && n + 1 < cap) {
        out[n++] = *p++;
    }
    out[n] = 0;
    return *p == '"';
}

/* {"command": "cgmNew", "kind": 0|6|7, "id": "81AB7Q"}, "cgmGlucose" {glucose,
 * trend}, "cgmState" {state}, "cgmSignalLoss" {on}, "cgmSkipWarmup".
 * Returns NULL, or what was wrong. */
const char *cgm_app_command(const char *body)
{
    char cmd[24], id[8] = "";
    long a, b;
    if (!json_str(body, "command", cmd, sizeof cmd)) {
        return "want {\"command\": ...}";
    }
    if (strcmp(cmd, "cgmNew") == 0) {
        if (!json_number(body, "kind", &a) || (a != CGM_OFF && a != CGM_G6 && a != CGM_G7)) {
            return "kind is 0 (off), 6 (G6) or 7 (G7)";
        }
        json_str(body, "id", id, sizeof id);
        if (a == CGM_G6 && id[0] && !cgm_valid_g6_id(id)) {
            return "a G6 transmitter id is 6 capital letters and digits";
        }
        LOCKED(cgm_new_sensor(&cgm, (cgm_kind_t)a, id[0] ? id : NULL));
    } else if (strcmp(cmd, "cgmGlucose") == 0) {
        if (!json_number(body, "glucose", &a) || a < 40 || a > 400) {
            return "glucose is 40 to 400 mg/dL";
        }
        if (!json_number(body, "trend", &b)) {
            b = 0;
        }
        if (b < -50 || b > 50) {
            return "trend is -50 to 50 tenths of mg/dL per minute";
        }
        LOCKED(cgm_set_glucose(&cgm, (uint16_t)a, (int8_t)b));
    } else if (strcmp(cmd, "cgmState") == 0) {
        if (!json_number(body, "state", &a) || a < 0 || a > 255) {
            return "state is 0 (automatic) or an algorithm state";
        }
        LOCKED(cgm_set_state(&cgm, (uint8_t)a));
    } else if (strcmp(cmd, "cgmSignalLoss") == 0) {
        if (!json_number(body, "on", &a)) {
            return "want {\"on\": true|false}";
        }
        LOCKED(cgm_set_signal_loss(&cgm, a != 0));
    } else if (strcmp(cmd, "cgmSkipWarmup") == 0) {
        LOCKED(cgm_skip_warmup(&cgm));
    } else {
        return "unknown CGM command";
    }
    return NULL;
}
