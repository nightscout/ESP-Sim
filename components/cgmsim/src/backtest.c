/*
 * Backtest for the simulated CGM: a stored trace replayed reading by reading,
 * so that two builds of an AID algorithm see the same glucose, and a log of
 * what the pump did against the trace position, to compare them by.
 *
 * TEST EQUIPMENT ONLY: the trace is whatever was uploaded. Never let an app
 * that controls a real pump or pod read it.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "backtest.h"
#include "cgm.h"

static void save(cgm_t *c)
{
    if (c->io->save) {
        c->io->save(c->io->ctx, &c->st);
    }
}

static int64_t now_s(const cgm_t *c)
{
    return c->io->now(c->io->ctx);
}

static uint16_t trace_at(const cgm_t *c, uint32_t i)
{
    if (!c->io->trace_at || i >= c->st.bt.len) {
        return 0;
    }
    uint16_t v = c->io->trace_at(c->io->ctx, i);
    return v == 0xffff ? 0 : v;
}

static int16_t clamp16(int64_t v)
{
    return (int16_t)(v > 32767 ? 32767 : v < -1 ? -1 : v);
}

static void log_line(cgm_t *c, uint8_t kind, uint32_t pos, int64_t a, int64_t b)
{
    if (!c->io->backtest_log) {
        return;
    }
    cgm_log_t e = {
        .kind = kind,
        .run = (uint16_t)c->st.bt.run,
        .t = (uint32_t)now_s(c),
        .pos = pos,
        .a = clamp16(a),
        .b = clamp16(b),
    };
    c->io->backtest_log(c->io->ctx, &e);
}

/* The reading slot now: the time of the reading the sensor made last or is
 * making (activated + n × 5 minutes). */
static int64_t slot_now(const cgm_t *c)
{
    const cgm_state_t *st = &c->st;
    int64_t now = now_s(c);
    int64_t n = now > st->activated ? (now - st->activated) / CGM_READING_INTERVAL : 0;
    return st->activated + n * CGM_READING_INTERVAL;
}

void cgm_backtest_load(cgm_t *c, uint32_t len, uint32_t sum, uint16_t first)
{
    cgm_backtest_t *bt = &c->st.bt;
    uint8_t end = bt->end;
    uint32_t run = bt->run;
    memset(bt, 0, sizeof *bt);
    bt->len = len;
    bt->sum = sum;
    bt->end = end;
    bt->run = len ? 0 : run;
    bt->hold = first;
    bt->play = CGM_BT_CUED;
    c->bt_seen = false;
    save(c);
}

void cgm_backtest_start(cgm_t *c)
{
    cgm_backtest_t *bt = &c->st.bt;
    if (!bt->len || bt->play == CGM_BT_PLAYING) {
        return;
    }
    if (bt->play == CGM_BT_ENDED) {
        cgm_backtest_restart(c);
        return;
    }
    bool fresh = bt->pos == 0 && bt->play == CGM_BT_CUED;
    if (fresh) {
        bt->run++;
        bt->loops = 0;
    }
    bt->play = CGM_BT_PLAYING;
    bt->slot_t = slot_now(c); /* this slot's reading is out: the trace starts with the next */
    c->bt_seen = false;        /* the pump's state now opens the run's log */
    log_line(c, CGM_LOG_START, bt->pos, fresh, -1);
    save(c);
}

void cgm_backtest_pause(cgm_t *c)
{
    cgm_backtest_t *bt = &c->st.bt;
    if (bt->play != CGM_BT_PLAYING) {
        return;
    }
    bt->play = CGM_BT_PAUSED;
    log_line(c, CGM_LOG_PAUSE, bt->cur, -1, -1);
    save(c);
}

void cgm_backtest_restart(cgm_t *c)
{
    cgm_backtest_t *bt = &c->st.bt;
    if (!bt->len) {
        return;
    }
    bt->pos = 0;
    bt->cur = 0;
    bt->play = CGM_BT_CUED;
    bt->hold = 0;
    for (uint32_t i = 0; i < bt->len && !bt->hold; i++) {
        bt->hold = trace_at(c, i);
    }
    cgm_backtest_start(c);
}

bool cgm_backtest_cue(cgm_t *c, uint32_t pos)
{
    cgm_backtest_t *bt = &c->st.bt;
    if (pos >= bt->len) {
        return false;
    }
    if (bt->play == CGM_BT_PLAYING) {
        cgm_backtest_pause(c);
    }
    bt->pos = pos;
    if (bt->play == CGM_BT_ENDED || bt->play == CGM_BT_CUED) {
        bt->play = pos ? CGM_BT_PAUSED : CGM_BT_CUED;
    }
    /* Held meanwhile: the reading before pos, or the first one after it. */
    uint16_t h = 0;
    for (uint32_t i = pos; i-- > 0 && !h;) {
        h = trace_at(c, i);
    }
    for (uint32_t i = pos; i < bt->len && !h; i++) {
        h = trace_at(c, i);
    }
    if (h) {
        bt->hold = h;
    }
    save(c);
    return true;
}

void cgm_backtest_set_end(cgm_t *c, cgm_bt_end_t end)
{
    c->st.bt.end = (uint8_t)end;
    save(c);
}

int64_t cgm_backtest_next(const cgm_t *c)
{
    const cgm_backtest_t *bt = &c->st.bt;
    if (c->st.pattern != CGM_PATTERN_BACKTEST || bt->play != CGM_BT_PLAYING) {
        return 0;
    }
    return slot_now(c) + CGM_READING_INTERVAL;
}

/* The Dexcom trend at trace reading i: the change over 15 minutes, tenths of
 * mg/dL a minute, from the trace itself so that it is the same every run. */
static int8_t trace_trend(const cgm_t *c, uint32_t i, uint16_t g)
{
    const cgm_backtest_t *bt = &c->st.bt;
    uint16_t before = 0;
    if (i >= 3) {
        before = trace_at(c, i - 3);
    } else if (bt->loops && bt->len > 3) {
        before = trace_at(c, bt->len + i - 3);
    }
    if (!before) {
        return 0;
    }
    int t = (g - before) * 10 / 15;
    return (int8_t)(t > 80 ? 80 : t < -80 ? -80 : t);
}

bool backtest_reading(cgm_t *c, int64_t slot_t, bool sensor_ok, uint16_t *glucose, int8_t *trend)
{
    cgm_backtest_t *bt = &c->st.bt;
    *trend = 0;
    if (!bt->len) {
        *glucose = c->st.glucose; /* no trace yet: the panel's value, flat */
        return true;
    }
    if (bt->play == CGM_BT_PLAYING && sensor_ok) {
        if (slot_t == bt->slot_t) {
            return false; /* this slot played already (a missing reading) */
        }
        bt->slot_t = slot_t;
        uint32_t i = bt->pos++;
        uint16_t g = trace_at(c, i);
        bt->cur = i;
        int64_t delivered = -1;
        if (c->bt_seen && c->bt_pump.delivered >= 0) {
            delivered = c->bt_pump.delivered - c->bt_delivered;
            c->bt_delivered = c->bt_pump.delivered;
        }
        log_line(c, CGM_LOG_READING, i, g, delivered);
        if (bt->pos >= bt->len) {
            if (bt->end == CGM_BT_END_LOOP) {
                bt->pos = 0;
                bt->loops++;
                log_line(c, CGM_LOG_LOOP, i, bt->loops, -1);
            } else {
                bt->play = CGM_BT_ENDED;
                log_line(c, CGM_LOG_END, i, -1, -1);
            }
        }
        if (!g) {
            return false;
        }
        bt->hold = g;
        *glucose = g;
        *trend = trace_trend(c, i, g);
        return true;
    }
    if (bt->play == CGM_BT_ENDED && bt->end == CGM_BT_END_STOP) {
        return false;
    }
    *glucose = bt->hold ? bt->hold : c->st.glucose;
    return true;
}

/* Once a second while playing: what the pump changed since the last look. */
void backtest_watch_pump(cgm_t *c)
{
    cgm_backtest_t *bt = &c->st.bt;
    if (c->st.pattern != CGM_PATTERN_BACKTEST || bt->play != CGM_BT_PLAYING || !c->io->pump_now) {
        return;
    }
    cgm_pump_t p;
    if (!c->io->pump_now(c->io->ctx, &p)) {
        return;
    }
    int64_t now = now_s(c);
    int64_t left = p.temp && p.temp_end > now ? (p.temp_end - now + 59) / 60 : p.temp ? -1 : 0;
    if (!c->bt_seen) {
        /* The baseline: the basal as the run starts (or the board restarts). */
        log_line(c, CGM_LOG_BASAL, bt->cur, p.rate, left);
        c->bt_delivered = p.delivered;
        c->bt_pump = p;
        c->bt_seen = true;
        return;
    }
    if (p.rate != c->bt_pump.rate || p.temp != c->bt_pump.temp ||
        (p.temp && p.temp_end != c->bt_pump.temp_end)) {
        log_line(c, CGM_LOG_BASAL, bt->cur, p.rate, left);
    }
    if (p.bolus_id != c->bt_pump.bolus_id && p.bolus_id) {
        log_line(c, CGM_LOG_BOLUS, bt->cur, p.bolus, -1);
    }
    if (c->bt_delivered < 0) {
        c->bt_delivered = p.delivered;
    }
    c->bt_pump = p;
}

/* ---- the uploaded trace ---------------------------------------------------------- */

int cgm_trace_parse(const char *text, size_t n, uint16_t *out, size_t cap, const char **err)
{
    size_t i = 0;
    int count = 0;
    const char *bad = NULL;
    while (i <= n) {
        /* One field: up to a separator or the end. */
        size_t s = i;
        while (i < n && text[i] != ',' && text[i] != '\n' && text[i] != '\r' && text[i] != ' ' && text[i] != '\t') {
            i++;
        }
        size_t len = i - s;
        bool sep_comma = i < n && text[i] == ',';
        bool last = i >= n;
        i++;
        if (len == 0 && !sep_comma && !(last && s > 0 && text[s - 1] == ',')) {
            continue; /* runs of white space are one separator */
        }
        long v = 0;
        if (len == 0 || (len == 1 && text[s] == '-') || (len == 4 && memcmp(text + s, "null", 4) == 0)) {
            v = 0;
        } else {
            char buf[12];
            if (len >= sizeof buf) {
                bad = "a reading is a whole number of mg/dL";
                break;
            }
            memcpy(buf, text + s, len);
            buf[len] = 0;
            char *e;
            v = strtol(buf, &e, 10);
            if (*e || v < 0 || v > 1000) {
                bad = "a reading is a whole number of mg/dL, 0 or empty for a missing one";
                break;
            }
            if (v) {
                v = v < 40 ? 40 : v > 400 ? 400 : v;
            }
        }
        if ((size_t)count >= cap) {
            bad = "too many readings";
            break;
        }
        out[count++] = (uint16_t)v;
    }
    if (bad) {
        if (err) {
            *err = bad;
        }
        return -1;
    }
    return count;
}

uint32_t cgm_trace_sum(uint32_t sum, const uint16_t *v, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        sum = (sum ^ (v[i] & 0xff)) * 16777619u;
        sum = (sum ^ (v[i] >> 8)) * 16777619u;
    }
    return sum;
}

/* ---- the log as CSV ---------------------------------------------------------------- */

size_t cgm_log_csv_header(char *out, size_t cap)
{
    int n = snprintf(out, cap, "run,position,trace_minute,time_utc,event,glucose_mgdl,insulin_u,basal_u_per_h,"
                               "temp_minutes_left,bolus_u\n");
    return n < 0 || (size_t)n >= cap ? 0 : (size_t)n;
}

/* Hundredths as units ("1.25"), or "" when not known. */
static const char *units(int v, char *buf)
{
    if (v < 0) {
        buf[0] = 0;
    } else {
        snprintf(buf, 12, "%d.%02d", v / 100, v % 100);
    }
    return buf;
}

size_t cgm_log_csv(const cgm_log_t *e, char *out, size_t cap)
{
    char when[24], u1[12], u2[12], num[12];
    time_t t = (time_t)e->t;
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(when, sizeof when, "%Y-%m-%dT%H:%M:%SZ", &tm);
    int n;
    const unsigned run = e->run, pos = e->pos;
    switch (e->kind) {
    case CGM_LOG_READING:
        n = snprintf(out, cap, "%u,%u,%u,%s,%s,%s,%s,,,\n", run, pos, pos * 5, when, e->a ? "reading" : "missing",
                     e->a ? (snprintf(num, sizeof num, "%d", e->a), num) : "", units(e->b, u1));
        break;
    case CGM_LOG_BASAL:
        n = snprintf(out, cap, "%u,%u,,%s,%s,,,%s,%s,\n", run, pos, when,
                     e->b > 0 || e->b == -1 ? "temp_basal" : "basal", units(e->a, u1),
                     e->b >= 0 ? (snprintf(num, sizeof num, "%d", e->b), num) : "");
        break;
    case CGM_LOG_BOLUS:
        n = snprintf(out, cap, "%u,%u,,%s,bolus,,,,,%s\n", run, pos, when, units(e->a, u2));
        break;
    case CGM_LOG_START:
        n = snprintf(out, cap, "%u,%u,,%s,%s,,,,,\n", run, pos, when, e->a == 1 ? "start" : "resume");
        break;
    case CGM_LOG_PAUSE:
        n = snprintf(out, cap, "%u,%u,,%s,pause,,,,,\n", run, pos, when);
        break;
    case CGM_LOG_END:
        n = snprintf(out, cap, "%u,%u,,%s,end,,,,,\n", run, pos, when);
        break;
    case CGM_LOG_LOOP:
        n = snprintf(out, cap, "%u,%u,,%s,loop,,,,,\n", run, pos, when);
        break;
    default:
        return 0;
    }
    return n < 0 || (size_t)n >= cap ? 0 : (size_t)n;
}

/* ---- the log's storage ------------------------------------------------------------------ */

#define LINE ((uint32_t)sizeof(cgm_log_t))

void cgm_logstore_open(cgm_logstore_t *ls, const cgm_flash_t *f, uint32_t base, uint32_t size, uint8_t epoch)
{
    memset(ls, 0, sizeof *ls);
    ls->f = f;
    ls->base = base;
    ls->size = size - size % CGM_FLASH_SECTOR;
    ls->epoch = epoch;
    /* The log runs up to the first line that is erased or from another epoch. */
    cgm_log_t e;
    while (ls->count < ls->size / LINE && f->read(f->ctx, base + ls->count * LINE, &e, LINE) && e.kind != 0xff &&
           e.epoch == epoch) {
        ls->count++;
    }
    ls->full = ls->count >= ls->size / LINE;
}

void cgm_logstore_clear(cgm_logstore_t *ls, uint8_t epoch)
{
    ls->epoch = epoch;
    ls->count = 0;
    ls->full = ls->size < LINE;
    if (ls->size) {
        ls->f->erase(ls->f->ctx, ls->base); /* the rest is erased as the log reaches it */
    }
}

bool cgm_logstore_append(cgm_logstore_t *ls, const cgm_log_t *e)
{
    if (ls->count >= ls->size / LINE) {
        ls->full = true;
        return false;
    }
    uint32_t off = ls->count * LINE;
    if (off % CGM_FLASH_SECTOR == 0 && off && !ls->f->erase(ls->f->ctx, ls->base + off)) {
        return false;
    }
    cgm_log_t w = *e;
    w.epoch = ls->epoch;
    if (!ls->f->write(ls->f->ctx, ls->base + off, &w, LINE)) {
        return false;
    }
    ls->count++;
    ls->full = ls->count >= ls->size / LINE;
    return true;
}

bool cgm_logstore_get(const cgm_logstore_t *ls, uint32_t i, cgm_log_t *e)
{
    return i < ls->count && ls->f->read(ls->f->ctx, ls->base + i * LINE, e, LINE);
}
