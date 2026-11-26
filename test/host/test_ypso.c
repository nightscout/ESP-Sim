/* Host tests for the simulated YpsoPump. Golden vectors are from the rockit
 * driver's COMMAND-SPEC (§3.3, §3.11); the sessions play the driver's side as
 * that spec describes it. All values are synthetic. */
#include <stdbool.h>
#include <stdlib.h>

#include "dash.h"
#include <stdio.h>

#include "dash_internal.h"
#include "test.h"
#include "ypso.h"

static int64_t clock_now = 1800000000;
static int saves;
static int64_t f_now(void *ctx) { return clock_now; }
static int32_t f_off(void *ctx) { return 3600; }
static void f_save(void *ctx, const ypso_state_t *st) { saves++; }
static const ypso_io_t yio = {.now = f_now, .utc_offset = f_off, .save = f_save};

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* ---- golden vectors -------------------------------------------------------------- */

static void test_ypso_vectors(void)
{
    uint8_t d[17];
    for (int i = 0; i < 17; i++) {
        d[i] = (uint8_t)i;
    }
    CHECK(ypso_crc16(d, 17) == 0x8f1e); /* §3.11: crc of 00..10 is 1e 8f */

    /* §3.3: key 00..1f; the frame decrypts to AA BB CC, reboot 8, counter 2743. */
    uint8_t key[32], frame[64], pt[64];
    for (int i = 0; i < 32; i++) {
        key[i] = (uint8_t)i;
    }
    size_t n = hex("873858d502cde7d78f6906561187572ee28bc6380dd1d0e092b58d09876aea28"
                   "292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f",
                   frame);
    CHECK(n == 55);
    uint32_t reboot = 0;
    uint64_t counter = 0;
    int len = ypso_open(key, frame, n, pt, &reboot, &counter);
    CHECK(len == 3 && pt[0] == 0xaa && pt[1] == 0xbb && pt[2] == 0xcc);
    CHECK(reboot == 8 && counter == 2743);
    /* And sealing it again with the same nonce gives the same bytes. */
    uint8_t again[64];
    size_t m = ypso_seal(key, (const uint8_t *)"\xaa\xbb\xcc", 3, 8, 2743, frame + n - 24, again);
    CHECK_BYTES(again, m, frame, n);
    /* A flipped bit is refused. */
    frame[3] ^= 1;
    CHECK(ypso_open(key, frame, n, pt, &reboot, &counter) < 0);

    /* UUIDs go to NimBLE least significant byte first. */
    uint8_t u[16];
    ypso_uuid_le("fb349b5f-8000-0080-0010-0000adde0000", u);
    CHECK(u[0] == 0x00 && u[2] == 0xde && u[3] == 0xad && u[15] == 0xfb);

    /* Which firmware has what. */
    CHECK(ypso_has_svc(YPSO_FW_V05, YS_CRYPTO) && !ypso_has_svc(YPSO_FW_V65, YS_CRYPTO));
    CHECK(!ypso_has_svc(YPSO_FW_V02, YS_CONTROL));
    CHECK(ypso_has_chr(YPSO_FW_V35, YC_EVENT_VALUE_ALT) && !ypso_has_chr(YPSO_FW_V05, YC_EVENT_VALUE_ALT));
}

/* ---- the driver's side --------------------------------------------------------------- */

typedef struct {
    ypso_t *y;
    uint8_t key[32];
    uint32_t reboot;
    uint64_t write_counter, read_counter;
    bool encrypted;
} phone_t;

/* Writes one command the way the driver does: sealed and framed, or raw in clear. */
static int ph_write(phone_t *ph, ypso_chr_t c, const uint8_t *p, size_t n)
{
    if (!ph->encrypted) {
        return ypso_on_write(ph->y, c, p, n);
    }
    uint8_t nonce[24], wire[200];
    dash_random(nonce, sizeof nonce);
    size_t w = ypso_seal(ph->key, p, n, ph->reboot, ++ph->write_counter, nonce, wire);
    uint8_t frames = (uint8_t)((w + 18) / 19);
    int rc = 0;
    for (uint8_t f = 0; f < frames; f++) {
        uint8_t pkt[20];
        size_t k = w - f * 19u < 19 ? w - f * 19u : 19;
        pkt[0] = (uint8_t)(((f + 1) << 4) | frames);
        memcpy(pkt + 1, wire + f * 19, k);
        rc = ypso_on_write(ph->y, c, pkt, k + 1);
        if (rc != 0) {
            break;
        }
    }
    return rc;
}

/* Reads one value: frame 1, then ExtendedRead until whole, then opens it. */
static int ph_read(phone_t *ph, ypso_chr_t c, uint8_t *out, size_t *n)
{
    uint8_t pkt[32];
    size_t len;
    int rc = ypso_on_read(ph->y, c, pkt, sizeof pkt, &len);
    if (rc != 0) {
        return rc;
    }
    if (!ph->encrypted) {
        memcpy(out, pkt, len);
        *n = len;
        return 0;
    }
    uint8_t wire[200];
    size_t w = 0;
    uint8_t frames = pkt[0] & 0x0f, frame = pkt[0] >> 4;
    CHECK(frame == 1);
    memcpy(wire, pkt + 1, len - 1);
    w = len - 1;
    while (frame < frames) {
        rc = ypso_on_read(ph->y, YC_EXTENDED_READ, pkt, sizeof pkt, &len);
        if (rc != 0) {
            return rc;
        }
        CHECK((pkt[0] >> 4) == frame + 1);
        frame = pkt[0] >> 4;
        memcpy(wire + w, pkt + 1, len - 1);
        w += len - 1;
    }
    uint32_t reboot;
    uint64_t counter;
    int pl = ypso_open(ph->key, wire, w, out, &reboot, &counter);
    if (pl < 0) {
        return -1;
    }
    CHECK(counter > ph->read_counter); /* the pump's counter only goes up */
    ph->read_counter = counter;
    ph->reboot = reboot;
    *n = (size_t)pl;
    return 0;
}

static void safevar(uint8_t out[8], int32_t v)
{
    uint32_t u = (uint32_t)v;
    memcpy(out, &u, 4);
    u = ~u;
    memcpy(out + 4, &u, 4);
}

static int32_t read_setting(phone_t *ph, int32_t id)
{
    uint8_t sv[8], v[64];
    size_t n = 0;
    safevar(sv, id);
    CHECK(ph_write(ph, YC_SETTING_ID, sv, 8) == 0);
    CHECK(ph_read(ph, YC_SETTING_VALUE, v, &n) == 0 && n == 8);
    CHECK(le32(v + 4) == ~le32(v));
    return (int32_t)le32(v);
}

static void run(ypso_t *y, int seconds)
{
    for (int i = 0; i < seconds; i++) {
        clock_now++;
        ypso_tick(y);
    }
}

static void test_ypso_encrypted(void)
{
    static ypso_t y;
    ypso_state_t st;
    ypso_state_fresh(&st, YPSO_FW_V05);
    st.serial = 12345678; /* synthetic */
    ypso_init(&y, &st, &yio);
    phone_t ph = {.y = &y, .encrypted = true, .reboot = st.reboot, .write_counter = 0};
    memcpy(ph.key, st.key, 32);
    ypso_on_connect(&y);

    /* Version strings: clear text, unframed, readable before anything else. */
    uint8_t v[64];
    size_t n = 0;
    CHECK(ypso_on_read(&y, YC_MASTER_VERSION, v, sizeof v, &n) == 0);
    CHECK(n == 10 && memcmp(v, "V05.02.03\0", 10) == 0);
    CHECK(ypso_on_read(&y, YC_BASE_VERSION, v, sizeof v, &n) == 0 && n == 3 && memcmp(v, "1.2", 3) == 0);
    CHECK(ypso_on_read(&y, YC_CONTROL_VERSION, v, sizeof v, &n) == 0 && memcmp(v, "1.4", 3) == 0);

    /* Nothing protected before the password; a wrong one is refused with ATT 0x08. */
    CHECK(ph_read(&ph, YC_GET_SYSTEM_STATUS, v, &n) == YPSO_ATT_INSUFFICIENT_AUTHORIZATION);
    uint8_t wrong[16] = {0};
    CHECK(ypso_on_write(&y, YC_AUTH_PASSWORD, wrong, 16) == 0);
    CHECK(ph_read(&ph, YC_GET_SYSTEM_STATUS, v, &n) == YPSO_ATT_INSUFFICIENT_AUTHORIZATION);
    CHECK(ypso_on_write(&y, YC_AUTH_PASSWORD, st.password, 16) == 0);

    /* The driver seeds its write counter from the pump's read counter (§7.2). */
    CHECK(ph_read(&ph, YC_GET_SYSTEM_STATUS, v, &n) == 0);
    CHECK(n == 20 && v[0] == 0x0a);                         /* active */
    CHECK(le32(v + 1) == 16000 && v[5] == 5);              /* 160 U, battery 100 % */
    CHECK(le32(v + 6) == 80 && le32(v + 10) == 100);       /* 0.80 U/h, no TBR */
    CHECK(ypso_crc16(v, 18) == (uint16_t)(v[18] | v[19] << 8));
    ph.write_counter = ph.read_counter;

    /* The serial: lower, then upper (§4.5). */
    int32_t lo = read_setting(&ph, 62), hi = read_setting(&ph, 63);
    CHECK((((uint32_t)hi << 16) | (uint16_t)lo) == 12345678);
    CHECK(read_setting(&ph, 14) == 80);
    /* An unknown setting id: 0x83. */
    uint8_t sv[8];
    safevar(sv, 99);
    CHECK(ph_write(&ph, YC_SETTING_ID, sv, 8) == YPSO_INVALID_ID);

    /* A 6-frame GetBolusStatus (44 bytes + 52), as the golden capture. */
    uint8_t pkt[32];
    CHECK(ypso_on_read(&y, YC_GET_BOLUS_STATUS, pkt, sizeof pkt, &n) == 0 && pkt[0] == 0x16);
    uint8_t rest;
    for (rest = 2; rest <= 6; rest++) {
        CHECK(ypso_on_read(&y, YC_EXTENDED_READ, pkt, sizeof pkt, &n) == 0 && pkt[0] == ((rest << 4) | 6));
    }
    ph.read_counter++; /* that read used one */

    /* A 2.00 U bolus: the 15-byte payload with its CRC. */
    uint8_t b[15] = {0};
    b[0] = 200;
    b[12] = 0x01;
    uint16_t crc = ypso_crc16(b, 13);
    b[13] = (uint8_t)crc;
    b[14] = (uint8_t)(crc >> 8);
    CHECK(ph_write(&ph, YC_START_STOP_BOLUS, b, 15) == 0);
    CHECK(ph_write(&ph, YC_START_STOP_BOLUS, b, 15) == YPSO_COMMAND_FAILED); /* one at a time */
    b[13] ^= 1;
    CHECK(ph_write(&ph, YC_START_STOP_BOLUS, b, 15) == YPSO_VALUE_CORRUPT);
    run(&y, 10);
    CHECK(ph_read(&ph, YC_GET_BOLUS_STATUS, v, &n) == 0 && n == 44);
    CHECK(ypso_crc16(v, 42) == (uint16_t)(v[42] | v[43] << 8));
    CHECK(v[0] == 1 && le32(v + 5) == 50 && le32(v + 9) == 200);
    run(&y, 40);
    CHECK(ph_read(&ph, YC_GET_BOLUS_STATUS, v, &n) == 0 && le32(v + 5) == le32(v + 9)); /* done */
    CHECK(y.st.reservoir <= 16000 - 200);

    /* A temp basal: 150 % for 30 min. A second while one runs: 0x86. 100 % cancels. */
    uint8_t t[16];
    safevar(t, 150);
    safevar(t + 8, 30);
    CHECK(ph_write(&ph, YC_START_STOP_TBR, t, 16) == 0);
    CHECK(ph_write(&ph, YC_START_STOP_TBR, t, 16) == YPSO_INTERNAL_ERROR);
    CHECK(ph_read(&ph, YC_GET_SYSTEM_STATUS, v, &n) == 0 && le32(v + 10) == 150 && le32(v + 14) == 30);
    safevar(t, 100);
    safevar(t + 8, 0);
    CHECK(ph_write(&ph, YC_START_STOP_TBR, t, 16) == 0);
    CHECK(ph_read(&ph, YC_GET_SYSTEM_STATUS, v, &n) == 0 && le32(v + 10) == 100);
    safevar(t, 150);
    safevar(t + 8, 20); /* not a multiple of 15 */
    CHECK(ph_write(&ph, YC_START_STOP_TBR, t, 16) == YPSO_VALUE_INVALID);
    /* The limits it reports (116-119) are the ones it takes: 500 % for 24 h, not 510 %. */
    CHECK(read_setting(&ph, 116) == 500 && read_setting(&ph, 117) == 1440 && read_setting(&ph, 118) == 500);
    safevar(t, 510);
    safevar(t + 8, 30);
    CHECK(ph_write(&ph, YC_START_STOP_TBR, t, 16) == YPSO_VALUE_INVALID);
    safevar(t, 500);
    safevar(t + 8, 1440);
    CHECK(ph_write(&ph, YC_START_STOP_TBR, t, 16) == 0);
    CHECK(ph_read(&ph, YC_GET_SYSTEM_STATUS, v, &n) == 0 && le32(v + 10) == 500);
    safevar(t, 100);
    safevar(t + 8, 0);
    CHECK(ph_write(&ph, YC_START_STOP_TBR, t, 16) == 0);

    /* An extended bolus: 1.00 U over 30 minutes. */
    memset(b, 0, sizeof b);
    b[0] = 100;
    b[4] = 30;
    b[12] = 0x02;
    crc = ypso_crc16(b, 13);
    b[13] = (uint8_t)crc;
    b[14] = (uint8_t)(crc >> 8);
    CHECK(ph_write(&ph, YC_START_STOP_BOLUS, b, 15) == 0);
    run(&y, 15 * 60);
    CHECK(ph_read(&ph, YC_GET_BOLUS_STATUS, v, &n) == 0);
    CHECK(le32(v + 14) != 0 && le32(v + 22) == 100 && le32(v + 18) == 50 && le32(v + 38) == 30);
    run(&y, 16 * 60);
    CHECK(ph_read(&ph, YC_GET_BOLUS_STATUS, v, &n) == 0 && le32(v + 14) == 0);

    /* History: newest first from index 0, each entry 19 bytes with its CRC; the index moves on. */
    CHECK(ph_read(&ph, YC_EVENT_COUNT, v, &n) == 0 && n == 8);
    int32_t count = (int32_t)le32(v);
    CHECK(count == 4); /* bolus, two TBR aborts, extended (the refused TBRs log nothing) */
    CHECK(ph_read(&ph, YC_EVENT_VALUE, v, &n) == 0 && n == 19);
    CHECK(ypso_crc16(v, 17) == (uint16_t)(v[17] | v[18] << 8));
    CHECK(v[4] == 3 && (v[5] | v[6] << 8) == 100 && (v[15] | v[16] << 8) == 0); /* the extended bolus */
    CHECK(ph_read(&ph, YC_EVENT_VALUE, v, &n) == 0 && v[4] == 32 && (v[15] | v[16] << 8) == 1); /* TBR abort */
    uint8_t idx[8];
    safevar(idx, 3);
    CHECK(ph_write(&ph, YC_EVENT_INDEX, idx, 8) == 0); /* History 1.4: a SAFEVAR index */
    CHECK(ph_read(&ph, YC_EVENT_VALUE, v, &n) == 0 && v[4] == 2 && (v[5] | v[6] << 8) == 200);
    safevar(idx, count);
    CHECK(ph_write(&ph, YC_EVENT_INDEX, idx, 8) == YPSO_ENTRY_INDEX_INVALID); /* the end of the log */

    /* The clock: date then time; a time write moves it and is logged. */
    CHECK(ph_read(&ph, YC_SYSTEM_DATE, v, &n) == 0 && n == 4 && (v[0] | v[1] << 8) == 2027);
    CHECK(ph_read(&ph, YC_SYSTEM_TIME, v, &n) == 0 && n == 3 && v[0] < 24);
    uint8_t tm[3] = {12, 0, 0};
    CHECK(ph_write(&ph, YC_SYSTEM_TIME, tm, 3) == 0);
    CHECK(ph_read(&ph, YC_SYSTEM_TIME, v, &n) == 0 && v[0] == 12 && v[1] == 0 && v[2] <= 1);

    /* Counters (§7): a replayed counter is behind, 0x8B; a jump forward is fine. */
    ph.write_counter -= 5;
    safevar(sv, 1);
    CHECK(ph_write(&ph, YC_SETTING_ID, sv, 8) == YPSO_COUNTER_ERROR);
    ph.write_counter += 100;
    CHECK(ph_write(&ph, YC_SETTING_ID, sv, 8) == 0);
    /* The wrong key: 0x8A. */
    ph.key[0] ^= 1;
    CHECK(ph_write(&ph, YC_SETTING_ID, sv, 8) == YPSO_ENCRYPTION_FAILED);
    ph.key[0] ^= 1;

    /* A pump restart: its reboot counter goes up and the next connection must
     * authorise again; the phone adopts the new reboot counter from a read. */
    ypso_on_disconnect(&y);
    ypso_reboot(&y);
    ypso_on_connect(&y);
    CHECK(ypso_on_write(&y, YC_AUTH_PASSWORD, st.password, 16) == 0);
    CHECK(ph_read(&ph, YC_GET_SYSTEM_STATUS, v, &n) == 0 && ph.reboot == st.reboot + 1);
    CHECK(ph_write(&ph, YC_SETTING_ID, sv, 8) == 0);

    /* Suspended: no bolus. */
    ypso_set_suspended(&y, true);
    memset(b, 0, sizeof b);
    b[0] = 100;
    b[12] = 0x01;
    crc = ypso_crc16(b, 13);
    b[13] = (uint8_t)crc;
    b[14] = (uint8_t)(crc >> 8);
    CHECK(ph_write(&ph, YC_START_STOP_BOLUS, b, 15) == YPSO_COMMAND_FAILED);
    CHECK(ph_read(&ph, YC_GET_SYSTEM_STATUS, v, &n) == 0 && v[0] == 0x03);
}

static void test_ypso_clear(void)
{
    static ypso_t y;
    ypso_state_t st;
    ypso_state_fresh(&st, YPSO_FW_V65);
    ypso_init(&y, &st, &yio);
    phone_t ph = {.y = &y, .encrypted = false};
    ypso_on_connect(&y);
    uint8_t v[64];
    size_t n;
    CHECK(ypso_on_read(&y, YC_CONTROL_VERSION, v, sizeof v, &n) == 0 && memcmp(v, "1.0", 3) == 0);
    CHECK(ypso_on_read(&y, YC_EXTENDED_READ, v, sizeof v, &n) != 0); /* no Crypto service */
    CHECK(ypso_on_write(&y, YC_AUTH_PASSWORD, st.password, 16) == 0);
    /* Everything raw: an 8-byte SAFEVAR selection, an 8-byte value. */
    CHECK(read_setting(&ph, 2) == 10);
    /* 18-byte status, no CRC; 13-byte bolus status. */
    CHECK(ph_read(&ph, YC_GET_SYSTEM_STATUS, v, &n) == 0 && n == 18);
    CHECK(ph_read(&ph, YC_GET_BOLUS_STATUS, v, &n) == 0 && n == 13);
    /* No extended bolus on Control 1.0/1.1 (HAS_SLOW is verControl > 1.1): the
     * 15-byte bolus that carries one is refused, nothing starts. */
    uint8_t ext[15] = {0};
    ext[4] = 100; /* 1.00 U extended */
    ext[8] = 30;  /* over 30 min */
    ext[12] = 2;
    uint16_t ec = ypso_crc16(ext, 13);
    ext[13] = (uint8_t)ec;
    ext[14] = (uint8_t)(ec >> 8);
    CHECK(ph_write(&ph, YC_START_STOP_BOLUS, ext, 15) == YPSO_VALUE_CORRUPT);
    CHECK(ph_read(&ph, YC_GET_BOLUS_STATUS, v, &n) == 0 && n == 13 && v[0] == 0);
    /* The OPN bolus: one SAFEVAR. */
    uint8_t sv[8];
    safevar(sv, 100);
    CHECK(ph_write(&ph, YC_START_STOP_BOLUS, sv, 8) == 0);
    run(&y, 30);
    CHECK(ph_read(&ph, YC_GET_BOLUS_STATUS, v, &n) == 0 && le32(v + 1) == 100 && le32(v + 5) == 100);
    /* OPN TBR steps are 10 %. */
    uint8_t t[16];
    safevar(t, 155);
    safevar(t + 8, 30);
    CHECK(ph_write(&ph, YC_START_STOP_TBR, t, 16) == YPSO_VALUE_INVALID);
    safevar(t, 150);
    CHECK(ph_write(&ph, YC_START_STOP_TBR, t, 16) == 0);
    /* History 1.1: a bare u16 index. */
    uint8_t idx[2] = {0, 0};
    CHECK(ph_write(&ph, YC_EVENT_INDEX, idx, 2) == 0);
    uint8_t sv_idx[8];
    safevar(sv_idx, 0);
    CHECK(ph_write(&ph, YC_EVENT_INDEX, sv_idx, 8) == YPSO_VALUE_INVALID);
    /* Newest first: the running temp basal's _RUNNING entry (9), 150 % for 30 min. */
    CHECK(ph_read(&ph, YC_EVENT_VALUE, v, &n) == 0 && n == 19 && v[4] == 9);
    CHECK((v[5] | v[6] << 8) == 150 && (v[7] | v[8] << 8) == 30);
    uint32_t tbr_t = le32(v), tbr_seq = le32(v + 11);
    /* Then the bolus, finished in place: type 2, its own sequence the bolus status gave. */
    CHECK(ph_read(&ph, YC_EVENT_VALUE, v, &n) == 0 && v[4] == 2 && (v[5] | v[6] << 8) == 100);
    uint8_t bs[16];
    size_t bn;
    CHECK(ph_read(&ph, YC_GET_BOLUS_STATUS, bs, &bn) == 0 && le32(bs + 9) == le32(v + 11));
    /* Cancelled 5 minutes on: the same entry becomes TBR_ABORT (32), same sequence, same start stamp. */
    run(&y, 300);
    safevar(t, 100);
    safevar(t + 8, 0);
    CHECK(ph_write(&ph, YC_START_STOP_TBR, t, 16) == 0);
    idx[0] = 0;
    CHECK(ph_write(&ph, YC_EVENT_INDEX, idx, 2) == 0);
    CHECK(ph_read(&ph, YC_EVENT_VALUE, v, &n) == 0 && v[4] == 32 && le32(v + 11) == tbr_seq && le32(v) == tbr_t);
    CHECK((v[5] | v[6] << 8) == 150 && (v[7] | v[8] << 8) == 5); /* ran 5 minutes */
}

/* Open: a phone with no credentials for this pump gets in (what rockit does
 * when it has none: no password write at all), and so does a wrong password.
 * Closed again, it is refused like a real pump. */
static void test_ypso_open(void)
{
    static ypso_t y;
    ypso_state_t st;
    ypso_state_fresh(&st, YPSO_FW_V65);
    ypso_init(&y, &st, &yio);
    phone_t ph = {.y = &y, .encrypted = false};
    uint8_t v[64];
    size_t n;
    ypso_set_open(&y, true);
    ypso_on_connect(&y);
    CHECK(read_setting(&ph, 2) == 10);
    CHECK(ph_read(&ph, YC_GET_SYSTEM_STATUS, v, &n) == 0 && n == 18);
    uint8_t wrong[16] = {0};
    CHECK(ypso_on_write(&y, YC_AUTH_PASSWORD, wrong, 16) == 0);
    CHECK(ph_read(&ph, YC_GET_SYSTEM_STATUS, v, &n) == 0);
    ypso_set_open(&y, false);
    ypso_on_disconnect(&y);
    ypso_on_connect(&y);
    CHECK(ph_read(&ph, YC_GET_SYSTEM_STATUS, v, &n) == YPSO_ATT_INSUFFICIENT_AUTHORIZATION);
    CHECK(ypso_on_write(&y, YC_AUTH_PASSWORD, wrong, 16) == 0);
    CHECK(ph_read(&ph, YC_GET_SYSTEM_STATUS, v, &n) == YPSO_ATT_INSUFFICIENT_AUTHORIZATION);
}

/* The pump keeps the board's local time: a clock 10 days out is set back, as a
 * date and a time change in its history; one already right is left alone. */
static void test_ypso_clock(void)
{
    static ypso_t y;
    ypso_state_t st;
    ypso_state_fresh(&st, YPSO_FW_V65);
    st.clock_offset = 10 * 86400 + 900;
    ypso_init(&y, &st, &yio);
    uint16_t before = y.st.n_events;
    CHECK(ypso_clock_to_local(&y));
    CHECK(y.st.clock_offset == 0);
    CHECK(y.st.n_events == before + 2);
    CHECK(y.st.events[before].type == 12 && y.st.events[before + 1].type == 13); /* date, then time */
    /* Stamped on the clock as it was before the change. */
    CHECK(y.st.events[before].t > (uint32_t)(clock_now + 3600 - 946684800 + 10 * 86400));
    CHECK(!ypso_clock_to_local(&y));
    /* A board clock step does not deliver days of basal. */
    int32_t res = y.st.reservoir;
    clock_now += 5 * 86400;
    ypso_tick(&y);
    CHECK(res - y.st.reservoir <= 1);
    clock_now -= 5 * 86400;
}

/* ---- the key exchange ------------------------------------------- */

static void t_hchacha(const uint8_t key[32], const uint8_t nonce[16], uint8_t out[32])
{
    uint32_t x[16] = {0x61707865, 0x3320646e, 0x79622d32, 0x6b206574};
    for (int i = 0; i < 8; i++) {
        x[4 + i] = le32(key + 4 * i);
    }
    for (int i = 0; i < 4; i++) {
        x[12 + i] = le32(nonce + 4 * i);
    }
    dash_chacha20_rounds(x);
    for (int i = 0; i < 4; i++) {
        memcpy(out + 4 * i, &x[i], 4);
        memcpy(out + 16 + 4 * i, &x[12 + i], 4);
    }
}

/* XChaCha20-Poly1305 with no associated data: out = ct ‖ tag. */
static void t_xseal(const uint8_t key[32], const uint8_t nonce[24], const uint8_t *p, size_t n, uint8_t *out)
{
    uint8_t sub[32], n12[12] = {0};
    t_hchacha(key, nonce, sub);
    memcpy(n12 + 4, nonce + 16, 8);
    dash_chachapoly_seal(sub, n12, p, n, out);
}

static void t_session_key(const uint8_t priv[32], const uint8_t pub[32], uint8_t out[32])
{
    static const uint8_t zero[16];
    uint8_t shared[32];
    dash_x25519(shared, priv, pub);
    t_hchacha(shared, zero, out);
}

/* The offer: frame 1 of NewPublicKey, then ExtendedRead. */
static int read_offer(ypso_t *y, uint8_t offer[64])
{
    uint8_t pkt[32];
    size_t len, w = 0;
    int rc = ypso_on_read(y, YC_NEW_PUBLIC_KEY, pkt, sizeof pkt, &len);
    if (rc != 0) {
        return rc;
    }
    uint8_t frames = pkt[0] & 0x0f;
    CHECK(pkt[0] >> 4 == 1 && frames == 4);
    memcpy(offer, pkt + 1, len - 1);
    w = len - 1;
    for (int f = 2; f <= frames; f++) {
        CHECK(ypso_on_read(y, YC_EXTENDED_READ, pkt, sizeof pkt, &len) == 0 && pkt[0] >> 4 == f);
        memcpy(offer + w, pkt + 1, len - 1);
        w += len - 1;
    }
    CHECK(w == 64);
    return 0;
}

/* Rockit's side (key_exchange.rs prepare, the server's public key in the blob):
 * the blob, framed into WriteChallengeUtcAppPubKey. The session key out. */
static int write_blob(ypso_t *y, const uint8_t offer[64], const uint8_t server[32], uint64_t counter,
                      uint8_t session[32])
{
    uint8_t server_pub[32], ks[32], pt[76], blob[116], nonce[24];
    dash_x25519_base(server_pub, server);
    t_session_key(server, offer + 32, ks);
    memcpy(pt, offer, 32);
    uint32_t ttl = 28 * 86400;
    memcpy(pt + 32, &ttl, 4);
    memcpy(pt + 36, server_pub, 32);
    memcpy(pt + 68, &counter, 8);
    dash_random(nonce, sizeof nonce);
    t_xseal(ks, nonce, pt, 76, blob);
    memcpy(blob + 92, nonce, 24);
    memcpy(session, ks, 32);
    int rc = 0;
    for (int f = 0; f < 7; f++) {
        uint8_t pkt[20];
        size_t k = 116 - f * 19 < 19 ? 116 - f * 19 : 19;
        pkt[0] = (uint8_t)(((f + 1) << 4) | 7);
        memcpy(pkt + 1, blob + f * 19, k);
        rc = ypso_on_write(y, YC_WRITE_CHALLENGE, pkt, k + 1);
        if (rc != 0) {
            break;
        }
    }
    return rc;
}

static void test_ypso_key_exchange(void)
{
    static ypso_t y;
    ypso_state_t st;
    ypso_state_fresh(&st, YPSO_FW_V05);
    ypso_init(&y, &st, &yio);
    ypso_set_open(&y, true);
    ypso_on_connect(&y);
    CHECK(ypso_on_write(&y, YC_AUTH_PASSWORD, st.password, 16) == 0);
    uint8_t offer[64], session[32], v[64];
    size_t n;
    CHECK(read_offer(&y, offer) == 0);
    uint8_t pub[32];
    ypso_pump_public(&y.st, pub);
    CHECK(memcmp(offer + 32, pub, 32) == 0);

    /* A server key the pump does not trust: 0x8A, the old key stays. */
    uint8_t other[32];
    memset(other, 0x44, 32);
    CHECK(write_blob(&y, offer, other, 5, session) == YPSO_ENCRYPTION_FAILED);
    CHECK(memcmp(y.st.key, st.key, 32) == 0);

    /* The trusted one: the new key, counters from the bottom. */
    CHECK(read_offer(&y, offer) == 0);
    CHECK(write_blob(&y, offer, y.st.server_priv, 1800000000000ULL, session) == 0);
    CHECK(memcmp(y.st.key, session, 32) == 0 && memcmp(y.st.key, st.key, 32) != 0);
    CHECK(y.st.read_counter == 0 && y.st.write_counter == 0 && y.st.kx_counter == 1800000000000ULL);
    /* The proving read, then a command, under the new key; the reboot counter learnt from the read. */
    phone_t ph = {.y = &y, .encrypted = true};
    memcpy(ph.key, session, 32);
    CHECK(ph_read(&ph, YC_GET_SYSTEM_STATUS, v, &n) == 0 && n == 20);
    CHECK(read_setting(&ph, 2) == 10);

    /* The same blob again: no offer read since, refused. A counter not above the last: 0x8B. */
    CHECK(write_blob(&y, offer, y.st.server_priv, 1800000000001ULL, session) == YPSO_ENCRYPTION_FAILED);
    CHECK(read_offer(&y, offer) == 0);
    CHECK(write_blob(&y, offer, y.st.server_priv, 1800000000000ULL, session) == YPSO_COUNTER_ERROR);
    CHECK(read_setting(&ph, 2) == 10); /* still on the key it took */

    /* Closed and not signed in: no offer. */
    ypso_set_open(&y, false);
    ypso_on_disconnect(&y);
    ypso_on_connect(&y);
    CHECK(read_offer(&y, offer) == YPSO_ATT_INSUFFICIENT_AUTHORIZATION);
}

/* RFC 7693 and RFC 9106 test vectors. */
void test_ypso_all(void)
{
    test_ypso_vectors();
    test_ypso_encrypted();
    test_ypso_clear();
    test_ypso_open();
    test_ypso_clock();
    test_ypso_key_exchange();
}
