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

/* ---- the status payload and the state machine -------------------------- */

static void test_status_payload(void)
{
    /* SynchronizeNotificationPacketTests.swift, reproduced byte for byte. */
    static const uint8_t basal_want[] = {32, 40, 64, 6,  25, 0,  14, 0,  84, 163,
                                         173, 17, 17, 64, 0, 152, 14, 0,  16};
    static const uint8_t storage_want[] = {32, 0, 17, 167, 0, 14, 0, 0, 0, 0, 0, 0};
    static const uint8_t bolus_want[] = {32, 34, 16, 0, 3, 0, 198, 12, 0, 0, 0, 0, 0};

    medtrum_status_t st;
    uint8_t out[MEDTRUM_STATUS_MAX];
    size_t n;

    /* testBasalDataProvided: absolute temp 0.85 U/h, reservoir 186.8 U. */
    memset(&st, 0, sizeof st);
    st.state = MEDTRUM_STATE_ACTIVE;
    st.mask = MEDTRUM_MASK_BASAL | MEDTRUM_MASK_RESERVOIR | MEDTRUM_MASK_UNUSED_AUTO_STATUS;
    st.basal_type = 6; /* BasalType.ABSOLUTE_TEMP */
    st.basal_sequence = 25;
    st.basal_patch_id = 14;
    st.basal_start_time = 296592212; /* 1685126612 - the Medtrum epoch */
    st.basal_rate = 17;
    st.basal_delivery = 4;
    st.reservoir = 3736;
    st.unused_auto_status[1] = 16;
    n = medtrum_status_payload(&st, out, sizeof out);
    CHECK_BYTES(out, n, basal_want, sizeof basal_want);

    /* testSequenceDataProvided: storage sequence 167, patch 14. */
    memset(&st, 0, sizeof st);
    st.state = MEDTRUM_STATE_ACTIVE;
    st.mask = MEDTRUM_MASK_STORAGE | MEDTRUM_MASK_UNUSED_CGM;
    st.storage_sequence = 167;
    st.storage_patch_id = 14;
    n = medtrum_status_payload(&st, out, sizeof out);
    CHECK_BYTES(out, n, storage_want, sizeof storage_want);

    /* testBolusProgress: 0.15 U delivered, reservoir 163.5 U. */
    memset(&st, 0, sizeof st);
    st.state = MEDTRUM_STATE_ACTIVE;
    st.mask = MEDTRUM_MASK_NORMAL_BOLUS | MEDTRUM_MASK_RESERVOIR | MEDTRUM_MASK_UNUSED_CGM;
    st.bolus_delivered = 3;
    st.reservoir = 3270;
    n = medtrum_status_payload(&st, out, sizeof out);
    CHECK_BYTES(out, n, bolus_want, sizeof bolus_want);

    /* testStateOnlyNotification: state, an empty mask, nothing else. */
    memset(&st, 0, sizeof st);
    st.state = MEDTRUM_STATE_RESERVOIR_EMPTY;
    n = medtrum_status_payload(&st, out, sizeof out);
    CHECK(n == 3);
    CHECK(out[0] == 98 && out[1] == 0 && out[2] == 0);

    /* Refuses a buffer that is too small. */
    CHECK(medtrum_status_payload(&st, out, 2) == 0);
}

static void test_patch_state(void)
{
    CHECK(medtrum_state_is_setup(MEDTRUM_STATE_IDLE));
    CHECK(medtrum_state_is_setup(MEDTRUM_STATE_FILLED));
    CHECK(medtrum_state_is_setup(MEDTRUM_STATE_PRIMING));
    CHECK(medtrum_state_is_setup(MEDTRUM_STATE_PRIMED));
    CHECK(!medtrum_state_is_setup(MEDTRUM_STATE_ACTIVE));
    CHECK(medtrum_state_is_running(MEDTRUM_STATE_ACTIVE));
    CHECK(medtrum_state_is_running(MEDTRUM_STATE_SUSPENDED));
    CHECK(!medtrum_state_is_running(MEDTRUM_STATE_FILLED));
    CHECK(medtrum_state_is_suspended(MEDTRUM_STATE_SUSPENDED));
    CHECK(medtrum_state_is_suspended(MEDTRUM_STATE_AUTO_SUSPENDED));
    CHECK(!medtrum_state_is_suspended(MEDTRUM_STATE_ACTIVE));
    CHECK(medtrum_state_is_fault(MEDTRUM_STATE_OCCLUSION));
    CHECK(medtrum_state_is_fault(MEDTRUM_STATE_PATCH_FAULT));
    CHECK(medtrum_state_is_fault(MEDTRUM_STATE_RESERVOIR_EMPTY));
    CHECK(medtrum_state_is_terminated(MEDTRUM_STATE_STOPPED));
    CHECK(medtrum_state_is_delivery_halted(MEDTRUM_STATE_SUSPENDED));
    CHECK(medtrum_state_is_delivery_halted(MEDTRUM_STATE_RESERVOIR_EMPTY));
    CHECK(!medtrum_state_is_delivery_halted(MEDTRUM_STATE_ACTIVE));
    CHECK(medtrum_state_before_priming(MEDTRUM_STATE_FILLED));
    CHECK(!medtrum_state_before_priming(MEDTRUM_STATE_PRIMED));
    CHECK(medtrum_state_completed_priming(MEDTRUM_STATE_PRIMED));
}

/* ---- the connect session ----------------------------------------------- */

/* Fills a patch with the status of SynchronizePacketTests' fragment vector. */
static void patch_filled(medtrum_patch_t *p)
{
    medtrum_patch_init(p, (uint8_t[]){0, 0, 0, 0});
    p->authenticated = true;
    p->status.state = MEDTRUM_STATE_FILLED;
    p->status.mask = MEDTRUM_MASK_RESERVOIR | MEDTRUM_MASK_BATTERY | MEDTRUM_MASK_STORAGE | MEDTRUM_MASK_AGE;
    p->status.reservoir = 3899;
    p->status.battery = 5913476;
    p->status.storage_sequence = 1;
    p->status.storage_patch_id = 50;
    p->status.patch_age = 1646;
    p->sync_tail = 182;
}

/* SynchronizePacketTests.swift: testResponseGivenSplittedPackets. */
static void test_sync_fragment_vector(void)
{
    static const uint8_t f1[] = {23, 3, 4, 1, 0, 0, 2, 160, 5, 59, 15, 132, 59, 90, 1, 0, 50, 0, 110, 48};
    static const uint8_t f2[] = {23, 3, 4, 2, 6, 0, 0, 182, 224};

    medtrum_patch_t p;
    patch_filled(&p);

    medtrum_frag_t resp[MEDTRUM_FRAGS_MAX];
    int n = medtrum_patch_handle(&p, MEDTRUM_CMD_SYNCHRONIZE, 4, NULL, 0, resp, MEDTRUM_FRAGS_MAX);
    CHECK(n == 2);
    if (n == 2) {
        CHECK_BYTES(resp[0].data, resp[0].len, f1, sizeof f1);
        CHECK_BYTES(resp[1].data, resp[1].len, f2, sizeof f2);
    }
}

/* SynchronizePacketTests.swift: testResponseContainingSyncData, with the
 * state the test asserts. */
static void test_sync_full_vector(void)
{
    static const uint8_t f1[] = {47, 3, 3, 1, 0,  0,  32, 238, 13, 128, 5,  0,  128, 0,  0,  6, 25, 0, 14, 150};
    static const uint8_t f2[] = {47, 3, 3, 2, 0,  84, 163, 173, 17, 17, 64, 0,  152, 14, 248, 137, 173, 17, 240, 202};
    static const uint8_t f3[] = {47, 3, 3, 3, 11, 90, 26, 0, 14, 0, 187, 31, 0, 0, 140, 14, 200, 238};
    static const uint8_t total[] = {47, 3, 3, 1, 0,  0,  32, 238, 13, 128, 5,  0,  128, 0,  0,  6,  25, 0, 14, 0,
                                    84, 163, 173, 17, 17, 64, 0,  152, 14, 248, 137, 173, 17, 240, 11, 90, 26, 0, 14, 0,
                                    187, 31, 0, 0, 140, 14, 200};

    medtrum_patch_t p;
    medtrum_patch_init(&p, (uint8_t[]){0, 0, 0, 0});
    p.authenticated = true;
    p.status.state = MEDTRUM_STATE_ACTIVE;
    p.status.mask = MEDTRUM_MASK_NORMAL_BOLUS | MEDTRUM_MASK_EXTENDED_BOLUS | MEDTRUM_MASK_BASAL |
                    MEDTRUM_MASK_RESERVOIR | MEDTRUM_MASK_START_TIME | MEDTRUM_MASK_BATTERY |
                    MEDTRUM_MASK_STORAGE | MEDTRUM_MASK_AGE | MEDTRUM_MASK_MAGNETO_PLACE;
    p.status.bolus_completed = true;
    p.status.bolus_delivered = 5;
    p.status.extended_bolus[0] = 128;
    p.status.basal_type = 6;
    p.status.basal_sequence = 25;
    p.status.basal_patch_id = 14;
    p.status.basal_start_time = 296592212;
    p.status.basal_rate = 17;
    p.status.basal_delivery = 4;
    p.status.reservoir = 3736;
    p.status.start_time = 296585720;
    p.status.battery = 5901296;
    p.status.storage_sequence = 26;
    p.status.storage_patch_id = 14;
    p.status.patch_age = 8123;
    p.status.magneto_placement = 3724;
    p.sync_tail = 200;

    medtrum_frag_t resp[MEDTRUM_FRAGS_MAX];
    int n = medtrum_patch_handle(&p, MEDTRUM_CMD_SYNCHRONIZE, 3, NULL, 0, resp, MEDTRUM_FRAGS_MAX);
    CHECK(n == 3);
    if (n == 3) {
        CHECK_BYTES(resp[0].data, resp[0].len, f1, sizeof f1);
        CHECK_BYTES(resp[1].data, resp[1].len, f2, sizeof f2);
        CHECK_BYTES(resp[2].data, resp[2].len, f3, sizeof f3);
    }

    medtrum_packet_t pkt;
    medtrum_decode_init(&pkt, MEDTRUM_CMD_SYNCHRONIZE);
    for (int i = 0; i < n; i++) {
        medtrum_decode(&pkt, resp[i].data, resp[i].len);
    }
    CHECK(!pkt.failed);
    CHECK(medtrum_complete(&pkt));
    CHECK_BYTES(pkt.data, pkt.len, total, sizeof total);
    CHECK(pkt.data[6] == MEDTRUM_STATE_ACTIVE);
}

/* GetRecord (99): the history answer the watch's cold sweep needs. The
 * simulator has no record log; each sequence the status advertises comes back
 * as a TDD record with the request's patch id and index. */
static void test_get_record(void)
{
    medtrum_patch_t p;
    medtrum_patch_init(&p, (uint8_t[]){1, 2, 3, 4});
    p.authenticated = true;
    p.status.state = MEDTRUM_STATE_ACTIVE;
    p.status.storage_sequence = 7;
    p.status.storage_patch_id = 0x1234;
    p.delivery.patch_id[0] = 0x34;
    p.delivery.patch_id[1] = 0x12;
    p.delivery.patch_time = 0x11223344;
    p.delivery.timezone_minutes = 60;

    uint8_t req[4] = {3, 0, 0x34, 0x12}; /* index 3, patch 0x1234 */
    medtrum_frag_t resp[MEDTRUM_FRAGS_MAX];
    int n = medtrum_patch_handle(&p, MEDTRUM_CMD_GET_RECORD, 9, req, sizeof req, resp, MEDTRUM_FRAGS_MAX);
    CHECK(n > 0);

    medtrum_packet_t pkt;
    medtrum_decode_init(&pkt, MEDTRUM_CMD_GET_RECORD);
    for (int i = 0; i < n; i++) {
        medtrum_decode(&pkt, resp[i].data, resp[i].len);
    }
    CHECK(!pkt.failed && medtrum_complete(&pkt));
    CHECK(pkt.response_code == MEDTRUM_RESPONSE_OK);
    CHECK(pkt.len >= 26);
    if (pkt.len >= 26) {
        CHECK(pkt.data[6] == 0xAA);          /* valid record header */
        CHECK(pkt.data[8] == 9);             /* TDD record */
        CHECK(pkt.data[14] == 0x34 && pkt.data[15] == 0x12); /* patch id */
        CHECK(pkt.data[16] == 3 && pkt.data[17] == 0);       /* sequence */
        CHECK(pkt.data[18] == 0x44 && pkt.data[19] == 0x33 && pkt.data[20] == 0x22 && pkt.data[21] == 0x11);
        CHECK(pkt.data[22] == 0x3c && pkt.data[23] == 0);    /* zone +60 min */
    }
}

/* A simulated phone: AUTH_REQ -> SYNCHRONIZE -> SUBSCRIBE. */
static void test_session(void)
{
    static const uint8_t auth_want[] = {11, 5, 0, 0, 0, 0, 2, 80, 12, 1, 3, 49};
    static const uint8_t sync1_want[] = {23, 3, 1, 1, 0, 0, 2, 160, 5, 59, 15, 132, 59, 90, 1, 0, 50, 0, 110, 132};
    static const uint8_t sync2_want[] = {23, 3, 1, 2, 6, 0, 0, 182, 223};
    static const uint8_t sub_want[] = {7, 4, 2, 0, 0, 0, 2, 177};

    medtrum_patch_t p;
    uint8_t sn[4] = {217, 249, 118, 170}; /* the app's serial */
    patch_filled(&p);
    memcpy(p.sn, sn, 4);
    p.device_type = 80;
    p.sw_version[0] = 12;
    p.sw_version[1] = 1;
    p.sw_version[2] = 3;

    /* The phone's AUTH_REQ: role 2, its token, key = genKey(reversed SN). */
    uint8_t reversed[4] = {sn[3], sn[2], sn[1], sn[0]};
    uint8_t key[4];
    medtrum_gen_key(reversed, key);
    uint8_t content[9] = {2, 155, 2, 0, 0, key[0], key[1], key[2], key[3]};

    medtrum_frag_t resp[MEDTRUM_FRAGS_MAX];
    int n = medtrum_patch_handle(&p, MEDTRUM_CMD_AUTH_REQ, 0, content, sizeof content, resp, MEDTRUM_FRAGS_MAX);
    CHECK(n == 1);
    CHECK(p.authenticated);
    CHECK_BYTES(p.session_token, 4, ((uint8_t[]){155, 2, 0, 0}), 4);
    if (n == 1) {
        CHECK_BYTES(resp[0].data, resp[0].len, auth_want, sizeof auth_want);
    }

    /* SYNCHRONIZE, sequence 1. */
    n = medtrum_patch_handle(&p, MEDTRUM_CMD_SYNCHRONIZE, 1, NULL, 0, resp, MEDTRUM_FRAGS_MAX);
    CHECK(n == 2);
    if (n == 2) {
        CHECK_BYTES(resp[0].data, resp[0].len, sync1_want, sizeof sync1_want);
        CHECK_BYTES(resp[1].data, resp[1].len, sync2_want, sizeof sync2_want);
    }
    medtrum_packet_t pkt;
    medtrum_decode_init(&pkt, MEDTRUM_CMD_SYNCHRONIZE);
    for (int i = 0; i < n; i++) {
        medtrum_decode(&pkt, resp[i].data, resp[i].len);
    }
    CHECK(!pkt.failed);
    CHECK(medtrum_complete(&pkt));
    CHECK(pkt.response_code == MEDTRUM_RESPONSE_OK);
    CHECK(pkt.data[6] == MEDTRUM_STATE_FILLED);
    CHECK((pkt.data[7] | (pkt.data[8] << 8)) == p.status.mask);
    CHECK((pkt.data[9] | (pkt.data[10] << 8)) == 3899);

    /* SUBSCRIBE, sequence 2, with the 0x0FFF mask. */
    uint8_t sub[2] = {0xff, 0x0f};
    n = medtrum_patch_handle(&p, MEDTRUM_CMD_SUBSCRIBE, 2, sub, sizeof sub, resp, MEDTRUM_FRAGS_MAX);
    CHECK(n == 1);
    CHECK(p.subscribed);
    CHECK(p.subscribe_mask == 0x0fff);
    if (n == 1) {
        CHECK_BYTES(resp[0].data, resp[0].len, sub_want, sizeof sub_want);
    }

    /* The READ-characteristic notification is the bare status payload. */
    uint8_t note[MEDTRUM_STATUS_MAX];
    size_t note_len = medtrum_patch_notification(&p, note, sizeof note);
    CHECK(note_len == 16); /* state + mask + reservoir + battery + storage + age */
    CHECK(note[0] == MEDTRUM_STATE_FILLED);
    CHECK((note[1] | (note[2] << 8)) == p.status.mask);
}

static void test_session_failures(void)
{
    medtrum_patch_t p;
    medtrum_patch_init(&p, (uint8_t[]){217, 249, 118, 170});
    p.device_type = 80;
    medtrum_frag_t resp[MEDTRUM_FRAGS_MAX];
    medtrum_packet_t pkt;

    /* A wrong key is refused with responseCode 7. */
    uint8_t bad[9] = {2, 155, 2, 0, 0, 0, 0, 0, 0};
    int n = medtrum_patch_handle(&p, MEDTRUM_CMD_AUTH_REQ, 0, bad, sizeof bad, resp, MEDTRUM_FRAGS_MAX);
    CHECK(n == 1);
    CHECK(!p.authenticated);
    if (n == 1) {
        medtrum_decode_init(&pkt, MEDTRUM_CMD_AUTH_REQ);
        medtrum_decode(&pkt, resp[0].data, resp[0].len);
        CHECK(!pkt.failed);
        CHECK(pkt.response_code == MEDTRUM_RESPONSE_BAD_TOKEN);
    }

    /* SYNCHRONIZE before authentication is refused too. */
    n = medtrum_patch_handle(&p, MEDTRUM_CMD_SYNCHRONIZE, 1, NULL, 0, resp, MEDTRUM_FRAGS_MAX);
    CHECK(n == 1);
    if (n == 1) {
        medtrum_decode_init(&pkt, MEDTRUM_CMD_SYNCHRONIZE);
        medtrum_decode(&pkt, resp[0].data, resp[0].len);
        CHECK(pkt.response_code == MEDTRUM_RESPONSE_BAD_TOKEN);
    }

    /* A command this build does not implement is left for the caller. */
    CHECK(medtrum_patch_handle(&p, MEDTRUM_CMD_GET_DEVICE_TYPE, 0, NULL, 0, resp, MEDTRUM_FRAGS_MAX) == -1);

    /* A response payload that will not fit the size byte is refused. */
    uint8_t big[250];
    memset(big, 0, sizeof big);
    CHECK(medtrum_encode_response(MEDTRUM_CMD_SYNCHRONIZE, 0, 0, big, sizeof big, resp, MEDTRUM_FRAGS_MAX) ==
          -1);
}

/* ---- the delivery lifecycle -------------------------------------------- */

/* Decodes a response and compares its payload (after the 6-byte prefix). */
static void check_payload(const medtrum_frag_t *frags, int n, uint8_t command, uint16_t rc,
                          const uint8_t *want, size_t want_len)
{
    medtrum_packet_t pkt;
    medtrum_decode_init(&pkt, command);
    for (int i = 0; i < n; i++) {
        medtrum_decode(&pkt, frags[i].data, frags[i].len);
    }
    CHECK(!pkt.failed);
    CHECK(medtrum_complete(&pkt));
    CHECK(pkt.response_code == rc);
    CHECK(pkt.len >= 6);
    if (pkt.len >= 6) {
        CHECK_BYTES(pkt.data + 6, pkt.len - 6, want, want_len);
    }
}

static void test_lifecycle(void)
{
    /* Expected response payloads, computed from the values below. */
    static const uint8_t prime_pl[] = {3};
    static const uint8_t activate_pl[] = {41, 0, 0, 0, 128, 81, 1, 0, 1, 20, 0, 1, 0, 41, 0, 128, 81, 1, 0};
    static const uint8_t note_active[] = {32, 232, 5, 1, 1, 0, 41, 0, 128, 81, 1, 0, 20, 0, 0, 152, 14,
                                          128, 81, 1, 0, 240, 11, 90, 1, 0, 41, 0, 0, 0, 0, 0};
    static const uint8_t setbasal_pl[] = {1, 20, 0, 2, 0, 41, 0, 128, 81, 1, 0};
    static const uint8_t settemp_pl[] = {6, 25, 0, 3, 0, 41, 0, 128, 81, 1, 0};
    static const uint8_t canceltemp_pl[] = {1, 20, 0, 4, 0, 41, 0, 0xbc, 0x51, 1, 0}; /* 86460 s */
    static const uint8_t state_active[] = {32};
    static const uint8_t state_suspended[] = {69};
    static const uint8_t gettime_pl[] = {188, 81, 1, 0};
    static const uint8_t stop_pl[] = {1, 0, 41, 0};

    medtrum_patch_t p;
    uint8_t sn[4] = {217, 249, 118, 170};
    medtrum_patch_init(&p, sn);
    p.device_type = 80;
    p.sw_version[0] = 12;
    p.sw_version[1] = 1;
    p.sw_version[2] = 3;
    medtrum_patch_set_patch_id(&p, (uint8_t[]){41, 0, 0, 0});
    medtrum_patch_set_reservoir(&p, 3736);  /* 186.8 U */
    medtrum_patch_set_battery(&p, 5901296); /* 5.97 V / 2.81 V */

    medtrum_frag_t resp[MEDTRUM_FRAGS_MAX];
    int n;
    uint8_t seq = 0;

    /* The patch starts filled, with the time set. */
    medtrum_patch_set_state(&p, MEDTRUM_STATE_FILLED);
    uint8_t set_time[] = {2, 0x80, 0x51, 0x01, 0x00}; /* 86400 Medtrum seconds */
    n = medtrum_patch_handle(&p, MEDTRUM_CMD_SET_TIME, seq++, set_time, sizeof set_time, resp,
                             MEDTRUM_FRAGS_MAX);
    CHECK(n == 1);
    CHECK(p.delivery.patch_time == 86400);

    /* PRIME: filled -> priming. */
    n = medtrum_patch_handle(&p, MEDTRUM_CMD_PRIME, seq++, NULL, 0, resp, MEDTRUM_FRAGS_MAX);
    CHECK(n == 1);
    check_payload(resp, n, MEDTRUM_CMD_PRIME, MEDTRUM_RESPONSE_OK, prime_pl, sizeof prime_pl);
    CHECK(p.status.state == MEDTRUM_STATE_PRIMING);
    CHECK(p.status.mask & MEDTRUM_MASK_SETUP);

    /* ACTIVATE: a one-entry schedule of 1.0 U/h, 40 U/h and 180 U/day limits. */
    uint8_t activate[] = {0, 12, 1, 6, 0, 0, 30, 32, 3, 16, 14, 0, 0, 1, 1, 0x00, 0x40, 0x01};
    n = medtrum_patch_handle(&p, MEDTRUM_CMD_ACTIVATE, seq++, activate, sizeof activate, resp,
                             MEDTRUM_FRAGS_MAX);
    CHECK(n == 2); /* a 25-byte response fragments */
    check_payload(resp, n, MEDTRUM_CMD_ACTIVATE, MEDTRUM_RESPONSE_OK, activate_pl, sizeof activate_pl);
    CHECK(p.status.state == MEDTRUM_STATE_ACTIVE);
    CHECK(p.delivery.patch_start == 86400);
    CHECK(p.delivery.scheduled_rate == 20);
    CHECK(!(p.status.mask & MEDTRUM_MASK_SETUP));

    /* The status notification reflects the activation. */
    uint8_t note[MEDTRUM_STATUS_MAX];
    size_t note_len = medtrum_patch_notification(&p, note, sizeof note);
    CHECK_BYTES(note, note_len, note_active, sizeof note_active);

    /* SET_BASAL_PROFILE: the same schedule, sequence advances. */
    uint8_t profile[] = {1, 1, 0x00, 0x40, 0x01};
    n = medtrum_patch_handle(&p, MEDTRUM_CMD_SET_BASAL_PROFILE, seq++, profile, sizeof profile, resp,
                             MEDTRUM_FRAGS_MAX);
    CHECK(n == 1);
    check_payload(resp, n, MEDTRUM_CMD_SET_BASAL_PROFILE, MEDTRUM_RESPONSE_OK, setbasal_pl,
                  sizeof setbasal_pl);
    CHECK(p.status.basal_sequence == 2);

    /* SET_TEMP_BASAL: 1.25 U/h for an hour. */
    uint8_t temp[] = {6, 0x19, 0x00, 0x3C, 0x00};
    n = medtrum_patch_handle(&p, MEDTRUM_CMD_SET_TEMP_BASAL, seq++, temp, sizeof temp, resp,
                             MEDTRUM_FRAGS_MAX);
    CHECK(n == 1);
    check_payload(resp, n, MEDTRUM_CMD_SET_TEMP_BASAL, MEDTRUM_RESPONSE_OK, settemp_pl, sizeof settemp_pl);
    CHECK(p.delivery.temp_rate == 25);
    CHECK(p.delivery.temp_end == 90000);
    CHECK(p.status.basal_type == MEDTRUM_BASAL_ABSOLUTE_TEMP);

    /* SET_BOLUS: 1.40 U. */
    uint8_t bolus[] = {1, 0x1C, 0x00, 0};
    n = medtrum_patch_handle(&p, MEDTRUM_CMD_SET_BOLUS, seq++, bolus, sizeof bolus, resp,
                             MEDTRUM_FRAGS_MAX);
    CHECK(n == 1);
    check_payload(resp, n, MEDTRUM_CMD_SET_BOLUS, MEDTRUM_RESPONSE_OK, state_active, sizeof state_active);
    CHECK(p.delivery.bolus_active);
    CHECK(p.delivery.bolus_total == 28);
    CHECK(p.status.mask & MEDTRUM_MASK_NORMAL_BOLUS);

    /* Ticking advances the bolus at 1.5 U/min and the patch age. */
    medtrum_patch_tick(&p, 1000000); /* records the clock */
    medtrum_patch_tick(&p, 1020000); /* +20 s: 10 pulses */
    CHECK(p.delivery.patch_time == 86420);
    CHECK(p.status.bolus_delivered == 10);
    medtrum_patch_tick(&p, 1060000); /* +40 s: the bolus completes */
    CHECK(p.delivery.patch_time == 86460);
    CHECK(p.status.bolus_delivered == 28);
    CHECK(p.status.bolus_completed);
    CHECK(!p.delivery.bolus_active);
    CHECK(p.status.patch_age == 60);

    /* CANCEL_TEMP_BASAL goes back to the schedule. */
    n = medtrum_patch_handle(&p, MEDTRUM_CMD_CANCEL_TEMP_BASAL, seq++, NULL, 0, resp, MEDTRUM_FRAGS_MAX);
    CHECK(n == 1);
    check_payload(resp, n, MEDTRUM_CMD_CANCEL_TEMP_BASAL, MEDTRUM_RESPONSE_OK, canceltemp_pl,
                  sizeof canceltemp_pl);
    CHECK(p.delivery.temp_end == 0);
    CHECK(p.status.basal_type == MEDTRUM_BASAL_STANDARD);

    /* CANCEL_BOLUS. */
    uint8_t cancel_bolus[] = {1};
    n = medtrum_patch_handle(&p, MEDTRUM_CMD_CANCEL_BOLUS, seq++, cancel_bolus, sizeof cancel_bolus, resp,
                             MEDTRUM_FRAGS_MAX);
    CHECK(n == 1);
    CHECK(!(p.status.mask & MEDTRUM_MASK_NORMAL_BOLUS));

    /* SUSPEND then RESUME. */
    uint8_t suspend[] = {3, 120};
    n = medtrum_patch_handle(&p, MEDTRUM_CMD_SUSPEND_PUMP, seq++, suspend, sizeof suspend, resp,
                             MEDTRUM_FRAGS_MAX);
    CHECK(n == 1);
    check_payload(resp, n, MEDTRUM_CMD_SUSPEND_PUMP, MEDTRUM_RESPONSE_OK, state_suspended,
                  sizeof state_suspended);
    CHECK(p.status.state == MEDTRUM_STATE_SUSPENDED);
    CHECK(p.status.mask & MEDTRUM_MASK_SUSPEND);

    n = medtrum_patch_handle(&p, MEDTRUM_CMD_RESUME_PUMP, seq++, NULL, 0, resp, MEDTRUM_FRAGS_MAX);
    CHECK(n == 1);
    check_payload(resp, n, MEDTRUM_CMD_RESUME_PUMP, MEDTRUM_RESPONSE_OK, state_active, sizeof state_active);
    CHECK(p.status.state == MEDTRUM_STATE_ACTIVE);
    CHECK(!(p.status.mask & MEDTRUM_MASK_SUSPEND));

    /* SET_PATCH updates the limits and the alarm setting. */
    uint8_t set_patch[] = {1, 0x20, 0x03, 0x10, 0x0E, 0, 0, 12, 0, 0, 30};
    n = medtrum_patch_handle(&p, MEDTRUM_CMD_SET_PATCH, seq++, set_patch, sizeof set_patch, resp,
                             MEDTRUM_FRAGS_MAX);
    CHECK(n == 1);
    check_payload(resp, n, MEDTRUM_CMD_SET_PATCH, MEDTRUM_RESPONSE_OK, state_active, sizeof state_active);
    CHECK(p.delivery.max_hourly == 800);
    CHECK(p.delivery.max_daily == 3600);

    /* An alarm is visible until CLEAR_ALARM. */
    medtrum_patch_set_alarm(&p, MEDTRUM_ALARM_LOW_BATTERY);
    CHECK(p.status.mask & MEDTRUM_MASK_ALARM);
    uint8_t clear[] = {4, 0};
    n = medtrum_patch_handle(&p, MEDTRUM_CMD_CLEAR_ALARM, seq++, clear, sizeof clear, resp,
                             MEDTRUM_FRAGS_MAX);
    CHECK(n == 1);
    CHECK(!(p.status.mask & MEDTRUM_MASK_ALARM));

    /* GET_TIME reflects the ticking clock. */
    n = medtrum_patch_handle(&p, MEDTRUM_CMD_GET_TIME, seq++, NULL, 0, resp, MEDTRUM_FRAGS_MAX);
    CHECK(n == 1);
    check_payload(resp, n, MEDTRUM_CMD_GET_TIME, MEDTRUM_RESPONSE_OK, gettime_pl, sizeof gettime_pl);

    /* SET_TIME_ZONE and SET_TIME move the clock. */
    uint8_t tz[] = {0x3C, 0x00, 0xA0, 0x86, 0x01, 0x00}; /* +60 min, 100000 s */
    n = medtrum_patch_handle(&p, MEDTRUM_CMD_SET_TIME_ZONE, seq++, tz, sizeof tz, resp, MEDTRUM_FRAGS_MAX);
    CHECK(n == 1);
    CHECK(p.delivery.timezone_minutes == 60);
    CHECK(p.delivery.patch_time == 100000);

    /* A negative offset survives the +65536 wire encoding. */
    uint8_t tz_neg[] = {0xD4, 0xFE, 0xA0, 0x86, 0x01, 0x00}; /* -300 min */
    n = medtrum_patch_handle(&p, MEDTRUM_CMD_SET_TIME_ZONE, seq++, tz_neg, sizeof tz_neg, resp,
                             MEDTRUM_FRAGS_MAX);
    CHECK(n == 1);
    CHECK(p.delivery.timezone_minutes == -300);

    uint8_t set_time2[] = {2, 0x40, 0x0D, 0x03, 0x00}; /* 200000 s */
    n = medtrum_patch_handle(&p, MEDTRUM_CMD_SET_TIME, seq++, set_time2, sizeof set_time2, resp,
                             MEDTRUM_FRAGS_MAX);
    CHECK(n == 1);
    CHECK(p.delivery.patch_time == 200000);

    /* STOP_PATCH reports the storage sequence and patch id, then stops. */
    n = medtrum_patch_handle(&p, MEDTRUM_CMD_STOP_PATCH, seq++, NULL, 0, resp, MEDTRUM_FRAGS_MAX);
    CHECK(n == 1);
    check_payload(resp, n, MEDTRUM_CMD_STOP_PATCH, MEDTRUM_RESPONSE_OK, stop_pl, sizeof stop_pl);
    CHECK(p.status.state == MEDTRUM_STATE_STOPPED);
    CHECK(p.status.mask == 0);
}

static void test_fault_mapping(void)
{
    medtrum_patch_t p;
    medtrum_patch_init(&p, (uint8_t[]){1, 2, 3, 4});
    medtrum_patch_set_state(&p, MEDTRUM_STATE_ACTIVE);
    CHECK(medtrum_state_is_running(p.status.state));

    medtrum_patch_set_fault(&p, MEDTRUM_STATE_OCCLUSION);
    CHECK(p.status.state == MEDTRUM_STATE_OCCLUSION);
    CHECK(medtrum_state_is_fault(p.status.state));
    CHECK(medtrum_state_is_terminated(p.status.state));
    CHECK(medtrum_state_is_delivery_halted(p.status.state));

    medtrum_patch_set_alarm(&p, MEDTRUM_ALARM_LOW_BATTERY | MEDTRUM_ALARM_LOW_RESERVOIR);
    CHECK(p.status.alarm_flags == 3);
    CHECK(p.status.mask & MEDTRUM_MASK_ALARM);
    medtrum_patch_set_alarm(&p, 0);
    CHECK(!(p.status.mask & MEDTRUM_MASK_ALARM));

    /* PRIME refuses a patch that is not filled. */
    medtrum_patch_t q;
    medtrum_patch_init(&q, (uint8_t[]){0, 0, 0, 0});
    medtrum_frag_t resp[MEDTRUM_FRAGS_MAX];
    int n = medtrum_patch_handle(&q, MEDTRUM_CMD_PRIME, 0, NULL, 0, resp, MEDTRUM_FRAGS_MAX);
    CHECK(n == 1);
    if (n == 1) {
        medtrum_packet_t pkt;
        medtrum_decode_init(&pkt, MEDTRUM_CMD_PRIME);
        medtrum_decode(&pkt, resp[0].data, resp[0].len);
        CHECK(pkt.response_code == MEDTRUM_RESPONSE_BAD_STATE);
    }
}

/* ---- request reassembly ------------------------------------------------- */

static void test_request_parser(void)
{
    medtrum_request_t r;

    /* The 6-byte empty-content single packets: size + cmd + seq + pkg + CRC +
     * the trailing 0x00 marker. Rejecting these was the P0 that stalled every
     * connect (SYNCHRONIZE and friends). */
    static const uint8_t sync[] = {0x05, 0x03, 0x00, 0x00, 0xfc, 0x00};
    static const uint8_t get_time[] = {0x05, 0x0b, 0x00, 0x00, 0xa1, 0x00};
    static const uint8_t prime[] = {0x05, 0x10, 0x00, 0x00, 0xa4, 0x00};
    static const uint8_t resume[] = {0x05, 0x1d, 0x00, 0x00, 0x44, 0x00};
    static const uint8_t stop[] = {0x05, 0x1f, 0x00, 0x00, 0xf8, 0x00};
    static const uint8_t cancel_temp[] = {0x05, 0x19, 0x00, 0x00, 0xa7, 0x00};
    const struct {
        const uint8_t *p;
        uint8_t cmd;
    } empty[] = {
        {sync, 3}, {get_time, 11}, {prime, 16}, {resume, 29}, {stop, 31}, {cancel_temp, 25},
    };
    for (size_t i = 0; i < sizeof empty / sizeof empty[0]; i++) {
        medtrum_request_init(&r);
        CHECK(medtrum_request_feed(&r, empty[i].p, 6));
        CHECK(r.command == empty[i].cmd);
        CHECK(r.sequence == 0);
        CHECK(r.content_len == 0);
    }

    /* The 15-byte AUTH_REQ carries 9 content bytes. */
    static const uint8_t auth[] = {0x0e, 0x05, 0x00, 0x00, 0x02, 0x9b, 0x02, 0x00,
                                   0x00, 0x48, 0xcd, 0x12, 0x08, 0x24, 0x00};
    medtrum_request_init(&r);
    CHECK(medtrum_request_feed(&r, auth, sizeof auth));
    CHECK(r.command == 5);
    CHECK(r.content_len == 9);
    CHECK_BYTES(r.content, r.content_len, auth + 4, 9);

    /* A 2-fragment ACTIVATE reassembles into its 24-byte content. */
    static const uint8_t act1[] = {29, 18, 0, 1, 0, 12, 1, 6, 0, 0, 30, 32, 3, 16, 14, 0, 0, 1, 3, 150};
    static const uint8_t act2[] = {29, 18, 0, 2, 16, 14, 0, 0, 1, 2, 12, 12, 12, 217, 9};
    medtrum_request_init(&r);
    CHECK(!medtrum_request_feed(&r, act1, sizeof act1));
    CHECK(medtrum_request_feed(&r, act2, sizeof act2));
    CHECK(r.command == 18);
    CHECK(r.sequence == 0);
    CHECK(r.content_len == 24);
    CHECK(memcmp(r.content, act1 + 4, 15) == 0); /* content[0..15] */
    CHECK(memcmp(r.content + 15, act2 + 4, 9) == 0); /* content[15..24] */

    /* A 3-fragment ACTIVATE. */
    static const uint8_t a3_1[] = {41, 18, 0, 1, 0, 12, 0, 3, 0, 0, 30, 32, 3, 16, 14, 0, 0, 1, 7, 173};
    static const uint8_t a3_2[] = {41, 18, 0, 2, 0, 160, 2, 240, 96, 2, 104, 33, 2, 224, 225, 1, 192, 3, 2, 253};
    static const uint8_t a3_3[] = {41, 18, 0, 3, 236, 36, 2, 100, 133, 2, 144, 163};
    medtrum_request_init(&r);
    CHECK(!medtrum_request_feed(&r, a3_1, sizeof a3_1));
    CHECK(!medtrum_request_feed(&r, a3_2, sizeof a3_2));
    CHECK(medtrum_request_feed(&r, a3_3, sizeof a3_3));
    CHECK(r.command == 18);
    CHECK(r.content_len == 36);

    /* A bad CRC, a frame shorter than the 6-byte minimum, and a continuation
     * with the wrong fragment index are all rejected. */
    uint8_t bad_crc[6] = {0x05, 0x03, 0x00, 0x00, 0xfc, 0x01};
    medtrum_request_init(&r);
    CHECK(!medtrum_request_feed(&r, bad_crc, sizeof bad_crc));

    uint8_t tiny[5] = {0x05, 0x03, 0x00, 0x00, 0xfc};
    medtrum_request_init(&r);
    CHECK(!medtrum_request_feed(&r, tiny, sizeof tiny));

    medtrum_request_init(&r);
    CHECK(!medtrum_request_feed(&r, act1, sizeof act1));
    uint8_t wrong_seq[9] = {23, 3, 4, 3, 6, 0, 0, 182, 224};
    wrong_seq[8] = medtrum_crc8(wrong_seq, 8);
    CHECK(!medtrum_request_feed(&r, wrong_seq, sizeof wrong_seq));
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
    test_status_payload();
    test_patch_state();
    test_sync_fragment_vector();
    test_sync_full_vector();
    test_get_record();
    test_session();
    test_session_failures();
    test_lifecycle();
    test_fault_mapping();
    test_request_parser();

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
