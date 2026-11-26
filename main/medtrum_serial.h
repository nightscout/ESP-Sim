/* The base serial, and the check that keeps the panel and the app agreeing.
 *
 * There are TWO orders, and they are related by a reversal:
 *
 *   the user types   4A12D828      (the panel shows this string)
 *   the advertisement carries     28 D8 12 4A     <-- reverse of what was typed
 *
 * MedtrumKit's scan compares the advertisement's manufacturer bytes with the
 * reverse of the typed serial (BluetoothManager.swift:271,287), so the
 * advertisement must carry the reverse. Its model check reads the typed serial
 * through Data.toInt64(), which is little-endian, and that cancellation is what
 * makes 4A12D828 validate as MD0201. Confirmed on hardware: with the reversed
 * advertisement Trio finds and offers the base.
 *
 * The model the app derives from the typed serial, measured with the core's own
 * decrypt (test/medtrum/serial_tests pins these numbers):
 *
 *   4A12D828 -> 126022576  MD0201      (valid)
 *   28D8124A -> 1847467079 INVALID
 *   52DF1614 -> 148016470  MD8301      (valid)
 *   1416DF52 -> 493115731  INVALID
 *
 * So this header parses that order and answers whether the app would accept the
 * serial. The panel refuses a serial the app would reject, instead of letting the
 * user find out from Trio.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "medtrum.h" /* medtrum_simple_decrypt */

static inline int medtrum_hexval(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* "4A12D828" -> the four bytes the advertisement carries. Exactly 8 hex digits. */
static inline bool medtrum_serial_parse(const char *hex, uint8_t sn[4]) {
    if (hex == NULL || strlen(hex) != 8) {
        return false;
    }
    for (int i = 0; i < 4; i++) {
        int hi = medtrum_hexval(hex[2 * i]);
        int lo = medtrum_hexval(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        sn[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

/* The model the app derives from the serial, as it does: the four bytes read
 * big-endian, then decrypted. "INVALID" is what makes Trio say "Incorrect serial
 * number received". */
static inline const char *medtrum_serial_model(const uint8_t sn[4]) {
    uint32_t word = ((uint32_t)sn[0] << 24) | ((uint32_t)sn[1] << 16) | ((uint32_t)sn[2] << 8) | sn[3];
    uint32_t type = medtrum_simple_decrypt(word);
    if (type >= 126000000u && type < 127000000u) return "MD0201";
    if (type >= 127000000u && type < 128000000u) return "MD5201";
    if (type >= 128000000u && type < 129000000u) return "MD8201";
    if (type >= 130000000u && type < 131000000u) return "MD0202";
    if (type >= 131000000u && type < 132000000u) return "MD5202";
    if (type >= 148000000u && type < 149000000u) return "MD8301";
    return "INVALID";
}

static inline bool medtrum_serial_valid(const uint8_t sn[4]) {
    return strcmp(medtrum_serial_model(sn), "INVALID") != 0;
}

/* The bytes the advertisement must carry for a serial the user typed: the
 * reverse. This is the one conversion in the firmware, and Trio needs it. */
static inline void medtrum_serial_advert_bytes(const uint8_t sn[4], uint8_t out[4]) {
    out[0] = sn[3];
    out[1] = sn[2];
    out[2] = sn[1];
    out[3] = sn[0];
}
