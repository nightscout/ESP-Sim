/*
 * A virtual person for the simulated CGM: glucose that moves the way a
 * person's does, from a simple model run a minute at a time.
 *
 *   rise   the liver's output, which basal insulin normally balances
 *          (more before dawn), and carbohydrate from meals as it absorbs;
 *   fall   insulin as it acts (the exponential curve oref and Loop use for
 *          rapid-acting insulin: peak at 75 minutes, all acted by 6 hours, so a
 *          dose acts when the AID expects it to), stronger during and after
 *          exercise;
 *   drift  a slow random wander (hormones, stress, everything unmodelled);
 *   sensor noise on top of the true value.
 *
 * The person plans their own day: breakfast, lunch and dinner at varied
 * times, sometimes snacks, each a food from a list with its own carbohydrate
 * and speed (juice is fast; porridge steady; pizza late and long). They treat
 * their own lows with glucose tablets or juice. The insulin is either the person's own (basal,
 * and meal boluses that are sometimes too small, too big, early or late, and
 * the odd correction) or only what the simulated pump delivers, so that an
 * AID's decisions show in the glucose.
 *
 * TEST EQUIPMENT ONLY: this is a toy model for exercising software, not a
 * physiological simulator. Its numbers mean nothing about any real person.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cgm.h"
#include "person.h"

#define INSULIN_PEAK 75.0f /* minutes: the insulin's peak activity */
#define INSULIN_DIA 360.0f /* minutes: all of it has acted (within the 7 hours kept) */

/* ---- randomness ------------------------------------------------------------------- */

static uint32_t rnd(cgm_person_t *p)
{
    uint32_t x = p->rng ? p->rng : 0x9e3779b9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    p->rng = x;
    return x;
}

static float uniform(cgm_person_t *p) /* [0, 1) */
{
    return (float)(rnd(p) >> 8) / 16777216.0f;
}

static float between(cgm_person_t *p, float lo, float hi)
{
    return lo + (hi - lo) * uniform(p);
}

static float gauss(cgm_person_t *p) /* about N(0, 1) */
{
    float s = 0;
    for (int i = 0; i < 6; i++) {
        s += uniform(p);
    }
    return (s - 3.0f) * 1.41421356f;
}

/* ---- curves --------------------------------------------------------------------------- */

/* t/τ² · e^(−t/τ): integrates to 1, peaks at τ. The fraction acting in minute t. */
static float curve(float t, float tau)
{
    if (t <= 0) {
        return 0;
    }
    return t / (tau * tau) * expf(-t / tau);
}

/* Rapid-acting insulin, as oref and LoopKit model it (ExponentialInsulinModel):
 * activity S/τ² · t · (1 − t/D) · e^(−t/τ) over the duration D, with τ and S
 * set by the peak and D so that it peaks there and integrates to 1. */
typedef struct {
    float tau, a, s;
} insulin_model_t;

static insulin_model_t insulin_model(void)
{
    const float tp = INSULIN_PEAK, td = INSULIN_DIA;
    insulin_model_t m;
    m.tau = tp * (1.0f - tp / td) / (1.0f - 2.0f * tp / td);
    m.a = 2.0f * m.tau / td;
    m.s = 1.0f / (1.0f - m.a + (1.0f + m.a) * expf(-td / m.tau));
    return m;
}

/* The fraction of a dose acting in minute t. */
static float insulin_activity(const insulin_model_t *m, float t)
{
    if (t <= 0 || t >= INSULIN_DIA) {
        return 0;
    }
    return m->s / (m->tau * m->tau) * t * (1.0f - t / INSULIN_DIA) * expf(-t / m->tau);
}

/* The fraction of a dose not yet acted at age t. */
static float insulin_remaining(const insulin_model_t *m, float t)
{
    if (t <= 0) {
        return 1;
    }
    if (t >= INSULIN_DIA) {
        return 0;
    }
    float r = 1.0f - m->s * (1.0f - m->a) *
                         ((t * t / (m->tau * INSULIN_DIA * (1.0f - m->a)) - t / m->tau - 1.0f) * expf(-t / m->tau) + 1.0f);
    return r < 0 ? 0 : r > 1 ? 1 : r;
}

static int64_t local_seconds(const cgm_t *c, int64_t t)
{
    return t + (c->io->utc_offset ? c->io->utc_offset(c->io->ctx) : 0);
}

/* Minutes since local midnight. */
static int minute_of_day(const cgm_t *c, int64_t t)
{
    int64_t l = local_seconds(c, t);
    return (int)(((l % 86400) + 86400) % 86400 / 60);
}

static int64_t local_day(const cgm_t *c, int64_t t)
{
    int64_t l = local_seconds(c, t);
    return l >= 0 ? l / 86400 : (l - 86399) / 86400;
}

/* ---- foods ------------------------------------------------------------------------------- */

enum { BREAKFAST, LUNCH, SNACK, DINNER, LATE, HYPO };

static const struct {
    const char *name;
    uint8_t when;
    uint8_t lo, hi;      /* grams of carbohydrate */
    uint16_t absorb;     /* minutes to absorb most of it */
    uint8_t delay;       /* minutes before it starts (fat, protein) */
} FOODS[] = {
    {"porridge with banana", BREAKFAST, 45, 60, 150, 0},
    {"toast and jam", BREAKFAST, 30, 45, 110, 0},
    {"cereal and milk", BREAKFAST, 40, 55, 120, 0},
    {"eggs on toast", BREAKFAST, 20, 30, 160, 15},
    {"yoghurt and granola", BREAKFAST, 30, 45, 140, 0},
    {"pancakes and syrup", BREAKFAST, 55, 75, 130, 10},
    {"sandwich", LUNCH, 40, 55, 150, 0},
    {"sushi", LUNCH, 50, 70, 130, 0},
    {"salad and a bread roll", LUNCH, 25, 35, 150, 10},
    {"burrito", LUNCH, 60, 80, 200, 30},
    {"soup and bread", LUNCH, 35, 50, 140, 0},
    {"burger and fries", LUNCH, 60, 80, 240, 40},
    {"an apple", SNACK, 15, 20, 90, 0},
    {"biscuits", SNACK, 20, 30, 80, 0},
    {"a banana", SNACK, 20, 27, 90, 0},
    {"crisps", SNACK, 15, 20, 120, 10},
    {"a muesli bar", SNACK, 20, 25, 100, 0},
    {"a latte and a muffin", SNACK, 35, 50, 120, 10},
    {"pasta bolognese", DINNER, 70, 90, 210, 20},
    {"pizza", DINNER, 70, 100, 300, 60},
    {"curry and rice", DINNER, 70, 90, 240, 30},
    {"stir-fry with noodles", DINNER, 55, 75, 180, 10},
    {"steak and potatoes", DINNER, 40, 60, 220, 30},
    {"fish and chips", DINNER, 70, 90, 260, 45},
    {"roast chicken and vegetables", DINNER, 30, 45, 200, 25},
    {"ice cream", LATE, 20, 30, 150, 20},
    {"toast", LATE, 15, 25, 100, 0},
    {"hot chocolate", LATE, 20, 25, 90, 0},
    {"glucose tablets", HYPO, 15, 16, 30, 0},
    {"orange juice", HYPO, 15, 20, 40, 0},
};
#define N_FOODS (sizeof FOODS / sizeof FOODS[0])

const char *cgm_food_name(uint8_t food)
{
    return food < N_FOODS ? FOODS[food].name : "a snack";
}

static uint8_t pick(cgm_person_t *p, uint8_t when)
{
    uint8_t list[N_FOODS], n = 0;
    for (uint8_t i = 0; i < N_FOODS; i++) {
        if (FOODS[i].when == when) {
            list[n++] = i;
        }
    }
    return list[rnd(p) % n];
}

static void say(cgm_t *c, const char *msg)
{
    if (c->io->log) {
        c->io->log(c->io->ctx, msg);
    }
}

/* ---- the day's plan --------------------------------------------------------------------- */

static void add_meal(cgm_t *c, int64_t t, uint8_t food, float grams)
{
    cgm_person_t *p = &c->st.person;
    int slot = 0;
    for (int i = 0; i < CGM_MEALS; i++) {
        if (p->meals[i].t < p->meals[slot].t) {
            slot = i; /* replace the oldest */
        }
    }
    cgm_meal_t *m = &p->meals[slot];
    memset(m, 0, sizeof *m);
    m->t = t;
    m->grams = (uint16_t)grams;
    m->food = food;
    m->absorb = food < N_FOODS ? FOODS[food].absorb : (uint16_t)(120 + grams);
    m->delay = food < N_FOODS ? FOODS[food].delay : 0;
    if (food < N_FOODS && FOODS[food].when == HYPO) {
        return; /* nobody boluses for a hypo treatment */
    }
    if (c->st.insulin_source == CGM_INSULIN_SELF && c->st.icr) {
        /* The person boluses for it: usually about right, sometimes not, a
         * quarter-hour early to twenty minutes late. */
        float dose = grams / c->st.icr * between(p, 0.75f, 1.25f);
        m->bolus = (uint16_t)(dose * 100);
        m->bolus_t = t + (int64_t)(between(p, -15, 20) * 60);
    }
}

static void plan_day(cgm_t *c, int64_t day)
{
    cgm_person_t *p = &c->st.person;
    int64_t midnight = day * 86400 - (c->io->utc_offset ? c->io->utc_offset(c->io->ctx) : 0);
    struct {
        uint8_t when;
        float at, spread, chance;
    } menu[] = {
        {BREAKFAST, 7.25f, 1.0f, 0.9f},
        {SNACK, 10.5f, 1.0f, 0.3f},
        {LUNCH, 12.5f, 1.0f, 0.95f},
        {SNACK, 15.5f, 1.5f, 0.5f},
        {DINNER, 18.75f, 1.0f, 1.0f},
        {LATE, 21.5f, 1.0f, 0.3f},
    };
    for (size_t i = 0; i < sizeof menu / sizeof menu[0]; i++) {
        if (uniform(p) >= menu[i].chance) {
            continue;
        }
        uint8_t food = pick(p, menu[i].when);
        float hour = menu[i].at + between(p, -menu[i].spread, menu[i].spread);
        add_meal(c, midnight + (int64_t)(hour * 3600), food, between(p, FOODS[food].lo, FOODS[food].hi));
    }
    if (uniform(p) < 0.35f) { /* exercise, some days, late afternoon */
        p->exercise_t = midnight + (int64_t)(between(p, 16, 19) * 3600);
        p->exercise_min = (uint16_t)between(p, 30, 75);
    }
    p->planned_day = day;
}

/* ---- the model ------------------------------------------------------------------------------- */

static void add_insulin(cgm_person_t *p, int32_t hundredths)
{
    int32_t v = p->ins[0] + hundredths;
    p->ins[0] = (uint16_t)(v > 65535 ? 65535 : v < 0 ? 0 : v);
}

static void step(cgm_t *c, int64_t minute)
{
    cgm_state_t *st = &c->st;
    cgm_person_t *p = &st->person;
    int64_t t = minute * 60;
    float isf = st->isf ? st->isf : 45, icr = st->icr ? st->icr : 10;
    float need = (st->basal_need ? st->basal_need : 80) / 100.0f / 60.0f; /* U a minute */

    int64_t day = local_day(c, t);
    if (day != p->planned_day) {
        plan_day(c, day);
    }

    /* Shift the insulin buckets to this minute's. */
    int64_t bucket = minute / 5;
    if (p->ins_bucket == 0 || bucket - p->ins_bucket > CGM_INSULIN_BUCKETS) {
        if (p->ins_bucket && bucket - p->ins_bucket > CGM_INSULIN_BUCKETS) {
            memset(p->ins, 0, sizeof p->ins);
        }
        p->ins_bucket = bucket;
    }
    while (p->ins_bucket < bucket) {
        memmove(p->ins + 1, p->ins, sizeof p->ins[0] * (CGM_INSULIN_BUCKETS - 1));
        p->ins[0] = 0;
        p->ins_bucket++;
    }

    /* Insulin in. */
    if (st->insulin_source == CGM_INSULIN_PUMP) {
        int64_t total = c->io->pump_insulin ? c->io->pump_insulin(c->io->ctx) : -1;
        if (total >= 0) {
            if (p->pump_seen && total >= p->pump_last) {
                add_insulin(p, (int32_t)(total - p->pump_last));
            }
            p->pump_last = (uint32_t)total;
            p->pump_seen = true;
        }
    } else {
        p->basal_milli += (int32_t)(need * 100000.0f); /* thousandths of a hundredth */
        if (p->basal_milli >= 1000) {
            add_insulin(p, p->basal_milli / 1000);
            p->basal_milli %= 1000;
        }
        for (int i = 0; i < CGM_MEALS; i++) {
            cgm_meal_t *m = &p->meals[i];
            if (m->bolus_t && t >= m->bolus_t) {
                add_insulin(p, m->bolus);
                m->bolus_t = 0;
            }
        }
        /* A correction when they notice a high: not every time, not at once,
         * and not again within two and a half hours. */
        float g = (float)p->g_milli / 1000.0f;
        if (g > 180 && t - p->correction_t > 150 * 60 && uniform(p) < 0.03f) {
            add_insulin(p, (int32_t)((g - 110) / isf * between(p, 0.6f, 1.0f) * 100));
            p->correction_t = t;
        }
    }

    /* Meals as they are eaten, for the log. */
    for (int i = 0; i < CGM_MEALS; i++) {
        cgm_meal_t *m = &p->meals[i];
        if (m->grams && m->t > t - 60 && m->t <= t) {
            char msg[80];
            snprintf(msg, sizeof msg, "the person eats %s (%u g of carbohydrate)", cgm_food_name(m->food), m->grams);
            say(c, msg);
        }
    }
    /* A low: the person treats it, and waits 20 minutes before treating again. */
    if ((float)p->g_milli / 1000.0f < 70 && t - p->treated_t > 20 * 60) {
        uint8_t food = pick(p, HYPO);
        add_meal(c, t, food, between(p, FOODS[food].lo, FOODS[food].hi));
        p->treated_t = t;
        char msg[80];
        snprintf(msg, sizeof msg, "low: the person treats it with %s", cgm_food_name(food));
        say(c, msg);
    }

    /* Exercise: insulin works harder during it and for two hours after. */
    float sens = 1.0f, burn = 0;
    if (p->exercise_t && t >= p->exercise_t) {
        int64_t into = (t - p->exercise_t) / 60;
        if (into < p->exercise_min) {
            sens = 1.6f;
            burn = 0.4f;
        } else if (into < p->exercise_min + 120) {
            sens = 1.25f;
        }
    }

    /* Insulin acting now. */
    insulin_model_t im = insulin_model();
    float acting = 0;
    for (int i = 0; i < CGM_INSULIN_BUCKETS; i++) {
        if (p->ins[i]) {
            float age = (float)(minute - (p->ins_bucket - i) * 5) + 2.5f;
            acting += p->ins[i] / 100.0f * insulin_activity(&im, age);
        }
    }
    /* Carbohydrate absorbing now. */
    float carbs = 0;
    for (int i = 0; i < CGM_MEALS; i++) {
        cgm_meal_t *m = &p->meals[i];
        if (m->grams && t >= m->t) {
            float age = (float)(t - m->t) / 60.0f - m->delay;
            if (age < 3.0f * m->absorb) {
                carbs += m->grams * curve(age, m->absorb / 4.0f);
            }
        }
    }
    /* The liver: what the basal need covers, more before dawn (4-8 am). */
    int mod = minute_of_day(c, t);
    float dawn = mod >= 240 && mod < 480 ? 0.3f * (1.0f - fabsf((float)(mod - 360) / 120.0f)) : 0;
    float liver = isf * need * (1.0f + dawn);

    /* The slow wander. */
    p->drift += (int32_t)(gauss(p) * 15.0f) - p->drift / 60;
    if (p->drift > 600) {
        p->drift = 600;
    } else if (p->drift < -600) {
        p->drift = -600;
    }

    float g = (float)p->g_milli / 1000.0f;
    float dg = liver + isf / icr * carbs - isf * acting * sens - burn + p->drift / 1000.0f;
    if (g < 70) {
        dg += (70 - g) * 0.02f; /* the body pushes back against a low */
    }
    g += dg;
    if (g < 25) {
        g = 25;
    } else if (g > 600) {
        g = 600;
    }
    p->g_milli = (int64_t)(g * 1000.0f);
    /* The sensor's error: correlated over minutes, a few mg/dL. */
    p->noise = (int32_t)(p->noise * 0.9f + gauss(p) * 900.0f);
}

void person_reset(cgm_t *c, int64_t now)
{
    cgm_person_t *p = &c->st.person;
    uint32_t seed = p->rng;
    memset(p, 0, sizeof *p);
    p->rng = seed ? seed : (uint32_t)now * 2654435761u + 1;
    p->g_milli = (int64_t)(c->st.glucose ? c->st.glucose : 120) * 1000;
    p->minute = now / 60;
    p->planned_day = INT64_MIN;
    p->correction_t = now;
}

void person_advance(cgm_t *c, int64_t now)
{
    cgm_person_t *p = &c->st.person;
    int64_t to = now / 60;
    if (to - p->minute > 24 * 60) {
        p->minute = to - 24 * 60; /* a board off for days: one day of catch-up */
    }
    while (p->minute < to) {
        p->minute++;
        step(c, p->minute);
    }
}

uint16_t person_reading(cgm_t *c)
{
    const cgm_person_t *p = &c->st.person;
    int g = (int)((p->g_milli + p->noise + 500) / 1000);
    return (uint16_t)(g < 40 ? 40 : g > 400 ? 400 : g);
}

void person_eat(cgm_t *c, int64_t now, uint16_t grams)
{
    add_meal(c, now, 0xff, grams);
}

int64_t person_last_meal(const cgm_t *c, int64_t now, uint16_t *grams, uint8_t *food)
{
    const cgm_person_t *p = &c->st.person;
    int64_t best = 0;
    for (int i = 0; i < CGM_MEALS; i++) {
        if (p->meals[i].grams && p->meals[i].t <= now && p->meals[i].t > best) {
            best = p->meals[i].t;
            *grams = p->meals[i].grams;
            *food = p->meals[i].food;
        }
    }
    return best;
}

void person_exercise(cgm_t *c, int64_t now, uint16_t minutes)
{
    c->st.person.exercise_t = now;
    c->st.person.exercise_min = minutes;
}

int64_t person_next_meal(const cgm_t *c, int64_t now, uint16_t *grams, uint8_t *food)
{
    const cgm_person_t *p = &c->st.person;
    int64_t best = 0;
    for (int i = 0; i < CGM_MEALS; i++) {
        if (p->meals[i].grams && p->meals[i].t > now && (!best || p->meals[i].t < best)) {
            best = p->meals[i].t;
            *grams = p->meals[i].grams;
            *food = p->meals[i].food;
        }
    }
    return best;
}

float person_insulin_acting(const cgm_t *c)
{
    /* Insulin on board: what has not yet acted, units. */
    const cgm_person_t *p = &c->st.person;
    insulin_model_t im = insulin_model();
    float iob = 0;
    for (int i = 0; i < CGM_INSULIN_BUCKETS; i++) {
        if (p->ins[i]) {
            float age = (float)(p->minute - (p->ins_bucket - i) * 5) + 2.5f;
            iob += p->ins[i] / 100.0f * insulin_remaining(&im, age);
        }
    }
    return iob;
}

float person_insulin_basal(const cgm_t *c)
{
    /* What a steady basal at the basal need keeps on board: the rate times the
     * time a unit stays on board. An AID counts its IOB above its scheduled
     * basal, so the panel shows the person's IOB less this too. */
    insulin_model_t im = insulin_model();
    float minutes = 0;
    for (int t = 0; t < (int)INSULIN_DIA; t++) {
        minutes += insulin_remaining(&im, (float)t + 0.5f);
    }
    float need = (c->st.basal_need ? c->st.basal_need : 80) / 100.0f / 60.0f;
    return need * minutes;
}
