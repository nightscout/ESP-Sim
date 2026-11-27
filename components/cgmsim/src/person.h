/* The virtual person (person.c), for cgm.c. SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "cgm.h"

void person_reset(cgm_t *c, int64_t now);
void person_advance(cgm_t *c, int64_t now);
uint16_t person_reading(cgm_t *c); /* the sensor's value now, 40-400 */
void person_eat(cgm_t *c, int64_t now, uint16_t grams);
void person_exercise(cgm_t *c, int64_t now, uint16_t minutes);
int64_t person_next_meal(const cgm_t *c, int64_t now, uint16_t *grams, uint8_t *food);
int64_t person_last_meal(const cgm_t *c, int64_t now, uint16_t *grams, uint8_t *food);
float person_insulin_acting(const cgm_t *c); /* insulin on board, units */
float person_insulin_basal(const cgm_t *c);  /* what the basal need alone keeps on board, units */
