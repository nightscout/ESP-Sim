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
/* G6 slots. A real transmitter serves the phone app (slot 0x02) and a
 * receiver, pump or watch (slot 0x01) in the same window: the second gets in
 * as the first lets go (rockit's CGM Read on a real G6, 2026-10). So it
 * advertises again after a link ends while the other slot has not had the
 * reading, for a short while if that is only the receiver's (there may be
 * none), and hangs up at once on a slot that already has it. */
#define SLOT_RX 0x01
#define SLOT_APP 0x02
#define RX_GRACE 10 /* seconds of advertising for the receiver slot after a link ends */
#define NO_TREND 0x7f

static const char ID_CHARS[] = "0123456789ABCDEFGHJKLMNPQRSTUVWXYZ";

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

static void send(cgm_t *c, cgm_chr_t chr, const uint8_t *p, size_t n)
{
    if (c->connected) {
        c->io->send(c->io->ctx, chr, p, n);
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
    }
    if (r.state == CGM_STATE_OK) {
        r.glucose = st->glucose;
        if (st->pattern == CGM_PATTERN_MANUAL) {
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
        c->window_end = now + WINDOW;
        c->rx_until = c->window_end; /* until a link ends: either slot may come first */
    }
    const cgm_reading_t *r = latest(c);
    bool app_due = r && r->seq != st->delivered;
    bool rx_due = r && st->kind == CGM_G6 && r->seq != st->delivered_rx && now < c->rx_until;
    if (!c->connected) {
        advertise(c, (app_due || rx_due) && !st->signal_loss && now < c->window_end);
    } else if ((c->sent_glucose && now - c->last_activity >= IDLE_END) || now - c->connected_at >= CONN_MAX) {
        logf_(c, "hanging up");
        c->io->disconnect(c->io->ctx);
    }
}

void cgm_on_connect(cgm_t *c)
{
    int64_t now = c->io->now(c->io->ctx);
    c->connected = true;
    c->advertising = false; /* a connection ends the advertising */
    c->sent_glucose = false;
    c->authed = false;
    c->auth_len = 0;
    c->connected_at = c->last_activity = now;
    c->slot = SLOT_APP; /* until an AuthRequestTx names another */
    c->slot_named = false;
    c->pushed_seq = 0;
    c->prev_delivered = c->st.delivered;
    logf_(c, "phone connected");
}

static void delivered(cgm_t *c, const cgm_reading_t *r);

void cgm_on_disconnect(cgm_t *c)
{
    c->connected = false;
    /* A reading pushed to a device that never said its slot: a passive
     * client, sharing the phone app's session. */
    const cgm_reading_t *r = latest(c);
    if (c->pushed_seq && !c->slot_named && r && r->seq == c->pushed_seq) {
        c->slot = SLOT_APP;
        delivered(c, r);
    }
    c->pushed_seq = 0;
    if (c->st.kind == CGM_G6) {
        /* The receiver slot gets a short chance now, if it has not had the reading. */
        int64_t until = c->io->now(c->io->ctx) + RX_GRACE;
        if (c->rx_until > until || c->slot == SLOT_APP) {
            c->rx_until = until;
        }
    }
    logf_(c, "phone disconnected");
}

static void delivered(cgm_t *c, const cgm_reading_t *r)
{
    c->sent_glucose = true;
    uint32_t *d = c->slot == SLOT_RX ? &c->st.delivered_rx : &c->st.delivered;
    if (*d != r->seq) {
        *d = r->seq;
        save(c);
    }
}

/* Passive mode: what the phone would see the Dexcom app ask for. */
static void push_glucose(cgm_t *c)
{
    const cgm_reading_t *r = latest(c);
    if (!r || c->sent_glucose) {
        return;
    }
    int64_t now = c->io->now(c->io->ctx);
    uint8_t m[19];
    if (c->st.kind == CGM_G7) {
        send(c, CGM_CHR_CONTROL, m, cgm_g7_glucose_msg(&c->st, r, now, m));
    } else {
        send(c, CGM_CHR_CONTROL, m, cgm_g6_time_msg(&c->st, now, m));
        send(c, CGM_CHR_CONTROL, m, cgm_g6_glucose_msg(&c->st, r, 0x4f, m));
    }
    if (c->st.kind == CGM_G6 && !c->slot_named) {
        /* Whose reading it is waits for the login (a device that subscribes
         * first may log in on the receiver slot): the phone app's slot must
         * not lose its turn to it. */
        c->sent_glucose = true;
        c->pushed_seq = r->seq;
        return;
    }
    delivered(c, r);
}

/* The readings the phone missed, before the one it was just sent. */
static void send_backfill(cgm_t *c)
{
    const cgm_reading_t *r = latest(c);
    if (!r || !c->sent_glucose) {
        return;
    }
    const cgm_state_t *st = &c->st;
    int first = -1, last = -1;
    for (int i = 0; i < st->n_hist - 1; i++) {
        if (st->hist[i].seq > c->prev_delivered) {
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
            send(c, CGM_CHR_BACKFILL, rec, cgm_g7_backfill_record(st, &st->hist[i], rec));
        }
        static const uint8_t done[2] = {0x59, 0x00};
        send(c, CGM_CHR_CONTROL, done, sizeof done);
        return;
    }
    /* G6: a 4-byte header and 8-byte records, cut into 20-byte frames that
     * start with their number and the backfill's id; then 0x51 on control. */
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
        f[1] = 0; /* the backfill's id */
        memcpy(f + 2, buf + off, k);
        send(c, CGM_CHR_BACKFILL, f, k + 2);
        total += k + 2;
    }
    uint8_t m[20] = {0x51, 0x00, 0x01, 0x00};
    put32(m + 4, tx_time(st, st->hist[first].t));
    put32(m + 8, tx_time(st, st->hist[last].t));
    put32(m + 12, (uint32_t)total);
    put16(m + 16, cgm_crc16(frames, total));
    send(c, CGM_CHR_CONTROL, m, with_crc(m, 18));
}

void cgm_on_subscribe(cgm_t *c, cgm_chr_t chr, bool on)
{
    c->last_activity = c->io->now(c->io->ctx);
    if (!on) {
        return;
    }
    switch (chr) {
    case CGM_CHR_AUTH: {
        /* What a phone sees once the Dexcom app has logged in, bonded. */
        static const uint8_t ok[3] = {0x05, 0x01, 0x01};
        send(c, CGM_CHR_AUTH, ok, sizeof ok);
        break;
    }
    case CGM_CHR_CONTROL:
        if (!c->authed) { /* active mode asks for each message itself */
            push_glucose(c);
        }
        break;
    case CGM_CHR_BACKFILL:
        send_backfill(c);
        break;
    default:
        break;
    }
}

static void session_reply(cgm_t *c, uint8_t op, uint32_t requested, int64_t now)
{
    /* 27|29 status received requested(4) sessionStart(4) transmitterTime(4) crc */
    uint8_t m[17] = {op, 0x00, 0x01};
    put32(m + 3, requested);
    put32(m + 7, c->st.session_start ? tx_time(&c->st, c->st.session_start) : 0xffffffff);
    put32(m + 11, tx_time(&c->st, now));
    send(c, CGM_CHR_CONTROL, m, with_crc(m, 15));
}

static void g6_control(cgm_t *c, const uint8_t *p, size_t n, int64_t now)
{
    cgm_state_t *st = &c->st;
    uint8_t m[20];
    const cgm_reading_t *r = latest(c);
    switch (p[0]) {
    case 0x24: /* time */
        send(c, CGM_CHR_CONTROL, m, cgm_g6_time_msg(st, now, m));
        break;
    case 0x30: /* glucose */
    case 0x4e:
        if (r) {
            send(c, CGM_CHR_CONTROL, m, cgm_g6_glucose_msg(st, r, p[0] == 0x30 ? 0x31 : 0x4f, m));
            delivered(c, r);
        }
        break;
    case 0x32: { /* calibration data: 33 00 … glucose(2) at 11, timestamp(4) at 13 */
        static const uint8_t tmpl[17] = {0x33, 0x00, 0x2b, 0x29, 0x00, 0x90, 0x01, 0x29, 0x00, 0xae, 0x00};
        memcpy(m, tmpl, 17);
        put16(m + 11, st->calibration);
        put32(m + 13, st->calibration ? tx_time(st, st->calibration_t) : 0);
        send(c, CGM_CHR_CONTROL, m, with_crc(m, 17));
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
            session_reply(c, 0x27, start, now);
        }
        break;
    case 0x28: /* stop sensor: stopTime(4) */
        if (n >= 5) {
            st->session_start = 0;
            save(c);
            logf_(c, "sensor session stopped");
            session_reply(c, 0x29, get32(p + 1), now);
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
            send(c, CGM_CHR_CONTROL, m, with_crc(m, 3));
        }
        break;
    case 0x42: /* reset */
        st->activated = now;
        st->session_start = 0;
        st->next_n = 0;
        st->n_hist = 0;
        st->delivered = 0;
        save(c);
        logf_(c, "transmitter reset");
        m[0] = 0x43;
        m[1] = 0x00;
        send(c, CGM_CHR_CONTROL, m, with_crc(m, 2));
        break;
    case 0x4a: { /* version: firmware 1.6.5.25, expiry 112 days */
        static const uint8_t tmpl[17] = {0x4b, 0x00, 0x01, 0x06, 0x05, 0x19, 0xdf, 0x29, 0x00,
                                         0x00, 0x51, 0x00, 0x03, 0x70, 0x00, 0xf0, 0x00};
        memcpy(m, tmpl, 17);
        send(c, CGM_CHR_CONTROL, m, with_crc(m, 17));
        break;
    }
    case 0x09: /* disconnect */
        c->io->disconnect(c->io->ctx);
        break;
    default:
        logf_(c, "ignoring control opcode 0x%02x", p[0]);
        break;
    }
}

void cgm_on_write(cgm_t *c, cgm_chr_t chr, const uint8_t *p, size_t n)
{
    int64_t now = c->io->now(c->io->ctx);
    c->last_activity = now;
    if (n == 0) {
        return;
    }
    if (chr == CGM_CHR_AUTH) {
        if (p[0] == 0x01 && n >= 9 && c->st.kind == CGM_G6) {
            /* AuthRequestTx: 01 token(8) slot. Answer 03 hash(token) challenge. */
            c->slot = n >= 10 && p[9] == SLOT_RX ? SLOT_RX : SLOT_APP;
            c->slot_named = true;
            c->prev_delivered = c->slot == SLOT_RX ? c->st.delivered_rx : c->st.delivered;
            const cgm_reading_t *r = latest(c);
            if (r && r->seq == c->prev_delivered && c->io->now(c->io->ctx) < c->window_end) {
                /* This slot already has this window's reading: the other's turn. */
                logf_(c, "slot %u already has reading %u: hanging up", (unsigned)c->slot, (unsigned)r->seq);
                c->pushed_seq = 0;
                c->io->disconnect(c->io->ctx);
                return;
            }
            if (r && c->pushed_seq == r->seq) {
                delivered(c, r); /* the reading pushed on subscribe: this slot's */
                c->pushed_seq = 0;
            }
            logf_(c, "login on the %s slot", c->slot == SLOT_RX ? "receiver" : "phone app");
            c->auth_value[0] = 0x03;
            cgm_g6_hash(c->st.id, p + 1, c->auth_value + 1);
            dash_random(c->challenge, 8);
            memcpy(c->auth_value + 9, c->challenge, 8);
            c->auth_len = 17;
        } else if (p[0] == 0x04 && n >= 9 && c->auth_len == 17) {
            /* AuthChallengeTx: 04 hash(challenge). */
            uint8_t want[8];
            cgm_g6_hash(c->st.id, c->challenge, want);
            c->authed = memcmp(want, p + 1, 8) == 0;
            c->auth_value[0] = 0x05;
            c->auth_value[1] = c->authed ? 0x01 : 0x02;
            c->auth_value[2] = c->authed ? 0x01 : 0x02; /* bonded: no Bluetooth pairing is asked for */
            c->auth_len = 3;
            logf_(c, "app %s the G6 login", c->authed ? "passed" : "failed");
        }
        /* 06 keep-alive and 07 bond request need no answer. */
        return;
    }
    if (chr != CGM_CHR_CONTROL) {
        return;
    }
    if (c->st.kind == CGM_G6) {
        g6_control(c, p, n, now);
    } else if (p[0] == 0x52) {
        /* Extended version, as a 10-day G7: session 10.5 days, warm-up 27 minutes. */
        static const uint8_t v[15] = {0x52, 0x00, 0xc0, 0xd7, 0x0d, 0x00, 0x54, 0x06,
                                      0x00, 0x02, 0x04, 0x04, 0xff, 0x0c, 0x00};
        send(c, CGM_CHR_CONTROL, v, sizeof v);
    }
}

size_t cgm_on_read(cgm_t *c, cgm_chr_t chr, uint8_t *out, size_t cap)
{
    c->last_activity = c->io->now(c->io->ctx);
    if (chr == CGM_CHR_AUTH && c->auth_len && c->auth_len <= cap) {
        memcpy(out, c->auth_value, c->auth_len);
        return c->auth_len;
    }
    return 0;
}

/* ---- the control panel ---------------------------------------------------------- */

void cgm_set_glucose(cgm_t *c, uint16_t glucose, int8_t trend)
{
    c->st.glucose = glucose < 40 ? 40 : glucose > 400 ? 400 : glucose;
    c->st.trend = trend;
    if (c->st.pattern == CGM_PATTERN_PERSON) {
        /* The virtual person's own glucose moves there and carries on from it
         * (their meals, insulin on board and the rest unchanged): otherwise the
         * next reading would put it straight back. */
        c->st.person.g_milli = (int64_t)c->st.glucose * 1000;
        c->st.person.noise = 0;
        logf_(c, "the person's glucose set to %u mg/dL", (unsigned)c->st.glucose);
    }
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
    if (on && c->connected) {
        c->io->disconnect(c->io->ctx);
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
    if (c->connected) {
        c->io->disconnect(c->io->ctx);
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
    c->st = fresh;
    save(c);
    char name[16];
    cgm_name(&c->st, name);
    logf_(c, "new %s: %s", kind == CGM_G6 ? "G6 transmitter" : kind == CGM_G7 ? "G7 sensor" : "(off)", name);
}

void cgm_set_pattern(cgm_t *c, cgm_pattern_t pattern, cgm_insulin_t source, uint16_t isf, uint16_t icr,
                     uint16_t basal_need)
{
    cgm_state_t *st = &c->st;
    bool start = pattern == CGM_PATTERN_PERSON &&
                 (st->pattern != CGM_PATTERN_PERSON || st->person.minute == 0 || st->insulin_source != source);
    st->insulin_source = (uint8_t)source;
    st->isf = isf ? isf : st->isf ? st->isf : 45;
    st->icr = icr ? icr : st->icr ? st->icr : 10;
    st->basal_need = basal_need ? basal_need : st->basal_need ? st->basal_need : 80;
    st->pattern = (uint8_t)pattern;
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
                     "\"warmupDone\":%lld,\"connected\":%s,\"advertising\":%s,\"nextReading\":%lld,\"now\":%lld",
                     st->kind, name, st->id, st->glucose, st->trend, cgm_current_state(st, now), st->forced_state,
                     st->signal_loss ? "true" : "false", (long long)st->activated, (long long)st->session_start,
                     (long long)st->warmup_done, c->connected ? "true" : "false", c->advertising ? "true" : "false",
                     (long long)(st->activated + (int64_t)st->next_n * CGM_READING_INTERVAL), (long long)now);
    if (n < 0 || (size_t)n >= cap) {
        return 0;
    }
    uint16_t next_g = 0, last_g = 0;
    uint8_t next_f = 0xff, last_f = 0xff;
    bool person = st->pattern == CGM_PATTERN_PERSON;
    int64_t next_t = person ? person_next_meal(c, now, &next_g, &next_f) : 0;
    int64_t last_t = person ? person_last_meal(c, now, &last_g, &last_f) : 0;
    int k = snprintf(out + n, cap - n,
                     ",\"pattern\":%u,\"insulinSource\":%u,\"isf\":%u,\"icr\":%u,\"basalNeed\":%u,"
                     "\"trueGlucose\":%d,\"iob\":%.2f,\"nextMeal\":%lld,\"nextMealGrams\":%u,\"nextFood\":\"%s\","
                     "\"lastMeal\":%lld,\"lastMealGrams\":%u,\"lastFood\":\"%s\"",
                     st->pattern, st->insulin_source, st->isf, st->icr, st->basal_need,
                     person ? cgm_person_glucose(c) : 0, person ? (double)person_insulin_acting(c) : 0.0,
                     (long long)next_t, next_g, next_t ? cgm_food_name(next_f) : "", (long long)last_t, last_g,
                     last_t ? cgm_food_name(last_f) : "");
    if (k < 0 || (size_t)(n + k) >= cap) {
        return 0;
    }
    n += k;
    int m = r ? snprintf(out + n, cap - n,
                         ",\"last\":{\"seq\":%u,\"t\":%u,\"glucose\":%u,\"state\":%u,\"sent\":%s}}", (unsigned)r->seq,
                         (unsigned)r->t, r->glucose, r->state, r->seq <= st->delivered ? "true" : "false")
              : snprintf(out + n, cap - n, ",\"last\":null}");
    if (m < 0 || (size_t)(n + m) >= cap) {
        return 0;
    }
    return (size_t)(n + m);
}
