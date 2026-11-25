/*
 * A simulated YpsoPump. Byte layouts, error codes and behaviour follow the
 * rockit driver's docs/COMMAND-SPEC.md (cited §n) and its codec; where the
 * spec leaves the pump's side open, the choice made here is commented.
 *
 * TEST EQUIPMENT ONLY: it delivers nothing.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ypso.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dash.h" /* crypto: ChaCha20-Poly1305, random, SHA-256 */
#include "dash_internal.h" /* X25519 */

/* Temp basal limits (settings 116-119): 500 % for up to 24 h, any percentage,
 * as the owner's pump accepts. */
#define TBR_LONG_MAX 500
#define TBR_LONG_MINUTES 1440
#define TBR_SHORT_MAX 500
#define TBR_SHORT_MINUTES 1440

/* ---- firmware --------------------------------------------------------------------- */

static const ypso_fw_info_t FW[YPSO_FW_COUNT] = {
    [YPSO_FW_V05] = {"V05.02.03 (encrypted, the validated pump)", "1.2", "V05.02.03", "V01.02.03", "1.7", "1.4", "1.4",
                     "1.0", true, true, false, true, true, 20, 1},
    [YPSO_FW_V35] = {"V35.01.00 (encrypted, study firmware)", "1.2", "V35.01.00", "V01.02.03", "1.7", "1.4", "1.4",
                     "1.0", true, true, true, true, true, 20, 1},
    [YPSO_FW_V65] = {"V65.00.06 (OPN, clear text, Control 1.0)", "1.1", "V65.00.06", "V01.00.00", "1.3", "1.1", "1.0",
                     "", true, false, false, false, false, 18, 10},
    [YPSO_FW_V03] = {"V03.00.00 (OPN, clear text, Control 1.1)", "1.1", "V03.00.00", "V01.00.00", "1.5", "1.1", "1.1",
                     "", true, false, false, false, false, 18, 10},
    [YPSO_FW_V02] = {"V02.00.00 (baseline, no Control service)", "1.0", "V02.00.00", "V01.00.00", "1.5", "1.1", "",
                     "", false, false, false, false, false, 18, 10},
};

const ypso_fw_info_t *ypso_fw(ypso_fw_t fw)
{
    return &FW[fw < YPSO_FW_COUNT ? fw : YPSO_FW_V05];
}

/* ---- GATT (§1) ----------------------------------------------------------------------- */

static const char *SVC[YS_COUNT] = {
    [YS_BASE] = "fb349b5f-8000-0080-0010-0000adde0000",
    [YS_SETTINGS] = "fb349b5f-8000-0080-0010-0000efbe0000",
    [YS_HISTORY] = "fb349b5f-8000-0080-0010-0000dec00000",
    [YS_CONTROL] = "fb349b5f-8000-0080-0010-0000feda0000",
    [YS_CRYPTO] = "fb349b5f-8000-0080-0010-0000feda0002",
};

#define U(x) "669a0c20-0008-969e-e211-fc" x
static const ypso_chr_info_t CHR[YC_COUNT] = {
    [YC_BASE_VERSION] = {YS_BASE, U("bee23b7bc5"), true, false, true},
    [YC_MASTER_VERSION] = {YS_BASE, U("beb0147bc5"), true, false, true},
    [YC_SUPERVISOR_VERSION] = {YS_BASE, U("beb1147bc5"), true, false, true},
    [YC_AUTH_PASSWORD] = {YS_BASE, U("beb2147bc5"), false, true, true},
    [YC_PAIRING_PASSKEY] = {YS_BASE, U("bed63b7bc5"), true, false, true},
    [YC_SETTINGS_VERSION] = {YS_SETTINGS, U("bee33b7bc5"), true, false, true},
    [YC_SETTING_ID] = {YS_SETTINGS, U("beb3147bc5"), false, true, false},
    [YC_SETTING_VALUE] = {YS_SETTINGS, U("beb4147bc5"), true, true, false},
    [YC_SETTING_ID_ALT] = {YS_SETTINGS, U("16889221e6"), false, true, false},
    [YC_SETTING_VALUE_ALT] = {YS_SETTINGS, U("bebd3b7bc5"), true, true, false},
    [YC_SYSTEM_DATE] = {YS_SETTINGS, U("bedc3b7bc5"), true, true, false},
    [YC_SYSTEM_TIME] = {YS_SETTINGS, U("bedd3b7bc5"), true, true, false},
    [YC_HISTORY_VERSION] = {YS_HISTORY, U("bee43b7bc5"), true, false, true},
    [YC_ALARM_COUNT] = {YS_HISTORY, U("bec83b7bc5"), true, false, false},
    [YC_ALARM_INDEX] = {YS_HISTORY, U("bec93b7bc5"), false, true, false},
    [YC_ALARM_VALUE] = {YS_HISTORY, U("beca3b7bc5"), true, false, false},
    [YC_EVENT_COUNT] = {YS_HISTORY, U("becb3b7bc5"), true, false, false},
    [YC_EVENT_INDEX] = {YS_HISTORY, U("becc3b7bc5"), false, true, false},
    [YC_EVENT_VALUE] = {YS_HISTORY, U("becd3b7bc5"), true, false, false},
    [YC_SYSTEM_COUNT] = {YS_HISTORY, "86a5a431-d442-2c8d-304b-19ee355571fc", true, false, false},
    [YC_SYSTEM_INDEX] = {YS_HISTORY, "381ddce9-e934-b4ae-e345-eb87283db426", false, true, false},
    [YC_SYSTEM_VALUE] = {YS_HISTORY, "ae3022af-2ec8-bf88-e64c-da68c9a3891a", true, false, false},
    [YC_COMPLAINT_COUNT] = {YS_HISTORY, "449753d3-3cc5-7594-ae4f-8d4efa2af4fd", true, false, false},
    [YC_COMPLAINT_INDEX] = {YS_HISTORY, "db75669b-e6cc-f6a4-1f4b-0becaeb81f34", false, true, false},
    [YC_COMPLAINT_VALUE] = {YS_HISTORY, "813a4e12-e1e2-47a0-2544-22664b3c9e62", true, false, false},
    [YC_ALARM_COUNT_ALT] = {YS_HISTORY, U("1688a021e6"), true, false, false},
    [YC_ALARM_INDEX_ALT] = {YS_HISTORY, U("1688a121e6"), false, true, false},
    [YC_ALARM_VALUE_ALT] = {YS_HISTORY, U("1688a221e6"), true, false, false},
    [YC_EVENT_COUNT_ALT] = {YS_HISTORY, U("16889a21e6"), true, false, false},
    [YC_EVENT_INDEX_ALT] = {YS_HISTORY, U("16889b21e6"), false, true, false},
    [YC_EVENT_VALUE_ALT] = {YS_HISTORY, U("16889c21e6"), true, false, false},
    [YC_SYSTEM_COUNT_ALT] = {YS_HISTORY, U("16889d21e6"), true, false, false},
    [YC_SYSTEM_INDEX_ALT] = {YS_HISTORY, U("16889e21e6"), false, true, false},
    [YC_SYSTEM_VALUE_ALT] = {YS_HISTORY, U("16889f21e6"), true, false, false},
    [YC_COUNTER_ID] = {YS_HISTORY, U("bece3b7bc5"), false, true, false},
    [YC_COUNTER_VALUE] = {YS_HISTORY, U("becf3b7bc5"), true, false, false},
    [YC_CONTROL_VERSION] = {YS_CONTROL, U("bee08b7bc5"), true, false, true},
    [YC_START_STOP_BOLUS] = {YS_CONTROL, U("bee18b7bc5"), false, true, false},
    [YC_GET_BOLUS_STATUS] = {YS_CONTROL, U("bee28b7bc5"), true, false, false},
    [YC_START_STOP_TBR] = {YS_CONTROL, U("bee38b7bc5"), false, true, false},
    [YC_GET_SYSTEM_STATUS] = {YS_CONTROL, U("bee48b7bc5"), true, false, false},
    [YC_CRYPTO_VERSION] = {YS_CRYPTO, U("ff0000000c"), true, false, true},
    [YC_NEW_PUBLIC_KEY] = {YS_CRYPTO, U("ff0000000a"), true, false, true},
    [YC_WRITE_CHALLENGE] = {YS_CRYPTO, U("ff0000000b"), false, true, true},
    [YC_EXTENDED_READ] = {YS_CRYPTO, U("ff000000ff"), true, false, true},
};
#undef U

const ypso_chr_info_t *ypso_chr(ypso_chr_t c)
{
    return &CHR[c];
}

const char *ypso_svc_uuid(ypso_svc_t s)
{
    return SVC[s];
}

bool ypso_has_svc(ypso_fw_t fw, ypso_svc_t s)
{
    const ypso_fw_info_t *f = ypso_fw(fw);
    return s == YS_CONTROL ? f->has_control : s == YS_CRYPTO ? f->encrypted : true;
}

static bool is_alt(ypso_chr_t c)
{
    return c == YC_SETTING_ID_ALT || c == YC_SETTING_VALUE_ALT || (c >= YC_ALARM_COUNT_ALT && c <= YC_SYSTEM_VALUE_ALT);
}

bool ypso_has_chr(ypso_fw_t fw, ypso_chr_t c)
{
    return ypso_has_svc(fw, CHR[c].svc) && (!is_alt(c) || ypso_fw(fw)->alt);
}

void ypso_uuid_le(const char *text, uint8_t out[16])
{
    uint8_t be[16];
    int n = 0;
    for (const char *p = text; *p && n < 32; p++) {
        int v = *p >= '0' && *p <= '9' ? *p - '0' : *p >= 'a' && *p <= 'f' ? *p - 'a' + 10 : -1;
        if (v < 0) {
            continue;
        }
        if (n % 2 == 0) {
            be[n / 2] = (uint8_t)(v << 4);
        } else {
            be[n / 2] |= (uint8_t)v;
        }
        n++;
    }
    for (int i = 0; i < 16; i++) {
        out[i] = be[15 - i];
    }
}

/* ---- helpers --------------------------------------------------------------------------- */

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, (uint16_t)v);
    put16(p + 2, (uint16_t)(v >> 16));
}

static uint32_t get32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put_safevar(uint8_t *p, int32_t v)
{
    put32(p, (uint32_t)v);
    put32(p + 4, ~(uint32_t)v);
}

/* A SAFEVAR's value, or false when its complement does not match. */
static bool get_safevar(const uint8_t *p, size_t n, int32_t *v)
{
    if (n < 8) {
        return false;
    }
    *v = (int32_t)get32(p);
    return get32(p + 4) == ~get32(p);
}

static void ylog(ypso_t *y, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void ylog(ypso_t *y, const char *fmt, ...)
{
    if (!y->io->log) {
        return;
    }
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    y->io->log(y->io->ctx, buf);
}

static uint8_t bitrev8(uint8_t b)
{
    b = (uint8_t)((b & 0xf0) >> 4 | (b & 0x0f) << 4);
    b = (uint8_t)((b & 0xcc) >> 2 | (b & 0x33) << 2);
    return (uint8_t)((b & 0xaa) >> 1 | (b & 0x55) << 1);
}

/* §3.11: 4-byte blocks byte-reversed and bit-reversed, reflected CRC-32 from
 * 0xFFFFFFFF with no final xor, the result bit-reversed, low 16 bits. */
uint16_t ypso_crc16(const uint8_t *p, size_t n)
{
    uint32_t crc = 0xffffffff;
    size_t padded = (n + 3) / 4 * 4;
    for (size_t i = 0; i < padded; i += 4) {
        for (int k = 0; k < 4; k++) {
            size_t src = i + 3 - (size_t)k;
            uint8_t b = src < n ? bitrev8(p[src]) : 0;
            crc ^= b;
            for (int j = 0; j < 8; j++) {
                crc = (crc & 1) ? (crc >> 1) ^ 0xedb88320 : crc >> 1;
            }
        }
    }
    uint32_t r = 0;
    for (int i = 0; i < 32; i++) {
        r |= ((crc >> i) & 1) << (31 - i);
    }
    return (uint16_t)r;
}

/* ---- XChaCha20-Poly1305, the pump's layout (§3.3) -------------------------------------- */

static void hchacha20(const uint8_t key[32], const uint8_t nonce[16], uint8_t out[32])
{
    uint32_t x[16] = {0x61707865, 0x3320646e, 0x79622d32, 0x6b206574};
    for (int i = 0; i < 8; i++) {
        x[4 + i] = get32(key + 4 * i);
    }
    for (int i = 0; i < 4; i++) {
        x[12 + i] = get32(nonce + 4 * i);
    }
    dash_chacha20_rounds(x);
    for (int i = 0; i < 4; i++) {
        put32(out + 4 * i, x[i]);
        put32(out + 16 + 4 * i, x[12 + i]);
    }
}

static void xnonce(const uint8_t key[32], const uint8_t nonce[24], uint8_t sub[32], uint8_t n12[12])
{
    hchacha20(key, nonce, sub);
    memset(n12, 0, 4);
    memcpy(n12 + 4, nonce + 16, 8);
}

size_t ypso_seal(const uint8_t key[32], const uint8_t *p, size_t n, uint32_t reboot, uint64_t counter,
                 const uint8_t nonce[24], uint8_t *out)
{
    uint8_t pt[YPSO_MSG_MAX];
    memcpy(pt, p, n);
    put32(pt + n, reboot);
    put32(pt + n + 4, (uint32_t)counter);
    put32(pt + n + 8, (uint32_t)(counter >> 32));
    uint8_t sub[32], n12[12];
    xnonce(key, nonce, sub, n12);
    dash_chachapoly_seal(sub, n12, pt, n + 12, out);
    memcpy(out + n + 12 + 16, nonce, 24);
    memset(sub, 0, sizeof sub);
    return n + 12 + 16 + 24;
}

int ypso_open(const uint8_t key[32], const uint8_t *in, size_t n, uint8_t *out, uint32_t *reboot, uint64_t *counter)
{
    if (n < 52 || n - 52 > YPSO_MSG_MAX) {
        return -1;
    }
    const uint8_t *nonce = in + n - 24;
    size_t ct = n - 24 - 16;
    uint8_t sub[32], n12[12];
    xnonce(key, nonce, sub, n12);
    int rc = dash_chachapoly_open(sub, n12, in, ct, out);
    memset(sub, 0, sizeof sub);
    if (rc != 0) {
        return -1;
    }
    size_t len = ct - 12;
    *reboot = get32(out + len);
    *counter = get32(out + len + 4) | (uint64_t)get32(out + len + 8) << 32;
    return (int)len;
}

/* ---- the key exchange (encrypted firmware) ---------------------------------------------- */
/*
 * What mylife, CamAPS and rockit's offline pairing run on an encrypted pump
 * (rockit's decision record ypso-offline-pair §1, key_exchange.rs):
 *
 *   1. the app reads NewPublicKey (framed, continued over ExtendedRead):
 *      a fresh 32-byte challenge, then the pump's X25519 public key;
 *   2. the app writes a blob to WriteChallengeUtcAppPubKey (clear, framed):
 *      XChaCha20-Poly1305(K_s)(challenge ‖ ttl u32 ‖ public key ‖ counter u64)
 *      ‖ nonce, with K_s = HChaCha20(X25519(pump private, server public), 0¹⁶);
 *   3. the pump's new session key is HChaCha20(X25519(pump private, the
 *      blob's public key), 0¹⁶), and it counts its messages from the bottom.
 *
 * A stock pump trusts the vendor's server key. This one trusts its own
 * (server_priv), made at random on the board. A blob that does not open is
 * refused 0x8A; a counter not above the last exchange's, 0x8B.
 */

static void save(ypso_t *y);

static void pump_private(const ypso_state_t *st, uint8_t out[32])
{
    /* Stable for this pump without a new saved field: from its own secrets. */
    uint8_t in[16 + 16 + 4];
    memcpy(in, "esp-sim ypso x25", 16);
    memcpy(in + 16, st->password, 16);
    put32(in + 32, st->serial);
    dash_sha256(in, sizeof in, out);
    memset(in, 0, sizeof in);
}

static void x25519_hchacha(const uint8_t priv[32], const uint8_t pub[32], uint8_t out[32])
{
    static const uint8_t zero[16];
    uint8_t shared[32];
    dash_x25519(shared, priv, pub);
    hchacha20(shared, zero, out);
    memset(shared, 0, sizeof shared);
}

void ypso_pump_public(const ypso_state_t *st, uint8_t out[32])
{
    uint8_t priv[32];
    pump_private(st, priv);
    dash_x25519_base(out, priv);
    memset(priv, 0, sizeof priv);
}

#define KX_PT 76    /* challenge 32, ttl 4, public key 32, counter 8 */
#define KX_BLOB 116 /* + tag 16 + nonce 24 */

/* The reassembled blob. 0, or the pump's refusal. */
static int key_exchange(ypso_t *y, const uint8_t *blob, size_t n)
{
    if (n != KX_BLOB || !y->challenge_set) {
        ylog(y, "key exchange refused: %s", n != KX_BLOB ? "the blob's length is wrong" : "no offer was read");
        return YPSO_ENCRYPTION_FAILED;
    }
    uint8_t priv[32], server_pub[32], ks[32], sub[32], n12[12], pt[KX_PT];
    pump_private(&y->st, priv);
    dash_x25519_base(server_pub, y->st.server_priv);
    x25519_hchacha(priv, server_pub, ks);
    xnonce(ks, blob + KX_PT + 16, sub, n12);
    int rc = dash_chachapoly_open(sub, n12, blob, KX_PT, pt);
    memset(ks, 0, sizeof ks);
    memset(sub, 0, sizeof sub);
    if (rc != 0 || memcmp(pt, y->challenge, 32) != 0) {
        memset(priv, 0, sizeof priv);
        memset(pt, 0, sizeof pt);
        ylog(y, "key exchange refused: %s", rc != 0 ? "the blob does not open under the trusted server key"
                                                     : "the blob is for another challenge");
        return YPSO_ENCRYPTION_FAILED;
    }
    uint32_t ttl = get32(pt + 32);
    uint64_t counter = get32(pt + 68) | (uint64_t)get32(pt + 72) << 32;
    if (counter <= y->st.kx_counter) {
        memset(priv, 0, sizeof priv);
        memset(pt, 0, sizeof pt);
        ylog(y, "key exchange refused: counter %llu not above %llu", (unsigned long long)counter,
             (unsigned long long)y->st.kx_counter);
        return YPSO_COUNTER_ERROR;
    }
    x25519_hchacha(priv, pt + 36, y->st.key);
    memset(priv, 0, sizeof priv);
    memset(pt, 0, sizeof pt);
    y->st.kx_counter = counter;
    y->st.read_counter = 0;
    y->st.write_counter = 0;
    y->challenge_set = false;
    memset(y->challenge, 0, sizeof y->challenge);
    ylog(y, "key exchange done: a new session key, valid %u days", (unsigned)(ttl / 86400));
    save(y);
    return YPSO_OK;
}

/* ---- the pump's clock -------------------------------------------------------------------- */

static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void civil_from_days(int64_t z, int *y, int *m, int *d)
{
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yoe + era * 400 + (*m <= 2));
}

#define EPOCH_2000 946684800LL

/* The pump's wall clock, as seconds since 1970 in its own (zone-less) time. */
static int64_t pump_now(ypso_t *y)
{
    int64_t now = y->io->now(y->io->ctx);
    int32_t off = y->io->utc_offset ? y->io->utc_offset(y->io->ctx) : 0;
    return now + off + y->st.clock_offset;
}

static uint32_t pump_t2000(ypso_t *y)
{
    int64_t t = pump_now(y) - EPOCH_2000;
    return t > 0 ? (uint32_t)t : 0;
}

/* ---- history ------------------------------------------------------------------------------ */

enum {
    EV_BOLUS_IMMEDIATE = 2,
    EV_BOLUS_DELAYED = 3,
    EV_BASAL_A_CHANGED = 7,
    EV_TBR = 10,
    EV_DATE_CHANGED = 12,
    EV_TIME_CHANGED = 13,
    EV_PUMP_MODE_CHANGED = 14,
    EV_BOLUS_COMBINED = 18,
    EV_BOLUS_IMMEDIATE_ABORT = 29,
    EV_BOLUS_DELAYED_ABORT = 30,
    EV_BOLUS_COMBINED_ABORT = 31,
    EV_TBR_ABORT = 32,
};

static void event(ypso_t *y, uint8_t type, uint16_t a, uint16_t b, uint16_t c)
{
    ypso_state_t *st = &y->st;
    if (st->n_events == YPSO_EVENTS) {
        memmove(st->events, st->events + 1, sizeof st->events[0] * (YPSO_EVENTS - 1));
        st->n_events--;
    }
    ypso_event_t *e = &st->events[st->n_events++];
    e->t = pump_t2000(y);
    e->seq = ++st->event_seq;
    e->type = type;
    e->v[0] = a;
    e->v[1] = b;
    e->v[2] = c;
    y->dirty = true;
}

/* §5.8: 19 bytes, newest first from index 0. */
static void entry_bytes(const ypso_event_t *e, uint16_t index, uint8_t out[19])
{
    put32(out, e->t);
    out[4] = e->type;
    put16(out + 5, e->v[0]);
    put16(out + 7, e->v[1]);
    put16(out + 9, e->v[2]);
    put32(out + 11, e->seq);
    put16(out + 15, index);
    put16(out + 17, ypso_crc16(out, 17));
}

/* ---- state ---------------------------------------------------------------------------------- */

void ypso_state_fresh(ypso_state_t *st, ypso_fw_t fw)
{
    memset(st, 0, sizeof *st);
    st->version = YPSO_STATE_VERSION;
    st->fw = (uint8_t)(fw < YPSO_FW_COUNT ? fw : YPSO_FW_V05);
    uint8_t r[4];
    dash_random(r, sizeof r);
    st->serial = 10000000 + get32(r) % 90000000; /* 8 digits, like a real one; made up */
    dash_random(st->key, sizeof st->key);
    dash_random(st->password, sizeof st->password);
    dash_random(r, sizeof r);
    st->reboot = 1 + r[0] % 20;
    st->read_counter = 1000 + (get32(r) >> 8) % 100000;
    st->reservoir = 16000; /* 160 U */
    st->battery = 5;
    st->profile = 3;
    for (int h = 0; h < 24; h++) {
        st->basal_a[h] = 80;
        st->basal_b[h] = 50;
    }
    st->tbr_percent = 100;
    dash_random(st->server_priv, sizeof st->server_priv);
}

void ypso_init(ypso_t *y, const ypso_state_t *st, const ypso_io_t *io)
{
    memset(y, 0, sizeof *y);
    y->st = *st;
    y->io = io;
    y->last_tick = io->now(io->ctx);
    static const uint8_t none[32];
    if (memcmp(y->st.server_priv, none, 32) == 0) {
        /* Saved before the key exchange was simulated. */
        dash_random(y->st.server_priv, sizeof y->st.server_priv);
        y->dirty = true;
    }
}

static void save(ypso_t *y)
{
    y->dirty = false;
    if (y->io->save) {
        y->io->save(y->io->ctx, &y->st);
    }
}

static int pump_hour(ypso_t *y)
{
    int64_t t = pump_now(y);
    return (int)(((t % 86400) + 86400) % 86400 / 3600);
}

static int32_t basal_now(ypso_t *y)
{
    ypso_state_t *st = &y->st;
    if (st->suspended || st->reservoir <= 0) {
        return 0;
    }
    int32_t rate = (st->profile == 3 ? st->basal_a : st->basal_b)[pump_hour(y)];
    return st->tbr_percent == 100 ? rate : rate * st->tbr_percent / 100;
}

static void take(ypso_t *y, int32_t hundredths)
{
    ypso_state_t *st = &y->st;
    st->delivered_total += (uint32_t)hundredths;
    if (st->reservoir > 0) {
        st->reservoir = st->reservoir > hundredths ? st->reservoir - hundredths : 0;
    }
}

#define BOLUS_STEP 5 /* hundredths per second: 3 U a minute */

static void end_immediate(ypso_t *y, bool aborted)
{
    ypso_state_t *st = &y->st;
    st->imm.status = 4;
    event(y, aborted ? EV_BOLUS_IMMEDIATE_ABORT : EV_BOLUS_IMMEDIATE, (uint16_t)st->imm.delivered, 0, 0);
    ylog(y, "bolus %s: %d.%02d U", aborted ? "stopped" : "done", (int)st->imm.delivered / 100,
         (int)st->imm.delivered % 100);
    if (aborted) {
        st->imm.total = st->imm.delivered; /* the driver reads "done" as delivered == total */
    }
}

static void end_slow(ypso_t *y, bool aborted)
{
    ypso_state_t *st = &y->st;
    bool combo = st->slow.now_total > 0;
    int32_t total = st->slow.delivered + st->slow.now_delivered;
    int32_t minutes = (int32_t)((y->io->now(y->io->ctx) - st->slow.start) / 60);
    if (combo) {
        event(y, aborted ? EV_BOLUS_COMBINED_ABORT : EV_BOLUS_COMBINED, (uint16_t)total,
              (uint16_t)st->slow.now_delivered, (uint16_t)(aborted ? minutes : st->slow.minutes));
    } else {
        event(y, aborted ? EV_BOLUS_DELAYED_ABORT : EV_BOLUS_DELAYED, (uint16_t)total,
              (uint16_t)(aborted ? minutes : st->slow.minutes), 0);
    }
    ylog(y, "extended bolus %s: %d.%02d U", aborted ? "stopped" : "done", (int)total / 100, (int)total % 100);
    memset(&st->slow, 0, sizeof st->slow);
}

static void end_tbr(ypso_t *y, bool aborted)
{
    ypso_state_t *st = &y->st;
    int32_t ran = (int32_t)((y->io->now(y->io->ctx) - st->tbr_start) / 60);
    event(y, aborted ? EV_TBR_ABORT : EV_TBR, (uint16_t)st->tbr_percent,
          (uint16_t)(aborted ? ran : st->tbr_minutes), 0);
    ylog(y, "temp basal %d%% %s", (int)st->tbr_percent, aborted ? "cancelled" : "ended");
    st->tbr_percent = 100;
    st->tbr_minutes = 0;
    st->tbr_start = 0;
}

void ypso_tick(ypso_t *y)
{
    ypso_state_t *st = &y->st;
    int64_t now = y->io->now(y->io->ctx);
    int64_t dt = now - y->last_tick;
    if (dt <= 0) {
        return;
    }
    if (dt > 3600) {
        dt = 3600; /* the board was off: don't run an hour of catch-up per tick */
    }
    y->last_tick = now;
    for (int64_t s = 0; s < dt; s++) {
        /* Basal, in thousandths of a hundredth: rate (hundredths/h) × 1000 / 3600 a second. */
        st->milli_hundredths += basal_now(y) * 1000 / 3600;
        if (st->milli_hundredths >= 1000) {
            take(y, st->milli_hundredths / 1000);
            st->milli_hundredths %= 1000;
        }
        if (st->imm.status == 1) {
            int32_t step = st->imm.total - st->imm.delivered < BOLUS_STEP ? st->imm.total - st->imm.delivered : BOLUS_STEP;
            st->imm.delivered += step;
            take(y, step);
            if (st->imm.delivered >= st->imm.total) {
                end_immediate(y, false);
            }
        }
        if (st->slow.seq) {
            if (st->slow.now_delivered < st->slow.now_total) {
                int32_t left = st->slow.now_total - st->slow.now_delivered;
                int32_t step = left < BOLUS_STEP ? left : BOLUS_STEP;
                st->slow.now_delivered += step;
                take(y, step);
            }
            int64_t el = now - st->slow.start;
            int32_t due = el >= (int64_t)st->slow.minutes * 60
                              ? st->slow.total
                              : (int32_t)((int64_t)st->slow.total * el / ((int64_t)st->slow.minutes * 60));
            if (due > st->slow.delivered) {
                take(y, due - st->slow.delivered);
                st->slow.delivered = due;
            }
            if (st->slow.delivered >= st->slow.total && st->slow.now_delivered >= st->slow.now_total) {
                end_slow(y, false);
            }
        }
    }
    if (st->tbr_percent != 100 && now - st->tbr_start >= (int64_t)st->tbr_minutes * 60) {
        end_tbr(y, false);
    }
    /* Saved when something was logged, else at most once a minute. */
    if (y->dirty || now - y->last_save >= 60) {
        y->last_save = now;
        save(y);
    }
}

/* ---- the connection -------------------------------------------------------------------- */

void ypso_on_connect(ypso_t *y)
{
    y->connected = true;
    y->authorized = false;
    y->setting_id = -1;
    y->counter_id = -1;
    memset(y->index, 0, sizeof y->index);
    y->rx_len = y->tx_len = y->tx_off = 0;
    y->rx_next = 1;
    ylog(y, "phone connected");
}

void ypso_on_disconnect(ypso_t *y)
{
    y->connected = false;
    y->authorized = false;
    if (y->dirty) {
        save(y);
    }
    ylog(y, "phone disconnected");
}

static const char *version_string(ypso_t *y, ypso_chr_t c)
{
    const ypso_fw_info_t *f = ypso_fw(y->st.fw);
    switch (c) {
    case YC_BASE_VERSION:
        return f->pump_base;
    case YC_MASTER_VERSION:
        return f->master;
    case YC_SUPERVISOR_VERSION:
        return f->supervisor;
    case YC_SETTINGS_VERSION:
        return f->settings;
    case YC_HISTORY_VERSION:
        return f->history;
    case YC_CONTROL_VERSION:
        return f->control;
    case YC_CRYPTO_VERSION:
        return f->crypto;
    default:
        return NULL;
    }
}

/* Which log a history characteristic belongs to: 0 event, 1 alarm, 2 system, 3 complaint. */
static int log_of(ypso_chr_t c, int *kind)
{
    static const struct {
        ypso_chr_t c;
        int log, kind;
    } map[] = {
        {YC_EVENT_COUNT, 0, 0},     {YC_EVENT_INDEX, 0, 1},     {YC_EVENT_VALUE, 0, 2},
        {YC_ALARM_COUNT, 1, 0},     {YC_ALARM_INDEX, 1, 1},     {YC_ALARM_VALUE, 1, 2},
        {YC_SYSTEM_COUNT, 2, 0},    {YC_SYSTEM_INDEX, 2, 1},    {YC_SYSTEM_VALUE, 2, 2},
        {YC_COMPLAINT_COUNT, 3, 0}, {YC_COMPLAINT_INDEX, 3, 1}, {YC_COMPLAINT_VALUE, 3, 2},
        {YC_EVENT_COUNT_ALT, 0, 0}, {YC_EVENT_INDEX_ALT, 0, 1}, {YC_EVENT_VALUE_ALT, 0, 2},
        {YC_ALARM_COUNT_ALT, 1, 0}, {YC_ALARM_INDEX_ALT, 1, 1}, {YC_ALARM_VALUE_ALT, 1, 2},
        {YC_SYSTEM_COUNT_ALT, 2, 0}, {YC_SYSTEM_INDEX_ALT, 2, 1}, {YC_SYSTEM_VALUE_ALT, 2, 2},
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) {
        if (map[i].c == c) {
            *kind = map[i].kind;
            return map[i].log;
        }
    }
    return -1;
}

/* ---- settings (§5.1) ------------------------------------------------------------------- */

static bool setting_get(ypso_t *y, int32_t id, int32_t *v)
{
    ypso_state_t *st = &y->st;
    if (id >= 14 && id <= 37) {
        *v = st->basal_a[id - 14];
    } else if (id >= 38 && id <= 61) {
        *v = st->basal_b[id - 38];
    } else {
        switch (id) {
        case 1: *v = st->profile; break;
        case 2: *v = st->suspended ? 3 : 10; break;
        case 3: *v = 10; break;           /* bolus step 0.10 U */
        case 5: *v = 1460; break;         /* remaining lifetime, days */
        case 62: *v = (int32_t)(st->serial & 0xffff); break;
        case 63: *v = (int32_t)(st->serial >> 16); break;
        case 64: *v = (int32_t)(st->event_seq & 0xffff); break;
        case 65: *v = (int32_t)(st->event_seq >> 16); break;
        case 112: *v = 2500; break;       /* bolus cap 25 U */
        case 113: *v = 1000; break;       /* basal cap 10 U/h */
        case 114: *v = 2; break;          /* basal min 0.02 U/h */
        case 115: *v = 4000; break;       /* basal max 40 U/h */
        case 116: *v = TBR_LONG_MAX; break;     /* TBR long scale max % */
        case 117: *v = TBR_LONG_MINUTES; break; /* TBR long duration max, minutes */
        case 118: *v = TBR_SHORT_MAX; break;    /* TBR short scale max % */
        case 119: *v = TBR_SHORT_MINUTES; break; /* TBR short duration max */
        case 120: *v = 1; break;          /* TBR end not indicated */
        default: return false;
        }
    }
    return true;
}

static int setting_set(ypso_t *y, int32_t id, int32_t v)
{
    ypso_state_t *st = &y->st;
    if (id >= 14 && id <= 61) {
        if (v < 0 || v > 4000) {
            return YPSO_VALUE_INVALID;
        }
        int32_t *slot = id <= 37 ? &st->basal_a[id - 14] : &st->basal_b[id - 38];
        int32_t old = *slot;
        *slot = v;
        if (old != v) {
            event(y, id <= 37 ? EV_BASAL_A_CHANGED : EV_BASAL_A_CHANGED + 1, (uint16_t)((id - 14) % 24), (uint16_t)v,
                  (uint16_t)old);
        }
        return YPSO_OK;
    }
    if (id == 1) {
        if (v != 3 && v != 4) {
            return YPSO_VALUE_INVALID;
        }
        st->profile = (uint8_t)v;
        y->dirty = true;
        return YPSO_OK;
    }
    int32_t cur;
    return setting_get(y, id, &cur) ? YPSO_VALUE_INVALID : YPSO_INVALID_ID; /* known but read-only here */
}

/* ---- status responses -------------------------------------------------------------------- */

static size_t system_status(ypso_t *y, uint8_t *out)
{
    const ypso_fw_info_t *f = ypso_fw(y->st.fw);
    ypso_state_t *st = &y->st;
    memset(out, 0, 20);
    out[0] = st->suspended ? 0x03 : 0x0a;
    put32(out + 1, (uint32_t)st->reservoir);
    out[5] = st->battery;
    int32_t rate = (st->profile == 3 ? st->basal_a : st->basal_b)[pump_hour(y)];
    put32(out + 6, (uint32_t)(st->suspended ? 0 : rate));
    put32(out + 10, (uint32_t)st->tbr_percent);
    int32_t left = 0;
    if (st->tbr_percent != 100) {
        int64_t el = y->io->now(y->io->ctx) - st->tbr_start;
        left = st->tbr_minutes - (int32_t)(el / 60);
        left = left < 0 ? 0 : left;
    }
    put32(out + 14, (uint32_t)left);
    if (f->status_len == 20) {
        put16(out + 18, ypso_crc16(out, 18));
    }
    return f->status_len;
}

static size_t bolus_status(ypso_t *y, uint8_t *out)
{
    ypso_state_t *st = &y->st;
    if (!ypso_fw(st->fw)->extended) {
        /* §5.4b: state, delivered, total, sequence. */
        out[0] = st->imm.status;
        put32(out + 1, (uint32_t)st->imm.delivered);
        put32(out + 5, (uint32_t)st->imm.total);
        put32(out + 9, st->imm.seq);
        return 13;
    }
    memset(out, 0, 44);
    out[0] = st->imm.status;
    put32(out + 1, st->imm.seq);
    put32(out + 5, (uint32_t)st->imm.delivered);
    put32(out + 9, (uint32_t)st->imm.total);
    out[13] = st->slow.status;
    put32(out + 14, st->slow.seq);
    put32(out + 18, (uint32_t)st->slow.delivered);
    put32(out + 22, (uint32_t)st->slow.total);
    put32(out + 26, (uint32_t)st->slow.now_delivered);
    put32(out + 30, (uint32_t)st->slow.now_total);
    int32_t el = st->slow.seq ? (int32_t)((y->io->now(y->io->ctx) - st->slow.start) / 60) : 0;
    put32(out + 34, (uint32_t)(el > st->slow.minutes ? st->slow.minutes : el));
    put32(out + 38, (uint32_t)st->slow.minutes);
    put16(out + 42, ypso_crc16(out, 42));
    return 44;
}

/* ---- commands ----------------------------------------------------------------------------- */

static int start_bolus(ypso_t *y, int32_t total, int32_t minutes, int32_t now_part, uint8_t type)
{
    ypso_state_t *st = &y->st;
    if (type == 0x01) {
        if (total == 0) { /* stop */
            if (st->imm.status == 1) {
                end_immediate(y, true);
            }
            return YPSO_OK;
        }
        if (st->suspended || st->imm.status == 1 || st->reservoir < total) {
            return YPSO_COMMAND_FAILED;
        }
        if (total < 10 || total > 2500) {
            return YPSO_VALUE_INVALID;
        }
        st->imm.status = 1;
        st->imm.seq = ++st->bolus_seq;
        st->imm.total = total;
        st->imm.delivered = 0;
        ylog(y, "bolus %d.%02d U", (int)total / 100, (int)total % 100);
        y->dirty = true;
        return YPSO_OK;
    }
    if (type == 0x02) {
        if (total == 0) {
            if (st->slow.seq) {
                end_slow(y, true);
            }
            return YPSO_OK;
        }
        if (st->suspended || st->slow.seq || st->reservoir < total) {
            return YPSO_COMMAND_FAILED;
        }
        if (total < 10 || total > 2500 || minutes < 15 || minutes > 720 || now_part < 0 || now_part > total - 10) {
            return YPSO_VALUE_INVALID;
        }
        st->slow.status = 1;
        st->slow.seq = ++st->bolus_seq;
        st->slow.total = total - now_part;
        st->slow.delivered = 0;
        st->slow.now_total = now_part;
        st->slow.now_delivered = 0;
        st->slow.minutes = minutes;
        st->slow.start = y->io->now(y->io->ctx);
        ylog(y, "extended bolus %d.%02d U over %d min", (int)total / 100, (int)total % 100, (int)minutes);
        y->dirty = true;
        return YPSO_OK;
    }
    return YPSO_VALUE_INVALID;
}

static int set_tbr(ypso_t *y, int32_t percent, int32_t minutes)
{
    /* What settings 116-119 report, enforced: up to the long ceiling for up to
     * its duration, above it up to the short ceiling for up to its own. */
    ypso_state_t *st = &y->st;
    const ypso_fw_info_t *f = ypso_fw(st->fw);
    if (percent == 100) { /* the cancel: 100 % / 0 */
        if (st->tbr_percent != 100) {
            end_tbr(y, true);
        }
        return YPSO_OK;
    }
    if (st->suspended) {
        return YPSO_COMMAND_FAILED;
    }
    if (st->tbr_percent != 100) {
        return YPSO_INTERNAL_ERROR; /* §3.6: a TBR already running; the driver clears it first */
    }
    int32_t longest = percent <= TBR_LONG_MAX ? TBR_LONG_MINUTES : TBR_SHORT_MINUTES;
    if (percent < 0 || percent > (TBR_LONG_MAX > TBR_SHORT_MAX ? TBR_LONG_MAX : TBR_SHORT_MAX) ||
        percent % f->tbr_step || minutes < 15 || minutes > longest || minutes % 15) {
        return YPSO_VALUE_INVALID;
    }
    st->tbr_percent = percent;
    st->tbr_minutes = minutes;
    st->tbr_start = y->io->now(y->io->ctx);
    ylog(y, "temp basal %d%% for %d min", (int)percent, (int)minutes);
    y->dirty = true;
    return YPSO_OK;
}

static int set_clock(ypso_t *y, bool date, const uint8_t *p, size_t n)
{
    int64_t cur = pump_now(y);
    int64_t day = cur >= 0 ? cur / 86400 : (cur - 86399) / 86400;
    int64_t sec = cur - day * 86400;
    int yr, mo, dd;
    civil_from_days(day, &yr, &mo, &dd);
    if (date) {
        if (n != 4) {
            return YPSO_VALUE_INVALID;
        }
        int ny = p[0] | p[1] << 8, nm = p[2], nd = p[3];
        if (ny < 2000 || ny > 2099 || nm < 1 || nm > 12 || nd < 1 || nd > 31) {
            return YPSO_VALUE_INVALID;
        }
        day = days_from_civil(ny, nm, nd);
        event(y, EV_DATE_CHANGED, (uint16_t)nd, (uint16_t)nm, (uint16_t)(ny - 2000));
    } else {
        if (n != 3 || p[0] > 23 || p[1] > 59 || p[2] > 59) {
            return YPSO_VALUE_INVALID;
        }
        sec = p[0] * 3600 + p[1] * 60 + p[2];
        event(y, EV_TIME_CHANGED, p[0], p[1], p[2]);
    }
    y->st.clock_offset += day * 86400 + sec - cur;
    return YPSO_OK;
}

/* A protected command's plaintext, once it arrived whole and decrypted. */
static int command(ypso_t *y, ypso_chr_t c, const uint8_t *p, size_t n)
{
    const ypso_fw_info_t *f = ypso_fw(y->st.fw);
    int32_t v, w;
    int kind;
    int lg = log_of(c, &kind);
    if (lg >= 0 && kind == 1) { /* an entry index */
        int32_t idx;
        if (f->index_safevar) {
            if (n != 8) {
                return YPSO_VALUE_INVALID;
            }
            if (!get_safevar(p, n, &idx)) {
                return YPSO_VALUE_CORRUPT;
            }
        } else {
            if (n != 2) {
                return YPSO_VALUE_INVALID;
            }
            idx = p[0] | p[1] << 8;
        }
        int count = lg == 0 ? y->st.n_events : 0;
        if (idx < 0 || idx >= count) {
            return YPSO_ENTRY_INDEX_INVALID;
        }
        y->index[lg] = idx;
        return YPSO_OK;
    }
    switch (c) {
    case YC_SETTING_ID:
    case YC_SETTING_ID_ALT:
        if (n != 8 || !get_safevar(p, n, &v)) {
            return YPSO_VALUE_CORRUPT;
        }
        if (!setting_get(y, v, &w)) {
            return YPSO_INVALID_ID;
        }
        y->setting_id = v;
        return YPSO_OK;
    case YC_SETTING_VALUE:
    case YC_SETTING_VALUE_ALT:
        if (n != 8 || !get_safevar(p, n, &v)) {
            return YPSO_VALUE_CORRUPT;
        }
        if (y->setting_id < 0) {
            return YPSO_INVALID_ID;
        }
        return setting_set(y, y->setting_id, v);
    case YC_COUNTER_ID:
        if (n != 8 || !get_safevar(p, n, &v)) {
            return YPSO_VALUE_CORRUPT;
        }
        if (v < 1 || v > 17) {
            return YPSO_INVALID_ID;
        }
        y->counter_id = v;
        return YPSO_OK;
    case YC_SYSTEM_DATE:
        return set_clock(y, true, p, n);
    case YC_SYSTEM_TIME:
        return set_clock(y, false, p, n);
    case YC_START_STOP_BOLUS:
        if (f->extended) {
            if (n != 15) {
                return YPSO_VALUE_INVALID;
            }
            if (ypso_crc16(p, 13) != (uint16_t)(p[13] | p[14] << 8)) {
                return YPSO_VALUE_CORRUPT;
            }
            return start_bolus(y, (int32_t)get32(p), (int32_t)get32(p + 4), (int32_t)get32(p + 8), p[12]);
        }
        if (n != 8 || !get_safevar(p, n, &v)) {
            return YPSO_VALUE_CORRUPT;
        }
        return start_bolus(y, v, 0, 0, 0x01);
    case YC_START_STOP_TBR:
        if (n != 16 || !get_safevar(p, 8, &v) || !get_safevar(p + 8, 8, &w)) {
            return YPSO_VALUE_CORRUPT;
        }
        return set_tbr(y, v, w);
    default:
        return YPSO_VALUE_INVALID;
    }
}

/* A protected read's plaintext. Returns an ATT status. */
static int answer(ypso_t *y, ypso_chr_t c, uint8_t *out, size_t *n)
{
    int kind;
    int lg = log_of(c, &kind);
    int32_t v;
    if (lg >= 0) {
        int count = lg == 0 ? y->st.n_events : 0;
        if (kind == 0) {
            put_safevar(out, count);
            *n = 8;
            return YPSO_OK;
        }
        if (kind == 2) {
            int idx = y->index[lg];
            if (idx < 0 || idx >= count) {
                return YPSO_ENTRY_INDEX_INVALID;
            }
            entry_bytes(&y->st.events[count - 1 - idx], (uint16_t)idx, out);
            y->index[lg] = idx + 1; /* §3.10: the index moves on after a value read */
            *n = 19;
            return YPSO_OK;
        }
        return YPSO_VALUE_INVALID;
    }
    switch (c) {
    case YC_SETTING_VALUE:
    case YC_SETTING_VALUE_ALT:
        if (y->setting_id < 0 || !setting_get(y, y->setting_id, &v)) {
            return YPSO_INVALID_ID;
        }
        put_safevar(out, v);
        *n = 8;
        return YPSO_OK;
    case YC_COUNTER_VALUE:
        if (y->counter_id < 0) {
            return YPSO_INVALID_ID;
        }
        put_safevar(out, (int32_t)(y->counter_id * 37 + (int32_t)(y->st.event_seq % 1000)));
        *n = 8;
        return YPSO_OK;
    case YC_SYSTEM_DATE: {
        int64_t t = pump_now(y);
        int yr, mo, dd;
        civil_from_days(t >= 0 ? t / 86400 : (t - 86399) / 86400, &yr, &mo, &dd);
        put16(out, (uint16_t)yr);
        out[2] = (uint8_t)mo;
        out[3] = (uint8_t)dd;
        *n = 4;
        return YPSO_OK;
    }
    case YC_SYSTEM_TIME: {
        int64_t s = ((pump_now(y) % 86400) + 86400) % 86400;
        out[0] = (uint8_t)(s / 3600);
        out[1] = (uint8_t)(s / 60 % 60);
        out[2] = (uint8_t)(s % 60);
        *n = 3;
        return YPSO_OK;
    }
    case YC_GET_SYSTEM_STATUS:
        *n = system_status(y, out);
        return YPSO_OK;
    case YC_GET_BOLUS_STATUS:
        *n = bolus_status(y, out);
        return YPSO_OK;
    default:
        return YPSO_VALUE_INVALID;
    }
}

int ypso_on_read(ypso_t *y, ypso_chr_t c, uint8_t *out, size_t cap, size_t *len)
{
    const ypso_fw_info_t *f = ypso_fw(y->st.fw);
    *len = 0;
    if (c >= YC_COUNT || !ypso_has_chr(y->st.fw, c) || !CHR[c].read) {
        return YPSO_VALUE_INVALID;
    }
    if (c == YC_EXTENDED_READ) {
        /* §3.2: the next frame of the read in progress. */
        if (y->tx_off >= y->tx_len || cap < YPSO_PKT) {
            return YPSO_VALUE_INVALID;
        }
        size_t k = y->tx_len - y->tx_off < 19 ? y->tx_len - y->tx_off : 19;
        out[0] = (uint8_t)((++y->tx_frame << 4 & 0xf0) | (y->tx_frames & 0x0f));
        memcpy(out + 1, y->tx + y->tx_off, k);
        y->tx_off += k;
        *len = k + 1;
        return YPSO_OK;
    }
    if (c == YC_NEW_PUBLIC_KEY) {
        /* The key exchange's offer: a fresh challenge and the pump's public
         * key, in clear, framed; frame 1 here, the rest through ExtendedRead. */
        if (!y->authorized && !y->open) {
            return YPSO_ATT_INSUFFICIENT_AUTHORIZATION;
        }
        if (cap < YPSO_PKT) {
            return YPSO_VALUE_INVALID;
        }
        dash_random(y->challenge, sizeof y->challenge);
        y->challenge_set = true;
        memcpy(y->tx, y->challenge, 32);
        ypso_pump_public(&y->st, y->tx + 32);
        y->tx_len = 64;
        y->tx_frames = (uint8_t)((y->tx_len + 18) / 19);
        y->tx_frame = 0;
        out[0] = (uint8_t)((++y->tx_frame << 4 & 0xf0) | (y->tx_frames & 0x0f));
        memcpy(out + 1, y->tx, 19);
        y->tx_off = 19;
        *len = 20;
        ylog(y, "key exchange: offer read");
        return YPSO_OK;
    }
    const char *ver = version_string(y, c);
    if (ver || c == YC_PAIRING_PASSKEY) {
        /* Clear text, unframed: e.g. "V05.02.03\0". */
        size_t k = ver ? strlen(ver) : 0;
        if (k + 1 > cap) {
            return YPSO_VALUE_INVALID;
        }
        memcpy(out, ver ? ver : "", k);
        out[k] = 0;
        *len = ver && c == YC_MASTER_VERSION ? k + 1 : k;
        return YPSO_OK;
    }
    if (!y->authorized && !y->open) {
        return YPSO_ATT_INSUFFICIENT_AUTHORIZATION;
    }
    uint8_t pt[48];
    size_t n = 0;
    int rc = answer(y, c, pt, &n);
    if (rc != YPSO_OK) {
        return rc;
    }
    if (!f->encrypted) {
        if (n > cap) {
            return YPSO_VALUE_INVALID;
        }
        memcpy(out, pt, n);
        *len = n;
        return YPSO_OK;
    }
    /* §3.3: sealed with the pump's next message counter, then framed; frame 1
     * here, the rest through ExtendedRead. A new read drops any left over. */
    uint8_t nonce[24];
    dash_random(nonce, sizeof nonce);
    y->st.read_counter++;
    y->dirty = true;
    y->tx_len = ypso_seal(y->st.key, pt, n, y->st.reboot, y->st.read_counter, nonce, y->tx);
    y->tx_frames = (uint8_t)((y->tx_len + 18) / 19);
    y->tx_frame = 0;
    y->tx_off = 0;
    if (cap < YPSO_PKT) {
        return YPSO_VALUE_INVALID;
    }
    size_t k = y->tx_len < 19 ? y->tx_len : 19;
    out[0] = (uint8_t)((++y->tx_frame << 4 & 0xf0) | (y->tx_frames & 0x0f));
    memcpy(out + 1, y->tx, k);
    y->tx_off = k;
    *len = k + 1;
    return YPSO_OK;
}

int ypso_on_write(ypso_t *y, ypso_chr_t c, const uint8_t *p, size_t n)
{
    const ypso_fw_info_t *f = ypso_fw(y->st.fw);
    if (c >= YC_COUNT || !ypso_has_chr(y->st.fw, c) || !CHR[c].write) {
        return YPSO_VALUE_INVALID;
    }
    if (c == YC_AUTH_PASSWORD) {
        /* Always clear text, never framed. A wrong one is refused at the next
         * protected operation, with ATT 0x08. */
        bool right = n == 16 && memcmp(p, y->st.password, 16) == 0;
        y->authorized = right || y->open;
        ylog(y, "authorisation password %s", right ? "accepted" : y->open ? "not this pump's, let in (open)" : "wrong");
        return YPSO_OK;
    }
    if (!y->authorized && !y->open) {
        return YPSO_ATT_INSUFFICIENT_AUTHORIZATION;
    }
    if (!f->encrypted) {
        int rc = command(y, c, p, n);
        if (y->dirty) {
            save(y);
        }
        return rc;
    }
    /* §3.1: reassemble the frames; each is one GATT write. The key
     * exchange's blob is framed the same way, but in clear. */
    if (n < 1 || n > YPSO_PKT) {
        return YPSO_VALUE_INVALID;
    }
    uint8_t this_frame = p[0] >> 4, frames = p[0] & 0x0f;
    if (frames == 0) {
        frames = 1;
    }
    if (this_frame == 1 || c != y->rx_chr) {
        y->rx_chr = c;
        y->rx_len = 0;
        y->rx_next = 1;
    }
    if (this_frame != y->rx_next || y->rx_len + n - 1 > sizeof y->rx) {
        y->rx_len = 0;
        y->rx_next = 1;
        return YPSO_VALUE_CORRUPT;
    }
    memcpy(y->rx + y->rx_len, p + 1, n - 1);
    y->rx_len += n - 1;
    y->rx_next++;
    if (this_frame < frames) {
        return YPSO_OK;
    }
    if (c == YC_WRITE_CHALLENGE) {
        int rc = key_exchange(y, y->rx, y->rx_len);
        y->rx_len = 0;
        y->rx_next = 1;
        return rc;
    }
    /* Whole: open, check the counters (§7, forward-tolerant), run. */
    uint8_t pt[YPSO_MSG_MAX];
    uint32_t reboot;
    uint64_t counter;
    int len = ypso_open(y->st.key, y->rx, y->rx_len, pt, &reboot, &counter);
    y->rx_len = 0;
    y->rx_next = 1;
    if (len < 0) {
        ylog(y, "a write that does not decrypt: wrong key");
        return YPSO_ENCRYPTION_FAILED;
    }
    if (reboot != y->st.reboot || counter <= y->st.write_counter) {
        ylog(y, "write counter %llu not ahead of %llu (reboot %u/%u)", (unsigned long long)counter,
             (unsigned long long)y->st.write_counter, (unsigned)reboot, (unsigned)y->st.reboot);
        return YPSO_COUNTER_ERROR;
    }
    y->st.write_counter = counter;
    y->dirty = true;
    int rc = command(y, c, pt, (size_t)len);
    save(y);
    return rc;
}

/* ---- the panel ---------------------------------------------------------------------------- */

void ypso_set_reservoir(ypso_t *y, int32_t hundredths)
{
    y->st.reservoir = hundredths < -1 ? -1 : hundredths > 30000 ? 30000 : hundredths;
    save(y);
}

void ypso_set_battery(ypso_t *y, uint8_t level)
{
    y->st.battery = level > 6 ? 6 : level;
    save(y);
}

void ypso_set_open(ypso_t *y, bool open)
{
    y->open = open;
}

void ypso_set_suspended(ypso_t *y, bool on)
{
    if (y->st.suspended == on) {
        return;
    }
    y->st.suspended = on;
    if (on) {
        if (y->st.imm.status == 1) {
            end_immediate(y, true);
        }
        if (y->st.slow.seq) {
            end_slow(y, true);
        }
        if (y->st.tbr_percent != 100) {
            end_tbr(y, true);
        }
    }
    event(y, EV_PUMP_MODE_CHANGED, on ? 3 : 10, 0, 0);
    save(y);
}

void ypso_reboot(ypso_t *y)
{
    y->st.reboot++;
    y->authorized = false;
    save(y);
    ylog(y, "pump restarted: reboot counter %u", (unsigned)y->st.reboot);
}

void ypso_name(const ypso_state_t *st, char out[24])
{
    snprintf(out, 24, "YpsoPump_%08u", (unsigned)st->serial);
}

size_t ypso_status_json(ypso_t *y, char *out, size_t cap)
{
    const ypso_state_t *st = &y->st;
    char name[24];
    ypso_name(st, name);
    int64_t now = y->io->now(y->io->ctx);
    int tbr_left = st->tbr_percent != 100 ? st->tbr_minutes - (int)((now - st->tbr_start) / 60) : 0;
    int n = snprintf(out, cap,
                     "{\"fw\":%u,\"firmware\":\"%s\",\"name\":\"%s\",\"serial\":%u,\"connected\":%s,\"authorized\":%s,\"open\":%s,"
                     "\"reservoir\":%ld,\"battery\":%u,\"suspended\":%s,\"basalNow\":%ld,\"tbrPercent\":%ld,"
                     "\"tbrLeft\":%d,\"bolusActive\":%s,\"bolusDelivered\":%ld,\"bolusTotal\":%ld,"
                     "\"extendedActive\":%s,\"extendedDelivered\":%ld,\"extendedTotal\":%ld,\"reboot\":%u,"
                     "\"readCounter\":%llu,\"writeCounter\":%llu,\"events\":%u,\"pumpTime\":%lld}",
                     st->fw, ypso_fw(st->fw)->master, name, (unsigned)st->serial, y->connected ? "true" : "false",
                     y->authorized ? "true" : "false", y->open ? "true" : "false", (long)st->reservoir, st->battery,
                     st->suspended ? "true" : "false", (long)basal_now(y), (long)st->tbr_percent,
                     tbr_left < 0 ? 0 : tbr_left, st->imm.status == 1 ? "true" : "false", (long)st->imm.delivered,
                     (long)st->imm.total, st->slow.seq ? "true" : "false",
                     (long)(st->slow.delivered + st->slow.now_delivered), (long)(st->slow.total + st->slow.now_total),
                     (unsigned)st->reboot, (unsigned long long)st->read_counter,
                     (unsigned long long)st->write_counter, st->n_events, (long long)pump_now(y));
    return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}
