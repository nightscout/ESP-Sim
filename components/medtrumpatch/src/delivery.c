/*
 * The delivery commands and state: priming, activation, the basal schedule,
 * temp basals, boluses, suspend/resume, alarms, and the clock.
 *
 * The request layouts and the response bodies mirror MedtrumKit's packet
 * classes (PrimePacket, ActivatePacket, SetBasalProfilePacket,
 * SetTempBasalPacket, SetBolusPacket, SuspendPumpPacket, ClearAlertPacket,
 * GetTimePacket, SetTimePacket, SetTimeZonePacket).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <string.h>

#include "medtrum.h"

#define BASAL_RESPONSE_LEN 11
#define ACTIVATE_RESPONSE_LEN 19

static uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void wr_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint16_t patch_id16(const medtrum_patch_t *p)
{
    return (uint16_t)(p->delivery.patch_id[0] | (p->delivery.patch_id[1] << 8));
}

/* The scheduled basal rate for a minute of the day. The profile is a count
 * then entries of (rate << 12) | startMinutes as a little-endian u24. */
static uint16_t basal_rate_at(const uint8_t *profile, size_t len, uint32_t minute)
{
    if (len < 1) {
        return 0;
    }
    uint8_t count = profile[0];
    uint16_t rate = 0;
    bool found = false;
    for (uint8_t i = 0; i < count; i++) {
        size_t off = 1 + (size_t)i * 3;
        if (off + 3 > len) {
            break;
        }
        uint32_t v = (uint32_t)profile[off] | ((uint32_t)profile[off + 1] << 8) |
                     ((uint32_t)profile[off + 2] << 16);
        if ((v & 0x0fff) <= minute) {
            rate = (uint16_t)(v >> 12);
            found = true;
        }
    }
    if (!found && len >= 4) {
        rate = (uint16_t)(((uint32_t)profile[1] | ((uint32_t)profile[2] << 8) |
                           ((uint32_t)profile[3] << 16)) >>
                          12);
    }
    return rate;
}

static uint32_t minute_of_day(uint32_t patch_time)
{
    return (patch_time / 60) % 1440;
}

/* Records a new basal in the status: STANDARD or ABSOLUTE_TEMP. */
static void set_basal_status(medtrum_patch_t *p, uint8_t type, uint16_t rate)
{
    p->status.basal_type = type;
    p->status.basal_rate = rate;
    p->status.basal_sequence = (uint16_t)(p->delivery.basal_sequence + 1);
    p->status.basal_patch_id = patch_id16(p);
    p->status.basal_start_time = p->delivery.patch_time;
    p->status.basal_delivery = 0;
    p->status.mask |= MEDTRUM_MASK_BASAL;
    p->delivery.basal_sequence = p->status.basal_sequence;
}

/* basalType(1) + basalValue(2) + basalSequence(2) + basalPatchId(2) +
 * basalStartTime(4), as SetBasalProfilePacket parses. */
static size_t basal_response(const medtrum_patch_t *p, uint8_t *out)
{
    out[0] = p->status.basal_type;
    wr_u16(out + 1, p->status.basal_rate);
    wr_u16(out + 3, p->status.basal_sequence);
    wr_u16(out + 5, p->status.basal_patch_id);
    wr_u32(out + 7, p->status.basal_start_time);
    return BASAL_RESPONSE_LEN;
}

/* patchId(4) + time(4) + basalType(1) + basalValue(2) + basalSequence(2) +
 * basalPatchId(2) + basalStartTime(4), as ActivatePacket parses. */
static size_t activate_response(const medtrum_patch_t *p, uint8_t *out)
{
    memcpy(out, p->delivery.patch_id, 4);
    wr_u32(out + 4, p->delivery.patch_start);
    out[8] = MEDTRUM_BASAL_STANDARD;
    wr_u16(out + 9, p->delivery.scheduled_rate);
    wr_u16(out + 11, p->delivery.basal_sequence);
    wr_u16(out + 13, patch_id16(p));
    wr_u32(out + 15, p->delivery.patch_time);
    return ACTIVATE_RESPONSE_LEN;
}

int medtrum_patch_command(medtrum_patch_t *p, uint8_t command, uint8_t sequence, const uint8_t *content,
                          size_t content_len, medtrum_frag_t *frags, size_t max_frags)
{
    uint8_t payload[ACTIVATE_RESPONSE_LEN];
    uint16_t rc = MEDTRUM_RESPONSE_OK;

    switch (command) {
    case MEDTRUM_CMD_GET_TIME:
        wr_u32(payload, p->delivery.patch_time);
        return medtrum_encode_response(command, sequence, rc, payload, 4, frags, max_frags);

    case MEDTRUM_CMD_SET_TIME:
        if (content == NULL || content_len < 5) {
            return -1;
        }
        p->delivery.patch_time = rd_u32(content + 1);
        payload[0] = p->status.state;
        return medtrum_encode_response(command, sequence, rc, payload, 1, frags, max_frags);

    case MEDTRUM_CMD_SET_TIME_ZONE: {
        if (content == NULL || content_len < 6) {
            return -1;
        }
        uint16_t raw = rd_u16(content);
        p->delivery.timezone_minutes = raw > 32767 ? (int32_t)raw - 65536 : (int32_t)raw;
        p->delivery.patch_time = rd_u32(content + 2);
        payload[0] = p->status.state;
        return medtrum_encode_response(command, sequence, rc, payload, 1, frags, max_frags);
    }

    case MEDTRUM_CMD_PRIME:
        if (p->status.state != MEDTRUM_STATE_FILLED) {
            return medtrum_encode_response(command, sequence, MEDTRUM_RESPONSE_BAD_STATE, NULL, 0, frags,
                                           max_frags);
        }
        p->status.state = MEDTRUM_STATE_PRIMING;
        p->status.prime_progress = 0;
        p->status.mask |= MEDTRUM_MASK_SETUP;
        payload[0] = p->status.state;
        return medtrum_encode_response(command, sequence, rc, payload, 1, frags, max_frags);

    case MEDTRUM_CMD_ACTIVATE: {
        /* autoSuspendEnable, autoSuspendTime, expirationTimer, alarmSetting,
         * lowSuspend, predictiveLowSuspend, predictiveLowSuspendRange,
         * hourlyMax(2), dailyMax(2), currentTDD(2), 1, basalProfile... */
        if (content == NULL || content_len < 14) {
            return -1;
        }
        p->delivery.expiry_timer = content[2];
        p->delivery.alarm_setting = content[3];
        p->delivery.max_hourly = rd_u16(content + 7);
        p->delivery.max_daily = rd_u16(content + 9);
        size_t plen = content_len - 14;
        if (plen > MEDTRUM_BASAL_PROFILE_MAX) {
            plen = MEDTRUM_BASAL_PROFILE_MAX;
        }
        memcpy(p->delivery.basal_profile, content + 14, plen);
        p->delivery.basal_profile_len = plen;
        p->delivery.patch_start = p->delivery.patch_time;
        p->delivery.scheduled_rate = basal_rate_at(p->delivery.basal_profile, plen,
                                                   minute_of_day(p->delivery.patch_time));
        p->delivery.storage_sequence = (uint16_t)(p->delivery.storage_sequence + 1);
        p->status.state = MEDTRUM_STATE_ACTIVE;
        p->status.storage_sequence = p->delivery.storage_sequence;
        p->status.storage_patch_id = patch_id16(p);
        p->status.start_time = p->delivery.patch_start;
        p->status.patch_age = 0;
        p->status.mask &= (uint16_t)~MEDTRUM_MASK_SETUP;
        p->status.mask |= MEDTRUM_MASK_STORAGE | MEDTRUM_MASK_START_TIME | MEDTRUM_MASK_AGE;
        set_basal_status(p, MEDTRUM_BASAL_STANDARD, p->delivery.scheduled_rate);
        size_t n = activate_response(p, payload);
        return medtrum_encode_response(command, sequence, rc, payload, n, frags, max_frags);
    }

    case MEDTRUM_CMD_STOP_PATCH: {
        wr_u16(payload, p->delivery.storage_sequence);
        wr_u16(payload + 2, patch_id16(p));
        p->status.state = MEDTRUM_STATE_STOPPED;
        p->status.mask = 0;
        memset(p->delivery.patch_id, 0, 4);
        p->delivery.temp_end = 0;
        p->delivery.temp_rate = 0;
        p->delivery.bolus_active = false;
        return medtrum_encode_response(command, sequence, rc, payload, 4, frags, max_frags);
    }

    case MEDTRUM_CMD_SET_PATCH:
        /* alarmSettings, hourlyMax(2), dailyMax(2), expirationTimer, ... */
        if (content == NULL || content_len < 11) {
            return -1;
        }
        p->delivery.alarm_setting = content[0];
        p->delivery.max_hourly = rd_u16(content + 1);
        p->delivery.max_daily = rd_u16(content + 3);
        p->delivery.expiry_timer = content[5];
        payload[0] = p->status.state;
        return medtrum_encode_response(command, sequence, rc, payload, 1, frags, max_frags);

    case MEDTRUM_CMD_SET_BASAL_PROFILE: {
        if (content == NULL || content_len < 1) {
            return -1;
        }
        size_t plen = content_len - 1;
        if (plen > MEDTRUM_BASAL_PROFILE_MAX) {
            plen = MEDTRUM_BASAL_PROFILE_MAX;
        }
        memcpy(p->delivery.basal_profile, content + 1, plen);
        p->delivery.basal_profile_len = plen;
        p->delivery.scheduled_rate = basal_rate_at(p->delivery.basal_profile, plen,
                                                   minute_of_day(p->delivery.patch_time));
        set_basal_status(p, content[0], p->delivery.scheduled_rate);
        size_t n = basal_response(p, payload);
        return medtrum_encode_response(command, sequence, rc, payload, n, frags, max_frags);
    }

    case MEDTRUM_CMD_SET_TEMP_BASAL:
        /* type(1), rate(2), durationMinutes(2) */
        if (content == NULL || content_len < 5) {
            return -1;
        }
        p->delivery.temp_rate = rd_u16(content + 1);
        p->delivery.temp_end = p->delivery.patch_time + (uint32_t)rd_u16(content + 3) * 60;
        set_basal_status(p, MEDTRUM_BASAL_ABSOLUTE_TEMP, p->delivery.temp_rate);
        {
            size_t n = basal_response(p, payload);
            return medtrum_encode_response(command, sequence, rc, payload, n, frags, max_frags);
        }

    case MEDTRUM_CMD_CANCEL_TEMP_BASAL:
        p->delivery.temp_end = 0;
        p->delivery.temp_rate = 0;
        set_basal_status(p, MEDTRUM_BASAL_STANDARD, p->delivery.scheduled_rate);
        {
            size_t n = basal_response(p, payload);
            return medtrum_encode_response(command, sequence, rc, payload, n, frags, max_frags);
        }

    case MEDTRUM_CMD_SET_BOLUS:
        /* bolusType(1), amount(2), 0 */
        if (content == NULL || content_len < 4) {
            return -1;
        }
        p->delivery.bolus_total = rd_u16(content + 1);
        p->delivery.bolus_start = p->delivery.patch_time;
        p->delivery.bolus_active = true;
        p->status.bolus_type = (uint8_t)(content[0] & 0x7f);
        p->status.bolus_completed = false;
        p->status.bolus_delivered = 0;
        p->status.mask |= MEDTRUM_MASK_NORMAL_BOLUS;
        payload[0] = p->status.state;
        return medtrum_encode_response(command, sequence, rc, payload, 1, frags, max_frags);

    case MEDTRUM_CMD_CANCEL_BOLUS:
        p->delivery.bolus_active = false;
        p->status.mask &= (uint16_t)~MEDTRUM_MASK_NORMAL_BOLUS;
        payload[0] = p->status.state;
        return medtrum_encode_response(command, sequence, rc, payload, 1, frags, max_frags);

    case MEDTRUM_CMD_SUSPEND_PUMP:
        p->status.state = MEDTRUM_STATE_SUSPENDED;
        p->status.suspend_time = p->delivery.patch_time;
        p->status.mask |= MEDTRUM_MASK_SUSPEND;
        payload[0] = p->status.state;
        return medtrum_encode_response(command, sequence, rc, payload, 1, frags, max_frags);

    case MEDTRUM_CMD_RESUME_PUMP:
        p->status.state = MEDTRUM_STATE_ACTIVE;
        p->status.mask &= (uint16_t)~MEDTRUM_MASK_SUSPEND;
        set_basal_status(p, MEDTRUM_BASAL_STANDARD, p->delivery.scheduled_rate);
        payload[0] = p->status.state;
        return medtrum_encode_response(command, sequence, rc, payload, 1, frags, max_frags);

    case MEDTRUM_CMD_CLEAR_ALARM:
        if (content != NULL && content_len >= 2) {
            uint16_t type = rd_u16(content);
            if (type == MEDTRUM_ALERT_HOURLY && p->status.state == MEDTRUM_STATE_HOURLY_MAX_SUSPENDED) {
                p->status.state = MEDTRUM_STATE_ACTIVE;
            } else if (type == MEDTRUM_ALERT_DAILY &&
                       p->status.state == MEDTRUM_STATE_DAILY_MAX_SUSPENDED) {
                p->status.state = MEDTRUM_STATE_ACTIVE;
            }
        }
        p->delivery.alarm_flags = 0;
        p->status.alarm_flags = 0;
        p->status.mask &= (uint16_t)~MEDTRUM_MASK_ALARM;
        payload[0] = p->status.state;
        return medtrum_encode_response(command, sequence, rc, payload, 1, frags, max_frags);

    case MEDTRUM_CMD_GET_RECORD: {
        /* GetRecordPacket: index(2) patchId(2). The simulator keeps no record
         * log; it answers each sequence the status advertises with a TDD
         * record (type 9), which the client parses and the watch's history
         * sweep accepts without inventing a dose. */
        if (content == NULL || content_len < 4) {
            return -1;
        }
        uint16_t index = rd_u16(content);
        uint16_t patch = rd_u16(content + 2);
        uint8_t record[20] = {0};
        record[0] = 0xAA; /* VALID_HEADER */
        record[2] = 9;    /* TDD record */
        memcpy(record + 4, p->sn, 4);
        wr_u16(record + 8, patch);
        wr_u16(record + 10, index);
        wr_u32(record + 12, p->delivery.patch_time);
        wr_u16(record + 16, (uint16_t)(int16_t)p->delivery.timezone_minutes);
        return medtrum_encode_response(command, sequence, rc, record, sizeof record, frags, max_frags);
    }

    default:
        return -1;
    }
}

/* The notification packs the basal rate and the delivered pulses into one
 * 24-bit value: the rate in the low 12 bits, the pulses in the next 12. */
#define BASAL_DELIVERY_MAX 0x0fff

/* A basal rate is in pulses of 0.05 U per HOUR, the patch clock counts
 * seconds and the reservoir counts whole pulses, so a span of seconds draws
 * seconds * rate / 3600 pulses, truncated. The pump can only deliver a
 * complete 0.05 U pulse: a span shorter than 3600/rate seconds draws nothing
 * yet. That fraction is not lost between ticks because the count is always
 * recomputed from the start of the current basal segment, not added up tick by
 * tick; only the increase over what the segment has already drawn is charged
 * to the reservoir, so a repeated tick cannot double-count. */
static void draw_basal_span(medtrum_patch_t *p, uint32_t from, uint32_t to, uint16_t rate)
{
    if (rate == 0 || to <= from) {
        return;
    }
    uint64_t pulses = (uint64_t)(to - from) * rate / 3600;
    uint64_t drawn = p->status.basal_delivery;
    bool clamped = false;
    if (pulses > drawn) {
        uint32_t increment = (uint32_t)(pulses - drawn);
        if (increment > p->status.reservoir) {
            /* Only what is left in the reservoir can be delivered. */
            increment = p->status.reservoir;
            clamped = true;
        }
        p->status.reservoir = (uint16_t)(p->status.reservoir - increment);
        drawn += increment;
    }
    if (drawn > BASAL_DELIVERY_MAX) {
        drawn = BASAL_DELIVERY_MAX;
        clamped = true; /* the 12 delivery bits cannot hold any more */
    }
    p->status.basal_delivery = (uint16_t)drawn;

    /* Start a fresh segment at this second when the count filled its 12 bits,
     * or when the reservoir could not meet the span. Forgetting the shortfall
     * is deliberate: an empty reservoir must not build a backlog that a refill
     * would then deliver in one burst. */
    if (clamped || p->status.basal_delivery >= BASAL_DELIVERY_MAX) {
        p->status.basal_delivery = 0;
        p->status.basal_start_time = to;
    }
}

/* Basal delivery for the pump seconds up to this tick, at the temp rate while
 * a temp basal runs and at the schedule otherwise. */
static void draw_basal(medtrum_patch_t *p)
{
    uint32_t from = p->status.basal_start_time;
    uint32_t to = p->delivery.patch_time;
    uint16_t rate = p->delivery.scheduled_rate;
    if (p->delivery.temp_end != 0) {
        rate = p->delivery.temp_rate;
        if (to > p->delivery.temp_end) {
            to = p->delivery.temp_end; /* the tick ends the temp basal */
        }
    }
    draw_basal_span(p, from, to, rate);
}

void medtrum_patch_tick(medtrum_patch_t *p, int64_t now_ms)
{
    if (p->delivery.last_ms == INT64_MIN) {
        p->delivery.last_ms = now_ms;
        return;
    }
    if (now_ms <= p->delivery.last_ms) {
        return;
    }
    uint32_t elapsed = (uint32_t)((now_ms - p->delivery.last_ms) / 1000);
    p->delivery.last_ms = now_ms;
    if (elapsed == 0) {
        return;
    }
    p->delivery.patch_time += elapsed;

    /* A bolus runs at 1.5 U/min: 0.5 pulses a second, one pulse every two.
     * The reservoir is charged the INCREMENT since the last tick, never the
     * running total, so a repeated tick cannot deliver the same pulse twice. */
    if (p->delivery.bolus_active) {
        uint32_t pulses = (p->delivery.patch_time - p->delivery.bolus_start) / 2;
        if (pulses >= p->delivery.bolus_total) {
            pulses = p->delivery.bolus_total;
            p->delivery.bolus_active = false;
            p->status.bolus_completed = true;
        }
        uint16_t delivered = (uint16_t)pulses;
        uint16_t increment = delivered > p->status.bolus_delivered
                                 ? (uint16_t)(delivered - p->status.bolus_delivered)
                                 : 0;
        if (increment > p->status.reservoir) {
            /* Only what is left in the reservoir can be delivered. */
            increment = p->status.reservoir;
        }
        p->status.reservoir = (uint16_t)(p->status.reservoir - increment);
        p->status.bolus_delivered = (uint16_t)(p->status.bolus_delivered + increment);
        if (p->status.reservoir == 0 && p->delivery.bolus_active) {
            /* A real patch reports an empty reservoir rather than pumping
             * air, so the bolus stops here, short of its total. */
            p->delivery.bolus_active = false;
        }
    }

    /* Basal draws too, in the same pump-time step: at the temp rate while a
     * temp basal runs, otherwise at the schedule. A halted patch (suspended or
     * stopped) draws nothing at all. */
    bool halted = medtrum_state_is_delivery_halted(p->status.state);
    /* A resume that does not go through RESUME_PUMP (CLEAR_ALARM from a max
     * suspension, the panel's set state or clear fault) leaves the old segment
     * anchor behind; without this edge reset the first tick back would charge
     * the whole halt. Start a fresh segment at the last tick instead, so only
     * the span since then is drawn. RESUME_PUMP also resets via
     * set_basal_status, which is harmless. */
    if (!halted && p->delivery.was_halted) {
        p->status.basal_delivery = 0;
        p->status.basal_start_time = p->delivery.patch_time - elapsed;
    }
    p->delivery.was_halted = halted;
    if (!halted) {
        draw_basal(p);
    }

    /* A temp basal ends on its own. */
    if (p->delivery.temp_end != 0 && p->delivery.patch_time >= p->delivery.temp_end) {
        uint32_t ended = p->delivery.temp_end;
        p->delivery.temp_end = 0;
        p->delivery.temp_rate = 0;
        set_basal_status(p, MEDTRUM_BASAL_STANDARD, p->delivery.scheduled_rate);
        /* The schedule resumed when the temp ended, not at this tick, so the
         * new segment is anchored there and the seconds since are drawn at the
         * scheduled rate. */
        p->status.basal_start_time = ended;
        if (!halted) {
            draw_basal_span(p, ended, p->delivery.patch_time, p->delivery.scheduled_rate);
        }
    }

    if (p->status.mask & MEDTRUM_MASK_AGE) {
        p->status.patch_age = p->delivery.patch_time - p->delivery.patch_start;
    }
}

/* Absolute, not a decrement: the control panel's "set the reservoir". */
void medtrum_patch_set_reservoir(medtrum_patch_t *p, uint16_t pulses)
{
    p->status.reservoir = pulses;
    p->status.mask |= MEDTRUM_MASK_RESERVOIR;
}

void medtrum_patch_set_battery(medtrum_patch_t *p, uint32_t raw)
{
    p->status.battery = raw;
    p->status.mask |= MEDTRUM_MASK_BATTERY;
}

void medtrum_patch_set_patch_id(medtrum_patch_t *p, const uint8_t id[4])
{
    memcpy(p->delivery.patch_id, id, 4);
}

void medtrum_patch_set_alarm(medtrum_patch_t *p, uint16_t flags)
{
    p->delivery.alarm_flags = flags;
    p->status.alarm_flags = flags;
    if (flags != 0) {
        p->status.mask |= MEDTRUM_MASK_ALARM;
    } else {
        p->status.mask &= (uint16_t)~MEDTRUM_MASK_ALARM;
    }
}

void medtrum_patch_set_fault(medtrum_patch_t *p, uint8_t state)
{
    p->status.state = state;
}
