/* Host tests for the simulated G6 transmitter and G7 sensor. Byte vectors
 * marked "upstream" are from CGMBLEKit's and G7SensorKit's own tests. */
#include <stdbool.h>
#include <math.h>
#include <stdlib.h>

#include "cgm.h"
#include "dash.h"
#include "test.h"

/* ---- a fake radio and clock ------------------------------------------------- */

#define MAX_SENT 64

typedef struct {
    int64_t now;
    struct {
        cgm_session_t session;
        cgm_chr_t chr;
        uint8_t p[32];
        size_t n;
    } sent[MAX_SENT];
    int n_sent;
    bool advertising;
    char adv_name[16];
    int disconnects;
    cgm_session_t last_disconnect;
    int disconnects_for[CGM_MAX_SESSIONS];
    int saves;
} fake_t;

static fake_t fk;
static const uint8_t AUTH_OK[3] = {0x05, 0x01, 0x01};

static void f_send(void *ctx, cgm_session_t session, cgm_chr_t chr, const uint8_t *p, size_t n)
{
    if (fk.n_sent < MAX_SENT && n <= sizeof fk.sent[0].p) {
        fk.sent[fk.n_sent].session = session;
        fk.sent[fk.n_sent].chr = chr;
        memcpy(fk.sent[fk.n_sent].p, p, n);
        fk.sent[fk.n_sent].n = n;
        fk.n_sent++;
    }
}
static void f_disconnect(void *ctx, cgm_session_t session)
{
    fk.disconnects++;
    fk.last_disconnect = session;
    if (session < CGM_MAX_SESSIONS) {
        fk.disconnects_for[session]++;
    }
}
static void f_advertise(void *ctx, bool on, const char *name, const uint8_t addr[6])
{
    fk.advertising = on;
    snprintf(fk.adv_name, sizeof fk.adv_name, "%s", name);
}
static int64_t f_now(void *ctx) { return fk.now; }
static void f_save(void *ctx, const cgm_state_t *st) { fk.saves++; }

static const cgm_io_t fio = {
    .send = f_send,
    .disconnect = f_disconnect,
    .advertise = f_advertise,
    .now = f_now,
    .save = f_save,
};

static void clear_sent(void) { fk.n_sent = 0; }

/* A phone connects: the radio stops advertising by itself. */
static void connect_n(cgm_t *c, cgm_session_t session)
{
    fk.advertising = false;
    cgm_on_connect(c, session);
}

static void connect(cgm_t *c) { connect_n(c, 0); }

/* Runs the clock forward a second at a time. */
static void run(cgm_t *c, int seconds)
{
    for (int i = 0; i < seconds; i++) {
        fk.now++;
        cgm_tick(c);
    }
}

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static bool crc_ok(const uint8_t *p, size_t n) { return n > 2 && cgm_crc16(p, n - 2) == le16(p + n - 2); }

/* ---- message formats against upstream vectors --------------------------------- */

static void test_formats(void)
{
    uint8_t want[32], got[32];
    size_t n;

    /* CRC-CCITT (XModem), and messages from CGMBLEKit's tests. */
    const char *vectors[] = {
        "2500470272007cff710001000000fa1d", "3100680a00008a715700cc0006ffc42a",
        "33002b290090012900ae00800050e929001225", "4b0001000011df2900005100037000f00009b6",
        "2700012bfd71002bfd710096fd71000f6a", "51000100b7ff52006604530032000000e6cb9805",
    };
    for (size_t i = 0; i < sizeof vectors / sizeof vectors[0]; i++) {
        n = hex(vectors[i], want);
        CHECK(crc_ok(want, n));
    }

    /* The G6 login hash (upstream: TransmitterIDTests). */
    n = hex("0123456789abcdef", want);
    cgm_g6_hash("123456", want, got);
    n = hex("e60d4a7999b0fbb2", want);
    CHECK_BYTES(got, 8, want, n);

    cgm_state_t st;
    memset(&st, 0, sizeof st);
    st.kind = CGM_G6;
    st.activated = 1700000000;

    /* G6 time (upstream). */
    st.session_start = st.activated + 0x71ff7c;
    n = cgm_g6_time_msg(&st, st.activated + 0x720247, got);
    CHECK_BYTES(got, n, want, hex("2500470272007cff710001000000fa1d", want));
    /* No session: start 0xffffffff (as upstream's "250096fd7100ffffffff01000000226d"). */
    st.session_start = 0;
    n = cgm_g6_time_msg(&st, st.activated + 0x71fd96, got);
    CHECK_BYTES(got, n, want, hex("250096fd7100ffffffff01000000226d", want));

    /* G6 glucose (upstream). */
    cgm_reading_t r = {.t = (uint32_t)(st.activated + 0x57718a), .seq = 0x0a68, .glucose = 204, .state = 6, .trend = -1};
    n = cgm_g6_glucose_msg(&st, &r, 0x31, got);
    CHECK_BYTES(got, n, want, hex("3100680a00008a715700cc0006ffc42a", want));

    /* G7 glucose: upstream "4e00c35501002601000106008a00060187000f" but for the
     * predicted value and calibration byte, which the simulator leaves out. */
    cgm_state_t s7;
    memset(&s7, 0, sizeof s7);
    s7.kind = CGM_G7;
    s7.activated = 1700000000;
    int64_t now = s7.activated + 0x0155c3;
    cgm_reading_t r7 = {.t = (uint32_t)(now - 6), .seq = 0x0126, .glucose = 138, .state = 6, .trend = 1};
    n = cgm_g7_glucose_msg(&s7, &r7, now, got);
    CHECK(n == 19);
    n = hex("4e00c35501002601000106008a000601", want);
    CHECK_BYTES(got, 16, want, n);
    CHECK(le16(got + 16) == 0xffff);
    /* In warm-up: no glucose, no trend (upstream "4e00cd000000030000010500ffff027fffff01"). */
    now = s7.activated + 0xcd;
    cgm_reading_t w = {.t = (uint32_t)(now - 5), .seq = 3, .glucose = 0, .state = 2, .trend = 0x7f};
    n = cgm_g7_glucose_msg(&s7, &w, now, got);
    CHECK_BYTES(got, 18, want, hex("4e00cd000000030000010500ffff027fffff", want));

    /* G7 backfill record (upstream G7BackfillMessage: "45a100 00 9600 06 0f fc"). */
    cgm_reading_t b = {.t = (uint32_t)(s7.activated + 0x00a145), .glucose = 0x96, .state = 6, .trend = -4};
    n = cgm_g7_backfill_record(&s7, &b, got);
    CHECK(n == 9);
    n = hex("45a10000960006", want);
    CHECK_BYTES(got, 7, want, n);
    CHECK(got[8] == 0xfc);

    /* Names. */
    char name[16];
    memcpy(st.id, "81AB7Q", 7);
    cgm_name(&st, name);
    CHECK(strcmp(name, "Dexcom7Q") == 0);
    memcpy(s7.id, "K3", 3);
    cgm_name(&s7, name);
    CHECK(strcmp(name, "DXCMK3") == 0);
    CHECK(cgm_valid_g6_id("81AB7Q"));
    CHECK(!cgm_valid_g6_id("81AB7"));
    CHECK(!cgm_valid_g6_id("81ab7q"));
}

/* ---- a G7 session, as G7SensorKit drives it ------------------------------------- */

static int find(cgm_chr_t chr, uint8_t op, int from)
{
    for (int i = from; i < fk.n_sent; i++) {
        if (fk.sent[i].chr == chr && fk.sent[i].p[0] == op) {
            return i;
        }
    }
    return -1;
}

/* The same, only for one central's session. */
static int find_session(cgm_chr_t chr, uint8_t op, int from, cgm_session_t session)
{
    for (int i = from; i < fk.n_sent; i++) {
        if (fk.sent[i].session == session && fk.sent[i].chr == chr && fk.sent[i].p[0] == op) {
            return i;
        }
    }
    return -1;
}

static int count_session(cgm_chr_t chr, cgm_session_t session)
{
    int n = 0;
    for (int i = 0; i < fk.n_sent; i++) {
        n += fk.sent[i].session == session && fk.sent[i].chr == chr;
    }
    return n;
}

static void test_g7(void)
{
    static cgm_t c;
    cgm_state_t st;
    memset(&fk, 0, sizeof fk);
    fk.now = 1800000000;
    cgm_state_fresh(&st, CGM_G7, NULL, fk.now);
    CHECK(st.kind == CGM_G7 && strlen(st.id) == 2);
    CHECK((st.addr[5] & 0xc0) == 0xc0);
    cgm_init(&c, &st, &fio);

    /* A reading at once, in warm-up, and an advertising window. */
    cgm_tick(&c);
    CHECK(c.st.n_hist == 1 && c.st.hist[0].state == CGM_STATE_WARMUP);
    CHECK(fk.advertising && strncmp(fk.adv_name, "DXCM", 4) == 0);
    run(&c, 61);
    CHECK(!fk.advertising); /* the window closed: reading 1 missed */

    /* Skip the warm-up: the next reading is OK, and due now. */
    cgm_set_glucose(&c, 150, 10);
    cgm_skip_warmup(&c);
    CHECK(cgm_current_state(&c.st, fk.now) == CGM_STATE_OK);
    run(&c, 1);
    const cgm_reading_t *r = &c.st.hist[c.st.n_hist - 1];
    CHECK(r->state == CGM_STATE_OK && r->glucose == 150 && r->trend == 10);
    CHECK(c.st.glucose == 155); /* +1 mg/dL/min for 5 minutes */
    CHECK(fk.advertising);

    /* The phone connects and listens to auth: it is told the session is
     * authenticated and bonded, subscribes to control, and gets the reading. */
    connect(&c);
    CHECK(!c.advertising);
    clear_sent();
    cgm_on_subscribe(&c, 0, CGM_CHR_AUTH, true);
    CHECK(fk.n_sent == 1 && fk.sent[0].chr == CGM_CHR_AUTH);
    CHECK_BYTES(fk.sent[0].p, fk.sent[0].n, AUTH_OK, 3);
    cgm_on_subscribe(&c, 0, CGM_CHR_CONTROL, true);
    int g = find(CGM_CHR_CONTROL, 0x4e, 0);
    CHECK(g >= 0 && fk.sent[g].n == 19);
    if (g >= 0) {
        const uint8_t *m = fk.sent[g].p;
        uint32_t ts = le32(m + 2), age = le16(m + 10);
        CHECK(le16(m + 12) == 150 && m[14] == 6);
        CHECK(fk.now - ts == c.st.activated);      /* the app's activation date */
        CHECK(fk.now - age == r->t);               /* the reading's time */
        CHECK(le16(m + 6) == r->seq);
    }
    /* Backfill: the warm-up reading the phone missed, then 0x59. */
    clear_sent();
    cgm_on_subscribe(&c, 0, CGM_CHR_BACKFILL, true);
    CHECK(fk.n_sent == 2);
    CHECK(fk.sent[0].chr == CGM_CHR_BACKFILL && fk.sent[0].n == 9 && fk.sent[0].p[6] == CGM_STATE_WARMUP);
    CHECK(fk.sent[1].chr == CGM_CHR_CONTROL && fk.sent[1].p[0] == 0x59);
    /* Extended version. */
    clear_sent();
    cgm_on_write(&c, 0, CGM_CHR_CONTROL, (const uint8_t *)"\x52", 1);
    CHECK(fk.n_sent == 1 && fk.sent[0].n == 15 && fk.sent[0].p[0] == 0x52);
    CHECK(le32(fk.sent[0].p + 2) == 907200 && le16(fk.sent[0].p + 6) == 27 * 60);
    /* Quiet for 3 seconds: the sensor hangs up. */
    run(&c, 3);
    CHECK(fk.disconnects == 1);
    cgm_on_disconnect(&c, 0);
    run(&c, 5);
    CHECK(!fk.advertising); /* delivered: no more advertising until the next reading */

    /* Signal loss: three readings are missed, then backfilled. */
    cgm_set_signal_loss(&c, true);
    run(&c, 3 * CGM_READING_INTERVAL);
    CHECK(!fk.advertising);
    cgm_set_signal_loss(&c, false);
    for (int i = 0; i < CGM_READING_INTERVAL && !fk.advertising; i++) {
        run(&c, 1); /* until the next reading's window opens */
    }
    CHECK(fk.advertising);
    connect(&c);
    clear_sent();
    cgm_on_subscribe(&c, 0, CGM_CHR_AUTH, true);
    cgm_on_subscribe(&c, 0, CGM_CHR_CONTROL, true);
    cgm_on_subscribe(&c, 0, CGM_CHR_BACKFILL, true);
    int recs = 0;
    for (int i = 0; i < fk.n_sent; i++) {
        recs += fk.sent[i].chr == CGM_CHR_BACKFILL;
    }
    /* A reconnect is a fresh session: it backfills the whole history, not only
     * the readings missed while the link was down. */
    CHECK(recs == c.st.n_hist - 1);
    CHECK(find(CGM_CHR_CONTROL, 0x59, 0) > find(CGM_CHR_CONTROL, 0x4e, 0));
    cgm_on_disconnect(&c, 0);

    /* After 10.5 days, the session has ended: no glucose. */
    fk.now = c.st.activated + 10 * 86400 + 12 * 3600 + 1;
    cgm_tick(&c);
    r = &c.st.hist[c.st.n_hist - 1];
    CHECK(r->state == CGM_STATE_G7_SESSION_ENDED && r->glucose == 0);
}

/* ---- a G6 session in passive mode (CGMBLEKit's default) --------------------------- */

static void test_g6_passive(void)
{
    static cgm_t c;
    cgm_state_t st;
    memset(&fk, 0, sizeof fk);
    fk.now = 1800000000;
    cgm_state_fresh(&st, CGM_G6, "81AB7Q", fk.now);
    CHECK(strcmp(st.id, "81AB7Q") == 0);
    cgm_init(&c, &st, &fio);
    cgm_skip_warmup(&c);
    cgm_set_glucose(&c, 100, -20);
    cgm_set_signal_loss(&c, true);
    run(&c, 2 * CGM_READING_INTERVAL);
    cgm_set_signal_loss(&c, false);
    run(&c, CGM_READING_INTERVAL);
    CHECK(fk.advertising && strcmp(fk.adv_name, "Dexcom7Q") == 0);

    connect(&c);
    clear_sent();
    cgm_on_subscribe(&c, 0, CGM_CHR_AUTH, true);
    cgm_on_subscribe(&c, 0, CGM_CHR_CONTROL, true);
    /* Time, then glucose (0x4f), both with good CRCs. */
    int t = find(CGM_CHR_CONTROL, 0x25, 0), g = find(CGM_CHR_CONTROL, 0x4f, 0);
    CHECK(t >= 0 && g > t);
    if (t < 0 || g < 0) {
        return;
    }
    CHECK(fk.sent[t].n == 16 && crc_ok(fk.sent[t].p, 16));
    CHECK(fk.sent[g].n == 16 && crc_ok(fk.sent[g].p, 16));
    uint32_t cur = le32(fk.sent[t].p + 2), start = le32(fk.sent[t].p + 6);
    CHECK(fk.now - cur == c.st.activated);
    CHECK(start <= cur && cur - start >= 2 * 3600); /* warm-up over */
    const cgm_reading_t *r = &c.st.hist[c.st.n_hist - 1];
    CHECK(le32(fk.sent[g].p + 6) == r->t - c.st.activated);
    CHECK(le16(fk.sent[g].p + 10) == r->glucose && fk.sent[g].p[12] == 6);
    CHECK((int8_t)fk.sent[g].p[13] == -20);

    /* Backfill: frames, then 0x51 whose length, CRC and times match them, as
     * CGMBLEKit checks. */
    clear_sent();
    cgm_on_subscribe(&c, 0, CGM_CHR_BACKFILL, true);
    uint8_t buf[512];
    size_t total = 0;
    int frames = 0;
    for (int i = 0; i < fk.n_sent; i++) {
        if (fk.sent[i].chr == CGM_CHR_BACKFILL) {
            CHECK(fk.sent[i].p[0] == frames + 1); /* numbered from 1 */
            memcpy(buf + total, fk.sent[i].p, fk.sent[i].n);
            total += fk.sent[i].n;
            frames++;
        }
    }
    int m = find(CGM_CHR_CONTROL, 0x51, 0);
    CHECK(frames >= 1 && m >= 0);
    if (m < 0) {
        return;
    }
    const uint8_t *bm = fk.sent[m].p;
    CHECK(fk.sent[m].n == 20 && crc_ok(bm, 20));
    CHECK(le32(bm + 12) == total);
    CHECK(le16(bm + 16) == cgm_crc16(buf, total));
    /* The records: the frames' payloads, after a 4-byte header. */
    uint8_t data[512];
    size_t dn = 0;
    for (size_t off = 0; off < total;) {
        size_t fl = (total - off) >= 20 ? 20 : total - off;
        memcpy(data + dn, buf + off + 2, fl - 2);
        dn += fl - 2;
        off += fl;
    }
    size_t nrec = (dn - 4) / 8;
    CHECK(nrec == 3);
    CHECK(le32(data + 4) == le32(bm + 4));
    CHECK(le32(data + 4 + 8 * (nrec - 1)) == le32(bm + 8));
    CHECK(le16(data + 4 + 4) == 100); /* the first missed reading */

    /* Version: 0x4b, 19 bytes, not a 180-day ("Anubis") transmitter. */
    clear_sent();
    uint8_t req[3] = {0x4a};
    uint16_t crc = cgm_crc16(req, 1);
    req[1] = (uint8_t)crc;
    req[2] = (uint8_t)(crc >> 8);
    cgm_on_write(&c, 0, CGM_CHR_CONTROL, req, 3);
    CHECK(fk.n_sent == 1 && fk.sent[0].n == 19 && fk.sent[0].p[0] == 0x4b && crc_ok(fk.sent[0].p, 19));
    CHECK(((fk.sent[0].p[14] << 8) | fk.sent[0].p[13]) != 180);
    cgm_on_disconnect(&c, 0);
}

/* ---- a G6 session in active mode: CGMBLEKit logs in itself ----------------------- */

static void send_cmd(cgm_t *c, const uint8_t *p, size_t n)
{
    uint8_t m[32];
    memcpy(m, p, n);
    uint16_t crc = cgm_crc16(m, n);
    m[n] = (uint8_t)crc;
    m[n + 1] = (uint8_t)(crc >> 8);
    cgm_on_write(c, 0, CGM_CHR_CONTROL, m, n + 2);
}

static void test_g6_active(void)
{
    static cgm_t c;
    cgm_state_t st;
    memset(&fk, 0, sizeof fk);
    fk.now = 1800000000;
    cgm_state_fresh(&st, CGM_G6, "8G1234", fk.now);
    cgm_init(&c, &st, &fio);
    cgm_tick(&c);
    connect(&c);

    /* AuthRequestTx 01 token 02; read: 03 hash(token) challenge. */
    uint8_t tok[10] = {0x01, 1, 2, 3, 4, 5, 6, 7, 8, 0x02}, v[32], h[8];
    cgm_on_write(&c, 0, CGM_CHR_AUTH, tok, sizeof tok);
    size_t n = cgm_on_read(&c, 0, CGM_CHR_AUTH, v, sizeof v);
    CHECK(n == 17 && v[0] == 0x03);
    cgm_g6_hash("8G1234", tok + 1, h);
    CHECK(memcmp(v + 1, h, 8) == 0);
    /* AuthChallengeTx 04 hash(challenge); read: 05 01 01. */
    uint8_t ch[9] = {0x04};
    cgm_g6_hash("8G1234", v + 9, ch + 1);
    cgm_on_write(&c, 0, CGM_CHR_AUTH, ch, sizeof ch);
    n = cgm_on_read(&c, 0, CGM_CHR_AUTH, v, sizeof v);
    CHECK_BYTES(v, n, AUTH_OK, 3);

    /* Subscribing to control delivers the current reading at once. A real app
     * logs in and then waits for the push instead of asking for the message. */
    clear_sent();
    cgm_on_subscribe(&c, 0, CGM_CHR_CONTROL, true);
    CHECK(fk.n_sent == 2 && fk.sent[0].p[0] == 0x25 && fk.sent[1].p[0] == 0x4f);
    clear_sent();

    /* Start the sensor 5 seconds after activation (startSensor). */
    fk.now += 10;
    uint8_t start[9] = {0x26, 5};
    send_cmd(&c, start, 9);
    CHECK(fk.n_sent == 1 && fk.sent[0].p[0] == 0x27 && fk.sent[0].n == 17 && crc_ok(fk.sent[0].p, 17));
    CHECK(c.st.session_start == c.st.activated + 5);

    clear_sent();
    send_cmd(&c, (const uint8_t *)"\x24", 1);
    send_cmd(&c, (const uint8_t *)"\x30", 1);
    send_cmd(&c, (const uint8_t *)"\x32", 1);
    send_cmd(&c, (const uint8_t *)"\x4a", 1);
    CHECK(fk.n_sent == 4);
    CHECK(fk.sent[0].p[0] == 0x25 && fk.sent[0].n == 16 && crc_ok(fk.sent[0].p, 16));
    CHECK(fk.sent[1].p[0] == 0x31 && fk.sent[1].n == 16 && crc_ok(fk.sent[1].p, 16));
    CHECK(fk.sent[2].p[0] == 0x33 && fk.sent[2].n == 19 && crc_ok(fk.sent[2].p, 19));
    CHECK(fk.sent[3].p[0] == 0x4b && fk.sent[3].n == 19);
    CHECK(c.sessions[0].sent_glucose);

    /* Calibrate to 110: the calibration data says so. */
    clear_sent();
    uint8_t cal[7] = {0x34, 110, 0, 20, 0, 0, 0};
    send_cmd(&c, cal, 7);
    CHECK(fk.n_sent == 1 && fk.sent[0].p[0] == 0x35 && fk.sent[0].n == 5 && crc_ok(fk.sent[0].p, 5));
    send_cmd(&c, (const uint8_t *)"\x32", 1);
    CHECK(fk.n_sent == 2 && le16(fk.sent[1].p + 11) == 110 && le32(fk.sent[1].p + 13) == 20);

    /* A wrong challenge answer is refused. */
    cgm_on_write(&c, 0, CGM_CHR_AUTH, tok, sizeof tok);
    cgm_on_read(&c, 0, CGM_CHR_AUTH, v, sizeof v);
    uint8_t bad[9] = {0x04};
    cgm_on_write(&c, 0, CGM_CHR_AUTH, bad, sizeof bad);
    n = cgm_on_read(&c, 0, CGM_CHR_AUTH, v, sizeof v);
    CHECK(n == 3 && v[0] == 0x05 && v[1] != 0x01);

    /* Stop the sensor: no session. */
    clear_sent();
    uint8_t stop[5] = {0x28, 50};
    send_cmd(&c, stop, 5);
    CHECK(fk.n_sent == 1 && fk.sent[0].p[0] == 0x29 && le32(fk.sent[0].p + 7) == 0xffffffff);
    CHECK(cgm_current_state(&c.st, fk.now) == CGM_STATE_STOPPED);

    /* 0x09 is DisconnectTx: after reading the app sets CONTROL notifications
     * off and ends the session, so the simulator must close the link too. */
    int d = fk.disconnects;
    cgm_on_write(&c, 0, CGM_CHR_CONTROL, (const uint8_t *)"\x09", 1);
    CHECK(fk.disconnects == d + 1);
}

/* The real app's pairing: it logs in, subscribes to CONTROL, and then waits
 * for the simulator to push a reading. The first arrives on the subscribe, the
 * next on the tick that makes it; a second subscribe repeats nothing. */
static void test_g6_pairing(void)
{
    static cgm_t c;
    cgm_state_t st;
    memset(&fk, 0, sizeof fk);
    fk.now = 1800000000;
    cgm_state_fresh(&st, CGM_G6, "81AB7Q", fk.now);
    cgm_init(&c, &st, &fio);
    cgm_skip_warmup(&c);
    cgm_set_glucose(&c, 100, 0);
    cgm_tick(&c);
    const cgm_reading_t *r = &c.st.hist[c.st.n_hist - 1];
    connect(&c);

    /* Log in. The push must wait for the CONTROL subscription, because the app
     * writes that CCCD only after the login. */
    uint8_t tok[10] = {0x01, 1, 2, 3, 4, 5, 6, 7, 8, 0x02}, v[32], ch[9] = {0x04};
    cgm_on_write(&c, 0, CGM_CHR_AUTH, tok, sizeof tok);
    cgm_on_read(&c, 0, CGM_CHR_AUTH, v, sizeof v);
    cgm_g6_hash("81AB7Q", v + 9, ch + 1);
    clear_sent();
    cgm_on_write(&c, 0, CGM_CHR_AUTH, ch, sizeof ch);
    CHECK(c.sessions[0].authed && fk.n_sent == 0);

    /* A DisconnectTx before the app has a reading is still pairing. It must not
     * tear the link down: that is the bonding loop. */
    int d = fk.disconnects;
    cgm_on_write(&c, 0, CGM_CHR_CONTROL, (const uint8_t *)"\x09", 1);
    CHECK(fk.disconnects == d);
    CHECK(c.sessions[0].connected);

    /* The subscribe delivers the current reading: time, then glucose. */
    cgm_on_subscribe(&c, 0, CGM_CHR_CONTROL, true);
    int t = find_session(CGM_CHR_CONTROL, 0x25, 0, 0);
    int g = find_session(CGM_CHR_CONTROL, 0x4f, 0, 0);
    CHECK(fk.n_sent == 2 && t == 0 && g == 1);
    CHECK(g >= 0 && le32(fk.sent[g].p + 2) == r->seq);

    /* Subscribing again must not repeat the same reading. */
    clear_sent();
    cgm_on_subscribe(&c, 0, CGM_CHR_CONTROL, true);
    CHECK(fk.n_sent == 0);

    /* The next reading is pushed without the app asking for it. */
    clear_sent();
    run(&c, CGM_READING_INTERVAL);
    const cgm_reading_t *next = &c.st.hist[c.st.n_hist - 1];
    CHECK(next->seq == r->seq + 1);
    t = find_session(CGM_CHR_CONTROL, 0x25, 0, 0);
    g = find_session(CGM_CHR_CONTROL, 0x4f, 0, 0);
    CHECK(fk.n_sent == 2 && t == 0 && g == 1);
    CHECK(g >= 0 && le32(fk.sent[g].p + 2) == next->seq);

    /* GlucoseBackfillTx from the app: answer with the history and 0x51, or the
     * app sits for 10 s and ends the session without a second entry. */
    clear_sent();
    uint8_t hist[18] = {0x50, 0x05, 0x02, 0x00, 0, 0, 0, 0, 0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0, 0, 0};
    send_cmd(&c, hist, sizeof hist);
    CHECK(find_session(CGM_CHR_BACKFILL, 1, 0, 0) >= 0);
    CHECK(find_session(CGM_CHR_CONTROL, 0x51, 0, 0) >= 0);

    /* A non-zero id and a narrow range: only the requested reading, with the id
     * echoed in the frame header and in 0x51. The two readings are 300 s apart,
     * so a start==end at the second one covers exactly one record. */
    clear_sent();
    uint32_t t1 = next->t - c.st.activated;
    uint8_t hist2[18] = {0x50, 0x05, 0x02, 0x2a, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    hist2[4] = (uint8_t)t1;
    hist2[5] = (uint8_t)(t1 >> 8);
    hist2[6] = (uint8_t)(t1 >> 16);
    hist2[7] = (uint8_t)(t1 >> 24);
    memcpy(hist2 + 8, hist2 + 4, 4);
    send_cmd(&c, hist2, sizeof hist2);
    int bf = find_session(CGM_CHR_BACKFILL, 1, 0, 0);
    int m51 = find_session(CGM_CHR_CONTROL, 0x51, 0, 0);
    CHECK(bf >= 0 && fk.sent[bf].p[1] == 0x2a); /* the echoed backfill id */
    CHECK(m51 >= 0 && fk.sent[m51].p[3] == 0x2a);
    CHECK(m51 >= 0 && le32(fk.sent[m51].p + 4) == t1 && le32(fk.sent[m51].p + 8) == t1);
    CHECK(m51 >= 0 && le32(fk.sent[m51].p + 12) == 14); /* 4-byte header + one 8-byte record + 2-byte frame */

    /* Once it has a reading, DisconnectTx is the end of this session. */
    d = fk.disconnects;
    cgm_on_write(&c, 0, CGM_CHR_CONTROL, (const uint8_t *)"\x09", 1);
    CHECK(fk.disconnects == d + 1);
    cgm_on_disconnect(&c, 0);
}

static void test_two_sessions(void)
{
    static cgm_t c;
    cgm_state_t st;

    /* Both centrals get the one cached reading; the second one connects after
     * the phone already collected it. */
    memset(&fk, 0, sizeof fk);
    fk.now = 1800000000;
    cgm_state_fresh(&st, CGM_G7, NULL, fk.now);
    cgm_init(&c, &st, &fio);
    cgm_skip_warmup(&c);
    cgm_set_glucose(&c, 140, 30);
    cgm_tick(&c);
    const cgm_reading_t *r = &c.st.hist[c.st.n_hist - 1];

    connect_n(&c, 0);
    clear_sent();
    cgm_on_subscribe(&c, 0, CGM_CHR_CONTROL, true);
    CHECK(c.sessions[0].sent_glucose && !c.sessions[1].sent_glucose);
    int g0 = find_session(CGM_CHR_CONTROL, 0x4e, 0, 0);
    CHECK(g0 >= 0 && le16(fk.sent[g0].p + 6) == r->seq && fk.sent[g0].session == 0);

    connect_n(&c, 1);
    cgm_on_subscribe(&c, 1, CGM_CHR_CONTROL, true);
    CHECK(c.sessions[1].sent_glucose);
    int g1 = find_session(CGM_CHR_CONTROL, 0x4e, 0, 1);
    CHECK(g1 >= 0 && le16(fk.sent[g1].p + 6) == r->seq && fk.sent[g1].session == 1);

    /* A session with its reading is hung up on its own: the other one still
     * collecting (here, still active) keeps its link. */
    clear_sent();
    run(&c, 2);
    uint8_t junk[8];
    cgm_on_read(&c, 1, CGM_CHR_AUTH, junk, sizeof junk);
    run(&c, 1);
    CHECK(fk.disconnects_for[0] == 1 && fk.disconnects_for[1] == 0);
    CHECK(fk.last_disconnect == 0);
    cgm_on_disconnect(&c, 0);
    CHECK(!c.sessions[0].connected && c.sessions[1].connected);
    run(&c, 2); /* the watch's own idle timer ends it later */
    CHECK(fk.disconnects_for[1] == 1);
    cgm_on_disconnect(&c, 1);

    /* The backfill cursor is per session. The phone starts from the oldest
     * reading it has never had; when it leaves after collecting the latest, the
     * watch connects with its own untouched cursor and backfills the older
     * readings too instead of inheriting the phone's. */
    memset(&fk, 0, sizeof fk);
    fk.now = 1800000000;
    cgm_state_fresh(&st, CGM_G7, NULL, fk.now);
    cgm_init(&c, &st, &fio);
    cgm_skip_warmup(&c);
    cgm_set_glucose(&c, 120, 0);
    cgm_tick(&c);                      /* one reading */
    run(&c, CGM_READING_INTERVAL);     /* a second */
    run(&c, CGM_READING_INTERVAL);     /* a third */
    CHECK(c.st.n_hist == 3 && c.st.delivered == 0);
    uint32_t last_seq = c.st.hist[c.st.n_hist - 1].seq;

    connect_n(&c, 0);
    clear_sent();
    cgm_on_subscribe(&c, 0, CGM_CHR_CONTROL, true);  /* the cached latest */
    cgm_on_subscribe(&c, 0, CGM_CHR_BACKFILL, true); /* the two older readings */
    CHECK(c.sessions[0].prev_delivered == 0 && c.st.delivered == last_seq);
    CHECK(count_session(CGM_CHR_BACKFILL, 0) == 2);
    CHECK(c.sessions[0].delivered == last_seq);

    /* The phone leaves; the watch was absent while the phone collected. */
    cgm_on_disconnect(&c, 0);
    CHECK(!c.sessions[0].connected);
    connect_n(&c, 1);
    clear_sent();
    cgm_on_subscribe(&c, 1, CGM_CHR_CONTROL, true);
    cgm_on_subscribe(&c, 1, CGM_CHR_BACKFILL, true);
    CHECK(c.sessions[1].prev_delivered == 0); /* its own cursor, not the phone's */
    CHECK(count_session(CGM_CHR_BACKFILL, 1) == 2);
    CHECK(c.sessions[1].delivered == last_seq);

    /* The G6 login is per session too: one central's challenge and login do
     * not authenticate the other. */
    memset(&fk, 0, sizeof fk);
    fk.now = 1800000000;
    cgm_state_fresh(&st, CGM_G6, "8G1234", fk.now);
    cgm_init(&c, &st, &fio);
    cgm_tick(&c);
    connect_n(&c, 0);
    connect_n(&c, 1);
    uint8_t tok[10] = {0x01, 1, 2, 3, 4, 5, 6, 7, 8, 0x02}, v0[32], v1[32];
    cgm_on_write(&c, 0, CGM_CHR_AUTH, tok, sizeof tok);
    cgm_on_write(&c, 1, CGM_CHR_AUTH, tok, sizeof tok);
    size_t n0 = cgm_on_read(&c, 0, CGM_CHR_AUTH, v0, sizeof v0);
    size_t n1 = cgm_on_read(&c, 1, CGM_CHR_AUTH, v1, sizeof v1);
    CHECK(n0 == 17 && n1 == 17 && v0[0] == 0x03 && v1[0] == 0x03);
    CHECK(memcmp(v0 + 9, v1 + 9, 8) != 0); /* independent challenges */

    uint8_t ch[9] = {0x04};
    cgm_g6_hash("8G1234", v0 + 9, ch + 1);
    cgm_on_write(&c, 0, CGM_CHR_AUTH, ch, sizeof ch);
    n0 = cgm_on_read(&c, 0, CGM_CHR_AUTH, v0, sizeof v0);
    CHECK_BYTES(v0, n0, AUTH_OK, 3);
    CHECK(c.sessions[0].authed && !c.sessions[1].authed);
    n1 = cgm_on_read(&c, 1, CGM_CHR_AUTH, v1, sizeof v1);
    CHECK(n1 == 17 && v1[0] == 0x03);
    cgm_g6_hash("8G1234", v1 + 9, ch + 1);
    cgm_on_write(&c, 1, CGM_CHR_AUTH, ch, sizeof ch);
    n1 = cgm_on_read(&c, 1, CGM_CHR_AUTH, v1, sizeof v1);
    CHECK_BYTES(v1, n1, AUTH_OK, 3);
    CHECK(c.sessions[1].authed);
}

static void test_tick_delivery(void)
{
    static cgm_t c;
    cgm_state_t st;
    memset(&fk, 0, sizeof fk);
    fk.now = 1800000000;
    cgm_state_fresh(&st, CGM_G7, NULL, fk.now);
    cgm_init(&c, &st, &fio);
    cgm_skip_warmup(&c);
    cgm_set_glucose(&c, 130, 0);
    cgm_tick(&c);
    uint32_t first_seq = c.st.hist[c.st.n_hist - 1].seq;

    /* A connected, subscribed central collects the reading it was offered. */
    connect_n(&c, 0);
    clear_sent();
    cgm_on_subscribe(&c, 0, CGM_CHR_CONTROL, true);
    int g = find_session(CGM_CHR_CONTROL, 0x4e, 0, 0);
    CHECK(g >= 0 && le16(fk.sent[g].p + 6) == first_seq);
    CHECK(c.sessions[0].control_on && c.sessions[0].sent_glucose);

    /* A second central joins and gets the cached reading too. */
    connect_n(&c, 1);
    cgm_on_subscribe(&c, 1, CGM_CHR_CONTROL, true);
    CHECK(c.sessions[1].control_on && c.sessions[1].sent_glucose);

    /* The next reading is made while both are still connected and subscribed:
     * tick delivers it to every session that has not had it. Both are idle, so
     * the same tick also asks each session to hang up. */
    clear_sent();
    fk.now += CGM_READING_INTERVAL;
    cgm_tick(&c);
    uint32_t next_seq = c.st.hist[c.st.n_hist - 1].seq;
    CHECK(next_seq == first_seq + 1);
    g = find_session(CGM_CHR_CONTROL, 0x4e, 0, 0);
    CHECK(g >= 0 && le16(fk.sent[g].p + 6) == next_seq);
    CHECK(c.sessions[0].sent_glucose && c.sessions[0].delivered == next_seq);
    g = find_session(CGM_CHR_CONTROL, 0x4e, 0, 1);
    CHECK(g >= 0 && le16(fk.sent[g].p + 6) == next_seq);
    CHECK(c.sessions[1].sent_glucose && c.sessions[1].delivered == next_seq);
    CHECK(fk.disconnects_for[0] == 1 && fk.disconnects_for[1] == 1);
    cgm_on_disconnect(&c, 0);
    cgm_on_disconnect(&c, 1);
}

/* A different central that takes a slot someone else used must start fresh.
 * Nothing is inferred from who the central is: the connect itself clears the
 * session. */
static void test_slot_reuse(void)
{
    static cgm_t c;
    cgm_state_t st;
    memset(&fk, 0, sizeof fk);
    fk.now = 1800000000;
    cgm_state_fresh(&st, CGM_G7, NULL, fk.now);
    cgm_init(&c, &st, &fio);
    cgm_skip_warmup(&c);
    cgm_set_glucose(&c, 120, 0);
    cgm_tick(&c);                      /* one reading */
    run(&c, CGM_READING_INTERVAL);     /* a second */
    run(&c, CGM_READING_INTERVAL);     /* a third */
    CHECK(c.st.n_hist == 3);
    uint32_t last_seq = c.st.hist[c.st.n_hist - 1].seq;

    /* The phone owns slot 0, collects the latest and backfills the two older
     * readings, then leaves with its cursor at the latest. */
    connect_n(&c, 0);
    clear_sent();
    cgm_on_subscribe(&c, 0, CGM_CHR_CONTROL, true);
    cgm_on_subscribe(&c, 0, CGM_CHR_BACKFILL, true);
    CHECK(c.sessions[0].delivered == last_seq && c.sessions[0].prev_delivered == 0);
    CHECK(count_session(CGM_CHR_BACKFILL, 0) == 2);
    cgm_on_disconnect(&c, 0);

    /* A second central reuses slot 0. Its connect is a fresh session, so it
     * cannot inherit the phone's cursor. */
    connect_n(&c, 0);
    CHECK(c.sessions[0].delivered == 0 && c.sessions[0].prev_delivered == 0);
    clear_sent();
    cgm_on_subscribe(&c, 0, CGM_CHR_CONTROL, true);
    cgm_on_subscribe(&c, 0, CGM_CHR_BACKFILL, true);
    CHECK(c.sessions[0].delivered == last_seq && c.sessions[0].prev_delivered == 0);
    CHECK(count_session(CGM_CHR_BACKFILL, 0) == 2);
    CHECK(find_session(CGM_CHR_CONTROL, 0x4e, 0, 0) >= 0);
    cgm_on_disconnect(&c, 0);
}

/* The peer's privacy address rotates on reconnect. The core never sees an
 * address, and a session is one connection, so the reconnect must clear every
 * field the previous connection left -- not resume. */
static void test_address_rotate(void)
{
    static cgm_t c;
    cgm_state_t st;
    memset(&fk, 0, sizeof fk);
    fk.now = 1800000000;
    cgm_state_fresh(&st, CGM_G7, NULL, fk.now);
    cgm_init(&c, &st, &fio);
    cgm_skip_warmup(&c);
    cgm_set_glucose(&c, 120, 0);
    cgm_tick(&c);
    run(&c, CGM_READING_INTERVAL);
    run(&c, CGM_READING_INTERVAL);
    CHECK(c.st.n_hist == 3);
    uint32_t last_seq = c.st.hist[c.st.n_hist - 1].seq;

    connect_n(&c, 0);
    clear_sent();
    cgm_on_subscribe(&c, 0, CGM_CHR_CONTROL, true);
    cgm_on_subscribe(&c, 0, CGM_CHR_BACKFILL, true);
    CHECK(c.sessions[0].delivered == last_seq);

    /* Dirty every field one connection describes, then close it. */
    c.sessions[0].authed = true;
    memset(c.sessions[0].challenge, 0xaa, sizeof c.sessions[0].challenge);
    memset(c.sessions[0].auth_value, 0xbb, sizeof c.sessions[0].auth_value);
    c.sessions[0].auth_len = 3;
    c.sessions[0].control_on = true;
    c.sessions[0].sent_glucose = true;
    c.sessions[0].prev_delivered = 42;
    c.sessions[0].connected_at = fk.now - 10;
    c.sessions[0].last_activity = fk.now - 10;
    cgm_on_disconnect(&c, 0);
    CHECK(c.sessions[0].delivered == last_seq); /* the close itself does not clear */

    /* The same slot reconnects with a rotated address; the connect clears all. */
    connect_n(&c, 0);
    CHECK(c.sessions[0].connected);
    CHECK(c.sessions[0].delivered == 0 && c.sessions[0].prev_delivered == 0);
    CHECK(!c.sessions[0].sent_glucose && !c.sessions[0].authed && !c.sessions[0].control_on);
    CHECK(c.sessions[0].auth_len == 0);
    CHECK(c.sessions[0].challenge[0] == 0 && c.sessions[0].auth_value[0] == 0);
    CHECK(c.sessions[0].connected_at == fk.now && c.sessions[0].last_activity == fk.now);

    clear_sent();
    cgm_on_subscribe(&c, 0, CGM_CHR_CONTROL, true);
    cgm_on_subscribe(&c, 0, CGM_CHR_BACKFILL, true);
    CHECK(c.sessions[0].delivered == last_seq && c.sessions[0].prev_delivered == 0);
    CHECK(count_session(CGM_CHR_BACKFILL, 0) == 2);
    CHECK(find_session(CGM_CHR_CONTROL, 0x4e, 0, 0) >= 0);
    cgm_on_disconnect(&c, 0);
}

/* A lone central's reading-boundary delivery and hang-up, as README documents:
 * for a G7 the tick that makes a new reading sends it before the idle hang-up,
 * and the reconnect still gets the latest reading again (the same seq may be
 * seen twice). A G6 keeps the link instead. This is what the old single
 * session sent nothing for. */
static void test_single_boundary(void)
{
    static cgm_t c;
    cgm_state_t st;

    /* G7: one CONTROL message, no time-then-glucose pair. */
    memset(&fk, 0, sizeof fk);
    fk.now = 1800000000;
    cgm_state_fresh(&st, CGM_G7, NULL, fk.now);
    cgm_init(&c, &st, &fio);
    cgm_skip_warmup(&c);
    cgm_set_glucose(&c, 130, 0);
    cgm_tick(&c);
    uint32_t first_seq = c.st.hist[c.st.n_hist - 1].seq;
    connect_n(&c, 0);
    clear_sent();
    cgm_on_subscribe(&c, 0, CGM_CHR_CONTROL, true);
    CHECK(find_session(CGM_CHR_CONTROL, 0x4e, 0, 0) >= 0);

    clear_sent();
    fk.now += CGM_READING_INTERVAL;
    cgm_tick(&c);
    uint32_t next_seq = c.st.hist[c.st.n_hist - 1].seq;
    CHECK(next_seq == first_seq + 1);
    CHECK(fk.n_sent == 1);
    CHECK(fk.sent[0].session == 0 && fk.sent[0].chr == CGM_CHR_CONTROL && fk.sent[0].p[0] == 0x4e);
    CHECK(le16(fk.sent[0].p + 6) == next_seq);
    CHECK(c.sessions[0].delivered == next_seq);
    CHECK(fk.disconnects_for[0] == 1); /* the same tick hangs it up */
    cgm_on_disconnect(&c, 0);

    /* The reconnect gets the latest once more, as before. */
    connect_n(&c, 0);
    clear_sent();
    cgm_on_subscribe(&c, 0, CGM_CHR_CONTROL, true);
    int g = find_session(CGM_CHR_CONTROL, 0x4e, 0, 0);
    CHECK(g >= 0 && le16(fk.sent[g].p + 6) == next_seq);
    cgm_on_disconnect(&c, 0);

    /* G6: time (0x25) then glucose (0x4f) to the one central. */
    memset(&fk, 0, sizeof fk);
    fk.now = 1800000000;
    cgm_state_fresh(&st, CGM_G6, "81AB7Q", fk.now);
    cgm_init(&c, &st, &fio);
    cgm_skip_warmup(&c);
    cgm_set_glucose(&c, 100, 0);
    cgm_tick(&c);
    first_seq = c.st.hist[c.st.n_hist - 1].seq;
    connect_n(&c, 0);
    clear_sent();
    cgm_on_subscribe(&c, 0, CGM_CHR_CONTROL, true);
    CHECK(find_session(CGM_CHR_CONTROL, 0x25, 0, 0) >= 0);
    CHECK(find_session(CGM_CHR_CONTROL, 0x4f, 0, 0) >= 0);

    clear_sent();
    fk.now += CGM_READING_INTERVAL;
    cgm_tick(&c);
    next_seq = c.st.hist[c.st.n_hist - 1].seq;
    CHECK(next_seq == first_seq + 1);
    CHECK(fk.n_sent == 2);
    CHECK(fk.sent[0].session == 0 && fk.sent[0].chr == CGM_CHR_CONTROL && fk.sent[0].p[0] == 0x25);
    CHECK(fk.sent[1].session == 0 && fk.sent[1].chr == CGM_CHR_CONTROL && fk.sent[1].p[0] == 0x4f);
    CHECK(c.sessions[0].delivered == next_seq);
    CHECK(fk.disconnects_for[0] == 0); /* a G6 keeps the link */
    cgm_on_disconnect(&c, 0);
}

/* A G6 keeps the link it has and pushes each reading on it; a G7 is hung up
 * after it has collected its reading. */
static void test_hangup(void)
{
    static cgm_t c;
    cgm_state_t st;

    /* G6: two sessions connect, take the current reading, stay quiet past the
     * idle timeout and the connection cap, and still get the next reading. */
    memset(&fk, 0, sizeof fk);
    fk.now = 1800000000;
    cgm_state_fresh(&st, CGM_G6, "81AB7Q", fk.now);
    cgm_init(&c, &st, &fio);
    cgm_set_glucose(&c, 110, 0);
    cgm_tick(&c);
    uint32_t first_seq = c.st.hist[c.st.n_hist - 1].seq;
    connect_n(&c, 0);
    clear_sent();
    cgm_on_subscribe(&c, 0, CGM_CHR_CONTROL, true);
    int g = find_session(CGM_CHR_CONTROL, 0x4f, 0, 0);
    CHECK(g >= 0 && le32(fk.sent[g].p + 2) == first_seq);
    CHECK(c.sessions[0].sent_glucose && c.sessions[0].delivered == first_seq);

    /* Past both IDLE_END and CONN_MAX: no hang-up. */
    run(&c, 31);
    CHECK(fk.disconnects_for[0] == 0);
    CHECK(c.sessions[0].connected);

    /* A second session joins and gets the cached reading too. */
    connect_n(&c, 1);
    clear_sent();
    cgm_on_subscribe(&c, 1, CGM_CHR_CONTROL, true);
    g = find_session(CGM_CHR_CONTROL, 0x4f, 0, 1);
    CHECK(g >= 0 && le32(fk.sent[g].p + 2) == first_seq);
    CHECK(c.sessions[1].sent_glucose && c.sessions[1].delivered == first_seq);

    /* The next reading reaches both sessions on the same links. */
    clear_sent();
    run(&c, CGM_READING_INTERVAL - 31);
    uint32_t next_seq = c.st.hist[c.st.n_hist - 1].seq;
    CHECK(next_seq == first_seq + 1);
    g = find_session(CGM_CHR_CONTROL, 0x4f, 0, 0);
    CHECK(g >= 0 && le32(fk.sent[g].p + 2) == next_seq);
    g = find_session(CGM_CHR_CONTROL, 0x4f, 0, 1);
    CHECK(g >= 0 && le32(fk.sent[g].p + 2) == next_seq);
    CHECK(count_session(CGM_CHR_CONTROL, 0) == 2); /* time, then glucose */
    CHECK(count_session(CGM_CHR_CONTROL, 1) == 2);
    CHECK(c.sessions[0].connected && c.sessions[1].connected);
    CHECK(fk.disconnects_for[0] == 0 && fk.disconnects_for[1] == 0);

    /* A G7 still collects one reading per link and is hung up on. */
    memset(&fk, 0, sizeof fk);
    fk.now = 1800000000;
    cgm_state_fresh(&st, CGM_G7, NULL, fk.now);
    cgm_init(&c, &st, &fio);
    cgm_tick(&c);
    connect_n(&c, 0);
    clear_sent();
    cgm_on_subscribe(&c, 0, CGM_CHR_CONTROL, true);
    CHECK(find_session(CGM_CHR_CONTROL, 0x4e, 0, 0) >= 0);
    run(&c, 2);
    CHECK(fk.disconnects_for[0] == 0); /* still collecting: IDLE_END not reached */
    run(&c, 1);
    CHECK(fk.disconnects_for[0] == 1); /* hung up after its reading */
    CHECK(fk.last_disconnect == 0);
}

/* A G6 advertises for as long as it is unconnected; only a G7 closes its
 * window and waits for the next reading. Signal loss silences both. */
static void test_advertise(void)
{
    static cgm_t c;
    cgm_state_t st;

    /* G6: one reading and no connection. The 60 s window is long gone. */
    memset(&fk, 0, sizeof fk);
    fk.now = 1800000000;
    cgm_state_fresh(&st, CGM_G6, "81AB7Q", fk.now);
    cgm_init(&c, &st, &fio);
    cgm_set_glucose(&c, 120, 0);
    cgm_tick(&c);
    CHECK(c.st.n_hist == 1 && fk.advertising);
    CHECK(strcmp(fk.adv_name, "Dexcom7Q") == 0);
    run(&c, 61);
    CHECK(c.window_end < fk.now); /* the window closed at 60 s */
    CHECK(fk.advertising);        /* but a G6 is still there */

    /* As close to five minutes after its last reading as the 5-minute reading
     * grid allows (299 s; the next reading is one second away): the old core
     * went silent at 60 s. */
    run(&c, CGM_READING_INTERVAL - 1 - 61);
    CHECK(fk.now - (int64_t)c.st.hist[c.st.n_hist - 1].t == CGM_READING_INTERVAL - 1);
    CHECK(c.window_end < fk.now);
    CHECK(fk.advertising);
    /* The next reading's window opens and closes; the G6 never goes away. */
    run(&c, 1);
    CHECK(fk.now - (int64_t)c.st.hist[c.st.n_hist - 1].t == 0);
    CHECK(fk.advertising);
    run(&c, 61);
    CHECK(c.window_end < fk.now);
    CHECK(fk.advertising);

    /* Signal loss is authoritative, G6 included; clearing it advertises again. */
    cgm_set_signal_loss(&c, true);
    cgm_tick(&c);
    CHECK(!fk.advertising);
    run(&c, CGM_READING_INTERVAL);
    CHECK(!fk.advertising);
    cgm_set_signal_loss(&c, false);
    cgm_tick(&c);
    CHECK(fk.advertising);
    run(&c, 61);
    CHECK(c.window_end < fk.now);
    CHECK(fk.advertising);

    /* A reading a phone collected and then left does not silence a G6. */
    connect_n(&c, 0);
    clear_sent();
    cgm_on_subscribe(&c, 0, CGM_CHR_CONTROL, true);
    CHECK(c.sessions[0].sent_glucose && c.st.delivered == c.st.hist[c.st.n_hist - 1].seq);
    cgm_on_disconnect(&c, 0);
    run(&c, 61);
    CHECK(c.window_end < fk.now);
    CHECK(fk.advertising);

    /* G7: the same reading, no connection. Its window still closes. */
    memset(&fk, 0, sizeof fk);
    fk.now = 1800000000;
    cgm_state_fresh(&st, CGM_G7, NULL, fk.now);
    cgm_init(&c, &st, &fio);
    cgm_tick(&c);
    CHECK(c.st.n_hist == 1 && fk.advertising);
    run(&c, 61);
    CHECK(c.window_end < fk.now);
    CHECK(!fk.advertising); /* reading 1 missed */
    run(&c, CGM_READING_INTERVAL - 1 - 61);
    CHECK(fk.now - (int64_t)c.st.hist[c.st.n_hist - 1].t == CGM_READING_INTERVAL - 1);
    CHECK(!fk.advertising); /* a G7 in the G6's state is not advertising */
    /* The next reading opens a window, and it closes again: unchanged. */
    run(&c, 1);
    CHECK(fk.now - (int64_t)c.st.hist[c.st.n_hist - 1].t == 0);
    CHECK(fk.advertising);
    run(&c, 61);
    CHECK(c.window_end < fk.now);
    CHECK(!fk.advertising);

    /* Signal loss stops a G7 too, and it stays off until the next window. */
    cgm_set_signal_loss(&c, true);
    cgm_tick(&c);
    CHECK(!fk.advertising);
    run(&c, CGM_READING_INTERVAL);
    CHECK(!fk.advertising);
    cgm_set_signal_loss(&c, false);
    cgm_tick(&c);
    CHECK(!fk.advertising); /* a G7 waits for the next reading's window */
    for (int i = 0; i < CGM_READING_INTERVAL && !fk.advertising; i++) {
        run(&c, 1);
    }
    CHECK(fk.advertising);
}

static void test_bad_session(void)
{
    static cgm_t c;
    cgm_state_t st;
    memset(&fk, 0, sizeof fk);
    fk.now = 1800000000;
    cgm_state_fresh(&st, CGM_G7, NULL, fk.now);
    cgm_init(&c, &st, &fio);
    cgm_tick(&c);
    clear_sent();

    /* Every core entry point ignores a token that names no session. */
    cgm_session_t bad = CGM_MAX_SESSIONS;
    uint8_t v[32], msg[2] = {0, 0};
    cgm_on_connect(&c, bad);
    cgm_on_subscribe(&c, bad, CGM_CHR_AUTH, true);
    cgm_on_write(&c, bad, CGM_CHR_CONTROL, msg, sizeof msg);
    CHECK(cgm_on_read(&c, bad, CGM_CHR_AUTH, v, sizeof v) == 0);
    cgm_on_disconnect(&c, bad);
    CHECK(fk.n_sent == 0);
    CHECK(!c.sessions[0].connected && !c.sessions[1].connected);
}

static void test_drop_all_sessions(void)
{
    static cgm_t c;
    cgm_state_t st;
    memset(&fk, 0, sizeof fk);
    fk.now = 1800000000;
    cgm_state_fresh(&st, CGM_G7, NULL, fk.now);
    cgm_init(&c, &st, &fio);
    cgm_tick(&c);
    connect_n(&c, 0);
    connect_n(&c, 1);

    /* Signal loss and a new sensor ask for every connected session to end. */
    int d0 = fk.disconnects_for[0], d1 = fk.disconnects_for[1];
    cgm_set_signal_loss(&c, true);
    CHECK(fk.disconnects_for[0] == d0 + 1 && fk.disconnects_for[1] == d1 + 1);
    cgm_set_signal_loss(&c, false);
    cgm_on_disconnect(&c, 0);
    cgm_on_disconnect(&c, 1);

    connect_n(&c, 0);
    connect_n(&c, 1);
    c.sessions[0].delivered = c.sessions[1].delivered = 99;
    d0 = fk.disconnects_for[0];
    d1 = fk.disconnects_for[1];
    cgm_new_sensor(&c, CGM_G6, "81AB7Q");
    CHECK(fk.disconnects_for[0] == d0 + 1 && fk.disconnects_for[1] == d1 + 1);
    CHECK(c.sessions[0].delivered == 0 && c.sessions[1].delivered == 0);
    cgm_on_disconnect(&c, 0);
    cgm_on_disconnect(&c, 1);

    /* G6: with the link kept, signal loss is the main way it ends. It drops
     * both sessions, and nothing advertises until the loss is cleared. */
    memset(&fk, 0, sizeof fk);
    fk.now = 1800000000;
    cgm_state_fresh(&st, CGM_G6, "81AB7Q", fk.now);
    cgm_init(&c, &st, &fio);
    cgm_tick(&c);
    CHECK(fk.advertising);
    connect_n(&c, 0);
    connect_n(&c, 1);
    d0 = fk.disconnects_for[0];
    d1 = fk.disconnects_for[1];
    cgm_set_signal_loss(&c, true);
    CHECK(fk.disconnects_for[0] == d0 + 1 && fk.disconnects_for[1] == d1 + 1);
    cgm_on_disconnect(&c, 0);
    cgm_on_disconnect(&c, 1);
    cgm_tick(&c);
    CHECK(!fk.advertising);
    run(&c, 61);
    CHECK(!fk.advertising);
    cgm_set_signal_loss(&c, false);
    cgm_tick(&c);
    CHECK(fk.advertising);
}

/* The glucose pattern generator: pure, deterministic, shared by both kinds. */
static void test_patterns(void)
{
    /* Fixed stays put (and keeps the manual trend when one is set). */
    cgm_pattern_t fixed = {CGM_PATTERN_FIXED, 123, 0, 0, 0};
    for (uint32_t n = 0; n < 50; n++) {
        cgm_next_reading_t r = cgm_pattern_next(&fixed, 123, n);
        CHECK(r.glucose == 123);
        CHECK(r.trend == 0);
    }
    fixed.trend = -10;
    CHECK(cgm_pattern_next(&fixed, 123, 7).glucose == 123);
    CHECK(cgm_pattern_next(&fixed, 123, 7).trend == -10);

    /* Sine: one 60-minute period is 12 readings. The wave stays inside
     * 150 +/- 50, hits both limits, and comes back to the same value after a
     * full period. */
    cgm_pattern_t sine = {CGM_PATTERN_SINE, 150, 0, 50, 60};
    cgm_next_reading_t seq[13];
    seq[0] = cgm_pattern_next(&sine, 0, 0);
    for (uint32_t n = 1; n <= 12; n++) {
        seq[n] = cgm_pattern_next(&sine, seq[n - 1].glucose, n);
    }
    for (uint32_t n = 0; n <= 12; n++) {
        CHECK(seq[n].glucose >= 100 && seq[n].glucose <= 200);
    }
    CHECK(seq[0].glucose == 150); /* the mean at phase 0 */
    CHECK(seq[3].glucose == 200); /* +amplitude at a quarter period */
    CHECK(seq[6].glucose == 150); /* back through the mean at half */
    CHECK(seq[9].glucose == 100); /* -amplitude at three quarters */
    CHECK(seq[12].glucose == seq[0].glucose); /* a full period repeats */
    for (uint32_t n = 1; n <= 12; n++) {
        int delta = (int)seq[n].glucose - (int)seq[n - 1].glucose;
        CHECK(delta == 0 ? seq[n].trend == 0 : (delta > 0) == (seq[n].trend > 0));
    }
    CHECK(seq[1].trend > 0); /* rising away from the mean */
    CHECK(seq[4].trend < 0); /* falling after the peak */
    /* The period parameter is honoured: 120 minutes makes the peak at n = 6. */
    sine.period_minutes = 120;
    seq[0] = cgm_pattern_next(&sine, 0, 0);
    for (uint32_t n = 1; n <= 12; n++) {
        seq[n] = cgm_pattern_next(&sine, seq[n - 1].glucose, n);
    }
    CHECK(seq[6].glucose == 200);
    CHECK(seq[12].glucose == 150);

    /* Walk: every step moves toward the target, never overshoots it, and stays
     * inside the 40..400 range until it arrives. */
    cgm_pattern_t walk = {CGM_PATTERN_WALK, 300, 0, 0, 0};
    uint16_t prev = 120;
    for (uint32_t n = 0; n < 100; n++) {
        cgm_next_reading_t r = cgm_pattern_next(&walk, prev, n);
        CHECK(r.glucose >= 40 && r.glucose <= 400);
        if (n > 0 && prev != 300) {
            CHECK(abs((int)r.glucose - 300) < abs((int)prev - 300));
        }
        if (r.glucose != prev) {
            CHECK((r.glucose > prev) == (300 > prev));
            CHECK((r.trend > 0) == (r.glucose > prev));
        }
        prev = r.glucose;
    }
    CHECK(prev == 300); /* it arrives, and then stays */
    walk.value = 60;
    prev = 300;
    for (uint32_t n = 0; n < 100; n++) {
        cgm_next_reading_t r = cgm_pattern_next(&walk, prev, n);
        CHECK(r.glucose >= 40 && r.glucose <= 400);
        if (n > 0 && prev != 60) {
            CHECK(abs((int)r.glucose - 60) < abs((int)prev - 60));
        }
        if (r.glucose != prev) {
            CHECK((r.glucose > prev) == (60 > prev));
            CHECK((r.trend > 0) == (r.glucose > prev));
        }
        prev = r.glucose;
    }
    CHECK(prev == 60);

    /* The pattern is part of the core state, so a saved blob keeps it and the
     * next reading after a restore is identical. */
    memset(&fk, 0, sizeof fk);
    fk.now = 1800000000;
    cgm_state_t st;
    cgm_state_fresh(&st, CGM_G7, NULL, fk.now);
    CHECK(st.version == CGM_STATE_VERSION);
    st.warmup_done = fk.now;
    st.pattern = CGM_PATTERN_SINE;
    st.glucose = 150;
    st.amplitude = 50;
    st.period_minutes = 60;
    static cgm_t c, restored;
    cgm_init(&c, &st, &fio);
    cgm_tick(&c);
    CHECK(c.st.n_hist == 1 && c.st.hist[0].glucose == 150);
    CHECK(c.st.hist[0].trend == 0); /* the first reading has no previous reading */
    CHECK(c.st.pattern == CGM_PATTERN_SINE && c.st.amplitude == 50 && c.st.period_minutes == 60);
    cgm_state_t saved = c.st; /* what sensor_app puts in its NVS blob */
    CHECK(saved.version == CGM_STATE_VERSION);
    cgm_init(&restored, &saved, &fio);
    CHECK(restored.st.pattern == CGM_PATTERN_SINE);
    CHECK(restored.st.amplitude == 50 && restored.st.period_minutes == 60);
    fk.now += CGM_READING_INTERVAL;
    cgm_tick(&c);
    cgm_tick(&restored);
    CHECK(c.st.n_hist == 2 && restored.st.n_hist == 2);
    CHECK(restored.st.hist[1].glucose == c.st.hist[1].glucose);
    CHECK(restored.st.hist[1].trend == c.st.hist[1].trend);

    /* A version-1 save (the person pattern, no generator parameters) is
     * brought forward: the old pattern numbers still mean the same thing. */
    cgm_state_t old = saved;
    old.version = 1;
    old.amplitude = 0;
    old.period_minutes = 0;
    old.pattern = CGM_PATTERN_PERSON;
    cgm_state_migrate(&old);
    CHECK(old.version == CGM_STATE_VERSION && old.pattern == CGM_PATTERN_PERSON);
    CHECK(old.amplitude == 50 && old.period_minutes == 240);

    /* The first OK reading after warm-up sits behind a warm-up entry with no
     * glucose. It must report no trend, not one faked from the walk's 120
     * starting point (the production sentinel path, not the pure function). */
    memset(&fk, 0, sizeof fk);
    fk.now = 1800000000;
    cgm_state_fresh(&st, CGM_G7, NULL, fk.now);
    cgm_init(&c, &st, &fio);
    cgm_set_pattern(&c, &sine);
    cgm_tick(&c); /* the warm-up reading, glucose 0 */
    CHECK(c.st.n_hist == 1 && c.st.hist[0].glucose == 0);
    cgm_skip_warmup(&c);
    fk.now += 1;
    cgm_tick(&c);
    const cgm_reading_t *first_ok = &c.st.hist[c.st.n_hist - 1];
    CHECK(first_ok->state == CGM_STATE_OK && first_ok->glucose > 0);
    CHECK(first_ok->trend == 0);

    /* Both kinds run through the same generator: a sine is not the static
     * glucose the core had before, and the panel's status names it. */
    const cgm_kind_t kinds[2] = {CGM_G6, CGM_G7};
    for (int k = 0; k < 2; k++) {
        memset(&fk, 0, sizeof fk);
        fk.now = 1800000000 + k * 86400;
        cgm_state_fresh(&st, kinds[k], kinds[k] == CGM_G6 ? "81AB7Q" : NULL, fk.now);
        st.warmup_done = fk.now;
        cgm_init(&c, &st, &fio);
        cgm_set_pattern(&c, &sine);
        cgm_tick(&c);
        bool varied = false;
        for (int i = 0; i < 12; i++) {
            fk.now += CGM_READING_INTERVAL;
            cgm_tick(&c);
            const cgm_reading_t *last = &c.st.hist[c.st.n_hist - 1];
            if (last->glucose != 150) varied = true;
        }
        CHECK(varied);
        char js[700];
        size_t n = cgm_status_json(&c, js, sizeof js);
        CHECK(n > 0 && strstr(js, "\"pattern\":\"sine\""));
    }
}

static void test_status(void)
{
    static cgm_t c;
    cgm_state_t st;
    memset(&fk, 0, sizeof fk);
    fk.now = 1800000000;
    cgm_state_fresh(&st, CGM_G7, NULL, fk.now);
    cgm_init(&c, &st, &fio);
    cgm_tick(&c); /* the warm-up reading has no glucose and must report no trend */
    char warm[700];
    size_t wn = cgm_status_json(&c, warm, sizeof warm);
    CHECK(wn > 0);
    const char *warm_last = strstr(warm, "\"last\":{");
    CHECK(warm_last && strstr(warm_last, "\"trend\":null"));
    cgm_set_glucose(&c, 100, -20);
    cgm_skip_warmup(&c);
    fk.now += 1;
    cgm_tick(&c);
    char js[700];
    size_t n = cgm_status_json(&c, js, sizeof js);
    CHECK(n > 0 && js[0] == '{' && js[n - 1] == '}');
    CHECK(strstr(js, "\"kind\":7") && strstr(js, "\"name\":\"DXCM"));
    const char *last = strstr(js, "\"last\":{");
    CHECK(last && strstr(last, "\"trend\":-20"));
    cgm_new_sensor(&c, CGM_G6, "81AB7Q");
    CHECK(c.st.kind == CGM_G6 && strcmp(c.st.id, "81AB7Q") == 0 && c.st.n_hist == 0);
    cgm_new_sensor(&c, CGM_OFF, NULL);
    cgm_tick(&c);
    CHECK(!fk.advertising);
}

/* ---- the virtual person ------------------------------------------------------------------ */

static int64_t pump_total = -1;
static int64_t f_pump(void *ctx) { return pump_total; }
static int32_t f_utc(void *ctx) { return 0; }

static void person_days(cgm_insulin_t source, int days, int *lo, int *hi, double *mean, double *sd, int *tir,
                        int *below, int *changes, bool feed_basal)
{
    static cgm_t c;
    static cgm_io_t io;
    io = fio;
    io.utc_offset = f_utc;
    io.pump_insulin = f_pump;
    memset(&fk, 0, sizeof fk);
    fk.now = 1800000000 - 1800000000 % 86400; /* a midnight */
    pump_total = source == CGM_INSULIN_PUMP ? 0 : -1;
    cgm_state_t st;
    cgm_state_fresh(&st, CGM_G7, NULL, fk.now);
    cgm_init(&c, &st, &io);
    cgm_skip_warmup(&c);
    cgm_set_person(&c, source, 45, 10, 80);
    double sum = 0, sq = 0;
    int n = 0, in = 0, low = 0, prev = -1;
    *lo = 1000;
    *hi = 0;
    *changes = 0;
    for (int s = 0; s < days * 86400; s++) {
        fk.now++;
        if (feed_basal && s % 45 == 0) {
            pump_total += 1; /* 0.80 U/h: one hundredth every 45 s */
        }
        cgm_tick(&c);
        if (c.st.n_hist && c.st.hist[c.st.n_hist - 1].seq != (uint32_t)prev) {
            const cgm_reading_t *r = &c.st.hist[c.st.n_hist - 1];
            prev = (int)r->seq;
            if (!r->glucose) {
                continue;
            }
            int g = r->glucose;
            *lo = g < *lo ? g : *lo;
            *hi = g > *hi ? g : *hi;
            sum += g;
            sq += (double)g * g;
            n++;
            in += g >= 70 && g <= 180;
            low += g < 70;
            *changes += r->trend != 0;
        }
    }
    *mean = sum / n;
    *sd = sqrt(sq / n - *mean * *mean);
    *tir = 100 * in / n;
    *below = 100 * low / n;
}

static void test_person(void)
{
    int lo, hi, tir, below, changes;
    double mean, sd;
    /* The person doses themselves: three days. */
    person_days(CGM_INSULIN_SELF, 3, &lo, &hi, &mean, &sd, &tir, &below, &changes, false);
    printf("  person, self-dosing, 3 days: %d-%d mg/dL, mean %.0f, SD %.0f, %d%% in 70-180, %d%% below 70\n", lo, hi,
           mean, sd, tir, below);
    CHECK(lo >= 40 && hi <= 400);
    CHECK(sd > 15);              /* it moves */
    CHECK(mean > 90 && mean < 220);
    CHECK(tir > 40);             /* a person, not a flat line, and not a disaster */
    CHECK(hi - lo > 80);
    CHECK(changes > 100);        /* the trend arrow moves */

    /* The pump delivers the basal need and nothing else: meals push it up,
     * nothing brings it down but the basal. */
    person_days(CGM_INSULIN_PUMP, 2, &lo, &hi, &mean, &sd, &tir, &below, &changes, true);
    printf("  person, pump basal only, 2 days: %d-%d mg/dL, mean %.0f, SD %.0f\n", lo, hi, mean, sd);
    CHECK(mean > 150); /* no meal insulin: high */

    /* No insulin at all: glucose climbs and stays high, as it would. */
    person_days(CGM_INSULIN_PUMP, 1, &lo, &hi, &mean, &sd, &tir, &below, &changes, false);
    printf("  person, no insulin, 1 day: %d-%d mg/dL, mean %.0f\n", lo, hi, mean);
    CHECK(hi >= 390);

    /* The food names. */
    CHECK(strcmp(cgm_food_name(0), "porridge with banana") == 0);
}

void test_cgm_all(void)
{
    test_formats();
    test_g7();
    test_g6_passive();
    test_g6_active();
    test_g6_pairing();
    test_two_sessions();
    test_tick_delivery();
    test_slot_reuse();
    test_address_rotate();
    test_single_boundary();
    test_hangup();
    test_advertise();
    test_bad_session();
    test_drop_all_sessions();
    test_patterns();
    test_status();
    test_person();
}
