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
#include "esp_partition.h"
#include "sdkconfig.h"
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
static uint8_t board_pump; /* PUMP_*, fixed at boot */
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

/* ---- backtest: the stored trace, its log, and what the pump is doing ------------------ */

#if CONFIG_ESPSIM_CGM_BACKTEST
#define TRACE_BYTES (64 * 1024)      /* the partition's first 64 KB: 32768 readings, 113 days */
#define RAM_TRACE_READINGS 4096      /* no partition: 14 days in RAM */
#define RAM_LOG_BYTES (16 * 1024)    /* and 1024 log lines */

static const esp_partition_t *bt_part;
static uint8_t *bt_ram;              /* no partition: trace then log, made on the first upload */
static uint32_t bt_trace_cap;        /* readings */
static uint32_t bt_upload_next;      /* the next reading an upload expects */
static uint32_t bt_upload_sum;
static uint16_t bt_upload_first;
static cgm_logstore_t bt_logs;
static bool bt_log_open;

static bool fl_read(void *ctx, uint32_t off, void *p, size_t n)
{
    if (bt_part) {
        return esp_partition_read(bt_part, off, p, n) == ESP_OK;
    }
    memcpy(p, bt_ram + off, n);
    return true;
}

static bool fl_write(void *ctx, uint32_t off, const void *p, size_t n)
{
    if (bt_part) {
        return esp_partition_write(bt_part, off, p, n) == ESP_OK;
    }
    for (size_t i = 0; i < n; i++) {
        bt_ram[off + i] &= ((const uint8_t *)p)[i]; /* as flash: written once after an erase */
    }
    return true;
}

static bool fl_erase(void *ctx, uint32_t off)
{
    if (bt_part) {
        return esp_partition_erase_range(bt_part, off, CGM_FLASH_SECTOR) == ESP_OK;
    }
    memset(bt_ram + off, 0xff, CGM_FLASH_SECTOR);
    return true;
}

static const cgm_flash_t bt_flash = {.read = fl_read, .write = fl_write, .erase = fl_erase};

static uint32_t bt_trace_bytes(void)
{
    return bt_part ? TRACE_BYTES : RAM_TRACE_READINGS * 2;
}

/* The storage, found (the partition) or made (RAM, only once wanted). */
static bool bt_storage(bool make)
{
    if (bt_part || bt_ram) {
        return true;
    }
    if (!make) {
        return false;
    }
    bt_ram = malloc(RAM_TRACE_READINGS * 2 + RAM_LOG_BYTES);
    if (!bt_ram) {
        return false;
    }
    memset(bt_ram, 0xff, RAM_TRACE_READINGS * 2 + RAM_LOG_BYTES);
    bt_trace_cap = RAM_TRACE_READINGS;
    cgm_logstore_open(&bt_logs, &bt_flash, bt_trace_bytes(), RAM_LOG_BYTES, store_load_bt_epoch());
    bt_log_open = true;
    ESP_LOGW(TAG, "no backtest partition: the trace and its log are kept in RAM (USB-flash for the partition)");
    return true;
}

static void bt_start(cgm_state_t *st)
{
    bt_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "backtest");
    if (bt_part && bt_part->size > TRACE_BYTES + CGM_FLASH_SECTOR) {
        bt_trace_cap = TRACE_BYTES / 2;
        cgm_logstore_open(&bt_logs, &bt_flash, TRACE_BYTES, bt_part->size - TRACE_BYTES, store_load_bt_epoch());
        bt_log_open = true;
        ESP_LOGI(TAG, "backtest: trace of %u readings, log of %u lines", (unsigned)st->bt.len, (unsigned)bt_logs.count);
    } else {
        bt_part = NULL;
        if (st->bt.len) {
            memset(&st->bt, 0, sizeof st->bt); /* it was in RAM: gone with the restart */
        }
    }
}

static uint16_t trace_at(void *ctx, uint32_t i)
{
    uint16_t v = 0xffff;
    if (i < bt_trace_cap && bt_storage(false)) {
        fl_read(NULL, i * 2, &v, 2);
    }
    return v;
}

static void backtest_log(void *ctx, const cgm_log_t *e)
{
    if (bt_log_open && !cgm_logstore_append(&bt_logs, e) && !bt_logs.full) {
        ESP_LOGW(TAG, "backtest: could not write the log");
    }
}

/* Called with the lock held. */
static void bt_clear_log(void)
{
    if (bt_log_open) {
        uint8_t e = (uint8_t)(bt_logs.epoch + 1);
        store_save_bt_epoch(e);
        cgm_logstore_clear(&bt_logs, e);
    }
}

/* What the pump is doing, for the backtest log. Each pump knows different things. */
static uint16_t pod_seen_pulses;
static uint32_t pod_bolus_id;
static int32_t pod_bolus;

static bool pump_now(void *ctx, cgm_pump_t *out)
{
    int64_t now = (int64_t)time(NULL);
    *out = (cgm_pump_t){.delivered = -1, .rate = -1, .bolus = -1};
    if (board_pump == PUMP_YPSO) {
        const ypso_state_t *y = ypso_app_state();
        out->delivered = y->delivered_total;
        int64_t local = now + utc_offset(NULL) + y->clock_offset;
        int hour = (int)(((local % 86400) + 86400) % 86400 / 3600);
        int32_t rate = (y->profile == 3 ? y->basal_a : y->basal_b)[hour];
        out->temp = y->tbr_percent != 100 && now < y->tbr_start + (int64_t)y->tbr_minutes * 60;
        out->temp_end = out->temp ? y->tbr_start + (int64_t)y->tbr_minutes * 60 : 0;
        out->rate = y->suspended || y->reservoir == 0 ? 0 : out->temp ? rate * y->tbr_percent / 100 : rate;
        out->bolus_id = y->bolus_seq;
        out->bolus = y->imm.seq == y->bolus_seq ? y->imm.total
                     : y->slow.seq == y->bolus_seq ? y->slow.total + y->slow.now_total
                                                   : -1;
        return true;
    }
    if (board_pump == PUMP_MEDTRUM) {
        medtrum_delivery_t d;
        if (!medtrum_app_delivery(&d)) {
            return false;
        }
        out->temp = d.temp_end != 0 && d.temp_end > d.patch_time;
        out->temp_end = out->temp ? now + (int64_t)(d.temp_end - d.patch_time) : 0;
        out->rate = (out->temp ? d.temp_rate : d.scheduled_rate) * 5;
        out->bolus_id = d.bolus_start;
        out->bolus = d.bolus_total * 5;
        return true;
    }
    /* The pod: it counts only its boluses (its basal and temp basal rates are
     * not modelled), so the log has its boluses and when a temp basal runs. */
    const dash_state_t *p = pod_state();
    if (!p->paired) {
        return false;
    }
    if (p->delivered != pod_seen_pulses) {
        if (p->delivered > pod_seen_pulses && pod_seen_pulses) {
            pod_bolus_id++;
            pod_bolus = (p->delivered - pod_seen_pulses) * 5;
        }
        pod_seen_pulses = p->delivered;
    }
    out->temp = p->temp_basal_end_ms > now * 1000;
    out->temp_end = out->temp ? p->temp_basal_end_ms / 1000 : 0;
    out->rate = p->basal_active && !p->fault ? -1 : 0;
    out->bolus_id = pod_bolus_id;
    out->bolus = pod_bolus;
    return true;
}
#endif

static const cgm_io_t io = {
    .send = ble_cgm_send,
    .disconnect = ble_cgm_disconnect,
    .advertise = ble_cgm_advertise,
    .now = now_s,
    .save = store_save_cgm,
    .log = log_line,
    .utc_offset = utc_offset,
    .pump_insulin = pump_insulin,
#if CONFIG_ESPSIM_CGM_BACKTEST
    .trace_at = trace_at,
    .pump_now = pump_now,
    .backtest_log = backtest_log,
#endif
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
    board_pump = store_load_pump();
    ypso_pump = board_pump == PUMP_YPSO;
    cgm_state_t st;
    if (!store_load_cgm(&st)) {
        memset(&st, 0, sizeof st);
        st.version = CGM_STATE_VERSION;
        st.kind = CGM_OFF;
    }
#if CONFIG_ESPSIM_CGM_BACKTEST
    bt_start(&st);
#else
    if (st.pattern == CGM_PATTERN_BACKTEST) {
        st.pattern = CGM_PATTERN_MANUAL; /* built without backtest: a fixed value */
    }
#endif
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
#if CONFIG_ESPSIM_CGM_BACKTEST
    /* Where the backtest keeps things: the page shows its mode only with this. */
    if (n > 1 && out[n - 1] == '}') {
        uint32_t cap_r = bt_part ? TRACE_BYTES / 2 : RAM_TRACE_READINGS;
        int k = snprintf(out + n - 1, cap - (n - 1),
                         ",\"btStore\":{\"where\":\"%s\",\"traceCap\":%u,\"logLines\":%u,\"logCap\":%u,"
                         "\"logFull\":%s,\"uploading\":%u}}",
                         bt_part ? "flash" : "ram", (unsigned)cap_r, (unsigned)(bt_log_open ? bt_logs.count : 0),
                         (unsigned)(bt_log_open ? bt_logs.size / sizeof(cgm_log_t) : RAM_LOG_BYTES / sizeof(cgm_log_t)),
                         bt_log_open && bt_logs.full ? "true" : "false", (unsigned)bt_upload_next);
        n = k > 0 && (size_t)k < cap - (n - 1) ? n - 1 + (size_t)k : 0;
    }
#endif
    return n;
}

#if CONFIG_ESPSIM_CGM_BACKTEST
const char *cgm_app_trace_chunk(uint32_t offset, const char *text, size_t n)
{
    static uint16_t v[1024];
    const char *err = "could not read the readings";
    int got = cgm_trace_parse(text, n, v, sizeof v / sizeof v[0], &err);
    if (got < 0) {
        return err;
    }
    if (!bt_storage(true)) {
        return "no room for a trace on this board";
    }
    if (offset == 0) {
        /* A new trace: the old one is gone, and so is its log. */
        LOCKED(cgm_backtest_load(&cgm, 0, 0, 0); bt_clear_log());
        for (uint32_t off = 0; off < bt_trace_bytes(); off += CGM_FLASH_SECTOR) {
            if (!fl_erase(NULL, off)) {
                return "could not erase the trace";
            }
        }
        bt_upload_next = 0;
        bt_upload_sum = CGM_TRACE_SUM_INIT;
        bt_upload_first = 0;
    } else if (offset != bt_upload_next) {
        return "chunks out of order: upload the trace again from the start";
    }
    if (offset + (uint32_t)got > bt_trace_cap) {
        return bt_part ? "the trace is too long: up to 32768 readings (113 days)"
                       : "the trace is too long for RAM: up to 4096 readings (14 days)";
    }
    if (got && !fl_write(NULL, offset * 2, v, (size_t)got * 2)) {
        return "could not store the trace";
    }
    for (int i = 0; i < got && !bt_upload_first; i++) {
        bt_upload_first = v[i];
    }
    bt_upload_sum = cgm_trace_sum(bt_upload_sum, v, (size_t)got);
    bt_upload_next = offset + (uint32_t)got;
    return NULL;
}

size_t cgm_app_log_csv(uint32_t *next, char *out, size_t cap)
{
    size_t n = 0;
    if (*next == 0) {
        char sum[64];
        LOCKED(snprintf(sum, sizeof sum, "# ESP-Sim CGM backtest log, trace %08x (%u readings)\n",
                        (unsigned)cgm.st.bt.sum, (unsigned)cgm.st.bt.len));
        n += (size_t)snprintf(out, cap, "%s", sum);
        n += cgm_log_csv_header(out + n, cap - n);
    }
    xSemaphoreTake(mu, portMAX_DELAY);
    uint32_t i = *next ? *next - 1 : 0;
    cgm_log_t e;
    while (bt_log_open && cap - n > 96 && cgm_logstore_get(&bt_logs, i, &e)) {
        n += cgm_log_csv(&e, out + n, cap - n);
        i++;
    }
    xSemaphoreGive(mu);
    *next = i + 1;
    return n;
}
#else
const char *cgm_app_trace_chunk(uint32_t offset, const char *text, size_t n)
{
    return "this firmware is built without backtest (CONFIG_ESPSIM_CGM_BACKTEST)";
}

size_t cgm_app_log_csv(uint32_t *next, char *out, size_t cap)
{
    return 0;
}
#endif

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
        long pattern, source = 0, isf = 0, icr = 0, basal = 0;
        if (!json_number(body, "pattern", &pattern) || pattern < 0 || pattern > 2) {
            return "pattern is 0 (manual), 1 (virtual person) or 2 (backtest)";
        }
#if !CONFIG_ESPSIM_CGM_BACKTEST
        if (pattern == CGM_PATTERN_BACKTEST) {
            return "this firmware is built without backtest (CONFIG_ESPSIM_CGM_BACKTEST)";
        }
#endif
        json_number(body, "source", &source);
        json_number(body, "isf", &isf);
        json_number(body, "icr", &icr);
        json_number(body, "basalNeed", &basal);
        if ((source != 0 && source != 1) || isf < 0 || isf > 400 || icr < 0 || icr > 100 || basal < 0 ||
            basal > 500) {
            return "source 0 (the person's own) or 1 (the pump's); isf up to 400, icr up to 100, basalNeed up to 500";
        }
        LOCKED(cgm_set_pattern(&cgm, (cgm_pattern_t)pattern, (cgm_insulin_t)source, (uint16_t)isf, (uint16_t)icr,
                               (uint16_t)basal));
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
#if CONFIG_ESPSIM_CGM_BACKTEST
    } else if (strcmp(cmd, "cgmBacktest") == 0) {
        /* {"action": "start"|"pause"|"restart"|"cue" (pos)|"end" (end 0 stop, 1 hold,
         * 2 loop)|"loaded" (length: the upload is complete)|"clearLog"} */
        char act[12];
        if (!json_str(body, "action", act, sizeof act)) {
            return "want {\"action\": ...}";
        }
        if (strcmp(act, "loaded") == 0) {
            if (!json_number(body, "length", &a) || a <= 0 || (uint32_t)a != bt_upload_next) {
                return "the upload is incomplete: send the trace again";
            }
            LOCKED(cgm_backtest_load(&cgm, (uint32_t)a, bt_upload_sum, bt_upload_first));
            bt_upload_next = 0;
            ESP_LOGI(TAG, "backtest: a trace of %ld readings, %08x", a, (unsigned)bt_upload_sum);
        } else if (strcmp(act, "start") == 0) {
            if (!cgm.st.bt.len) {
                return "upload a trace first";
            }
            LOCKED(cgm_backtest_start(&cgm));
        } else if (strcmp(act, "pause") == 0) {
            LOCKED(cgm_backtest_pause(&cgm));
        } else if (strcmp(act, "restart") == 0) {
            if (!cgm.st.bt.len) {
                return "upload a trace first";
            }
            LOCKED(cgm_backtest_restart(&cgm));
        } else if (strcmp(act, "cue") == 0) {
            bool ok = false;
            if (json_number(body, "pos", &a) && a >= 0) {
                LOCKED(ok = cgm_backtest_cue(&cgm, (uint32_t)a));
            }
            if (!ok) {
                return "pos is a reading in the trace, from 0";
            }
        } else if (strcmp(act, "end") == 0) {
            if (!json_number(body, "end", &a) || a < 0 || a > 2) {
                return "end is 0 (stop), 1 (hold the last value) or 2 (loop)";
            }
            LOCKED(cgm_backtest_set_end(&cgm, (cgm_bt_end_t)a));
        } else if (strcmp(act, "clearLog") == 0) {
            LOCKED(bt_clear_log());
        } else {
            return "unknown backtest action";
        }
#endif
    } else if (strcmp(cmd, "cgmSkipWarmup") == 0) {
        LOCKED(cgm_skip_warmup(&cgm));
    } else {
        return "unknown CGM command";
    }
    return NULL;
}
