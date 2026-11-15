/*
 * The simulated Medtrum Nano patch on the board: wires the portable patch
 * core (medtrumpatch) to the radio, the clock and flash, runs the connect
 * session and the delivery commands, and takes the control panel's commands.
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

static const char *TAG = "medtrum";

/* The default serials: the bytes the app reads out of the advertisement, in
 * order. Reversed and decrypted they are MD0201 (200U) and MD8301 (300U). */
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
static SemaphoreHandle_t mu;

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

/* ---- the serial and its model ------------------------------------------ */

/* MedtrumKit reads the serial big-endian and simpleDecrypts it. */
static const char *model_name(const uint8_t sn[4])
{
    uint32_t word = ((uint32_t)sn[0] << 24) | ((uint32_t)sn[1] << 16) | ((uint32_t)sn[2] << 8) | sn[3];
    uint32_t type = medtrum_simple_decrypt(word);
    if (type >= 126000000u && type < 127000000u) return "MD0201";
    if (type >= 127000000u && type < 128000000u) return "MD5201";
    if (type >= 128000000u && type < 129000000u) return "MD8201";
    if (type >= 130000000u && type < 131000000u) return "MD0202";
    if (type >= 131000000u && type < 132000000u) return "MD5202";
    if (type >= 148000000u && type < 149000000u) return "MD8301";
    return "INVALID";
}

static bool serial_hex(const char *s, uint8_t sn[4])
{
    if (strlen(s) != 8) {
        return false;
    }
    for (int i = 0; i < 4; i++) {
        char b[3] = {s[i * 2], s[i * 2 + 1], 0};
        char *end;
        long v = strtol(b, &end, 16);
        if (*end != 0) {
            return false;
        }
        sn[i] = (uint8_t)v;
    }
    return true;
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
}

/* A fresh patch identity, as the reference assigns on activation. */
static void assign_patch_id(void)
{
    uint8_t id[4];
    esp_fill_random(id, sizeof id);
    medtrum_patch_set_patch_id(&patch, id);
}

bool medtrum_app_state_valid(const medtrum_saved_t *st)
{
    return st->magic == MEDTRUM_STATE_MAGIC && st->version == MEDTRUM_STATE_VERSION;
}

/* ---- requests ----------------------------------------------------------- */

/* Reassembled by the component's parser (src/request.c), which the host tests
 * cover directly. */
static medtrum_request_t req;

static void notify_status(void)
{
    uint8_t payload[MEDTRUM_STATUS_MAX];
    size_t n = medtrum_patch_notification(&patch, payload, sizeof payload);
    if (n > 0) {
        ble_medtrum_notify_status(NULL, payload, n);
    }
}

static void on_write(const uint8_t *data, size_t n)
{
    if (!medtrum_request_feed(&req, data, n)) {
        return;
    }
    /* Only a command that changed the model needs a flash commit; a GET_TIME
     * poll, a sync or a subscribe must not wear NVS. */
    medtrum_status_t was_status = patch.status;
    medtrum_delivery_t was_delivery = patch.delivery;
    medtrum_frag_t frags[MEDTRUM_FRAGS_MAX];
    int count = medtrum_patch_handle(&patch, req.command, req.sequence, req.content, req.content_len, frags,
                                     MEDTRUM_FRAGS_MAX);
    if (count < 0) {
        ESP_LOGW(TAG, "unhandled command %u", req.command);
        return;
    }
    for (int i = 0; i < count; i++) {
        io.notify(io.ctx, frags[i].data, frags[i].len);
    }
    if (memcmp(&was_status, &patch.status, sizeof was_status) != 0 ||
        memcmp(&was_delivery, &patch.delivery, sizeof was_delivery) != 0) {
        save_state();
    }
    notify_status();
}

/* ---- the run loop ------------------------------------------------------- */

static void medtrum_task(void *arg)
{
    for (;;) {
        medtrum_ev_t ev;
        bool got = io.next_event(io.ctx, &ev, 1000);
        LOCKED({
            medtrum_patch_tick(&patch, now_ms(NULL));
            if (!got) {
                notify_status();
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
                    on_write(ev.data, ev.len);
                    break;
                case MEDTRUM_EV_STOP:
                    return;
                default:
                    break;
                }
            }
        });
    }
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
    if (patch.delivery.patch_id[0] == 0 && patch.delivery.patch_id[1] == 0 &&
        patch.delivery.patch_id[2] == 0 && patch.delivery.patch_id[3] == 0) {
        assign_patch_id();
        save_state();
    }
    ESP_LOGW(TAG, "simulating a Medtrum %s, serial %02X%02X%02X%02X. TEST EQUIPMENT: delivers nothing.",
             model_name(patch.sn), patch.sn[0], patch.sn[1], patch.sn[2], patch.sn[3]);
    io.advertise(io.ctx, true, patch.sn);
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
    size_t n;
    LOCKED({
        n = (size_t)snprintf(out, cap,
                             "{\"serial\":\"%02X%02X%02X%02X\",\"model\":\"%s\",\"state\":%u,"
                             "\"reservoir\":%.2f,\"battery\":%.2f,\"patchTime\":%u,\"connected\":%s}",
                             patch.sn[0], patch.sn[1], patch.sn[2], patch.sn[3], model_name(patch.sn),
                             (unsigned)patch.status.state, patch.status.reservoir * 0.05,
                             (double)(patch.status.battery >> 12) / 512.0,
                             (unsigned)patch.delivery.patch_time, ble_medtrum_connected() ? "true" : "false");
    });
    return n < cap ? n : 0;
}

/* {"command": "medtrumSetSerial", "sn": "4A12D828"}, "medtrumSetModel" {units:
 * 200|300}, "medtrumSetReservoir" {units}, "medtrumSetState" {state},
 * "medtrumSetFault" {state}, "medtrumReset". Returns NULL, or what was wrong. */
const char *medtrum_app_command(const char *body)
{
    char cmd[32], hex[16];
    long v;
    if (!json_str(body, "command", cmd, sizeof cmd)) {
        return "want {\"command\": ...}";
    }
    if (strcmp(cmd, "medtrumSetSerial") == 0) {
        uint8_t sn[4];
        if (!json_str(body, "sn", hex, sizeof hex) || !serial_hex(hex, sn)) {
            return "sn is 8 hex digits, e.g. \"4A12D828\"";
        }
        LOCKED({
            memcpy(patch.sn, sn, 4);
            patch.authenticated = false;
            save_state();
            io.advertise(io.ctx, true, patch.sn);
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
            io.advertise(io.ctx, true, patch.sn);
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
