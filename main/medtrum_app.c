/*
 * The simulated Medtrum Nano patch on the board: wires the portable patch
 * core (components/medtrumpatch) to the radio, the clock and flash, runs the
 * connect session and the delivery commands, and takes the control panel's
 * commands.
 *
 * TEST EQUIPMENT ONLY: it delivers nothing, and what it reports is made up.
 * Never use it, or an app paired to it, for therapy.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "app.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "medtrum_serial.h"

static const char *TAG = "medtrum";

/* The default base serial: the bytes the app reads out of the advertisement,
 * in the order the panel shows it. Reversed and decrypted they are MD0201
 * (200U) or MD8301 (300U). The panel pivots either without a rebuild. */
#if CONFIG_ESPSIM_MEDTRUM_300
#define MEDTRUM_DEFAULT_SN \
    { 0x52, 0xDF, 0x16, 0x14 }
#define MEDTRUM_DEFAULT_UNITS 300
#else
#define MEDTRUM_DEFAULT_SN \
    { 0x4A, 0x12, 0xD8, 0x28 }
#define MEDTRUM_DEFAULT_UNITS 200
#endif

static medtrum_patch_t patch;
static medtrum_request_t req;
static SemaphoreHandle_t mu;

/* The model as last committed to flash, so the run loop can save the drift the
 * tick produces without a commit per tick. */
static medtrum_status_t saved_status;
static medtrum_delivery_t saved_delivery;
static int64_t saved_at_ms;

/* The draw moves the reservoir every tick; a minute's lag after a hard reboot
 * is honest, where a save per tick would wear the flash for nothing. */
#define TICK_SAVE_MS 60000

static int64_t now_ms(void *ctx)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static void random_bytes(void *ctx, uint8_t *p, size_t n)
{
    esp_fill_random(p, n);
}

static void log_line(void *ctx, const char *msg)
{
    ESP_LOGI(TAG, "%s", msg);
}

static const medtrum_io_t io = {
    .ctx = NULL,
    .next_event = ble_medtrum_next_event,
    .notify = ble_medtrum_notify,
    .disconnect = ble_medtrum_disconnect,
    .advertise = ble_medtrum_advertise,
    .now_ms = now_ms,
    .random = random_bytes,
    .log = log_line,
};

#define LOCKED(stmt)                       \
    do {                                   \
        xSemaphoreTake(mu, portMAX_DELAY); \
        stmt;                              \
        xSemaphoreGive(mu);                \
    } while (0)

/* The panel and the app's serial field show patch.sn as typed (4A12D828); the
 * advertisement must carry it reversed (28 D8 12 4A), because MedtrumKit
 * compares the advertisement with the reverse of the typed serial. */
static void advertise_now(void)
{
    uint8_t sn[4];
    medtrum_serial_advert_bytes(patch.sn, sn);
    io.advertise(io.ctx, true, sn);
}

/* ---- persistence -------------------------------------------------------- */

static void save_state(void)
{
    medtrum_saved_t st = {
        .magic = MEDTRUM_STATE_MAGIC,
        .version = MEDTRUM_STATE_VERSION,
        .device_type = patch.device_type,
        .status = patch.status,
        .delivery = patch.delivery,
    };
    memcpy(st.sn, patch.sn, 4);
    memcpy(st.sw_version, patch.sw_version, 3);
    store_save_medtrum(NULL, &st);
    saved_status = patch.status;
    saved_delivery = patch.delivery;
    saved_at_ms = now_ms(NULL);
}

/* A fresh patch identity, as the reference assigns on activation. The app
 * validates every later notification against a 16-bit patch id
 * (basal.patchId / storage.patchId), so only the low two bytes are random;
 * the high two stay zero. */
static void assign_patch_id(void)
{
    uint8_t id[4] = {0, 0, 0, 0};
    esp_fill_random(id, 2);
    medtrum_patch_set_patch_id(&patch, id);
}

bool medtrum_app_state_valid(const medtrum_saved_t *st)
{
    return st->magic == MEDTRUM_STATE_MAGIC && st->version == MEDTRUM_STATE_VERSION;
}

/* ---- requests ----------------------------------------------------------- */

/* Reassembles one write and answers it. Returns true when the status should be
 * notified (a request was handled), and *count is the response's fragments. */
static bool on_write(const uint8_t *data, size_t n, medtrum_frag_t *frags, int *count)
{
    if (!medtrum_request_feed(&req, data, n)) {
        return false;
    }
    /* Only a command that changed the model needs a flash commit; a GET_TIME
     * poll, a sync or a subscribe must not wear NVS. */
    medtrum_status_t was_status = patch.status;
    medtrum_delivery_t was_delivery = patch.delivery;
    int got = medtrum_patch_handle(&patch, req.command, req.sequence, req.content, req.content_len, frags,
                                   MEDTRUM_FRAGS_MAX);
    if (got < 0) {
        ESP_LOGW(TAG, "unhandled command %u", req.command);
        return false;
    }
    *count = got;
    if (memcmp(&was_status, &patch.status, sizeof was_status) != 0 ||
        memcmp(&was_delivery, &patch.delivery, sizeof was_delivery) != 0) {
        save_state();
    }
    return true;
}

/* ---- the run loop ------------------------------------------------------- */

static void medtrum_task(void *arg)
{
    bool stop = false;
    while (!stop) {
        medtrum_ev_t ev;
        bool got = io.next_event(io.ctx, &ev, 1000);
        /* Collect the replies and the status payload under the lock, then send
         * them after it has been released: the web server takes the same lock,
         * and a notification can wait for a radio buffer. */
        medtrum_frag_t frags[MEDTRUM_FRAGS_MAX];
        int count = 0;
        bool send_status = false;
        uint8_t status_payload[MEDTRUM_STATUS_MAX];
        size_t status_len = 0;
        LOCKED({
            medtrum_patch_tick(&patch, now_ms(NULL));
            if (!got) {
                send_status = true;
            } else {
                switch (ev.type) {
                case MEDTRUM_EV_CONNECTED:
                    ESP_LOGI(TAG, "phone connected");
                    break;
                case MEDTRUM_EV_DISCONNECTED:
                    ESP_LOGI(TAG, "phone disconnected");
                    patch.authenticated = false;
                    save_state();
                    break;
                case MEDTRUM_EV_WRITE:
                    send_status = on_write(ev.data, ev.len, frags, &count);
                    break;
                case MEDTRUM_EV_STOP:
                    stop = true;
                    break;
                default:
                    break;
                }
            }
            if (send_status) {
                status_len = medtrum_patch_notification(&patch, status_payload, sizeof status_payload);
            }
            /* The tick moves the reservoir and the bolus/basal progress, which
             * only a command path used to commit: a hard reboot would then
             * restore a reservoir the patch no longer has. Commit that drift at
             * most once a minute, and only when it changed. */
            if (now_ms(NULL) - saved_at_ms >= TICK_SAVE_MS &&
                (memcmp(&saved_status, &patch.status, sizeof saved_status) != 0 ||
                 memcmp(&saved_delivery, &patch.delivery, sizeof saved_delivery) != 0)) {
                save_state();
            }
        });
        for (int i = 0; i < count; i++) {
            io.notify(io.ctx, frags[i].data, frags[i].len);
        }
        if (status_len > 0) {
            ble_medtrum_notify_status(NULL, status_payload, status_len);
        }
    }
    vTaskDelete(NULL);
}

void medtrum_app_start(void)
{
    mu = xSemaphoreCreateMutex();
    medtrum_request_init(&req);

    medtrum_saved_t st;
    bool have = store_load_medtrum(&st) && medtrum_app_state_valid(&st);
    uint8_t sn[4] = MEDTRUM_DEFAULT_SN;
    if (have) {
        memcpy(sn, st.sn, 4);
    }
    medtrum_patch_init(&patch, sn);
    patch.device_type = 80;
    patch.sw_version[0] = 12;
    patch.sw_version[1] = 1;
    patch.sw_version[2] = 3;
    if (have) {
        patch.device_type = st.device_type;
        memcpy(patch.sw_version, st.sw_version, 3);
        patch.status = st.status;
        patch.delivery = st.delivery;
        patch.delivery.last_ms = INT64_MIN; /* the wall clock restarted */
    } else {
        medtrum_patch_set_state(&patch, MEDTRUM_STATE_FILLED);
        medtrum_patch_set_reservoir(&patch, (uint16_t)(MEDTRUM_DEFAULT_UNITS * 20));
        medtrum_patch_set_battery(&patch, 5901296); /* 5.97 V / 2.81 V */
        save_state();
    }
    if (patch.delivery.patch_id[0] == 0 && patch.delivery.patch_id[1] == 0 && patch.delivery.patch_id[2] == 0 &&
        patch.delivery.patch_id[3] == 0) {
        assign_patch_id();
        save_state();
    }
    saved_status = patch.status;
    saved_delivery = patch.delivery;
    saved_at_ms = now_ms(NULL);

    uint8_t adv[4];
    medtrum_serial_advert_bytes(patch.sn, adv);
    ESP_LOGW(TAG, "simulating a Medtrum %s, serial %02X%02X%02X%02X (advertises %02X%02X%02X%02X). "
                  "TEST EQUIPMENT: delivers nothing.",
             medtrum_serial_model(patch.sn), patch.sn[0], patch.sn[1], patch.sn[2], patch.sn[3], adv[0], adv[1],
             adv[2], adv[3]);
    advertise_now();
    xTaskCreatePinnedToCore(medtrum_task, "medtrum", 12288, NULL, 5, NULL, tskNO_AFFINITY);
}

/* ---- the panel ---------------------------------------------------------- */

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
        return false;
    }
    *out = v;
    return true;
}

/* The number after "key", as a float: the reservoir is set in 0.05 U steps. */
static bool json_double(const char *j, const char *key, double *out)
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
    double v = strtod(p, &end);
    if (end == p) {
        return false;
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

size_t medtrum_app_status_json(char *out, size_t cap)
{
    if (mu == NULL) {
        return 0; /* the app has not started; no lock to take */
    }
    size_t n;
    LOCKED({
        uint16_t id = (uint16_t)(patch.delivery.patch_id[0] | (patch.delivery.patch_id[1] << 8));
        n = (size_t)snprintf(out, cap,
                             "{\"serial\":\"%02X%02X%02X%02X\",\"model\":\"%s\",\"state\":%u,"
                             "\"reservoir\":%.2f,\"battery\":%.2f,\"patchTime\":%u,\"patchId\":\"%04X\","
                             "\"prime\":%u,\"connected\":%s}",
                             patch.sn[0], patch.sn[1], patch.sn[2], patch.sn[3], medtrum_serial_model(patch.sn),
                             (unsigned)patch.status.state, patch.status.reservoir * 0.05,
                             (double)(patch.status.battery >> 12) / 512.0, (unsigned)patch.delivery.patch_time, id,
                             (unsigned)patch.status.prime_progress,
                             ble_medtrum_connected() ? "true" : "false");
    });
    return n < cap ? n : 0;
}

/* {"command": "medtrumSetSerial", "sn": "4A12D828"}, "medtrumSetModel" {units:
 * 200|300}, "medtrumSetReservoir" {units}, "medtrumSetState" {state},
 * "medtrumSetFault" {state}, "medtrumReset". Returns NULL, or what was wrong. */
const char *medtrum_app_command(const char *body)
{
    if (mu == NULL) {
        return "the Medtrum base is not running";
    }
    char cmd[32], hex[16];
    long v;
    if (!json_str(body, "command", cmd, sizeof cmd)) {
        return "want {\"command\": ...}";
    }
    if (strcmp(cmd, "medtrumSetSerial") == 0) {
        uint8_t sn[4];
        if (!json_str(body, "sn", hex, sizeof hex) || !medtrum_serial_parse(hex, sn)) {
            return "sn is 8 hex digits, e.g. \"4A12D828\"";
        }
        /* The app derives the model from this serial and refuses a base it
         * cannot read one from, so refuse it here rather than let the phone say
         * "Incorrect serial number received" after a scan. */
        if (!medtrum_serial_valid(sn)) {
            return "that serial does not validate in the app: 4A12D828 (200U) or 52DF1614 (300U)";
        }
        LOCKED({
            memcpy(patch.sn, sn, 4);
            patch.authenticated = false;
            save_state();
            advertise_now();
        });
    } else if (strcmp(cmd, "medtrumSetModel") == 0) {
        if (!json_number(body, "units", &v) || (v != 200 && v != 300)) {
            return "units is 200 or 300";
        }
        uint8_t sn[4] = {0x4A, 0x12, 0xD8, 0x28};
        if (v == 300) {
            sn[0] = 0x52;
            sn[1] = 0xDF;
            sn[2] = 0x16;
            sn[3] = 0x14;
        }
        LOCKED({
            memcpy(patch.sn, sn, 4);
            patch.authenticated = false;
            save_state();
            advertise_now();
        });
    } else if (strcmp(cmd, "medtrumSetReservoir") == 0) {
        double units;
        if (!json_double(body, "units", &units) || units < 0 || units > 300) {
            return "units is 0 to 300";
        }
        LOCKED({
            /* One pulse is 0.05 U; round 186.8 to 3736 pulses. */
            medtrum_patch_set_reservoir(&patch, (uint16_t)lround(units / 0.05));
            save_state();
        });
    } else if (strcmp(cmd, "medtrumSetState") == 0) {
        if (!json_number(body, "state", &v) || v < 0 || v > 255) {
            return "state is 0 to 255";
        }
        LOCKED({
            medtrum_patch_set_state(&patch, (uint8_t)v);
            save_state();
        });
    } else if (strcmp(cmd, "medtrumSetFault") == 0) {
        if (!json_number(body, "state", &v) || v < 0 || v > 255) {
            return "state is 0 to 255";
        }
        LOCKED({
            medtrum_patch_set_fault(&patch, (uint8_t)v);
            save_state();
        });
    } else if (strcmp(cmd, "medtrumReset") == 0) {
        LOCKED({
            uint8_t sn[4];
            memcpy(sn, patch.sn, 4);
            medtrum_patch_init(&patch, sn);
            patch.device_type = 80;
            patch.sw_version[0] = 12;
            patch.sw_version[1] = 1;
            patch.sw_version[2] = 3;
            medtrum_patch_set_state(&patch, MEDTRUM_STATE_FILLED);
            medtrum_patch_set_reservoir(&patch, (uint16_t)(MEDTRUM_DEFAULT_UNITS * 20));
            medtrum_patch_set_battery(&patch, 5901296);
            assign_patch_id();
            save_state();
        });
    } else {
        return "unknown Medtrum command";
    }
    return NULL;
}
