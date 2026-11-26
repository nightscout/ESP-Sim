/*
 * A simulated Dexcom G6 transmitter or G7 sensor. The messages are the ones
 * LoopKit's CGMBLEKit (G6) and G7SensorKit (G7) read; see test/host/test_cgm.c.
 *
 * Both apps' code normally watches the Dexcom app talk to the sensor
 * ("passive mode"): it subscribes to the auth characteristic, waits for an
 * authenticated-and-bonded message, subscribes to control and reads what goes
 * by. With no Dexcom app here, the simulator sends those messages itself.
 * CGMBLEKit can also run the G6 session itself ("active mode"): it logs in
 * with a key made from the transmitter id and asks for each message.
 *
 * TEST EQUIPMENT ONLY: the readings are made up.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "cgm.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "dash.h" /* dash_aes128_encrypt_block, dash_random */
#include "person.h"

#define G6_WARMUP (2 * 3600)
#define G6_SESSION (10 * 86400)
#define G7_WARMUP (27 * 60)
#define G7_SESSION (10 * 86400 + 12 * 3600) /* 10 days, and the 12-hour grace period */
#define WINDOW 60    /* seconds of advertising per reading */
#define IDLE_END 3   /* seconds of quiet, once the reading is out, before hanging up */
#define CONN_MAX 30  /* seconds a connection may last */
#define NO_TREND 0x7f

static const char ID_CHARS[] = "0123456789ABCDEFGHJKLMNPQRSTUVWXYZ";

/* Sine for the pattern generator, scaled by 10000, one degree per entry (361
 * so 360 repeats 0). Integer-only: the host tests and the firmware link no
 * libm. */
static const int16_t SIN_TABLE[361] = {
        0,   175,   349,   523,   698,   872,  1045,  1219,  1392,  1564,  1736,  1908,
     2079,  2250,  2419,  2588,  2756,  2924,  3090,  3256,  3420,  3584,  3746,  3907,
     4067,  4226,  4384,  4540,  4695,  4848,  5000,  5150,  5299,  5446,  5592,  5736,
     5878,  6018,  6157,  6293,  6428,  6561,  6691,  6820,  6947,  7071,  7193,  7314,
     7431,  7547,  7660,  7771,  7880,  7986,  8090,  8192,  8290,  8387,  8480,  8572,
     8660,  8746,  8829,  8910,  8988,  9063,  9135,  9205,  9272,  9336,  9397,  9455,
     9511,  9563,  9613,  9659,  9703,  9744,  9781,  9816,  9848,  9877,  9903,  9925,
     9945,  9962,  9976,  9986,  9994,  9998, 10000,  9998,  9994,  9986,  9976,  9962,
     9945,  9925,  9903,  9877,  9848,  9816,  9781,  9744,  9703,  9659,  9613,  9563,
     9511,  9455,  9397,  9336,  9272,  9205,  9135,  9063,  8988,  8910,  8829,  8746,
     8660,  8572,  8480,  8387,  8290,  8192,  8090,  7986,  7880,  7771,  7660,  7547,
     7431,  7314,  7193,  7071,  6947,  6820,  6691,  6561,  6428,  6293,  6157,  6018,
     5878,  5736,  5592,  5446,  5299,  5150,  5000,  4848,  4695,  4540,  4384,  4226,
     4067,  3907,  3746,  3584,  3420,  3256,  3090,  2924,  2756,  2588,  2419,  2250,
     2079,  1908,  1736,  1564,  1392,  1219,  1045,   872,   698,   523,   349,   175,
        0,  -175,  -349,  -523,  -698,  -872, -1045, -1219, -1392, -1564, -1736, -1908,
    -2079, -2250, -2419, -2588, -2756, -2924, -3090, -3256, -3420, -3584, -3746, -3907,
    -4067, -4226, -4384, -4540, -4695, -4848, -5000, -5150, -5299, -5446, -5592, -5736,
    -5878, -6018, -6157, -6293, -6428, -6561, -6691, -6820, -6947, -7071, -7193, -7314,
    -7431, -7547, -7660, -7771, -7880, -7986, -8090, -8192, -8290, -8387, -8480, -8572,
    -8660, -8746, -8829, -8910, -8988, -9063, -9135, -9205, -9272, -9336, -9397, -9455,
    -9511, -9563, -9613, -9659, -9703, -9744, -9781, -9816, -9848, -9877, -9903, -9925,
    -9945, -9962, -9976, -9986, -9994, -9998, -10000, -9998, -9994, -9986, -9976, -9962,
    -9945, -9925, -9903, -9877, -9848, -9816, -9781, -9744, -9703, -9659, -9613, -9563,
    -9511, -9455, -9397, -9336, -9272, -9205, -9135, -9063, -8988, -8910, -8829, -8746,
    -8660, -8572, -8480, -8387, -8290, -8192, -8090, -7986, -7880, -7771, -7660, -7547,
    -7431, -7314, -7193, -7071, -6947, -6820, -6691, -6561, -6428, -6293, -6157, -6018,
    -5878, -5736, -5592, -5446, -5299, -5150, -5000, -4848, -4695, -4540, -4384, -4226,
    -4067, -3907, -3746, -3584, -3420, -3256, -3090, -2924, -2756, -2588, -2419, -2250,
    -2079, -1908, -1736, -1564, -1392, -1219, -1045,  -872,  -698,  -523,  -349,  -175,
        0,
};

static uint16_t clamp_glucose(int32_t g)
{
    return (uint16_t)(g < 40 ? 40 : g > 400 ? 400 : g);
}

static int32_t sin_scaled(uint32_t phase_milli)
{
    uint32_t deg = (phase_milli / 1000) % 360;
    uint32_t frac = phase_milli % 1000;
    int32_t a = SIN_TABLE[deg], b = SIN_TABLE[deg + 1];
    return a + (b - a) * (int32_t)frac / 1000;
}

/* A deterministic 32-bit mix. The walk has no RNG so that the same reading
 * number always gives the same reading, before and after a reboot. */
static uint32_t mix32(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

const char *cgm_pattern_name(cgm_pattern_kind_t kind)
{
    return kind == CGM_PATTERN_SINE ? "sine"
           : kind == CGM_PATTERN_WALK ? "walk"
           : kind == CGM_PATTERN_PERSON ? "person"
                                        : "fixed";
}

bool cgm_pattern_from_name(const char *name, cgm_pattern_kind_t *out)
{
    if (!name || !out) {
        return false;
    }
    if (strcmp(name, "fixed") == 0) {
        *out = CGM_PATTERN_FIXED;
    } else if (strcmp(name, "person") == 0) {
        *out = CGM_PATTERN_PERSON;
    } else if (strcmp(name, "sine") == 0) {
        *out = CGM_PATTERN_SINE;
    } else if (strcmp(name, "walk") == 0) {
        *out = CGM_PATTERN_WALK;
    } else {
        return false;
    }
    return true;
}

cgm_next_reading_t cgm_pattern_next(const cgm_pattern_t *p, uint16_t previous, uint32_t reading_number)
{
    cgm_next_reading_t out = {.glucose = clamp_glucose(p->value), .trend = p->trend};
    if (p->kind != CGM_PATTERN_SINE && p->kind != CGM_PATTERN_WALK) {
        if (out.trend > 126) out.trend = 126;
        if (out.trend < -127) out.trend = -127;
        return out;
    }
    if (p->kind == CGM_PATTERN_SINE) {
        uint32_t period = p->period_minutes ? p->period_minutes : 240;
        uint64_t phase = (uint64_t)reading_number * CGM_READING_INTERVAL * 360000U / ((uint64_t)period * 60U);
        int32_t wave = (int32_t)p->amplitude * sin_scaled((uint32_t)(phase % 360000U)) / 10000;
        out.glucose = clamp_glucose((int32_t)p->value + wave);
    } else {
        uint16_t start = previous ? previous : 120;
        if (reading_number == 0 || start == p->value) {
            out.glucose = clamp_glucose(start);
        } else {
            int32_t diff = (int32_t)p->value - (int32_t)start;
            uint32_t frac = 40 + mix32(reading_number * 2654435761U + (uint32_t)p->value * 977U) % 81;
            int32_t step = (diff < 0 ? -diff : diff) * (int32_t)frac / 100;
            if (step < 1) step = 1;
            int32_t next = (int32_t)start + (diff > 0 ? step : -step);
            if ((diff > 0 && next > p->value) || (diff < 0 && next < p->value)) {
                next = p->value;
            }
            out.glucose = clamp_glucose(next);
        }
    }
    /* The curves report the slope that actually occurred over the 5-minute
     * reading interval, in tenths of a mg/dL per minute. The first reading has
     * no previous reading, so it carries no trend. */
    if (previous) {
        int32_t tenths = ((int32_t)out.glucose - (int32_t)previous) * 2;
        out.trend = (int8_t)(tenths > 126 ? 126 : tenths < -127 ? -127 : tenths);
    } else {
        out.trend = 0;
    }
    return out;
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, (uint16_t)v);
    put16(p + 2, (uint16_t)(v >> 16));
}

static uint32_t get32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void logf_(cgm_t *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void logf_(cgm_t *c, const char *fmt, ...)
{
    if (!c->io->log) {
        return;
    }
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    c->io->log(c->io->ctx, buf);
}

uint16_t cgm_crc16(const uint8_t *p, size_t n)
{
    uint16_t crc = 0;
    while (n--) {
        crc ^= (uint16_t)(*p++) << 8;
        for (int i = 0; i < 8; i++) {
            crc = (crc & 0x8000) ? (uint16_t)(crc << 1 ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

/* Appends the CRC to the n bytes at p; returns n + 2. */
static size_t with_crc(uint8_t *p, size_t n)
{
    put16(p + n, cgm_crc16(p, n));
    return n + 2;
}

void cgm_g6_hash(const char *id, const uint8_t data[8], uint8_t out[8])
{
    uint8_t key[16], in[16], enc[16];
    memcpy(key, "00", 2);
    memcpy(key + 2, id, 6);
    memcpy(key + 8, "00", 2);
    memcpy(key + 10, id, 6);
    memcpy(in, data, 8);
    memcpy(in + 8, data, 8);
    dash_aes128_encrypt_block(key, in, enc);
    memcpy(out, enc, 8);
}

bool cgm_valid_g6_id(const char *id)
{
    if (!id || strlen(id) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        if (!strchr(ID_CHARS, id[i]) || id[i] == 0) {
            return false;
        }
    }
    return true;
}

void cgm_name(const cgm_state_t *st, char out[16])
{
    if (st->kind == CGM_G6) {
        snprintf(out, 16, "Dexcom%s", st->id + 4);
    } else if (st->kind == CGM_G7) {
        snprintf(out, 16, "DXCM%s", st->id);
    } else {
        out[0] = 0;
    }
}

static void random_chars(char *out, size_t n)
{
    uint8_t r[8];
    dash_random(r, n);
    for (size_t i = 0; i < n; i++) {
        out[i] = ID_CHARS[r[i] % (sizeof ID_CHARS - 1)];
    }
    out[n] = 0;
}

void cgm_state_fresh(cgm_state_t *st, cgm_kind_t kind, const char *id, int64_t now)
{
    memset(st, 0, sizeof *st);
    st->version = CGM_STATE_VERSION;
    st->kind = (uint8_t)kind;
    if (kind == CGM_G6) {
        if (cgm_valid_g6_id(id)) {
            memcpy(st->id, id, 7);
        } else {
            st->id[0] = '8';
            st->id[1] = '1';
            random_chars(st->id + 2, 4);
        }
    } else if (kind == CGM_G7) {
        random_chars(st->id, 2);
    }
    dash_random(st->addr, 6);
    st->addr[5] |= 0xc0; /* a random static address */
    st->activated = now;
    st->session_start = now;
    st->warmup_done = now + (kind == CGM_G6 ? G6_WARMUP : G7_WARMUP);
    st->glucose = 120;
    st->isf = 45;
    st->icr = 10;
    st->basal_need = 80;
    st->pattern = CGM_PATTERN_FIXED;
    st->amplitude = 50;
    st->period_minutes = 240;
}

void cgm_state_migrate(cgm_state_t *st)
{
    if (st->version == CGM_STATE_VERSION) {
        return;
    }
    /* Version 1 had the person's MANUAL/PERSON pattern and no generator
     * parameters. The patterns are numbered so the old values still mean the
     * same thing (FIXED 0, PERSON 1), so only the new fields need defaults. */
    if (st->version <= 1) {
        if (st->amplitude == 0) {
            st->amplitude = 50;
        }
        if (st->period_minutes == 0) {
            st->period_minutes = 240;
        }
    }
    st->version = CGM_STATE_VERSION;
}

uint8_t cgm_current_state(const cgm_state_t *st, int64_t now)
{
    if (st->forced_state) {
        return st->forced_state;
    }
    if (st->kind == CGM_G6) {
        if (!st->session_start || now - st->session_start >= G6_SESSION) {
            return CGM_STATE_STOPPED;
        }
    } else if (now - st->activated >= G7_SESSION) {
        return CGM_STATE_G7_SESSION_ENDED;
    }
    return now < st->warmup_done ? CGM_STATE_WARMUP : CGM_STATE_OK;
}

/* ---- messages ---------------------------------------------------------------- */

static uint32_t tx_time(const cgm_state_t *st, int64_t t)
{
    return t > st->activated ? (uint32_t)(t - st->activated) : 0;
}

size_t cgm_g6_time_msg(const cgm_state_t *st, int64_t now, uint8_t out[16])
{
    /* 25 status current(4) sessionStart(4) 01000000 crc */
    memset(out, 0, 16);
    out[0] = 0x25;
    put32(out + 2, tx_time(st, now));
    put32(out + 6, st->session_start ? tx_time(st, st->session_start) : 0xffffffff);
    out[10] = 0x01;
    return with_crc(out, 14);
}

size_t cgm_g6_glucose_msg(const cgm_state_t *st, const cgm_reading_t *r, uint8_t opcode, uint8_t out[16])
{
    /* 31|4f status seq(4) timestamp(4) glucose(2) state trend crc */
    out[0] = opcode;
    out[1] = 0;
    put32(out + 2, r->seq);
    put32(out + 6, tx_time(st, r->t));
    put16(out + 10, r->glucose);
    out[12] = r->state;
    out[13] = (uint8_t)r->trend;
    return with_crc(out, 14);
}

size_t cgm_g7_glucose_msg(const cgm_state_t *st, const cgm_reading_t *r, int64_t now, uint8_t out[19])
{
    /* 4e 00 timestamp(4) seq(2) 00 01 age(2) glucose(2) state trend predicted(2) calibration */
    uint32_t age = now > (int64_t)r->t ? (uint32_t)(now - r->t) : 0;
    if (age > 0xffff) {
        age = 0xffff;
    }
    bool has = r->glucose != 0;
    out[0] = 0x4e;
    out[1] = 0;
    put32(out + 2, tx_time(st, now));
    put16(out + 6, (uint16_t)r->seq);
    out[8] = 0;
    out[9] = 1;
    put16(out + 10, (uint16_t)age);
    put16(out + 12, has ? r->glucose : 0xffff);
    out[14] = r->state;
    out[15] = has ? (uint8_t)r->trend : NO_TREND;
    put16(out + 16, 0xffff);
    out[18] = 0x0e;
    return 19;
}

size_t cgm_g7_backfill_record(const cgm_state_t *st, const cgm_reading_t *r, uint8_t out[9])
{
    /* timestamp(3) 00 glucose(2) state flags trend */
    uint32_t t = tx_time(st, r->t);
    bool has = r->glucose != 0;
    out[0] = (uint8_t)t;
    out[1] = (uint8_t)(t >> 8);
    out[2] = (uint8_t)(t >> 16);
    out[3] = 0;
    put16(out + 4, has ? r->glucose : 0xffff);
    out[6] = r->state;
    out[7] = 0x0e;
    out[8] = has ? (uint8_t)r->trend : NO_TREND;
    return 9;
}

/* ---- the session ---------------------------------------------------------------- */

static void save(cgm_t *c)
{
    if (c->io->save) {
        c->io->save(c->io->ctx, &c->st);
    }
}

/* The central's session, or NULL for a token that names no slot. */
static cgm_session_state_t *session_at(cgm_t *c, cgm_session_t session)
{
    return session < CGM_MAX_SESSIONS ? &c->sessions[session] : NULL;
}

static bool any_connected(const cgm_t *c)
{
    for (int i = 0; i < CGM_MAX_SESSIONS; i++) {
        if (c->sessions[i].connected) {
            return true;
        }
    }
    return false;
}

/* Forget every session's delivery history: a new sensor or a transmitter reset
 * starts a fresh reading history, so no cursor can point into the old one. */
static void clear_delivery_history(cgm_t *c)
{
    for (int i = 0; i < CGM_MAX_SESSIONS; i++) {
        c->sessions[i].delivered = 0;
        c->sessions[i].prev_delivered = 0;
    }
}

static void send(cgm_t *c, cgm_session_t session, cgm_chr_t chr, const uint8_t *p, size_t n)
{
    cgm_session_state_t *s = session_at(c, session);
    if (s && s->connected) {
        c->io->send(c->io->ctx, session, chr, p, n);
    }
}

static void advertise(cgm_t *c, bool on)
{
    if (c->advertising == on) {
        return;
    }
    c->advertising = on;
    char name[16];
    cgm_name(&c->st, name);
    c->io->advertise(c->io->ctx, on, name, c->st.addr);
}

static const cgm_reading_t *latest(const cgm_t *c)
{
    return c->st.n_hist ? &c->st.hist[c->st.n_hist - 1] : NULL;
}

static void push_glucose(cgm_t *c, cgm_session_t session);

static void make_reading(cgm_t *c, uint32_t n, int64_t now)
{
    cgm_state_t *st = &c->st;
    cgm_reading_t r = {
        .t = (uint32_t)(st->activated + (int64_t)n * CGM_READING_INTERVAL),
        .seq = n + 1,
        .state = cgm_current_state(st, now),
        .trend = st->trend,
    };
    if (st->pattern == CGM_PATTERN_PERSON) {
        /* The virtual person: the sensor's value now; the trend over the last
         * 15 minutes, as a Dexcom reports it. */
        person_advance(c, now);
        st->glucose = person_reading(c);
        int trend = 0;
        for (int i = st->n_hist - 1; i >= 0; i--) {
            if (st->hist[i].glucose && r.seq - st->hist[i].seq == 3) {
                trend = (st->glucose - st->hist[i].glucose) * 10 / 15;
                break;
            }
        }
        r.trend = (int8_t)(trend > 80 ? 80 : trend < -80 ? -80 : trend);
        if (r.state == CGM_STATE_OK) {
            r.glucose = st->glucose;
        } else {
            r.trend = NO_TREND;
        }
    } else if (r.state == CGM_STATE_OK) {
        const cgm_reading_t *prev_r = latest(c);
        /* 0 is the generator's "no previous reading" sentinel. A warm-up (or
         * other error-state) entry has no glucose, so it must not seed a curve's
         * trend or a walk's path. */
        uint16_t prev = prev_r && prev_r->glucose ? prev_r->glucose : 0;
        cgm_pattern_t p = {
            .kind = (cgm_pattern_kind_t)st->pattern,
            .value = st->glucose,
            .trend = st->trend,
            .amplitude = st->amplitude,
            .period_minutes = st->period_minutes,
        };
        cgm_next_reading_t next = cgm_pattern_next(&p, prev, n);
        r.glucose = next.glucose;
        r.trend = next.trend;
        /* A fixed pattern is the manual one: the panel's value is the next
         * reading, and the reading after it moves by the trend. The curves keep
         * `glucose` as their mean/target and derive every reading from n. */
        if (st->pattern == CGM_PATTERN_FIXED) {
            int g = st->glucose + (st->trend * 5 + (st->trend >= 0 ? 5 : -5)) / 10;
            st->glucose = (uint16_t)(g < 40 ? 40 : g > 400 ? 400 : g);
        }
    } else {
        r.trend = NO_TREND;
    }
    if (st->n_hist == CGM_HISTORY) {
        memmove(st->hist, st->hist + 1, sizeof st->hist[0] * (CGM_HISTORY - 1));
        st->n_hist--;
    }
    st->hist[st->n_hist++] = r;
    logf_(c, "reading %u: %u mg/dL, state %u%s", (unsigned)r.seq, r.glucose, r.state,
          st->signal_loss ? " (signal loss: not sent)" : "");
}

void cgm_init(cgm_t *c, const cgm_state_t *st, const cgm_io_t *io)
{
    memset(c, 0, sizeof *c);
    c->st = *st;
    c->io = io;
    /* After a restart, offer an unsent reading at once. */
    c->window_end = io->now(io->ctx) + WINDOW;
}

void cgm_tick(cgm_t *c)
{
    cgm_state_t *st = &c->st;
    int64_t now = c->io->now(c->io->ctx);
    if (st->kind == CGM_OFF) {
        advertise(c, false);
        return;
    }
    if (st->pattern == CGM_PATTERN_PERSON) {
        person_advance(c, now); /* a step a minute, between readings too */
    }
    uint32_t n = now > st->activated ? (uint32_t)((now - st->activated) / CGM_READING_INTERVAL) : 0;
    if (st->n_hist == 0 || n >= st->next_n) {
        make_reading(c, n, now);
        st->next_n = n + 1;
        save(c);
        /* A G7's window. A G6 never reads window_end, so writing it there is
         * harmless. */
        c->window_end = now + WINDOW;
    }
    const cgm_reading_t *r = latest(c);
    bool undelivered = r && r->seq != st->delivered;
    /* A connected central that asked for CONTROL notifications gets a reading it
     * has not had, whether it was made this tick or it only subscribed after
     * the reading was made. Authentication is not a reason to stop: the real
     * app logs in and still expects a pushed reading every five minutes. */
    for (int i = 0; i < CGM_MAX_SESSIONS; i++) {
        const cgm_session_state_t *s = &c->sessions[i];
        if (s->connected && s->control_on) {
            push_glucose(c, (cgm_session_t)i);
        }
    }
    if (!any_connected(c)) {
        /* A G6 transmitter advertises for as long as no central is connected.
         * A G7 opens a window per reading and closes it. Signal loss silences
         * both on purpose. */
        bool want = st->kind == CGM_G6 || (undelivered && now < c->window_end);
        advertise(c, want && !st->signal_loss);
    }
    /* A G7 collects one reading per link: once it has its reading and has gone
     * quiet it is hung up on, and no G7 link outlives CONN_MAX. A G6 keeps the
     * link it has and pushes every reading on it; signal loss and a new sensor
     * end its sessions elsewhere. */
    if (st->kind == CGM_G7) {
        for (int i = 0; i < CGM_MAX_SESSIONS; i++) {
            const cgm_session_state_t *s = &c->sessions[i];
            if (!s->connected) {
                continue;
            }
            if ((s->sent_glucose && now - s->last_activity >= IDLE_END) || now - s->connected_at >= CONN_MAX) {
                logf_(c, "hanging up");
                c->io->disconnect(c->io->ctx, (cgm_session_t)i);
            }
        }
    }
}

void cgm_on_connect(cgm_t *c, cgm_session_t session)
{
    cgm_session_state_t *s = session_at(c, session);
    if (!s) {
        logf_(c, "ignoring connect for unknown session %u", (unsigned)session);
        return;
    }
    int64_t now = c->io->now(c->io->ctx);
    /* A session is one connection, never one slot: a reconnect may be the same
     * central with a rotated privacy address, so nothing may be inferred. The
     * new connection starts with no conversation and no delivery history. */
    memset(s, 0, sizeof *s);
    s->connected = true;
    c->advertising = false; /* a connection ends the advertising */
    s->connected_at = s->last_activity = now;
    logf_(c, "phone connected");
}

void cgm_on_disconnect(cgm_t *c, cgm_session_t session)
{
    cgm_session_state_t *s = session_at(c, session);
    if (!s) {
        logf_(c, "ignoring disconnect for unknown session %u", (unsigned)session);
        return;
    }
    s->connected = false;
    s->control_on = false;
    logf_(c, "phone disconnected");
}

static void delivered(cgm_t *c, cgm_session_t session, const cgm_reading_t *r)
{
    cgm_session_state_t *s = session_at(c, session);
    if (!s) {
        return;
    }
    s->sent_glucose = true;
    s->delivered = r->seq;
    if (c->st.delivered != r->seq) {
        c->st.delivered = r->seq;
        save(c);
    }
}

/* Passive mode: what the phone would see the Dexcom app ask for. */
static void push_glucose(cgm_t *c, cgm_session_t session)
{
    const cgm_reading_t *r = latest(c);
    cgm_session_state_t *s = session_at(c, session);
    if (!s || !s->connected || !r) {
        return;
    }
    if (s->sent_glucose && s->delivered == r->seq) {
        return; /* this connection already has the current reading */
    }
    int64_t now = c->io->now(c->io->ctx);
    uint8_t m[19];
    if (c->st.kind == CGM_G7) {
        send(c, session, CGM_CHR_CONTROL, m, cgm_g7_glucose_msg(&c->st, r, now, m));
    } else {
        send(c, session, CGM_CHR_CONTROL, m, cgm_g6_time_msg(&c->st, now, m));
        send(c, session, CGM_CHR_CONTROL, m, cgm_g6_glucose_msg(&c->st, r, 0x4f, m));
    }
    delivered(c, session, r);
}

/* The G6 backfill frames for history indices [first, last], then the 0x51
 * response that names the same times and buffer. */
static void send_g6_backfill_range(cgm_t *c, cgm_session_t session, int first, int last, uint8_t id)
{
    const cgm_state_t *st = &c->st;
    uint8_t buf[4 + 8 * CGM_HISTORY] = {0};
    size_t n = 4;
    for (int i = first; i <= last; i++) {
        const cgm_reading_t *h = &st->hist[i];
        put32(buf + n, tx_time(st, h->t));
        put16(buf + n + 4, h->glucose);
        buf[n + 6] = h->state;
        buf[n + 7] = (uint8_t)h->trend;
        n += 8;
    }
    uint8_t frames[sizeof buf + 2 * 32];
    size_t total = 0;
    uint8_t frame_no = 1;
    for (size_t off = 0; off < n; off += 18) {
        size_t k = n - off < 18 ? n - off : 18;
        uint8_t *f = frames + total;
        f[0] = frame_no++;
        f[1] = id;
        memcpy(f + 2, buf + off, k);
        send(c, session, CGM_CHR_BACKFILL, f, k + 2);
        total += k + 2;
    }
    uint8_t m[20] = {0x51, 0x00, 0x01, id};
    put32(m + 4, tx_time(st, st->hist[first].t));
    put32(m + 8, tx_time(st, st->hist[last].t));
    put32(m + 12, (uint32_t)total);
    put16(m + 16, cgm_crc16(frames, total));
    send(c, session, CGM_CHR_CONTROL, m, with_crc(m, 18));
}

/* The readings the session missed, before the one it was just sent. Its own
 * backfill cursor (prev_delivered) keeps it to what this central has not had. */
static void send_backfill(cgm_t *c, cgm_session_t session)
{
    cgm_session_state_t *s = session_at(c, session);
    const cgm_reading_t *r = latest(c);
    if (!s || !r || !s->sent_glucose) {
        return;
    }
    const cgm_state_t *st = &c->st;
    int first = -1, last = -1;
    for (int i = 0; i < st->n_hist - 1; i++) {
        if (st->hist[i].seq > s->prev_delivered) {
            if (first < 0) {
                first = i;
            }
            last = i;
        }
    }
    if (first < 0) {
        return;
    }
    logf_(c, "backfilling %d readings", last - first + 1);
    if (st->kind == CGM_G7) {
        uint8_t rec[9];
        for (int i = first; i <= last; i++) {
            send(c, session, CGM_CHR_BACKFILL, rec, cgm_g7_backfill_record(st, &st->hist[i], rec));
        }
        static const uint8_t done[2] = {0x59, 0x00};
        send(c, session, CGM_CHR_CONTROL, done, sizeof done);
        return;
    }
    send_g6_backfill_range(c, session, first, last, 0);
}

void cgm_on_subscribe(cgm_t *c, cgm_session_t session, cgm_chr_t chr, bool on)
{
    cgm_session_state_t *s = session_at(c, session);
    if (!s) {
        logf_(c, "ignoring subscribe for unknown session %u", (unsigned)session);
        return;
    }
    s->last_activity = c->io->now(c->io->ctx);
    if (chr == CGM_CHR_CONTROL) {
        s->control_on = on;
    }
    if (!on) {
        return;
    }
    switch (chr) {
    case CGM_CHR_AUTH: {
        /* What a phone sees once the Dexcom app has logged in, bonded. */
        static const uint8_t ok[3] = {0x05, 0x01, 0x01};
        send(c, session, CGM_CHR_AUTH, ok, sizeof ok);
        break;
    }
    case CGM_CHR_CONTROL:
        /* Sent below, once the CCCD is actually on and a notification can
         * reach the app. */
        break;
    case CGM_CHR_BACKFILL:
        send_backfill(c, session);
        break;
    default:
        break;
    }
    /* The app subscribes to CONTROL and only then waits for a reading: hand it
     * the current one here, so a pairing flow does not sit until the next
     * five-minute boundary. A repeat subscribe is a no-op: push_glucose knows
     * this connection already has the reading. */
    if (on && chr == CGM_CHR_CONTROL) {
        push_glucose(c, session);
    }
}

static void session_reply(cgm_t *c, cgm_session_t session, uint8_t op, uint32_t requested, int64_t now)
{
    /* 27|29 status received requested(4) sessionStart(4) transmitterTime(4) crc */
    uint8_t m[17] = {op, 0x00, 0x01};
    put32(m + 3, requested);
    put32(m + 7, c->st.session_start ? tx_time(&c->st, c->st.session_start) : 0xffffffff);
    put32(m + 11, tx_time(&c->st, now));
    send(c, session, CGM_CHR_CONTROL, m, with_crc(m, 15));
}

static void g6_control(cgm_t *c, cgm_session_t session, const uint8_t *p, size_t n, int64_t now)
{
    cgm_state_t *st = &c->st;
    uint8_t m[20];
    const cgm_reading_t *r = latest(c);
    switch (p[0]) {
    case 0x24: /* time */
        send(c, session, CGM_CHR_CONTROL, m, cgm_g6_time_msg(st, now, m));
        break;
    case 0x30: /* glucose */
    case 0x4e:
        if (r) {
            send(c, session, CGM_CHR_CONTROL, m, cgm_g6_glucose_msg(st, r, p[0] == 0x30 ? 0x31 : 0x4f, m));
            delivered(c, session, r);
        }
        break;
    case 0x32: { /* calibration data: 33 00 … glucose(2) at 11, timestamp(4) at 13 */
        static const uint8_t tmpl[17] = {0x33, 0x00, 0x2b, 0x29, 0x00, 0x90, 0x01, 0x29, 0x00, 0xae, 0x00};
        memcpy(m, tmpl, 17);
        put16(m + 11, st->calibration);
        put32(m + 13, st->calibration ? tx_time(st, st->calibration_t) : 0);
        send(c, session, CGM_CHR_CONTROL, m, with_crc(m, 17));
        break;
    }
    case 0x26: /* start sensor: startTime(4) secondsSince1970(4) */
        if (n >= 5) {
            uint32_t start = get32(p + 1);
            st->session_start = st->activated + start;
            st->warmup_done = st->session_start + G6_WARMUP;
            st->forced_state = 0;
            save(c);
            logf_(c, "sensor session started");
            session_reply(c, session, 0x27, start, now);
        }
        break;
    case 0x28: /* stop sensor: stopTime(4) */
        if (n >= 5) {
            st->session_start = 0;
            save(c);
            logf_(c, "sensor session stopped");
            session_reply(c, session, 0x29, get32(p + 1), now);
        }
        break;
    case 0x34: /* calibrate: glucose(2) time(4) */
        if (n >= 7) {
            st->calibration = (uint16_t)(p[1] | p[2] << 8);
            st->calibration_t = (uint32_t)(st->activated + get32(p + 3));
            if (st->calibration >= 40 && st->calibration <= 400) {
                st->glucose = st->calibration;
            }
            save(c);
            logf_(c, "calibrated to %u mg/dL", st->calibration);
            m[0] = 0x35;
            m[1] = 0x00;
            m[2] = 0x00;
            send(c, session, CGM_CHR_CONTROL, m, with_crc(m, 3));
        }
        break;
    case 0x42: /* reset */
        st->activated = now;
        st->session_start = 0;
        st->next_n = 0;
        st->n_hist = 0;
        st->delivered = 0;
        clear_delivery_history(c);
        save(c);
        logf_(c, "transmitter reset");
        m[0] = 0x43;
        m[1] = 0x00;
        send(c, session, CGM_CHR_CONTROL, m, with_crc(m, 2));
        break;
    case 0x4a: { /* version: firmware 1.6.5.25, expiry 112 days */
        static const uint8_t tmpl[17] = {0x4b, 0x00, 0x01, 0x06, 0x05, 0x19, 0xdf, 0x29, 0x00,
                                         0x00, 0x51, 0x00, 0x03, 0x70, 0x00, 0xf0, 0x00};
        memcpy(m, tmpl, 17);
        send(c, session, CGM_CHR_CONTROL, m, with_crc(m, 17));
        break;
    }
    case 0x50: { /* GlucoseBackfillTx: id at 3, start(4) at 4, end(4) at 8 */
        uint8_t id = n >= 4 ? p[3] : 0;
        uint32_t start = n >= 12 ? get32(p + 4) : 0;
        uint32_t end = n >= 12 ? get32(p + 8) : 0xffffffffu;
        int first = -1, last = -1;
        for (int i = 0; i < st->n_hist; i++) {
            uint32_t t = tx_time(st, st->hist[i].t);
            if (t >= start && t <= end) {
                if (first < 0) {
                    first = i;
                }
                last = i;
            }
        }
        logf_(c, "backfill requested by the app: %d readings", first < 0 ? 0 : last - first + 1);
        if (first < 0) {
            /* An empty answer still closes the request. */
            memset(m, 0, sizeof m);
            m[0] = 0x51;
            m[1] = 0x00;
            m[2] = 0x01;
            m[3] = id;
            put32(m + 4, start);
            put32(m + 8, end);
            send(c, session, CGM_CHR_CONTROL, m, with_crc(m, 18));
            break;
        }
        send_g6_backfill_range(c, session, first, last, id);
        break;
    }
    case 0x09: { /* DisconnectTx: the app ends this session */
        cgm_session_state_t *s = session_at(c, session);
        /* An app that sends this before it has been given a reading is still
         * pairing; dropping the link there puts it back into bonding forever.
         * Keep the link until the app has a reading, then honour DisconnectTx:
         * the radio drains any queued frame first, and a G6 advertises while
         * unconnected, so the next reading is collected on a fresh session. */
        if (!s || !s->sent_glucose) {
            logf_(c, "g6 opcode 0x09 before a reading: keeping the link to finish pairing");
            break;
        }
        logf_(c, "g6 opcode 0x09: app ended the session");
        c->io->disconnect(c->io->ctx, session);
        break;
    }
    default:
        logf_(c, "ignoring control opcode 0x%02x", p[0]);
        break;
    }
}

void cgm_on_write(cgm_t *c, cgm_session_t session, cgm_chr_t chr, const uint8_t *p, size_t n)
{
    cgm_session_state_t *s = session_at(c, session);
    if (!s) {
        logf_(c, "ignoring write for unknown session %u", (unsigned)session);
        return;
    }
    int64_t now = c->io->now(c->io->ctx);
    s->last_activity = now;
    if (n == 0) {
        return;
    }
    if (chr == CGM_CHR_AUTH) {
        if (p[0] == 0x01 && n >= 9 && c->st.kind == CGM_G6) {
            /* AuthRequestTx: 01 token(8) 02. Answer 03 hash(token) challenge. */
            s->auth_value[0] = 0x03;
            cgm_g6_hash(c->st.id, p + 1, s->auth_value + 1);
            dash_random(s->challenge, 8);
            memcpy(s->auth_value + 9, s->challenge, 8);
            s->auth_len = 17;
        } else if (p[0] == 0x04 && n >= 9 && s->auth_len == 17) {
            /* AuthChallengeTx: 04 hash(challenge). */
            uint8_t want[8];
            cgm_g6_hash(c->st.id, s->challenge, want);
            s->authed = memcmp(want, p + 1, 8) == 0;
            s->auth_value[0] = 0x05;
            s->auth_value[1] = s->authed ? 0x01 : 0x02;
            s->auth_value[2] = s->authed ? 0x01 : 0x02; /* bonded: no Bluetooth pairing is asked for */
            s->auth_len = 3;
            logf_(c, "app %s the G6 login", s->authed ? "passed" : "failed");
            /* No reading here: the app subscribes to CONTROL after the login,
             * so the push belongs in cgm_on_subscribe. */
        }
        /* 06 keep-alive and 07 bond request need no answer. */
        return;
    }
    if (chr != CGM_CHR_CONTROL) {
        return;
    }
    if (c->st.kind == CGM_G6) {
        g6_control(c, session, p, n, now);
    } else if (p[0] == 0x52) {
        /* Extended version, as a 10-day G7: session 10.5 days, warm-up 27 minutes. */
        static const uint8_t v[15] = {0x52, 0x00, 0xc0, 0xd7, 0x0d, 0x00, 0x54, 0x06,
                                      0x00, 0x02, 0x04, 0x04, 0xff, 0x0c, 0x00};
        send(c, session, CGM_CHR_CONTROL, v, sizeof v);
    }
}

size_t cgm_on_read(cgm_t *c, cgm_session_t session, cgm_chr_t chr, uint8_t *out, size_t cap)
{
    cgm_session_state_t *s = session_at(c, session);
    if (!s) {
        return 0;
    }
    s->last_activity = c->io->now(c->io->ctx);
    if (chr == CGM_CHR_AUTH && s->auth_len && s->auth_len <= cap) {
        memcpy(out, s->auth_value, s->auth_len);
        return s->auth_len;
    }
    return 0;
}

/* ---- the control panel ---------------------------------------------------------- */

void cgm_set_glucose(cgm_t *c, uint16_t glucose, int8_t trend)
{
    c->st.pattern = CGM_PATTERN_FIXED;
    c->st.glucose = glucose < 40 ? 40 : glucose > 400 ? 400 : glucose;
    c->st.trend = trend;
    save(c);
}

void cgm_set_state(cgm_t *c, uint8_t forced_state)
{
    c->st.forced_state = forced_state;
    save(c);
}

void cgm_set_signal_loss(cgm_t *c, bool on)
{
    c->st.signal_loss = on;
    save(c);
    if (on) {
        for (int i = 0; i < CGM_MAX_SESSIONS; i++) {
            if (c->sessions[i].connected) {
                c->io->disconnect(c->io->ctx, (cgm_session_t)i);
            }
        }
    }
}

void cgm_skip_warmup(cgm_t *c)
{
    cgm_state_t *st = &c->st;
    int64_t now = c->io->now(c->io->ctx);
    int64_t left = st->warmup_done - now;
    if (left <= 0) {
        return;
    }
    /* Move the start back, so that the app agrees the warm-up is over. */
    if (st->kind == CGM_G7) {
        st->activated -= left;
        st->session_start = st->activated;
    } else {
        st->session_start -= left;
        if (st->session_start < st->activated) {
            st->activated = st->session_start;
        }
    }
    st->warmup_done = now;
    /* Readings are due from the new start: the next one now. */
    st->next_n = (uint32_t)((now - st->activated) / CGM_READING_INTERVAL);
    save(c);
}

void cgm_new_sensor(cgm_t *c, cgm_kind_t kind, const char *id)
{
    for (int i = 0; i < CGM_MAX_SESSIONS; i++) {
        if (c->sessions[i].connected) {
            c->io->disconnect(c->io->ctx, (cgm_session_t)i);
        }
    }
    advertise(c, false);
    cgm_state_t fresh;
    cgm_state_fresh(&fresh, kind, id, c->io->now(c->io->ctx));
    fresh.glucose = c->st.glucose ? c->st.glucose : 120;
    fresh.trend = c->st.trend;
    /* The person wearing it carries on: same pattern, same body. */
    fresh.pattern = c->st.pattern;
    fresh.insulin_source = c->st.insulin_source;
    fresh.isf = c->st.isf;
    fresh.icr = c->st.icr;
    fresh.basal_need = c->st.basal_need;
    fresh.person = c->st.person;
    fresh.amplitude = c->st.amplitude;
    fresh.period_minutes = c->st.period_minutes;
    c->st = fresh;
    clear_delivery_history(c);
    save(c);
    char name[16];
    cgm_name(&c->st, name);
    logf_(c, "new %s: %s", kind == CGM_G6 ? "G6 transmitter" : kind == CGM_G7 ? "G7 sensor" : "(off)", name);
}

void cgm_set_pattern(cgm_t *c, const cgm_pattern_t *p)
{
    cgm_state_t *st = &c->st;
    st->pattern = p->kind == CGM_PATTERN_SINE ? CGM_PATTERN_SINE
                  : p->kind == CGM_PATTERN_WALK ? CGM_PATTERN_WALK
                                                 : CGM_PATTERN_FIXED;
    st->glucose = p->value < 40 ? 40 : p->value > 400 ? 400 : p->value;
    st->trend = p->trend;
    st->amplitude = p->amplitude > 180 ? 180 : p->amplitude;
    st->period_minutes = p->period_minutes < 10 ? 10 : p->period_minutes > 1440 ? 1440 : p->period_minutes;
    save(c);
}

void cgm_set_person(cgm_t *c, cgm_insulin_t source, uint16_t isf, uint16_t icr, uint16_t basal_need)
{
    cgm_state_t *st = &c->st;
    bool start = st->pattern != CGM_PATTERN_PERSON || st->person.minute == 0 || st->insulin_source != source;
    st->insulin_source = (uint8_t)source;
    st->isf = isf ? isf : st->isf ? st->isf : 45;
    st->icr = icr ? icr : st->icr ? st->icr : 10;
    st->basal_need = basal_need ? basal_need : st->basal_need ? st->basal_need : 80;
    st->pattern = CGM_PATTERN_PERSON;
    if (start) {
        person_reset(c, c->io->now(c->io->ctx));
        logf_(c, "virtual person: %s insulin, ISF %u, ICR %u, basal %u.%02u U/h",
              source == CGM_INSULIN_PUMP ? "the pump's" : "their own", st->isf, st->icr, st->basal_need / 100,
              st->basal_need % 100);
    }
    save(c);
}

void cgm_eat(cgm_t *c, uint16_t grams)
{
    if (c->st.pattern == CGM_PATTERN_PERSON) {
        person_eat(c, c->io->now(c->io->ctx), grams);
        logf_(c, "the person eats %u g", grams);
        save(c);
    }
}

void cgm_exercise(cgm_t *c, uint16_t minutes)
{
    if (c->st.pattern == CGM_PATTERN_PERSON) {
        person_exercise(c, c->io->now(c->io->ctx), minutes);
        logf_(c, "the person exercises for %u min", minutes);
        save(c);
    }
}

int cgm_person_glucose(const cgm_t *c)
{
    return (int)(c->st.person.g_milli / 1000);
}

size_t cgm_status_json(cgm_t *c, char *out, size_t cap)
{
    const cgm_state_t *st = &c->st;
    int64_t now = c->io->now(c->io->ctx);
    char name[16];
    cgm_name(st, name);
    const cgm_reading_t *r = latest(c);
    int n = snprintf(out, cap,
                     "{\"kind\":%u,\"name\":\"%s\",\"id\":\"%s\",\"glucose\":%u,\"trend\":%d,\"state\":%u,"
                     "\"forcedState\":%u,\"signalLoss\":%s,\"activated\":%lld,\"sessionStart\":%lld,"
                     "\"warmupDone\":%lld,\"now\":%lld,\"connected\":%s,\"advertising\":%s,\"nextReading\":%lld",
                     st->kind, name, st->id, st->glucose, st->trend, cgm_current_state(st, now), st->forced_state,
                     st->signal_loss ? "true" : "false", (long long)st->activated, (long long)st->session_start,
                     (long long)st->warmup_done, (long long)now, any_connected(c) ? "true" : "false",
                     c->advertising ? "true" : "false",
                     (long long)(st->activated + (int64_t)st->next_n * CGM_READING_INTERVAL));
    if (n < 0 || (size_t)n >= cap) {
        return 0;
    }
    uint16_t next_g = 0, last_g = 0;
    uint8_t next_f = 0xff, last_f = 0xff;
    bool person = st->pattern == CGM_PATTERN_PERSON;
    int64_t next_t = person ? person_next_meal(c, now, &next_g, &next_f) : 0;
    int64_t last_t = person ? person_last_meal(c, now, &last_g, &last_f) : 0;
    int k = snprintf(out + n, cap - n,
                     ",\"pattern\":\"%s\",\"amplitude\":%u,\"periodMinutes\":%u,"
                     "\"insulinSource\":%u,\"isf\":%u,\"icr\":%u,\"basalNeed\":%u,"
                     "\"trueGlucose\":%d,\"iob\":%.2f,\"nextMeal\":%lld,\"nextMealGrams\":%u,\"nextFood\":\"%s\","
                     "\"lastMeal\":%lld,\"lastMealGrams\":%u,\"lastFood\":\"%s\"",
                     cgm_pattern_name((cgm_pattern_kind_t)st->pattern), st->amplitude, st->period_minutes,
                     st->insulin_source, st->isf, st->icr, st->basal_need,
                     person ? cgm_person_glucose(c) : 0, person ? (double)person_insulin_acting(c) : 0.0,
                     (long long)next_t, next_g, next_t ? cgm_food_name(next_f) : "", (long long)last_t, last_g,
                     last_t ? cgm_food_name(last_f) : "");
    if (k < 0 || (size_t)(n + k) >= cap) {
        return 0;
    }
    n += k;
    char tr[5] = "null";
    if (r && r->trend != NO_TREND) {
        snprintf(tr, sizeof tr, "%d", r->trend);
    }
    int m = r ? snprintf(out + n, cap - n,
                         ",\"last\":{\"seq\":%u,\"t\":%u,\"glucose\":%u,\"state\":%u,\"trend\":%s,\"sent\":%s}}",
                         (unsigned)r->seq, (unsigned)r->t, r->glucose, r->state, tr,
                         r->seq <= st->delivered ? "true" : "false")
              : snprintf(out + n, cap - n, ",\"last\":null}");
    if (m < 0 || (size_t)(n + m) >= cap) {
        return 0;
    }
    return (size_t)(n + m);
}
