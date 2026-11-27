/* Backtest playback (backtest.c), for cgm.c. SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "cgm.h"

/* The reading for the slot at slot_t: false when there is none to send (a
 * missing trace reading, a slot already played, the trace stopped). */
bool backtest_reading(cgm_t *c, int64_t slot_t, bool sensor_ok, uint16_t *glucose, int8_t *trend);
/* About once a second: logs what the pump changed. */
void backtest_watch_pump(cgm_t *c);
