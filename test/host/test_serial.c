/* Host tests for src/medtrum_serial.h: which serial the app accepts, and the
 * parsing the panel's serial field goes through.
 *
 * The numbers below are the core's own decrypt, so this is the app's validation
 * pinned in the same code path the panel uses.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <string.h>

#include "medtrum_serial.h"

static int checks, failures;

#define CHECK(x)                                                \
    do {                                                        \
        checks++;                                               \
        if (!(x)) {                                             \
            failures++;                                         \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
        }                                                       \
    } while (0)

static void test_accepted(void) {
    uint8_t sn[4];
    CHECK(medtrum_serial_parse("4A12D828", sn));
    CHECK(strcmp(medtrum_serial_model(sn), "MD0201") == 0);
    CHECK(medtrum_serial_valid(sn));
    CHECK(medtrum_serial_parse("52DF1614", sn));
    CHECK(strcmp(medtrum_serial_model(sn), "MD8301") == 0);
    CHECK(medtrum_serial_valid(sn));
    /* Lower case is the same serial. */
    CHECK(medtrum_serial_parse("4a12d828", sn));
    CHECK(medtrum_serial_valid(sn));
}

/* The reversed strings look plausible and are what a careful reader of
 * MedtrumKit's source would type. The app rejects them, so the panel must too. */
static void test_rejected_by_the_app(void) {
    uint8_t sn[4];
    CHECK(medtrum_serial_parse("28D8124A", sn));
    CHECK(!medtrum_serial_valid(sn));
    CHECK(strcmp(medtrum_serial_model(sn), "INVALID") == 0);
    CHECK(medtrum_serial_parse("1416DF52", sn));
    CHECK(!medtrum_serial_valid(sn));
}

static void test_parse_rejects(void) {
    uint8_t sn[4];
    CHECK(!medtrum_serial_parse("", sn));
    CHECK(!medtrum_serial_parse("4A12D82", sn));   /* 7 digits */
    CHECK(!medtrum_serial_parse("4A12D8280", sn)); /* 9 digits */
    CHECK(!medtrum_serial_parse("4A12D82G", sn));  /* not hex */
    CHECK(!medtrum_serial_parse("4A12 828", sn));  /* space */
    CHECK(!medtrum_serial_parse(NULL, sn));
}

/* The advertisement must carry the typed serial reversed. Trio was only found
 * with it reversed on hardware; if this flips, the base stops being offered. */
static void test_advert_bytes(void) {
    uint8_t sn[4] = {0}, adv[4] = {0};
    CHECK(medtrum_serial_parse("4A12D828", sn));
    medtrum_serial_advert_bytes(sn, adv);
    CHECK(adv[0] == 0x28 && adv[1] == 0xD8 && adv[2] == 0x12 && adv[3] == 0x4A);
    CHECK(medtrum_serial_parse("52DF1614", sn));
    medtrum_serial_advert_bytes(sn, adv);
    CHECK(adv[0] == 0x14 && adv[1] == 0x16 && adv[2] == 0xDF && adv[3] == 0x52);
    /* reversing twice is the identity */
    uint8_t back[4] = {0};
    medtrum_serial_advert_bytes(adv, back);
    CHECK(back[0] == 0x52 && back[1] == 0xDF && back[2] == 0x16 && back[3] == 0x14);
}

int main(void) {
    test_advert_bytes();
    test_accepted();
    test_rejected_by_the_app();
    test_parse_rejects();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
