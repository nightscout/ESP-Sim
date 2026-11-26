/*
 * The simulated YpsoPump on the board: wires the portable engine (ypsosim)
 * to the radio, the clock and flash, ticks it once a second, takes the
 * control panel's commands.
 *
 * TEST EQUIPMENT ONLY: it delivers nothing. Its keys and password are its own,
 * made at random: no real pump's credentials are used.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "app.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "ypso";

static ypso_t pump;
static SemaphoreHandle_t mu;

static int64_t now_s(void *ctx)
{
    return (int64_t)time(NULL);
}

static int32_t utc_offset(void *ctx)
{
    time_t t = time(NULL);
    struct tm lt, ut;
    localtime_r(&t, &lt);
    gmtime_r(&t, &ut);
    lt.tm_isdst = 0;
    return (int32_t)difftime(mktime(&lt), mktime(&ut));
}

static void log_line(void *ctx, const char *msg)
{
    ESP_LOGI(TAG, "%s", msg);
}

static const ypso_io_t io = {
    .now = now_s,
    .utc_offset = utc_offset,
    .save = store_save_ypso,
    .log = log_line,
};

#define LOCKED(stmt)                       \
    do {                                   \
        xSemaphoreTake(mu, portMAX_DELAY); \
        stmt;                              \
        xSemaphoreGive(mu);                \
    } while (0)

static void tick_task(void *arg)
{
    /* The zone's offset (a new zone from the panel, or daylight saving): the
     * pump's clock keeps its time across the change, then is set to the new
     * local time as a person would, so the change is in its history. */
    int32_t known = utc_offset(NULL);
    time_t last = time(NULL);
    for (;;) {
        int32_t off = utc_offset(NULL);
        time_t now = time(NULL);
        LOCKED({
            /* The board's clock going back (network time after a restart from
             * a save that was ahead): a real pump's clock never jumps back by
             * itself, so it keeps its time and is then set back, with the
             * change in its history. Forward is the time the board was off. */
            if (now < last - 5) {
                ESP_LOGW(TAG, "the board's clock went back %lld s", (long long)(last - now));
                pump.st.clock_offset += last - now;
                ypso_clock_to_local(&pump);
            }
            if (off != known) {
                pump.st.clock_offset += known - off;
                known = off;
                ypso_clock_to_local(&pump);
            }
            ypso_tick(&pump);
        });
        last = now;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void ypso_app_start(void)
{
    mu = xSemaphoreCreateMutex();
    ypso_state_t st;
    if (!store_load_ypso(&st)) {
        ypso_state_fresh(&st, YPSO_FW_V05);
        store_save_ypso(NULL, &st);
        ESP_LOGI(TAG, "a new YpsoPump");
    }
    ypso_init(&pump, &st, &io);
    store_save_ypso(NULL, &pump.st); /* keeps a server key made for an older save */
    ypso_set_open(&pump, store_load_ypso_open());
    /* A real pump's clock runs on its battery. The board's may have restarted
     * from a save older than the pump's newest entry: never before that. */
    if (pump.st.n_events) {
        const ypso_event_t *e = &pump.st.events[pump.st.n_events - 1];
        int64_t newest = (int64_t)e->t + 946684800LL - utc_offset(NULL) - pump.st.clock_offset;
        struct timeval tv;
        gettimeofday(&tv, NULL);
        if (newest >= tv.tv_sec) {
            tv.tv_sec = (time_t)newest + 1;
            tv.tv_usec = 0;
            settimeofday(&tv, NULL);
            ESP_LOGW(TAG, "the board's clock was behind the pump's newest history entry: moved up to it");
        }
    }
    if (time(NULL) > 1750000000) { /* the board knows the time (it survives a restart) */
        ypso_clock_to_local(&pump);
    }
    char name[24];
    ypso_name(&st, name);
    ESP_LOGW(TAG, "simulating %s, firmware %s: delivers nothing, for test phones only", name, ypso_fw(st.fw)->master);
    ESP_LOGI(TAG, "%s", pump.open ? "open: any phone gets in without the pump's password"
                                  : "closed: only the pump's own sign-in password gets in");
    xTaskCreatePinnedToCore(tick_task, "ypso", 4096, NULL, 4, NULL, tskNO_AFFINITY);
}

const ypso_state_t *ypso_app_state(void)
{
    return &pump.st;
}

void ypso_app_on_connect(void)
{
    LOCKED(ypso_on_connect(&pump));
}

void ypso_app_on_disconnect(void)
{
    LOCKED(ypso_on_disconnect(&pump));
}

int ypso_app_on_read(ypso_chr_t c, uint8_t *out, size_t cap, size_t *len)
{
    int rc;
    LOCKED(rc = ypso_on_read(&pump, c, out, cap, len));
    return rc;
}

int ypso_app_on_write(ypso_chr_t c, const uint8_t *p, size_t n)
{
    int rc;
    LOCKED(rc = ypso_on_write(&pump, c, p, n));
    return rc;
}

/* The board's clock or zone changed: the pump keeps local time. */
void ypso_app_clock_changed(void)
{
    if (mu) {
        LOCKED(ypso_clock_to_local(&pump));
    }
}

uint32_t ypso_app_delivered(void)
{
    uint32_t v;
    LOCKED(v = pump.st.delivered_total);
    return v;
}

/* The newest `max` event-log entries as text, newest first, as a phone reads
 * them (index 0 the newest): for /ypsohistory. */
size_t ypso_app_history_text(char *out, size_t cap, int max)
{
    size_t n = 0;
    if (!mu) {
        return (size_t)snprintf(out, cap, "no YpsoPump\n");
    }
    xSemaphoreTake(mu, portMAX_DELAY);
    n += (size_t)snprintf(out + n, cap - n, "%u entries, newest seq %lu; index seq type stamp(pump wall) v0 v1 v2\n",
                          (unsigned)pump.st.n_events, (unsigned long)pump.st.event_seq);
    for (int i = 0; i < max && i < pump.st.n_events && n + 80 < cap; i++) {
        const ypso_event_t *e = &pump.st.events[pump.st.n_events - 1 - i];
        time_t w = (time_t)e->t + 946684800;
        struct tm tm;
        gmtime_r(&w, &tm);
        n += (size_t)snprintf(out + n, cap - n, "%3d %6lu %3u %04d-%02d-%02d %02d:%02d:%02d %u %u %u\n", i,
                              (unsigned long)e->seq, e->type, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                              tm.tm_min, tm.tm_sec, e->v[0], e->v[1], e->v[2]);
    }
    xSemaphoreGive(mu);
    return n;
}

size_t ypso_app_status_json(char *out, size_t cap)
{
    size_t n;
    LOCKED(n = ypso_status_json(&pump, out, cap));
    if (n == 0 || n + 40 > cap) {
        return 0;
    }
    /* The passkey the phone is waiting for, as the pump's screen would show it. */
    n--; /* drop the closing brace */
    int32_t pk = ble_passkey();
    if (pk >= 0) {
        n += (size_t)snprintf(out + n, cap - n, ",\"passkey\":\"%06ld\"}", (long)pk);
    } else {
        n += (size_t)snprintf(out + n, cap - n, ",\"passkey\":null}");
    }
    return n;
}

/* ---- the panel's commands -------------------------------------------------------- */

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
    if (strncmp(p, "true", 4) == 0 || strncmp(p, "false", 5) == 0) {
        *out = *p == 't';
        return true;
    }
    char *end;
    long v = strtol(p, &end, 10);
    if (end == p) {
        return false;
    }
    *out = v;
    return true;
}

static bool command_is(const char *j, const char *name)
{
    char pat[48];
    snprintf(pat, sizeof pat, "\"%s\"", name);
    const char *c = strstr(j, "\"command\"");
    return c && strstr(c, pat);
}

const char *ypso_app_command(const char *body, char *reply, size_t cap)
{
    long v;
    reply[0] = 0;
    if (command_is(body, "ypsoReservoir")) {
        if (!json_number(body, "hundredths", &v) || v < -1 || v > 30000) {
            return "hundredths is -1 (no cartridge) to 30000 (300 U)";
        }
        LOCKED(ypso_set_reservoir(&pump, (int32_t)v));
    } else if (command_is(body, "ypsoBattery")) {
        if (!json_number(body, "level", &v) || v < 0 || v > 6) {
            return "level is 0 to 5 (×20 %), or 6 (invalid)";
        }
        LOCKED(ypso_set_battery(&pump, (uint8_t)v));
    } else if (command_is(body, "ypsoSuspend")) {
        if (!json_number(body, "on", &v)) {
            return "want {\"on\": true|false}";
        }
        LOCKED(ypso_set_suspended(&pump, v != 0));
    } else if (command_is(body, "ypsoOpen")) {
        if (!json_number(body, "on", &v)) {
            return "want {\"on\": true|false}";
        }
        LOCKED(ypso_set_open(&pump, v != 0));
        store_save_ypso_open(v != 0);
    } else if (command_is(body, "ypsoReboot")) {
        LOCKED(ypso_reboot(&pump));
    } else if (command_is(body, "ypsoForgetBonds")) {
        ble_forget_bonds();
        ESP_LOGI(TAG, "Bluetooth bonds forgotten: the phone pairs again");
    } else {
        return "unknown YpsoPump command";
    }
    return NULL;
}
