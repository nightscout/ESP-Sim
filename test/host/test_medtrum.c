/*
 * Host tests for the Medtrum patch core. Every byte vector is hardcoded from
 * MedtrumKit's own tests (MedtrumKitTests/), named in each case.
 *
 * The core is portable C: this binary links only components/medtrumpatch.
 */
#include <stdio.h>
#include <string.h>

#include "medtrum.h"
#include "test.h"

int failures, checks;

/* Encodes a single-packet request and compares it byte for byte. */
static void expect_single(uint8_t cmd, const uint8_t *content, size_t content_len, const uint8_t *want,
                          size_t want_len)
{
    medtrum_frag_t frags[MEDTRUM_FRAGS_MAX];
    int n = medtrum_encode(cmd, 0, content, content_len, frags, MEDTRUM_FRAGS_MAX);
    CHECK(n == 1);
    if (n == 1) {
        CHECK_BYTES(frags[0].data, frags[0].len, want, want_len);
    }
}

/* ---- CRC-8 ------------------------------------------------------------- */

static void test_crc8(void)
{
    /* The CRC bytes embedded in MedtrumKit's request vectors. */
    uint8_t auth[] = {0x0e, 0x05, 0x00, 0x00, 0x02, 0x9b, 0x02, 0x00, 0x00, 0x48, 0xcd, 0x12, 0x08};
    CHECK(medtrum_crc8(auth, sizeof auth) == 0x24);

    uint8_t sync[] = {0x05, 0x03, 0x00, 0x00};
    CHECK(medtrum_crc8(sync, sizeof sync) == 0xfc);

    uint8_t sub[] = {0x07, 0x04, 0x00, 0x00, 0xff, 0x0f};
    CHECK(medtrum_crc8(sub, sizeof sub) == 0xa3);

    uint8_t empty[] = {0x05, 0x10, 0x00, 0x00};
    CHECK(medtrum_crc8(empty, sizeof empty) == 0xa4);

    /* CRC of nothing is the init value. */
    CHECK(medtrum_crc8(NULL, 0) == 0);
}

/* ---- crypto ------------------------------------------------------------ */

static void test_crypto(void)
{
    /* CryptoTests.swift: testKeyGen */
    uint8_t sn[4] = {217, 249, 118, 170};
    uint8_t key[4];
    medtrum_gen_key(sn, key);
    CHECK_BYTES(key, 4, ((uint8_t[]){235, 57, 134, 200}), 4);

    /* CryptoTests.swift: testKeyDecrypt */
    uint32_t sn_word = 2859923929u;
    uint32_t dec = medtrum_simple_decrypt(sn_word);
    uint8_t dec_le[4] = {(uint8_t)dec, (uint8_t)(dec >> 8), (uint8_t)(dec >> 16), (uint8_t)(dec >> 24)};
    CHECK_BYTES(dec_le, 4, ((uint8_t[]){33, 191, 130, 7}), 4);

    /* The two directions are inverses of one another. */
    CHECK(medtrum_simple_decrypt(medtrum_simple_crypt(sn_word)) == sn_word);
    CHECK(medtrum_simple_crypt(medtrum_simple_decrypt(sn_word)) == sn_word);

    /* randomGen: x <- 16807x mod (2^31 - 1). */
    CHECK(medtrum_random_gen(0) == 0);
    CHECK(medtrum_random_gen(1) == 16807);
}

/* ---- request encoding -------------------------------------------------- */

static void test_encode_single(void)
{
    /* AuthorizePacketTests.swift. The key is genKey of the reversed serial. */
    uint8_t rev_sn[4] = {0xaa, 0x76, 0xf9, 0xd9};
    uint8_t key[4];
    medtrum_gen_key(rev_sn, key);
    CHECK_BYTES(key, 4, ((uint8_t[]){0x48, 0xcd, 0x12, 0x08}), 4);
    uint8_t auth[] = {0x02, 0x9b, 0x02, 0x00, 0x00, key[0], key[1], key[2], key[3]};
    expect_single(MEDTRUM_CMD_AUTH_REQ, auth, sizeof auth,
                  (uint8_t[]){0x0e, 0x05, 0x00, 0x00, 0x02, 0x9b, 0x02, 0x00, 0x00, 0x48, 0xcd, 0x12,
                              0x08, 0x24, 0x00},
                  15);

    /* SynchronizePacketTests.swift */
    expect_single(MEDTRUM_CMD_SYNCHRONIZE, NULL, 0,
                  (uint8_t[]){0x05, 0x03, 0x00, 0x00, 0xfc, 0x00}, 6);

    /* SubscribePacketTests.swift */
    expect_single(MEDTRUM_CMD_SUBSCRIBE, (uint8_t[]){0xff, 0x0f}, 2,
                  (uint8_t[]){0x07, 0x04, 0x00, 0x00, 0xff, 0x0f, 0xa3, 0x00}, 8);

    /* GetTimePacketTest.swift */
    expect_single(MEDTRUM_CMD_GET_TIME, NULL, 0, (uint8_t[]){0x05, 0x0b, 0x00, 0x00, 0xa1, 0x00}, 6);

    /* SetTimePacketTests.swift (2025-03-11T20:43:20Z) */
    expect_single(MEDTRUM_CMD_SET_TIME, (uint8_t[]){0x02, 0x28, 0x33, 0x0d, 0x15}, 5,
                  (uint8_t[]){0x0a, 0x0a, 0x00, 0x00, 0x02, 0x28, 0x33, 0x0d, 0x15, 0x1a, 0x00}, 11);

    /* SetTimeZonePacketTests.swift: CET and UTC+13 */
    expect_single(MEDTRUM_CMD_SET_TIME_ZONE, (uint8_t[]){0x3c, 0x00, 0x28, 0x33, 0x0d, 0x15}, 6,
                  (uint8_t[]){0x0b, 0x0c, 0x00, 0x00, 0x3c, 0x00, 0x28, 0x33, 0x0d, 0x15, 0x6e, 0x00}, 12);
    expect_single(MEDTRUM_CMD_SET_TIME_ZONE, (uint8_t[]){0x6c, 0xfd, 0x28, 0x33, 0x0d, 0x15}, 6,
                  (uint8_t[]){0x0b, 0x0c, 0x00, 0x00, 0x6c, 0xfd, 0x28, 0x33, 0x0d, 0x15, 0x78, 0x00}, 12);

    /* PrimePacketTests.swift */
    expect_single(MEDTRUM_CMD_PRIME, NULL, 0, (uint8_t[]){0x05, 0x10, 0x00, 0x00, 0xa4, 0x00}, 6);

    /* SetBolusPacketTests.swift (1.40 U) */
    expect_single(MEDTRUM_CMD_SET_BOLUS, (uint8_t[]){0x01, 0x1c, 0x00, 0x00}, 4,
                  (uint8_t[]){0x09, 0x13, 0x00, 0x00, 0x01, 0x1c, 0x00, 0x00, 0xfd, 0x00}, 10);

    /* CancelBolusPacketTests.swift */
    expect_single(MEDTRUM_CMD_CANCEL_BOLUS, (uint8_t[]){0x01}, 1,
                  (uint8_t[]){0x06, 0x14, 0x00, 0x00, 0x01, 0x10, 0x00}, 7);

    /* SetBasalProfilePacketTests.swift */
    expect_single(MEDTRUM_CMD_SET_BASAL_PROFILE,
                  (uint8_t[]){0x01, 0x03, 0x10, 0x0e, 0x00, 0x00, 0x01, 0x02, 0x0c, 0x0c, 0x0c}, 11,
                  (uint8_t[]){0x10, 0x15, 0x00, 0x00, 0x01, 0x03, 0x10, 0x0e, 0x00, 0x00,
                              0x01, 0x02, 0x0c, 0x0c, 0x0c, 0x43, 0x00},
                  17);

    /* SetTempBasalPacketTests.swift (1.25 U/h for 1 h) */
    expect_single(MEDTRUM_CMD_SET_TEMP_BASAL, (uint8_t[]){0x06, 0x19, 0x00, 0x3c, 0x00}, 5,
                  (uint8_t[]){0x0a, 0x18, 0x00, 0x00, 0x06, 0x19, 0x00, 0x3c, 0x00, 0x3b, 0x00}, 11);

    /* CancelTempBasalPacketTests.swift */
    expect_single(MEDTRUM_CMD_CANCEL_TEMP_BASAL, NULL, 0,
                  (uint8_t[]){0x05, 0x19, 0x00, 0x00, 0xa7, 0x00}, 6);

    /* ResumePumpPacketTests.swift */
    expect_single(MEDTRUM_CMD_RESUME_PUMP, NULL, 0, (uint8_t[]){0x05, 0x1d, 0x00, 0x00, 0x44, 0x00}, 6);

    /* StopPatchPacketTests.swift */
    expect_single(MEDTRUM_CMD_STOP_PATCH, NULL, 0, (uint8_t[]){0x05, 0x1f, 0x00, 0x00, 0xf8, 0x00}, 6);

    /* SetPatchPacketTests.swift */
    expect_single(MEDTRUM_CMD_SET_PATCH,
                  (uint8_t[]){0x01, 0x20, 0x03, 0x10, 0x0e, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x1e}, 11,
                  (uint8_t[]){0x10, 0x23, 0x00, 0x00, 0x01, 0x20, 0x03, 0x10, 0x0e, 0x00,
                              0x00, 0x0c, 0x00, 0x00, 0x1e, 0x2e, 0x00},
                  17);
}

static void test_encode_fragmented(void)
{
    medtrum_frag_t frags[MEDTRUM_FRAGS_MAX];
    int n;

    /* ActivatePacketTests.swift: two fragments. */
    uint8_t c2[] = {0, 12, 1, 6, 0, 0, 30, 32, 3, 16, 14, 0, 0, 1, 3, 16, 14, 0, 0, 1, 2, 12, 12, 12};
    uint8_t w1[] = {29, 18, 0, 1, 0, 12, 1, 6, 0, 0, 30, 32, 3, 16, 14, 0, 0, 1, 3, 150};
    uint8_t w2[] = {29, 18, 0, 2, 16, 14, 0, 0, 1, 2, 12, 12, 12, 217, 9};
    n = medtrum_encode(MEDTRUM_CMD_ACTIVATE, 0, c2, sizeof c2, frags, MEDTRUM_FRAGS_MAX);
    CHECK(n == 2);
    if (n == 2) {
        CHECK_BYTES(frags[0].data, frags[0].len, w1, sizeof w1);
        CHECK_BYTES(frags[1].data, frags[1].len, w2, sizeof w2);
    }

    /* MedtrumBasePacketTests.swift: three fragments. */
    uint8_t c3[] = {0,  12, 0,  3,  0,   0,  30,  32, 3, 16, 14, 0, 0, 1, 7,   0,   160, 2,
                    240, 96, 2,  104, 33,  2,  224, 225, 1, 192, 3,  2, 236, 36, 2, 100, 133, 2};
    uint8_t v1[] = {41, 18, 0, 1, 0, 12, 0, 3, 0, 0, 30, 32, 3, 16, 14, 0, 0, 1, 7, 173};
    uint8_t v2[] = {41, 18, 0, 2, 0, 160, 2, 240, 96, 2, 104, 33, 2, 224, 225, 1, 192, 3, 2, 253};
    uint8_t v3[] = {41, 18, 0, 3, 236, 36, 2, 100, 133, 2, 144, 163};
    n = medtrum_encode(MEDTRUM_CMD_ACTIVATE, 0, c3, sizeof c3, frags, MEDTRUM_FRAGS_MAX);
    CHECK(n == 3);
    if (n == 3) {
        CHECK_BYTES(frags[0].data, frags[0].len, v1, sizeof v1);
        CHECK_BYTES(frags[1].data, frags[1].len, v2, sizeof v2);
        CHECK_BYTES(frags[2].data, frags[2].len, v3, sizeof v3);
    }

    /* Refuses to overflow, and refuses an over-long body. */
    CHECK(medtrum_encode(MEDTRUM_CMD_ACTIVATE, 0, c3, sizeof c3, frags, 2) == -1);
    uint8_t big[MEDTRUM_CONTENT_MAX + 1];
    memset(big, 0, sizeof big);
    CHECK(medtrum_encode(MEDTRUM_CMD_ACTIVATE, 0, big, sizeof big, frags, MEDTRUM_FRAGS_MAX) == -1);
}

/* ---- response decoding ------------------------------------------------- */

static void test_decode_single(void)
{
    medtrum_packet_t p;

    /* AuthorizePacketTests.swift: deviceType 80, swVersion "12.1.3". */
    uint8_t auth[] = {11, 5, 0, 0, 0, 0, 0, 80, 12, 1, 3, 96};
    medtrum_decode_init(&p, MEDTRUM_CMD_AUTH_REQ);
    medtrum_decode(&p, auth, sizeof auth);
    CHECK(!p.failed);
    CHECK(medtrum_complete(&p));
    CHECK(medtrum_has_enough(&p, 10));
    CHECK(p.data_size == 11);
    CHECK(p.sequence == 0);
    CHECK(p.response_code == MEDTRUM_RESPONSE_OK);
    CHECK(p.len == 11);
    CHECK(p.data[7] == 80);
    CHECK(p.data[8] == 12 && p.data[9] == 1 && p.data[10] == 3);

    /* GetTimePacketTest.swift: the payload is 291040992 Medtrum seconds. */
    uint8_t gt[] = {10, 11, 0, 0, 0, 0, 224, 238, 88, 17, 137};
    medtrum_decode_init(&p, MEDTRUM_CMD_GET_TIME);
    medtrum_decode(&p, gt, sizeof gt);
    CHECK(!p.failed);
    CHECK(medtrum_complete(&p));
    CHECK(medtrum_has_enough(&p, 10));
    uint32_t secs = (uint32_t)p.data[6] | ((uint32_t)p.data[7] << 8) | ((uint32_t)p.data[8] << 16) |
                    ((uint32_t)p.data[9] << 24);
    CHECK(secs == 291040992u); /* 1679575392 - 1388534400 (the Medtrum epoch) */

    /* SetBasalProfilePacketTests.swift: STANDARD, 1.1 U/h, seq 3, patch 146. */
    uint8_t basal[] = {17, 21, 16, 0, 0, 0, 1, 22, 0, 3, 0, 146, 0, 224, 238, 88, 17, 44};
    medtrum_decode_init(&p, MEDTRUM_CMD_SET_BASAL_PROFILE);
    medtrum_decode(&p, basal, sizeof basal);
    CHECK(!p.failed);
    CHECK(medtrum_complete(&p));
    CHECK(medtrum_has_enough(&p, 17));
    CHECK(p.sequence == 0);
    CHECK(p.response_code == MEDTRUM_RESPONSE_OK);
    CHECK(p.data[6] == 1); /* BasalType.STANDARD */
    CHECK((p.data[7] | (p.data[8] << 8)) == 22); /* 22 * 0.05 = 1.1 U/h */
    CHECK((p.data[9] | (p.data[10] << 8)) == 3);
    CHECK((p.data[11] | (p.data[12] << 8)) == 146);

    /* ActivatePacketTests.swift: patchId {41,0,0,0}, sequence echoed as 1. */
    uint8_t act[] = {26, 18, 19, 1, 0, 0, 41, 0, 0, 0, 152, 91, 28, 17, 1, 30, 0, 1, 0, 41, 0, 224, 238, 88, 17,
                     184, 0};
    medtrum_decode_init(&p, MEDTRUM_CMD_ACTIVATE);
    medtrum_decode(&p, act, sizeof act);
    CHECK(!p.failed);
    CHECK(medtrum_complete(&p));
    CHECK(medtrum_has_enough(&p, 25));
    CHECK(p.sequence == 1);
    CHECK(p.response_code == MEDTRUM_RESPONSE_OK);
    CHECK_BYTES(p.data + 6, 4, ((uint8_t[]){41, 0, 0, 0}), 4);

    /* A response code other than 0 is preserved. */
    uint8_t skip[] = {11, 5, 0, 0, 0, 0, 0, 80, 12, 1, 3, 96};
    skip[4] = 0x00;
    skip[5] = 0x40; /* MEDTRUM_RESPONSE_SKIP, little-endian */
    skip[11] = medtrum_crc8(skip, 11);
    medtrum_decode_init(&p, MEDTRUM_CMD_AUTH_REQ);
    medtrum_decode(&p, skip, sizeof skip);
    CHECK(!p.failed);
    CHECK(p.response_code == MEDTRUM_RESPONSE_SKIP);
}

static void test_decode_fragmented(void)
{
    medtrum_packet_t p;

    /* SynchronizePacketTests.swift: two fragments, ending at .filled. */
    uint8_t s1[] = {23, 3, 4, 1, 0, 0, 2, 160, 5, 59, 15, 132, 59, 90, 1, 0, 50, 0, 110, 48};
    uint8_t s2[] = {23, 3, 4, 2, 6, 0, 0, 182, 224};
    medtrum_decode_init(&p, MEDTRUM_CMD_SYNCHRONIZE);
    medtrum_decode(&p, s1, sizeof s1);
    CHECK(!p.failed);
    CHECK(p.started);
    CHECK(p.data_size == 23);
    CHECK(p.sequence == 1);
    CHECK(p.response_code == MEDTRUM_RESPONSE_OK);
    CHECK(!medtrum_complete(&p));
    medtrum_decode(&p, s2, sizeof s2);
    CHECK(!p.failed);
    CHECK(medtrum_complete(&p));
    CHECK(medtrum_has_enough(&p, 9));
    CHECK(p.sequence == 2);
    CHECK(p.len == 23);
    CHECK(p.data[6] == 2); /* PatchState.filled */
}

static void test_decode_failures(void)
{
    medtrum_packet_t p;

    /* AuthorizePacketTests.swift: the response is one CRC byte short. */
    uint8_t short_auth[] = {11, 5, 0, 0, 0, 0, 0, 80, 12, 1, 3};
    medtrum_decode_init(&p, MEDTRUM_CMD_AUTH_REQ);
    medtrum_decode(&p, short_auth, sizeof short_auth);
    CHECK(p.failed);

    /* A first fragment shorter than the 6-byte header plus CRC. */
    uint8_t tiny[] = {11, 5, 0, 0, 0, 0};
    medtrum_decode_init(&p, MEDTRUM_CMD_AUTH_REQ);
    medtrum_decode(&p, tiny, sizeof tiny);
    CHECK(p.failed);

    /* The command byte does not match the packet being decoded. */
    uint8_t auth[] = {11, 5, 0, 0, 0, 0, 0, 80, 12, 1, 3, 96};
    medtrum_decode_init(&p, MEDTRUM_CMD_SYNCHRONIZE);
    medtrum_decode(&p, auth, sizeof auth);
    CHECK(p.failed);

    /* A continuation with the wrong fragment index. */
    uint8_t s1[] = {23, 3, 4, 1, 0, 0, 2, 160, 5, 59, 15, 132, 59, 90, 1, 0, 50, 0, 110, 48};
    uint8_t bad2[] = {23, 3, 4, 3, 6, 0, 0, 182, 224};
    bad2[8] = medtrum_crc8(bad2, 8);
    medtrum_decode_init(&p, MEDTRUM_CMD_SYNCHRONIZE);
    medtrum_decode(&p, s1, sizeof s1);
    medtrum_decode(&p, bad2, sizeof bad2);
    CHECK(p.failed);
}

/* ---- framing round trip ------------------------------------------------ */

static void test_round_trip(void)
{
    uint8_t content[] = {0,  12, 0,  3,  0,   0,  30,  32, 3, 16, 14, 0, 0, 1, 7,   0,   160, 2,
                         240, 96, 2,  104, 33,  2,  224, 225, 1, 192, 3,  2, 236, 36, 2, 100, 133, 2};
    medtrum_frag_t frags[MEDTRUM_FRAGS_MAX];
    int n = medtrum_encode(MEDTRUM_CMD_ACTIVATE, 7, content, sizeof content, frags, MEDTRUM_FRAGS_MAX);
    CHECK(n == 3);
    if (n != 3) {
        return;
    }

    medtrum_packet_t p;
    medtrum_decode_init(&p, MEDTRUM_CMD_ACTIVATE);
    for (int i = 0; i < n; i++) {
        medtrum_decode(&p, frags[i].data, frags[i].len);
    }
    CHECK(!p.failed);
    CHECK(medtrum_complete(&p));
    CHECK(p.len == sizeof content + 5); /* header + content + CRC */
    CHECK(p.data[0] == (uint8_t)(sizeof content + 5));
    CHECK(p.data[1] == MEDTRUM_CMD_ACTIVATE);
    CHECK(p.data[2] == 7);
    CHECK(memcmp(p.data + 4, content, sizeof content) == 0);
}

/* ---- the platform boundary --------------------------------------------- */

static void fake_random(void *ctx, uint8_t *p, size_t n)
{
    (void)ctx;
    memset(p, 0xaa, n);
}

static void test_io(void)
{
    medtrum_io_t io;
    memset(&io, 0, sizeof io);
    io.random = fake_random;

    uint8_t token[4];
    medtrum_gen_session_token(&io, token);
    CHECK_BYTES(token, 4, ((uint8_t[]){0xaa, 0xaa, 0xaa, 0xaa}), 4);

    /* With no platform randomness, the token is all zeroes, not undefined. */
    medtrum_gen_session_token(NULL, token);
    CHECK_BYTES(token, 4, ((uint8_t[]){0, 0, 0, 0}), 4);
}

int main(void)
{
    test_crc8();
    test_crypto();
    test_encode_single();
    test_encode_fragmented();
    test_decode_single();
    test_decode_fragmented();
    test_decode_failures();
    test_round_trip();
    test_io();

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
