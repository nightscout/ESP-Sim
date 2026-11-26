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
#include <math.h>

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

static int32_t utc_offset(void *ctx)
{
    time_t t = time(NULL);
    struct tm lt, ut;
    localtime_r(&t, &lt);
    gmtime_r(&t, &ut);
    lt.tm_isdst = 0;
    return (int32_t)difftime(mktime(&lt), mktime(&ut));
}

/* What the simulated pump has put into the virtual person, hundredths of a
 * unit. The YpsoPump counts everything it delivers. The pod counts only its
 * boluses, so its running basal is added here at the person's basal need
 * (its temp basals are not modelled). */
static bool ypso_pump;
static int64_t pod_basal_milli; /* thousandths of a hundredth */
static int64_t pod_basal_last;

static int64_t pump_insulin(void *ctx)
{
    if (ypso_pump) {
        return ypso_app_delivered();
    }
    const dash_state_t *p = pod_state();
    int64_t now = (int64_t)time(NULL);
    if (pod_basal_last && p->paired && p->basal_active && !p->fault && p->reservoir > 0) {
        int64_t dt = now - pod_basal_last;
        if (dt > 0 && dt < 3600) {
            uint16_t need = cgm.st.basal_need ? cgm.st.basal_need : 80;
            pod_basal_milli += dt * need * 1000 / 3600;
        }
    }
    pod_basal_last = now;
    return (int64_t)p->delivered * 5 + pod_basal_milli / 1000;
}

/* The radio names the session on every call; the queue and the deferred
 * hang-up live in ble.c. */
static void io_send(void *ctx, cgm_session_t session, cgm_chr_t chr, const uint8_t *p, size_t n)
{
    ble_cgm_send_session(session, chr, p, n);
}

static void io_disconnect(void *ctx, cgm_session_t session)
{
    ble_cgm_disconnect_session(session);
}

static const cgm_io_t io = {
    .send = io_send,
    .disconnect = io_disconnect,
    .advertise = ble_cgm_advertise,
    .now = now_s,
    .save = store_save_cgm,
    .log = log_line,
    .utc_offset = utc_offset,
    .pump_insulin = pump_insulin,
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

void cgm_app_start(void)
{
    mu = xSemaphoreCreateMutex();
    ypso_pump = store_load_pump() == 1;
    cgm_state_t st;
    /* store_load_cgm() rejects a version it does not know, but it has already
     * read the blob into st. A version-1 save is brought forward by
     * cgm_state_migrate() instead of being thrown away. */
    bool loaded = store_load_cgm(&st);
    if (!loaded && st.version != 1) {
        memset(&st, 0, sizeof st);
        st.kind = CGM_OFF;
    }
    cgm_state_migrate(&st);
    cgm_init(&cgm, &st, &io);
    if (st.kind != CGM_OFF) {
        char name[16];
        cgm_name(&st, name);
        ESP_LOGW(TAG, "simulating a Dexcom %s, %s: made-up readings, for test phones only", st.kind == CGM_G6 ? "G6" : "G7",
                 name);
    }
    xTaskCreatePinnedToCore(tick_task, "cgm", 4096, NULL, 4, NULL, tskNO_AFFINITY);
}

/* The radio names the central it is talking about: one core session per BLE
 * link, up to CGM_MAX_SESSIONS. The sessionless entry points below keep the
 * app.h contract for any caller that has not been re-signed. */
void cgm_app_on_connect_session(cgm_session_t session)
{
    LOCKED(cgm_on_connect(&cgm, session));
}

void cgm_app_on_disconnect_session(cgm_session_t session)
{
    LOCKED(cgm_on_disconnect(&cgm, session));
}

void cgm_app_on_subscribe_session(cgm_session_t session, cgm_chr_t chr, bool on)
{
    LOCKED(cgm_on_subscribe(&cgm, session, chr, on));
}

void cgm_app_on_write_session(cgm_session_t session, cgm_chr_t chr, const uint8_t *p, size_t n)
{
    LOCKED(cgm_on_write(&cgm, session, chr, p, n));
}

size_t cgm_app_on_read_session(cgm_session_t session, cgm_chr_t chr, uint8_t *out, size_t cap)
{
    size_t n;
    LOCKED(n = cgm_on_read(&cgm, session, chr, out, cap));
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
    } else if (strcmp(cmd, "cgmPattern") == 0) {
        /* The older panel's selector: 0 is the manual (fixed) value/trend,
         * 1 is the ESP-Sim virtual person. */
        long pattern, source = 0, isf = 0, icr = 0, basal = 0;
        if (!json_number(body, "pattern", &pattern) || (pattern != 0 && pattern != 1)) {
            return "pattern is 0 (manual) or 1 (virtual person)";
        }
        json_number(body, "source", &source);
        json_number(body, "isf", &isf);
        json_number(body, "icr", &icr);
        json_number(body, "basalNeed", &basal);
        if ((source != 0 && source != 1) || isf < 0 || isf > 400 || icr < 0 || icr > 100 || basal < 0 ||
            basal > 500) {
            return "source 0 (the person's own) or 1 (the pump's); isf up to 400, icr up to 100, basalNeed up to 500";
        }
        if (pattern == 1) {
            LOCKED(cgm_set_person(&cgm, (cgm_insulin_t)source, (uint16_t)isf, (uint16_t)icr, (uint16_t)basal));
        } else {
            LOCKED({
                cgm_pattern_t p;
                p.kind = CGM_PATTERN_FIXED;
                p.value = cgm.st.glucose;
                p.trend = cgm.st.trend;
                p.amplitude = cgm.st.amplitude;
                p.period_minutes = cgm.st.period_minutes;
                cgm_set_pattern(&cgm, &p);
            });
        }
    } else if (strcmp(cmd, "cgmSetPattern") == 0) {
        /* The fork's pattern selector: fixed keeps the manual value/trend
         * reachable, sine takes a mean/amplitude/period, walk a target. */
        char pname[8];
        if (!json_str(body, "pattern", pname, sizeof pname) || strcmp(pname, "person") == 0) {
            return "pattern is fixed, sine or walk";
        }
        cgm_pattern_kind_t kind;
        if (!cgm_pattern_from_name(pname, &kind)) {
            return "pattern is fixed, sine or walk";
        }
        long value = 120, trend = 0, amplitude = 50, period = 240;
        if (!json_number(body, "value", &value) && !json_number(body, "glucose", &value)) {
            return "value is 40 to 400";
        }
        if (value < 40 || value > 400) {
            return "value is 40 to 400";
        }
        /* Start from what the sensor already has, so switching pattern does not
         * reset parameters the new pattern does not use. */
        LOCKED({
            trend = cgm.st.trend;
            amplitude = cgm.st.amplitude;
            period = cgm.st.period_minutes;
        });
        if (kind == CGM_PATTERN_FIXED) {
            if (json_number(body, "trend", &trend) && (trend < -127 || trend > 126)) {
                return "trend is -127 to 126";
            }
        } else if (kind == CGM_PATTERN_SINE) {
            if (json_number(body, "amplitude", &amplitude) && (amplitude < 0 || amplitude > 180)) {
                return "amplitude is 0 to 180";
            }
            if (json_number(body, "periodMinutes", &period) && (period < 10 || period > 1440)) {
                return "periodMinutes is 10 to 1440";
            }
        }
        cgm_pattern_t p = {.kind = kind,
                           .value = (uint16_t)value,
                           .trend = (int8_t)trend,
                           .amplitude = (uint16_t)amplitude,
                           .period_minutes = (uint16_t)period};
        LOCKED(cgm_set_pattern(&cgm, &p));
    } else if (strcmp(cmd, "cgmEat") == 0) {
        if (!json_number(body, "grams", &a) || a < 1 || a > 200) {
            return "grams is 1 to 200";
        }
        LOCKED(cgm_eat(&cgm, (uint16_t)a));
    } else if (strcmp(cmd, "cgmExercise") == 0) {
        if (!json_number(body, "minutes", &a) || a < 5 || a > 240) {
            return "minutes is 5 to 240";
        }
        LOCKED(cgm_exercise(&cgm, (uint16_t)a));
    } else if (strcmp(cmd, "cgmSkipWarmup") == 0) {
        LOCKED(cgm_skip_warmup(&cgm));
    } else {
        return "unknown CGM command";
    }
    return NULL;
}
