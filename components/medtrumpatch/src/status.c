/*
 * The patch's reported status: the field mask and the payload builder that
 * both the READ-characteristic notification and a SYNCHRONIZE response use.
 *
 * The layout mirrors MedtrumKit's NotificationPacket.fields.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <string.h>

#include "medtrum.h"

bool medtrum_state_is_setup(uint8_t state)
{
    switch (state) {
    case MEDTRUM_STATE_NONE:
    case MEDTRUM_STATE_IDLE:
    case MEDTRUM_STATE_FILLED:
    case MEDTRUM_STATE_PRIMING:
    case MEDTRUM_STATE_PRIMED:
    case MEDTRUM_STATE_EJECTING:
    case MEDTRUM_STATE_EJECTED:
        return true;
    default:
        return false;
    }
}

bool medtrum_state_is_running(uint8_t state)
{
    switch (state) {
    case MEDTRUM_STATE_ACTIVE:
    case MEDTRUM_STATE_ACTIVE_ALT:
    case MEDTRUM_STATE_LOW_BG_SUSPENDED:
    case MEDTRUM_STATE_LOW_BG_SUSPENDED2:
    case MEDTRUM_STATE_AUTO_SUSPENDED:
    case MEDTRUM_STATE_HOURLY_MAX_SUSPENDED:
    case MEDTRUM_STATE_DAILY_MAX_SUSPENDED:
    case MEDTRUM_STATE_PAUSED:
    case MEDTRUM_STATE_SUSPENDED:
        return true;
    default:
        return false;
    }
}

bool medtrum_state_is_terminated(uint8_t state)
{
    switch (state) {
    case MEDTRUM_STATE_OCCLUSION:
    case MEDTRUM_STATE_EXPIRED:
    case MEDTRUM_STATE_RESERVOIR_EMPTY:
    case MEDTRUM_STATE_PATCH_FAULT:
    case MEDTRUM_STATE_PATCH_FAULT2:
    case MEDTRUM_STATE_BASE_FAULT:
    case MEDTRUM_STATE_BATTERY_OUT:
    case MEDTRUM_STATE_NO_CALIBRATION:
    case MEDTRUM_STATE_STOPPED:
        return true;
    default:
        return false;
    }
}

bool medtrum_state_is_suspended(uint8_t state)
{
    switch (state) {
    case MEDTRUM_STATE_LOW_BG_SUSPENDED:
    case MEDTRUM_STATE_LOW_BG_SUSPENDED2:
    case MEDTRUM_STATE_AUTO_SUSPENDED:
    case MEDTRUM_STATE_HOURLY_MAX_SUSPENDED:
    case MEDTRUM_STATE_DAILY_MAX_SUSPENDED:
    case MEDTRUM_STATE_PAUSED:
    case MEDTRUM_STATE_SUSPENDED:
        return true;
    default:
        return false;
    }
}

bool medtrum_state_is_fault(uint8_t state)
{
    switch (state) {
    case MEDTRUM_STATE_OCCLUSION:
    case MEDTRUM_STATE_RESERVOIR_EMPTY:
    case MEDTRUM_STATE_PATCH_FAULT:
    case MEDTRUM_STATE_PATCH_FAULT2:
    case MEDTRUM_STATE_BATTERY_OUT:
        return true;
    default:
        return false;
    }
}

bool medtrum_state_is_delivery_halted(uint8_t state)
{
    return medtrum_state_is_suspended(state) || medtrum_state_is_terminated(state);
}

bool medtrum_state_before_priming(uint8_t state)
{
    return state == MEDTRUM_STATE_NONE || state == MEDTRUM_STATE_IDLE || state == MEDTRUM_STATE_FILLED;
}

bool medtrum_state_completed_priming(uint8_t state)
{
    return state == MEDTRUM_STATE_EJECTED || state == MEDTRUM_STATE_EJECTING || state == MEDTRUM_STATE_PRIMED;
}

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

size_t medtrum_status_payload(const medtrum_status_t *st, uint8_t *out, size_t cap)
{
    uint8_t b[MEDTRUM_STATUS_MAX];
    size_t n = 0;
    uint16_t m = st->mask;

    b[n++] = st->state;
    put_u16(b + n, m);
    n += 2;

    if (m & MEDTRUM_MASK_SUSPEND) {
        put_u32(b + n, st->suspend_time);
        n += 4;
    }
    if (m & MEDTRUM_MASK_NORMAL_BOLUS) {
        b[n++] = (uint8_t)(st->bolus_type | (st->bolus_completed ? 0x80 : 0x00));
        put_u16(b + n, st->bolus_delivered);
        n += 2;
    }
    if (m & MEDTRUM_MASK_EXTENDED_BOLUS) {
        memcpy(b + n, st->extended_bolus, 3);
        n += 3;
    }
    if (m & MEDTRUM_MASK_BASAL) {
        uint32_t rate_delivery = ((uint32_t)st->basal_delivery << 12) | (st->basal_rate & 0x0fff);
        b[n++] = st->basal_type;
        put_u16(b + n, st->basal_sequence);
        n += 2;
        put_u16(b + n, st->basal_patch_id);
        n += 2;
        put_u32(b + n, st->basal_start_time);
        n += 4;
        b[n++] = (uint8_t)rate_delivery;
        b[n++] = (uint8_t)(rate_delivery >> 8);
        b[n++] = (uint8_t)(rate_delivery >> 16);
    }
    if (m & MEDTRUM_MASK_SETUP) {
        b[n++] = st->prime_progress;
    }
    if (m & MEDTRUM_MASK_RESERVOIR) {
        put_u16(b + n, st->reservoir);
        n += 2;
    }
    if (m & MEDTRUM_MASK_START_TIME) {
        put_u32(b + n, st->start_time);
        n += 4;
    }
    if (m & MEDTRUM_MASK_BATTERY) {
        b[n++] = (uint8_t)st->battery;
        b[n++] = (uint8_t)(st->battery >> 8);
        b[n++] = (uint8_t)(st->battery >> 16);
    }
    if (m & MEDTRUM_MASK_STORAGE) {
        put_u16(b + n, st->storage_sequence);
        n += 2;
        put_u16(b + n, st->storage_patch_id);
        n += 2;
    }
    if (m & MEDTRUM_MASK_ALARM) {
        put_u16(b + n, st->alarm_flags);
        n += 2;
        memcpy(b + n, st->alarm_unused, 2);
        n += 2;
    }
    if (m & MEDTRUM_MASK_AGE) {
        put_u32(b + n, st->patch_age);
        n += 4;
    }
    if (m & MEDTRUM_MASK_MAGNETO_PLACE) {
        put_u16(b + n, st->magneto_placement);
        n += 2;
    }
    if (m & MEDTRUM_MASK_UNUSED_CGM) {
        memcpy(b + n, st->unused_cgm, 5);
        n += 5;
    }
    if (m & MEDTRUM_MASK_UNUSED_COMMAND_CONFIRM) {
        memcpy(b + n, st->unused_command_confirm, 2);
        n += 2;
    }
    if (m & MEDTRUM_MASK_UNUSED_AUTO_STATUS) {
        memcpy(b + n, st->unused_auto_status, 2);
        n += 2;
    }
    if (m & MEDTRUM_MASK_UNUSED_LEGACY) {
        memcpy(b + n, st->unused_legacy, 2);
        n += 2;
    }

    if (n > cap) {
        return 0;
    }
    memcpy(out, b, n);
    return n;
}
