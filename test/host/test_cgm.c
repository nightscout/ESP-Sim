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
        cgm_chr_t chr;
        uint8_t p[32];
        size_t n;
    } sent[MAX_SENT];
    int n_sent;
    bool advertising;
    char adv_name[16];
    int disconnects;
    int saves;
} fake_t;

static fake_t fk;
static const uint8_t AUTH_OK[3] = {0x05, 0x01, 0x01};

static void f_send(void *ctx, cgm_chr_t chr, const uint8_t *p, size_t n)
{
    if (fk.n_sent < MAX_SENT && n <= sizeof fk.sent[0].p) {
        fk.sent[fk.n_sent].chr = chr;
        memcpy(fk.sent[fk.n_sent].p, p, n);
        fk.sent[fk.n_sent].n = n;
        fk.n_sent++;
    }
}
static void f_disconnect(void *ctx) { fk.disconnects++; }
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
static void connect(cgm_t *c)
{
    fk.advertising = false;
    cgm_on_connect(c);
}

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
    cgm_on_subscribe(&c, CGM_CHR_AUTH, true);
    CHECK(fk.n_sent == 1 && fk.sent[0].chr == CGM_CHR_AUTH);
    CHECK_BYTES(fk.sent[0].p, fk.sent[0].n, AUTH_OK, 3);
    cgm_on_subscribe(&c, CGM_CHR_CONTROL, true);
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
    cgm_on_subscribe(&c, CGM_CHR_BACKFILL, true);
    CHECK(fk.n_sent == 2);
    CHECK(fk.sent[0].chr == CGM_CHR_BACKFILL && fk.sent[0].n == 9 && fk.sent[0].p[6] == CGM_STATE_WARMUP);
    CHECK(fk.sent[1].chr == CGM_CHR_CONTROL && fk.sent[1].p[0] == 0x59);
    /* Extended version. */
    clear_sent();
    cgm_on_write(&c, CGM_CHR_CONTROL, (const uint8_t *)"\x52", 1);
    CHECK(fk.n_sent == 1 && fk.sent[0].n == 15 && fk.sent[0].p[0] == 0x52);
    CHECK(le32(fk.sent[0].p + 2) == 907200 && le16(fk.sent[0].p + 6) == 27 * 60);
    /* Quiet for 3 seconds: the sensor hangs up. */
    run(&c, 3);
    CHECK(fk.disconnects == 1);
    cgm_on_disconnect(&c);
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
    cgm_on_subscribe(&c, CGM_CHR_AUTH, true);
    cgm_on_subscribe(&c, CGM_CHR_CONTROL, true);
    cgm_on_subscribe(&c, CGM_CHR_BACKFILL, true);
    int recs = 0;
    for (int i = 0; i < fk.n_sent; i++) {
        recs += fk.sent[i].chr == CGM_CHR_BACKFILL;
    }
    CHECK(recs == 3);
    CHECK(find(CGM_CHR_CONTROL, 0x59, 0) > find(CGM_CHR_CONTROL, 0x4e, 0));
    cgm_on_disconnect(&c);

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
    cgm_on_subscribe(&c, CGM_CHR_AUTH, true);
    cgm_on_subscribe(&c, CGM_CHR_CONTROL, true);
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
    cgm_on_subscribe(&c, CGM_CHR_BACKFILL, true);
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
    cgm_on_write(&c, CGM_CHR_CONTROL, req, 3);
    CHECK(fk.n_sent == 1 && fk.sent[0].n == 19 && fk.sent[0].p[0] == 0x4b && crc_ok(fk.sent[0].p, 19));
    CHECK(((fk.sent[0].p[14] << 8) | fk.sent[0].p[13]) != 180);
    cgm_on_disconnect(&c);
}

/* ---- a G6 session in active mode: CGMBLEKit logs in itself ----------------------- */

static void send_cmd(cgm_t *c, const uint8_t *p, size_t n)
{
    uint8_t m[32];
    memcpy(m, p, n);
    uint16_t crc = cgm_crc16(m, n);
    m[n] = (uint8_t)crc;
    m[n + 1] = (uint8_t)(crc >> 8);
    cgm_on_write(c, CGM_CHR_CONTROL, m, n + 2);
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
    cgm_on_write(&c, CGM_CHR_AUTH, tok, sizeof tok);
    size_t n = cgm_on_read(&c, CGM_CHR_AUTH, v, sizeof v);
    CHECK(n == 17 && v[0] == 0x03);
    cgm_g6_hash("8G1234", tok + 1, h);
    CHECK(memcmp(v + 1, h, 8) == 0);
    /* AuthChallengeTx 04 hash(challenge); read: 05 01 01. */
    uint8_t ch[9] = {0x04};
    cgm_g6_hash("8G1234", v + 9, ch + 1);
    cgm_on_write(&c, CGM_CHR_AUTH, ch, sizeof ch);
    n = cgm_on_read(&c, CGM_CHR_AUTH, v, sizeof v);
    CHECK_BYTES(v, n, AUTH_OK, 3);

    /* Subscribing to control sends nothing unasked in active mode. */
    clear_sent();
    cgm_on_subscribe(&c, CGM_CHR_CONTROL, true);
    CHECK(fk.n_sent == 0);

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
    CHECK(c.sent_glucose);

    /* Calibrate to 110: the calibration data says so. */
    clear_sent();
    uint8_t cal[7] = {0x34, 110, 0, 20, 0, 0, 0};
    send_cmd(&c, cal, 7);
    CHECK(fk.n_sent == 1 && fk.sent[0].p[0] == 0x35 && fk.sent[0].n == 5 && crc_ok(fk.sent[0].p, 5));
    send_cmd(&c, (const uint8_t *)"\x32", 1);
    CHECK(fk.n_sent == 2 && le16(fk.sent[1].p + 11) == 110 && le32(fk.sent[1].p + 13) == 20);

    /* A wrong challenge answer is refused (on the receiver slot: the phone's has this reading). */
    tok[9] = 0x01;
    cgm_on_write(&c, CGM_CHR_AUTH, tok, sizeof tok);
    cgm_on_read(&c, CGM_CHR_AUTH, v, sizeof v);
    uint8_t bad[9] = {0x04};
    cgm_on_write(&c, CGM_CHR_AUTH, bad, sizeof bad);
    n = cgm_on_read(&c, CGM_CHR_AUTH, v, sizeof v);
    CHECK(n == 3 && v[0] == 0x05 && v[1] != 0x01);

    /* Stop the sensor: no session. */
    clear_sent();
    uint8_t stop[5] = {0x28, 50};
    send_cmd(&c, stop, 5);
    CHECK(fk.n_sent == 1 && fk.sent[0].p[0] == 0x29 && le32(fk.sent[0].p + 7) == 0xffffffff);
    CHECK(cgm_current_state(&c.st, fk.now) == CGM_STATE_STOPPED);

    /* Disconnect message. */
    int d = fk.disconnects;
    cgm_on_write(&c, CGM_CHR_CONTROL, (const uint8_t *)"\x09", 1);
    CHECK(fk.disconnects == d + 1);
}

/* ---- G6 slots: the phone app and a watch in the same window ---------------------- */

/* Logs in on `slot` (active mode). False if the transmitter hung up instead. */
static bool login(cgm_t *c, const char *id, uint8_t slot)
{
    int d = fk.disconnects;
    uint8_t tok[10] = {0x01, 9, 8, 7, 6, 5, 4, 3, 2, slot}, v[32];
    cgm_on_write(c, CGM_CHR_AUTH, tok, sizeof tok);
    if (fk.disconnects != d) {
        return false;
    }
    size_t n = cgm_on_read(c, CGM_CHR_AUTH, v, sizeof v);
    uint8_t ch[9] = {0x04};
    cgm_g6_hash(id, v + 9, ch + 1);
    cgm_on_write(c, CGM_CHR_AUTH, ch, sizeof ch);
    n = cgm_on_read(c, CGM_CHR_AUTH, v, sizeof v);
    return n == 3 && v[1] == 0x01;
}

static bool read_glucose(cgm_t *c)
{
    clear_sent();
    send_cmd(c, (const uint8_t *)"\x30", 1);
    return find(CGM_CHR_CONTROL, 0x31, 0) >= 0;
}

static void test_g6_slots(void)
{
    static cgm_t c;
    cgm_state_t st;
    memset(&fk, 0, sizeof fk);
    fk.now = 1800000000;
    cgm_state_fresh(&st, CGM_G6, "8G1234", fk.now);
    cgm_init(&c, &st, &fio);
    cgm_skip_warmup(&c);
    run(&c, CGM_READING_INTERVAL); /* a fresh window */
    CHECK(fk.advertising);

    /* The phone app (slot 2) first. When it lets go, the transmitter is there for the watch. */
    connect(&c);
    CHECK(login(&c, "8G1234", 0x02) && read_glucose(&c));
    cgm_on_disconnect(&c);
    run(&c, 1);
    CHECK(fk.advertising);
    /* The phone again: its slot has the reading, so it is let go at once. */
    connect(&c);
    CHECK(!login(&c, "8G1234", 0x02));
    cgm_on_disconnect(&c);
    run(&c, 1);
    CHECK(fk.advertising);
    /* The watch (slot 1) gets the same reading. Then nothing is left to give. */
    connect(&c);
    CHECK(login(&c, "8G1234", 0x01) && read_glucose(&c));
    CHECK(c.st.delivered_rx == c.st.delivered);
    cgm_on_disconnect(&c);
    run(&c, 1);
    CHECK(!fk.advertising);

    /* The next window: the watch first, then the phone, whose slot is due all window. */
    for (int i = 0; i < CGM_READING_INTERVAL && !fk.advertising; i++) {
        run(&c, 1);
    }
    connect(&c);
    CHECK(login(&c, "8G1234", 0x01) && read_glucose(&c));
    cgm_on_disconnect(&c);
    run(&c, 20);
    CHECK(fk.advertising);
    connect(&c);
    CHECK(login(&c, "8G1234", 0x02) && read_glucose(&c));
    cgm_on_disconnect(&c);
    run(&c, 1);
    CHECK(!fk.advertising);

    /* The watch as rockit's does it: it subscribes to the control messages
     * first (the reading is pushed, as to a passive client) and then logs in
     * on the receiver slot. The phone app's slot keeps its turn. */
    for (int i = 0; i < CGM_READING_INTERVAL && !fk.advertising; i++) {
        run(&c, 1);
    }
    connect(&c);
    clear_sent();
    cgm_on_subscribe(&c, CGM_CHR_CONTROL, true);
    CHECK(find(CGM_CHR_CONTROL, 0x4f, 0) >= 0);
    CHECK(login(&c, "8G1234", 0x01) && read_glucose(&c));
    CHECK(c.st.delivered_rx != c.st.delivered);
    cgm_on_disconnect(&c);
    run(&c, 1);
    CHECK(fk.advertising);
    connect(&c);
    CHECK(login(&c, "8G1234", 0x02));
    CHECK(read_glucose(&c));
    cgm_on_disconnect(&c);
    run(&c, 1);
    CHECK(!fk.advertising);
    /* A passive client that never logs in: the phone app's session, so its slot. */
    for (int i = 0; i < CGM_READING_INTERVAL && !fk.advertising; i++) {
        run(&c, 1);
    }
    uint32_t app = c.st.delivered;
    connect(&c);
    cgm_on_subscribe(&c, CGM_CHR_CONTROL, true);
    cgm_on_disconnect(&c);
    CHECK(c.st.delivered != app);
    run(&c, 70); /* past this window */

    /* No watch at all: after the phone, the receiver slot's chance is short. */
    for (int i = 0; i < CGM_READING_INTERVAL && !fk.advertising; i++) {
        run(&c, 1);
    }
    connect(&c);
    CHECK(login(&c, "8G1234", 0x02) && read_glucose(&c));
    cgm_on_disconnect(&c);
    run(&c, 1);
    CHECK(fk.advertising);
    run(&c, 10);
    CHECK(!fk.advertising);
}

static void test_status(void)
{
    static cgm_t c;
    cgm_state_t st;
    memset(&fk, 0, sizeof fk);
    fk.now = 1800000000;
    cgm_state_fresh(&st, CGM_G7, NULL, fk.now);
    cgm_init(&c, &st, &fio);
    cgm_tick(&c);
    char js[600];
    size_t n = cgm_status_json(&c, js, sizeof js);
    CHECK(n > 0 && js[0] == '{' && js[n - 1] == '}');
    CHECK(strstr(js, "\"kind\":7") && strstr(js, "\"name\":\"DXCM"));
    cgm_new_sensor(&c, CGM_G6, "81AB7Q");
    CHECK(c.st.kind == CGM_G6 && strcmp(c.st.id, "81AB7Q") == 0 && c.st.n_hist == 0);
    cgm_new_sensor(&c, CGM_OFF, NULL);
    cgm_tick(&c);
    CHECK(!fk.advertising);
}

static void test_person(void);
static void test_backtest(void);

void test_cgm_all(void)
{
    test_formats();
    test_g7();
    test_g6_passive();
    test_g6_active();
    test_g6_slots();
    test_status();
    test_person();
    test_backtest();
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
    cgm_set_pattern(&c, CGM_PATTERN_PERSON, source, 45, 10, 80);
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

    /* The panel's Set moves the person's own glucose: the readings carry on
     * from it, not from where the person was (stuck high above 400). */
    {
        static cgm_t c;
        static cgm_io_t io;
        io = fio;
        io.utc_offset = f_utc;
        io.pump_insulin = f_pump;
        memset(&fk, 0, sizeof fk);
        fk.now = 1800000000;
        pump_total = 0;
        cgm_state_t st;
        cgm_state_fresh(&st, CGM_G6, NULL, fk.now);
        cgm_init(&c, &st, &io);
        cgm_skip_warmup(&c);
        cgm_set_pattern(&c, CGM_PATTERN_PERSON, CGM_INSULIN_PUMP, 45, 10, 80);
        c.st.person.g_milli = 598000;
        cgm_set_glucose(&c, 120, 0);
        CHECK(c.st.person.g_milli == 120000);
        uint32_t seen = c.st.n_hist ? c.st.hist[c.st.n_hist - 1].seq : 0;
        int got = 0;
        for (int s = 0; s < 20 * 60 && !got; s++) {
            fk.now++;
            cgm_tick(&c);
            if (c.st.n_hist && c.st.hist[c.st.n_hist - 1].seq != seen && c.st.hist[c.st.n_hist - 1].glucose) {
                got = c.st.hist[c.st.n_hist - 1].glucose;
            }
        }
        printf("  person set to 120 from 598: next reading %d mg/dL\n", got);
        CHECK(got > 80 && got < 180);
    }

    /* The food names. */
    CHECK(strcmp(cgm_food_name(0), "porridge with banana") == 0);
}

/* ---- backtest: a stored trace, replayed ---------------------------------------------------- */

/* A synthetic trace: made up here, no one's real readings. */
static uint16_t bt_trace[64];
static uint32_t bt_len;
static uint16_t f_trace_at(void *ctx, uint32_t i) { return i < bt_len ? bt_trace[i] : 0xffff; }

static cgm_pump_t bt_pump;
static bool f_pump_now(void *ctx, cgm_pump_t *out)
{
    *out = bt_pump;
    return true;
}

static cgm_log_t bt_log[256];
static int bt_nlog;
static void f_bt_log(void *ctx, const cgm_log_t *e)
{
    if (bt_nlog < 256) {
        bt_log[bt_nlog++] = *e;
    }
}

static int bt_count(uint8_t kind)
{
    int n = 0;
    for (int i = 0; i < bt_nlog; i++) {
        n += bt_log[i].kind == kind;
    }
    return n;
}

/* A G7, warmed up, on the backtest pattern with the trace loaded. */
static void bt_setup(cgm_t *c, cgm_io_t *io)
{
    *io = fio;
    io->trace_at = f_trace_at;
    io->pump_now = f_pump_now;
    io->backtest_log = f_bt_log;
    memset(&fk, 0, sizeof fk);
    fk.now = 1800000000;
    bt_nlog = 0;
    memset(&bt_pump, 0, sizeof bt_pump);
    bt_pump.rate = 80;
    cgm_state_t st;
    cgm_state_fresh(&st, CGM_G7, NULL, fk.now);
    cgm_init(c, &st, io);
    cgm_skip_warmup(c);
    cgm_set_pattern(c, CGM_PATTERN_BACKTEST, CGM_INSULIN_PUMP, 0, 0, 0);
    uint16_t first = 0;
    for (uint32_t i = 0; i < bt_len && !first; i++) {
        first = bt_trace[i];
    }
    cgm_backtest_load(c, bt_len, cgm_trace_sum(CGM_TRACE_SUM_INIT, bt_trace, bt_len), first);
}

/* Runs to the next reading slot and returns the reading made there (0: none). */
static int bt_next(cgm_t *c, int8_t *trend)
{
    uint32_t before = c->st.n_hist ? c->st.hist[c->st.n_hist - 1].seq : 0;
    run(c, CGM_READING_INTERVAL);
    const cgm_reading_t *r = c->st.n_hist ? &c->st.hist[c->st.n_hist - 1] : NULL;
    if (!r || r->seq == before) {
        return 0;
    }
    if (trend) {
        *trend = r->trend;
    }
    return r->glucose;
}

/* A fake NOR flash: writes only clear bits, erase sets a sector to 0xff. */
static uint8_t nor[4 * CGM_FLASH_SECTOR];
static int nor_erases;
static bool nor_read(void *ctx, uint32_t off, void *p, size_t n)
{
    if (off + n > sizeof nor) {
        return false;
    }
    memcpy(p, nor + off, n);
    return true;
}
static bool nor_write(void *ctx, uint32_t off, const void *p, size_t n)
{
    if (off + n > sizeof nor) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        nor[off + i] &= ((const uint8_t *)p)[i];
    }
    return true;
}
static bool nor_erase(void *ctx, uint32_t off)
{
    if (off % CGM_FLASH_SECTOR || off >= sizeof nor) {
        return false;
    }
    memset(nor + off, 0xff, CGM_FLASH_SECTOR);
    nor_erases++;
    return true;
}
static const cgm_flash_t nor_flash = {.read = nor_read, .write = nor_write, .erase = nor_erase};

static void test_backtest_parse(void)
{
    uint16_t v[16];
    const char *err = NULL;
    const char *t = "120,121, 0,,-,null\n500\r\n30 \n";
    CHECK(cgm_trace_parse(t, strlen(t), v, 16, &err) == 8);
    static const uint16_t want[8] = {120, 121, 0, 0, 0, 0, 400, 40};
    CHECK(memcmp(v, want, sizeof want) == 0);
    t = "100 101\t102";
    CHECK(cgm_trace_parse(t, strlen(t), v, 16, &err) == 3 && v[2] == 102);
    t = "100,abc";
    err = NULL;
    CHECK(cgm_trace_parse(t, strlen(t), v, 16, &err) == -1 && err);
    t = "100,-5";
    CHECK(cgm_trace_parse(t, strlen(t), v, 16, &err) == -1);
    t = "100,1001";
    CHECK(cgm_trace_parse(t, strlen(t), v, 16, &err) == -1);
    t = "1,2,3";
    CHECK(cgm_trace_parse(t, strlen(t), v, 2, &err) == -1);
    CHECK(cgm_trace_parse("", 0, v, 16, &err) == 0);
    /* The checksum: order matters, and it chains across chunks. */
    static const uint16_t a[3] = {100, 110, 120}, b[3] = {110, 100, 120};
    uint32_t whole = cgm_trace_sum(CGM_TRACE_SUM_INIT, a, 3);
    CHECK(whole != cgm_trace_sum(CGM_TRACE_SUM_INIT, b, 3));
    CHECK(whole == cgm_trace_sum(cgm_trace_sum(CGM_TRACE_SUM_INIT, a, 1), a + 1, 2));
}

static void test_backtest_play(void)
{
    static cgm_t c;
    static cgm_io_t io;
    /* 100, 110, 120, 130, a gap, 150, 160, 170 */
    static const uint16_t tr[8] = {100, 110, 120, 130, 0, 150, 160, 170};
    memcpy(bt_trace, tr, sizeof tr);
    bt_len = 8;
    bt_setup(&c, &io);
    CHECK(c.st.bt.play == CGM_BT_CUED && c.st.bt.hold == 100);
    /* Cued: the first value, flat, and nothing logged. */
    CHECK(bt_next(&c, NULL) == 100);
    CHECK(bt_nlog == 0);
    cgm_backtest_start(&c);
    CHECK(c.st.bt.run == 1 && bt_count(CGM_LOG_START) == 1);
    int8_t trend = 0;
    int got[8];
    for (int i = 0; i < 4; i++) {
        got[i] = bt_next(&c, &trend);
    }
    CHECK(got[0] == 100 && got[1] == 110 && got[2] == 120 && got[3] == 130);
    CHECK(trend == (130 - 100) * 10 / 15); /* 15 minutes back, in the trace itself */
    /* The gap: no reading at all, then on. */
    CHECK(bt_next(&c, NULL) == 0);
    CHECK(c.st.bt.cur == 4);
    CHECK(bt_next(&c, NULL) == 150);
    char json[1400];
    CHECK(cgm_status_json(&c, json, sizeof json) > 0 && strstr(json, "\"backtest\":{\"len\":8,") &&
          strstr(json, "\"pos\":6,"));
    /* Pause: the last value held, the trace stays put. */
    cgm_backtest_pause(&c);
    CHECK(bt_next(&c, NULL) == 150 && bt_next(&c, NULL) == 150);
    CHECK(c.st.bt.pos == 6);
    cgm_backtest_start(&c);
    CHECK(bt_next(&c, NULL) == 160 && bt_next(&c, NULL) == 170);
    /* The end, stopping: no more readings. */
    CHECK(c.st.bt.play == CGM_BT_ENDED && bt_count(CGM_LOG_END) == 1);
    CHECK(bt_next(&c, NULL) == 0 && bt_next(&c, NULL) == 0);
    CHECK(bt_count(CGM_LOG_READING) == 8);
    /* Holding instead. */
    cgm_backtest_set_end(&c, CGM_BT_END_HOLD);
    CHECK(bt_next(&c, NULL) == 170);
    /* Start once ended: a new run from the beginning. */
    cgm_backtest_start(&c);
    CHECK(c.st.bt.run == 2 && c.st.bt.pos == 0);
    CHECK(bt_next(&c, NULL) == 100);
    /* Looping: round again as the same run, the trend across the join. */
    cgm_backtest_set_end(&c, CGM_BT_END_LOOP);
    for (int i = 0; i < 7; i++) {
        bt_next(&c, NULL);
    }
    CHECK(c.st.bt.loops == 1 && c.st.bt.play == CGM_BT_PLAYING && bt_count(CGM_LOG_LOOP) == 1);
    CHECK(bt_next(&c, &trend) == 100 && trend == (100 - 150) * 10 / 15);
    /* Cue: move to a position, paused, holding the reading before it. */
    CHECK(cgm_backtest_cue(&c, 3) && c.st.bt.play == CGM_BT_PAUSED && c.st.bt.hold == 120);
    CHECK(!cgm_backtest_cue(&c, 8));
    cgm_backtest_start(&c);
    CHECK(c.st.bt.run == 2 && bt_next(&c, NULL) == 130);
    /* Restart: a new run. */
    cgm_backtest_restart(&c);
    CHECK(c.st.bt.run == 3 && bt_next(&c, NULL) == 100);
    /* A new sensor keeps the trace and its place. */
    uint32_t pos = c.st.bt.pos;
    cgm_new_sensor(&c, CGM_G6, "81AB7Q");
    CHECK(c.st.bt.len == 8 && c.st.bt.pos == pos && c.st.pattern == CGM_PATTERN_BACKTEST);
    /* The trace waits out the warm-up: readings then have no glucose. */
    CHECK(bt_next(&c, NULL) == 0);
    CHECK(c.st.bt.pos == pos);
    cgm_skip_warmup(&c); /* a reading is due at once: the trace's next */
    run(&c, 1);
    CHECK(c.st.hist[c.st.n_hist - 1].glucose == bt_trace[pos] && c.st.bt.cur == pos);
    /* No trace (being replaced): the panel's value, flat. */
    cgm_backtest_load(&c, 0, 0, 0);
    cgm_set_glucose(&c, 140, 0);
    CHECK(bt_next(&c, NULL) == 140);
}

/* The point of it: the pump changes nothing, so two runs see the same glucose. */
static void test_backtest_same_runs(void)
{
    static cgm_t c;
    static cgm_io_t io;
    for (uint32_t i = 0; i < 40; i++) {
        bt_trace[i] = (uint16_t)(120 + 60 * sin(i / 6.0) + (i % 7 == 3 ? 0 : 0));
    }
    bt_trace[17] = 0;
    bt_len = 40;
    int seen[2][40];
    for (int r = 0; r < 2; r++) {
        bt_setup(&c, &io);
        cgm_backtest_start(&c);
        for (int i = 0; i < 40; i++) {
            /* A different pump each run: run 2 gives far more insulin. */
            bt_pump.delivered += r ? 50 : 2;
            seen[r][i] = bt_next(&c, NULL);
        }
    }
    CHECK(memcmp(seen[0], seen[1], sizeof seen[0]) == 0);
    CHECK(seen[0][17] == 0 && seen[0][0] == bt_trace[0] && seen[0][39] == bt_trace[39]);
}

static void test_backtest_log(void)
{
    static cgm_t c;
    static cgm_io_t io;
    static const uint16_t tr[6] = {180, 190, 200, 210, 200, 190};
    memcpy(bt_trace, tr, sizeof tr);
    bt_len = 6;
    bt_setup(&c, &io);
    bt_pump.delivered = 1000;
    cgm_backtest_start(&c);
    run(&c, 2);
    /* Opening the run: the basal as it is. */
    CHECK(bt_nlog == 2 && bt_log[1].kind == CGM_LOG_BASAL && bt_log[1].a == 80 && bt_log[1].b == 0);
    bt_next(&c, NULL); /* reading 0 */
    /* The AID sets a temp basal of 2.50 U/h for 30 minutes, then a bolus of 1.20 U. */
    bt_pump.rate = 250;
    bt_pump.temp = true;
    bt_pump.temp_end = fk.now + 1800;
    run(&c, 5);
    bt_pump.bolus_id = 7;
    bt_pump.bolus = 120;
    bt_pump.delivered += 120 + 10;
    bt_next(&c, NULL); /* reading 1 */
    const cgm_log_t *t = NULL, *b = NULL, *r1 = NULL;
    for (int i = 0; i < bt_nlog; i++) {
        if (bt_log[i].kind == CGM_LOG_BASAL && bt_log[i].a == 250) {
            t = &bt_log[i];
        }
        if (bt_log[i].kind == CGM_LOG_BOLUS) {
            b = &bt_log[i];
        }
        if (bt_log[i].kind == CGM_LOG_READING && bt_log[i].pos == 1) {
            r1 = &bt_log[i];
        }
    }
    CHECK(t && t->pos == 0 && t->b == 30 && t->run == 1);
    CHECK(b && b->a == 120 && b->pos == 0);
    CHECK(r1 && r1->a == 190 && r1->b == 130); /* the insulin since reading 0 */
    /* Nothing is logged while paused. */
    cgm_backtest_pause(&c);
    int n = bt_nlog;
    bt_pump.rate = 0;
    bt_next(&c, NULL);
    CHECK(bt_nlog == n);

    /* As CSV. */
    char line[160];
    CHECK(cgm_log_csv_header(line, sizeof line) > 0 && strncmp(line, "run,position,trace_minute,", 26) == 0);
    cgm_log_t e = {.kind = CGM_LOG_READING, .run = 2, .t = 1800000000, .pos = 12, .a = 154, .b = 35};
    CHECK(cgm_log_csv(&e, line, sizeof line) > 0);
    CHECK(strcmp(line, "2,12,60,2027-01-15T08:00:00Z,reading,154,0.35,,,\n") == 0);
    e = (cgm_log_t){.kind = CGM_LOG_BASAL, .run = 2, .t = 1800000000, .pos = 12, .a = 250, .b = 30};
    cgm_log_csv(&e, line, sizeof line);
    CHECK(strcmp(line, "2,12,,2027-01-15T08:00:00Z,temp_basal,,,2.50,30,\n") == 0);
    e = (cgm_log_t){.kind = CGM_LOG_BASAL, .run = 2, .t = 1800000000, .pos = 12, .a = -1, .b = 0};
    cgm_log_csv(&e, line, sizeof line);
    CHECK(strcmp(line, "2,12,,2027-01-15T08:00:00Z,basal,,,,0,\n") == 0);
    e = (cgm_log_t){.kind = CGM_LOG_BOLUS, .run = 2, .t = 1800000000, .pos = 13, .a = 120, .b = -1};
    cgm_log_csv(&e, line, sizeof line);
    CHECK(strcmp(line, "2,13,,2027-01-15T08:00:00Z,bolus,,,,,1.20\n") == 0);
    e = (cgm_log_t){.kind = CGM_LOG_READING, .run = 2, .t = 1800000000, .pos = 14, .a = 0, .b = -1};
    cgm_log_csv(&e, line, sizeof line);
    CHECK(strcmp(line, "2,14,70,2027-01-15T08:00:00Z,missing,,,,,\n") == 0);
}

static void test_backtest_logstore(void)
{
    memset(nor, 0, sizeof nor); /* never erased: garbage */
    cgm_logstore_t ls;
    cgm_logstore_open(&ls, &nor_flash, CGM_FLASH_SECTOR, 2 * CGM_FLASH_SECTOR, 1);
    cgm_logstore_clear(&ls, 1);
    CHECK(ls.count == 0 && !ls.full);
    int per = CGM_FLASH_SECTOR / (int)sizeof(cgm_log_t);
    nor_erases = 0;
    cgm_log_t e = {.kind = CGM_LOG_READING};
    for (int i = 0; i < per + 3; i++) {
        e.pos = (uint32_t)i;
        if (!cgm_logstore_append(&ls, &e)) { CHECK(false); }
    }
    CHECK(ls.count == (uint32_t)per + 3 && nor_erases == 1); /* the second sector, as reached */
    /* Opened again (a restart): the same lines. */
    cgm_logstore_t again;
    cgm_logstore_open(&again, &nor_flash, CGM_FLASH_SECTOR, 2 * CGM_FLASH_SECTOR, 1);
    CHECK(again.count == ls.count);
    cgm_log_t got;
    CHECK(cgm_logstore_get(&again, (uint32_t)per + 2, &got) && got.pos == (uint32_t)per + 2 && got.epoch == 1);
    CHECK(!cgm_logstore_get(&again, again.count, &got));
    /* Cleared, then a full first sector: the old lines in the second sector,
     * not yet erased, are another epoch's and do not count. */
    cgm_logstore_clear(&again, 2);
    for (int i = 0; i < per; i++) {
        e.pos = 1000u + (uint32_t)i;
        cgm_logstore_append(&again, &e);
    }
    cgm_logstore_open(&ls, &nor_flash, CGM_FLASH_SECTOR, 2 * CGM_FLASH_SECTOR, 2);
    CHECK(ls.count == (uint32_t)per);
    /* Full: appends refused, nothing past the end written. */
    for (int i = 0; i < per; i++) {
        cgm_logstore_append(&ls, &e);
    }
    CHECK(ls.full && !cgm_logstore_append(&ls, &e) && ls.count == 2u * (uint32_t)per);
    CHECK(nor[0] == 0 && nor[3 * CGM_FLASH_SECTOR] == 0); /* outside the region: untouched */
}

static void test_backtest(void)
{
    test_backtest_parse();
    test_backtest_play();
    test_backtest_same_runs();
    test_backtest_log();
    test_backtest_logstore();
}
