/*
 * A simulated Dexcom G6 transmitter or G7 sensor, as the controller apps'
 * code sees one (LoopKit's CGMBLEKit for G6, G7SensorKit for G7).
 *
 * TEST EQUIPMENT ONLY: the glucose readings are made up. Never let an app
 * that controls a real pump or pod read them.
 *
 * Portable C: the platform supplies a cgm_io_t (radio, clock, storage) and the
 * AES block cipher (dash_aes128_encrypt_block, from the pod core).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    CGM_OFF = 0,
    CGM_G6 = 6,
    CGM_G7 = 7,
} cgm_kind_t;

/* The sensor's GATT characteristics (service F8083532-849E-531C-C594-30F1F86A4EA5). */
typedef enum {
    CGM_CHR_COMM = 0, /* …3533 */
    CGM_CHR_CONTROL,  /* …3534 */
    CGM_CHR_AUTH,     /* …3535 */
    CGM_CHR_BACKFILL, /* …3536 */
    CGM_CHR_COUNT,
} cgm_chr_t;

#define CGM_READING_INTERVAL 300 /* seconds */
#define CGM_HISTORY 36           /* readings kept for backfill: 3 hours */
#define CGM_STATE_VERSION 1

/* Algorithm states, as both G6 and G7 number them. */
enum {
    CGM_STATE_STOPPED = 1,
    CGM_STATE_WARMUP = 2,
    CGM_STATE_OK = 6,
    CGM_STATE_G6_SESSION_FAILURE = 16,
    CGM_STATE_G7_TEMPORARY_ISSUE = 18,
    CGM_STATE_G7_EXPIRED = 24,
    CGM_STATE_G7_FAILED = 25,
    CGM_STATE_G7_SESSION_ENDED = 26,
};

typedef struct {
    uint32_t t;      /* Unix seconds */
    uint32_t seq;    /* reading number */
    uint16_t glucose; /* mg/dL; 0 when there is none */
    uint8_t state;
    int8_t trend;    /* tenths of mg/dL per minute */
} cgm_reading_t;

typedef enum {
    CGM_PATTERN_MANUAL = 0, /* the panel's glucose, moved by the panel's trend */
    CGM_PATTERN_PERSON = 1, /* a virtual person: meals, insulin, dawn, exercise, drift, sensor noise */
} cgm_pattern_t;

typedef enum {
    CGM_INSULIN_SELF = 0, /* the person doses themselves: basal, and (imperfect) meal boluses */
    CGM_INSULIN_PUMP = 1, /* only what the simulated pump delivers: the AID's own insulin */
} cgm_insulin_t;

#define CGM_MEALS 8
#define CGM_INSULIN_BUCKETS 84 /* 5-minute buckets: 7 hours of insulin */

typedef struct {
    int64_t t;          /* Unix seconds: eaten (or to be) */
    uint16_t grams;
    uint16_t absorb;    /* minutes */
    int64_t bolus_t;    /* CGM_INSULIN_SELF: when the person boluses for it, 0 once given */
    uint16_t bolus;     /* hundredths of a unit */
    uint8_t food;       /* index into the food list (cgm_food_name) */
    uint8_t delay;      /* minutes before it starts to absorb (fat and protein) */
} cgm_meal_t;

/* The virtual person's state, saved with the sensor. */
typedef struct {
    int64_t g_milli;        /* true glucose, thousandths of mg/dL */
    int64_t minute;         /* the last simulated minute (Unix seconds / 60) */
    uint32_t rng;
    int32_t drift;          /* thousandths of mg/dL a minute: hormones, stress, the unexplained */
    int32_t noise;          /* the sensor's error, thousandths of mg/dL */
    uint32_t pump_last;     /* the pump's cumulative delivery when last seen, hundredths */
    bool pump_seen;
    int32_t basal_milli;    /* self-dosed basal not yet counted, thousandths of a hundredth */
    uint16_t ins[CGM_INSULIN_BUCKETS]; /* insulin per 5 minutes, hundredths; [0] the current */
    int64_t ins_bucket;     /* the current bucket's minute / 5 */
    cgm_meal_t meals[CGM_MEALS];
    int64_t planned_day;    /* local day number the meals were planned for */
    int64_t exercise_t;     /* Unix seconds, 0 none */
    uint16_t exercise_min;
    int64_t correction_t;   /* the person's last correction bolus */
    int64_t treated_t;      /* the person's last low treatment */
} cgm_person_t;

typedef struct {
    uint8_t version;
    uint8_t kind;          /* cgm_kind_t */
    char id[8];            /* G6: the 6-character transmitter id. G7: the 2 characters after "DXCM" */
    uint8_t addr[6];       /* the sensor's random static Bluetooth address */
    int64_t activated;     /* Unix seconds: transmitter (G6) or sensor (G7) start */
    int64_t session_start; /* Unix seconds: G6 sensor session start, 0 if none. G7: = activated */
    int64_t warmup_done;   /* Unix seconds: warm-up ends (the panel can skip it) */
    /* What the panel set. */
    uint16_t glucose;      /* the next reading, mg/dL */
    int8_t trend;          /* tenths of mg/dL per minute, applied every reading */
    uint8_t forced_state;  /* 0: from the session's age */
    bool signal_loss;      /* stop advertising: readings are missed, and backfilled later */
    /* Readings. */
    uint32_t next_n;       /* the next reading is due at activated + next_n * 5 minutes */
    uint32_t delivered;    /* the last reading's seq the phone was sent, 0 if none */
    uint16_t calibration;  /* G6: the last calibration, mg/dL */
    uint32_t calibration_t;
    uint8_t n_hist;
    cgm_reading_t hist[CGM_HISTORY]; /* oldest first */
    /* Added later, at the end: an older save loads with these zero, which is
     * the manual pattern (see store_load_cgm). */
    uint8_t pattern;        /* cgm_pattern_t */
    uint8_t insulin_source; /* cgm_insulin_t */
    uint16_t isf;           /* the person: mg/dL one unit lowers */
    uint16_t icr;           /* grams of carbohydrate one unit covers */
    uint16_t basal_need;    /* hundredths of a unit an hour that hold glucose level */
    cgm_person_t person;
    /* G6 receiver slot (0x01: a receiver, pump or watch): the last reading's
     * seq it was sent. `delivered` above is the phone app's slot (0x02). */
    uint32_t delivered_rx;
} cgm_state_t;

typedef struct {
    void *ctx;
    /* Sends p on chr to the connected phone (a notification). */
    void (*send)(void *ctx, cgm_chr_t chr, const uint8_t *p, size_t n);
    void (*disconnect)(void *ctx);
    /* Advertise (or stop) under name, from the sensor's address. */
    void (*advertise)(void *ctx, bool on, const char *name, const uint8_t addr[6]);
    int64_t (*now)(void *ctx); /* Unix seconds */
    void (*save)(void *ctx, const cgm_state_t *st);
    void (*log)(void *ctx, const char *msg);
    /* The local zone, seconds east of UTC (meal times). NULL: UTC. */
    int32_t (*utc_offset)(void *ctx);
    /* The simulated pump's cumulative delivery, hundredths of a unit, or -1
     * when there is no pump to read. NULL: none. */
    int64_t (*pump_insulin)(void *ctx);
} cgm_io_t;

typedef struct {
    cgm_state_t st;
    const cgm_io_t *io;
    /* The connection. */
    bool connected;
    bool sent_glucose;
    bool authed;              /* G6 active mode: the app passed the challenge */
    uint8_t challenge[8];
    uint8_t auth_value[17];   /* what a read of the auth characteristic returns */
    uint8_t auth_len;
    int64_t connected_at, last_activity;
    uint32_t prev_delivered;  /* this connection's slot's last reading when it began */
    uint8_t slot;             /* G6: 0x01 receiver, 0x02 phone app (also a passive client) */
    bool slot_named;          /* G6: this connection's AuthRequestTx said its slot */
    uint32_t pushed_seq;      /* the reading pushed on subscribe before a login (0 none):
                               * counted for the slot it then logs in to, else the app's */
    /* Advertising. */
    bool advertising;
    int64_t window_end;
    int64_t rx_until;         /* G6: advertise for the receiver slot until then */
} cgm_t;

/* A fresh G6 transmitter (id NULL: a random one) or G7 sensor, started now. */
void cgm_state_fresh(cgm_state_t *st, cgm_kind_t kind, const char *id, int64_t now);
/* The name it advertises: "Dexcom" + the id's last 2 (G6), "DXCM" + 2 (G7). */
void cgm_name(const cgm_state_t *st, char out[16]);
bool cgm_valid_g6_id(const char *id);

void cgm_init(cgm_t *c, const cgm_state_t *st, const cgm_io_t *io);
/* Call about once a second: makes readings, opens and closes the advertising
 * window, ends idle connections. */
void cgm_tick(cgm_t *c);

/* From the radio. read returns the value's length. */
void cgm_on_connect(cgm_t *c);
void cgm_on_disconnect(cgm_t *c);
void cgm_on_subscribe(cgm_t *c, cgm_chr_t chr, bool on);
void cgm_on_write(cgm_t *c, cgm_chr_t chr, const uint8_t *p, size_t n);
size_t cgm_on_read(cgm_t *c, cgm_chr_t chr, uint8_t *out, size_t cap);

/* The control panel. */
void cgm_set_glucose(cgm_t *c, uint16_t glucose, int8_t trend);
void cgm_set_state(cgm_t *c, uint8_t forced_state);
void cgm_set_signal_loss(cgm_t *c, bool on);
void cgm_skip_warmup(cgm_t *c);
/* The virtual person (CGM_PATTERN_PERSON). 0 keeps a value as it is. */
void cgm_set_pattern(cgm_t *c, cgm_pattern_t pattern, cgm_insulin_t source, uint16_t isf, uint16_t icr,
                     uint16_t basal_need);
void cgm_eat(cgm_t *c, uint16_t grams);        /* a snack now, on top of the person's own meals */
const char *cgm_food_name(uint8_t food);
void cgm_exercise(cgm_t *c, uint16_t minutes); /* exercise starting now */
/* The person's true glucose, mg/dL (tests). */
int cgm_person_glucose(const cgm_t *c);
void cgm_new_sensor(cgm_t *c, cgm_kind_t kind, const char *id);
/* A JSON object describing the CGM, for the panel. */
size_t cgm_status_json(cgm_t *c, char *out, size_t cap);

/* ---- exposed for tests ------------------------------------------------------ */

uint16_t cgm_crc16(const uint8_t *p, size_t n); /* CRC-CCITT (XModem), as CGMBLEKit */
/* G6: AES-128-ECB of (data || data) under "00"+id+"00"+id, first 8 bytes. */
void cgm_g6_hash(const char *id, const uint8_t data[8], uint8_t out[8]);
/* The state a reading made now would have. */
uint8_t cgm_current_state(const cgm_state_t *st, int64_t now);
size_t cgm_g6_time_msg(const cgm_state_t *st, int64_t now, uint8_t out[16]);
size_t cgm_g6_glucose_msg(const cgm_state_t *st, const cgm_reading_t *r, uint8_t opcode, uint8_t out[16]);
size_t cgm_g7_glucose_msg(const cgm_state_t *st, const cgm_reading_t *r, int64_t now, uint8_t out[19]);
size_t cgm_g7_backfill_record(const cgm_state_t *st, const cgm_reading_t *r, uint8_t out[9]);
