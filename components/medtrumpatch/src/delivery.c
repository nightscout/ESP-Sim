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

    default:
        return -1;
    }
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

    /* A bolus runs at 1.5 U/min: 0.5 pulses a second, one pulse every two. */
    if (p->delivery.bolus_active) {
        uint32_t delivered = (p->delivery.patch_time - p->delivery.bolus_start) / 2;
        if (delivered >= p->delivery.bolus_total) {
            delivered = p->delivery.bolus_total;
            p->delivery.bolus_active = false;
            p->status.bolus_completed = true;
        }
        p->status.bolus_delivered = (uint16_t)delivered;
    }

    /* A temp basal ends on its own. */
    if (p->delivery.temp_end != 0 && p->delivery.patch_time >= p->delivery.temp_end) {
        p->delivery.temp_end = 0;
        p->delivery.temp_rate = 0;
        set_basal_status(p, MEDTRUM_BASAL_STANDARD, p->delivery.scheduled_rate);
    }

    if (p->status.mask & MEDTRUM_MASK_AGE) {
        p->status.patch_age = p->delivery.patch_time - p->delivery.patch_start;
    }
}

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
