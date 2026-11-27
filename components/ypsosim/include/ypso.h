/*
 * A simulated YpsoPump, as the rockit YpsoPump driver (rockit-driver-ypso,
 * docs/COMMAND-SPEC.md) talks to one: its GATT services, framing,
 * XChaCha20-Poly1305 session, counters, settings, history, bolus and temp
 * basal, in each of the firmware versions the driver handles.
 *
 * TEST EQUIPMENT ONLY: it delivers nothing; its reservoir and history are made
 * up. Its session key and password are its own, made at random when it
 * becomes a new pump: no real pump's credentials are used or needed.
 *
 * Portable C: the platform supplies a ypso_io_t and the crypto in dash.h.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- firmware versions ---------------------------------------------------- */

typedef enum {
    YPSO_FW_V05 = 0, /* V05.02.03: the hardware-validated pump. Encrypted, Control 1.4 */
    YPSO_FW_V35,     /* V35.01.00: study firmware. Encrypted, with the _alt characteristics */
    YPSO_FW_V65,     /* V65.00.06: OPN. Clear text, Control 1.0 */
    YPSO_FW_V03,     /* V03.00.00: OPN. Clear text, Control 1.1 */
    YPSO_FW_V02,     /* V02.00.00: the oldest baseline. Clear text, no Control service */
    YPSO_FW_COUNT,
} ypso_fw_t;

typedef struct {
    const char *label;    /* for the panel */
    const char *pump_base, *master, *supervisor, *settings, *history, *control, *crypto;
    bool has_control;     /* the Control service is there */
    bool encrypted;       /* the Crypto service is there: everything else is sealed */
    bool alt;             /* the V35 _alt characteristics are there too */
    bool extended;        /* verControl > 1.1: 15-byte bolus, 44-byte bolus status, extended bolus */
    bool index_safevar;   /* verHistory >= 1.2: entry indexes are SAFEVARs, else bare u16 */
    uint8_t status_len;   /* GetSystemStatus: 20 (with CRC) or 18 */
    uint8_t tbr_step;     /* percent: 1 with OPN_CAPS (PumpBase 1.1-1.2), else 10 */
} ypso_fw_info_t;

const ypso_fw_info_t *ypso_fw(ypso_fw_t fw);

/* ---- GATT ------------------------------------------------------------------ */

typedef enum {
    YS_BASE = 0,
    YS_SETTINGS,
    YS_HISTORY,
    YS_CONTROL,
    YS_CRYPTO,
    YS_COUNT,
} ypso_svc_t;

typedef enum {
    /* PumpBaseService */
    YC_BASE_VERSION = 0,
    YC_MASTER_VERSION,
    YC_SUPERVISOR_VERSION,
    YC_AUTH_PASSWORD,
    YC_PAIRING_PASSKEY,
    /* SettingsService */
    YC_SETTINGS_VERSION,
    YC_SETTING_ID,
    YC_SETTING_VALUE,
    YC_SETTING_ID_ALT,
    YC_SETTING_VALUE_ALT,
    YC_SYSTEM_DATE,
    YC_SYSTEM_TIME,
    /* HistoryService */
    YC_HISTORY_VERSION,
    YC_ALARM_COUNT,
    YC_ALARM_INDEX,
    YC_ALARM_VALUE,
    YC_EVENT_COUNT,
    YC_EVENT_INDEX,
    YC_EVENT_VALUE,
    YC_SYSTEM_COUNT,
    YC_SYSTEM_INDEX,
    YC_SYSTEM_VALUE,
    YC_COMPLAINT_COUNT,
    YC_COMPLAINT_INDEX,
    YC_COMPLAINT_VALUE,
    YC_ALARM_COUNT_ALT,
    YC_ALARM_INDEX_ALT,
    YC_ALARM_VALUE_ALT,
    YC_EVENT_COUNT_ALT,
    YC_EVENT_INDEX_ALT,
    YC_EVENT_VALUE_ALT,
    YC_SYSTEM_COUNT_ALT,
    YC_SYSTEM_INDEX_ALT,
    YC_SYSTEM_VALUE_ALT,
    YC_COUNTER_ID,
    YC_COUNTER_VALUE,
    /* ControlService */
    YC_CONTROL_VERSION,
    YC_START_STOP_BOLUS,
    YC_GET_BOLUS_STATUS,
    YC_START_STOP_TBR,
    YC_GET_SYSTEM_STATUS,
    /* CryptoService */
    YC_CRYPTO_VERSION,
    YC_NEW_PUBLIC_KEY,
    YC_WRITE_CHALLENGE,
    YC_EXTENDED_READ,
    YC_COUNT,
} ypso_chr_t;

typedef struct {
    ypso_svc_t svc;
    const char *uuid; /* "669a0c20-0008-…", as text */
    bool read, write;
    bool clear;       /* never sealed: version strings, the password, ExtendedRead's own frames */
} ypso_chr_info_t;

const ypso_chr_info_t *ypso_chr(ypso_chr_t c);
const char *ypso_svc_uuid(ypso_svc_t s);
/* Whether this firmware has the service / characteristic. */
bool ypso_has_svc(ypso_fw_t fw, ypso_svc_t s);
bool ypso_has_chr(ypso_fw_t fw, ypso_chr_t c);
/* A UUID's 16 bytes, least significant first (as NimBLE wants). */
void ypso_uuid_le(const char *text, uint8_t out[16]);

/* ATT status codes the pump answers with (COMMAND-SPEC §3.6). */
enum {
    YPSO_OK = 0x00,
    YPSO_ATT_INSUFFICIENT_AUTHORIZATION = 0x08,
    YPSO_VALUE_CORRUPT = 0x81,
    YPSO_VALUE_INVALID = 0x82,
    YPSO_INVALID_ID = 0x83,
    YPSO_ENTRY_INDEX_INVALID = 0x84,
    YPSO_INTERNAL_ERROR = 0x86,
    YPSO_COMMAND_FAILED = 0x87,
    YPSO_ENCRYPTION_FAILED = 0x8a,
    YPSO_COUNTER_ERROR = 0x8b,
};

/* ---- state ------------------------------------------------------------------- */

#define YPSO_STATE_VERSION 1
#define YPSO_EVENTS 160
#define YPSO_ALARMS 32
#define YPSO_PKT 20       /* a frame: one header byte, up to 19 payload */
#define YPSO_MSG_MAX 160  /* the largest sealed message: 44 + 52 rounded up */

typedef struct {
    uint32_t t;      /* seconds since 2000-01-01, pump time */
    uint32_t seq;    /* log sequence number */
    uint16_t v[3];
    uint8_t type;
} ypso_event_t;

typedef struct {
    uint8_t version;
    uint8_t fw;                /* ypso_fw_t */
    uint32_t serial;           /* 8 digits */
    uint8_t key[32];           /* the session key the pump shares with the app */
    uint8_t password[16];      /* the authorisation password */
    uint32_t reboot;           /* the pump's reboot counter */
    uint64_t read_counter;     /* the pump's message counter, on its responses */
    uint64_t write_counter;    /* the last counter accepted on an app's write */
    int64_t clock_offset;      /* pump clock minus the board's local time, seconds */
    /* Delivery. */
    int32_t reservoir;         /* hundredths of a unit; -1: no cartridge */
    uint8_t battery;           /* 0-5 (×20 %), 6 invalid */
    bool suspended;
    uint8_t profile;           /* setting 1: 3 = profile A */
    int32_t basal_a[24], basal_b[24]; /* hundredths U/h */
    int32_t tbr_percent;       /* 100: none */
    int32_t tbr_minutes;
    int64_t tbr_start;         /* Unix seconds */
    uint32_t bolus_seq;        /* the last bolus's event sequence */
    struct {
        uint8_t status;        /* 0 none, 1 active, 4 finished */
        uint32_t seq;
        int32_t total, delivered;
    } imm;
    struct {
        uint8_t status;
        uint32_t seq;          /* 0: none running */
        int32_t total, delivered;     /* the extended part */
        int32_t now_total, now_delivered; /* a combination bolus's immediate part */
        int32_t minutes;
        int64_t start;
    } slow;
    int32_t milli_hundredths;  /* basal not yet taken from the reservoir, thousandths of a hundredth */
    uint32_t event_seq;
    uint32_t delivered_total;  /* hundredths of a unit, ever: what a body would have received */
    uint16_t n_events;
    ypso_event_t events[YPSO_EVENTS]; /* oldest first */
    /* The key exchange (encrypted firmware). Added later: a state saved before
     * has these zero, and ypso_init fills them in. */
    uint8_t server_priv[32];   /* the server key this pump trusts, made at random on the board */
    uint64_t kx_counter;       /* the last key exchange's counter: the next must be greater */
    uint32_t tbr_seq;          /* the running temp basal's history entry (its _RUNNING record) */
    /* The alarm log. Added later: a state saved before has none. */
    uint32_t alarm_seq;
    uint16_t n_alarms;
    ypso_event_t alarms[YPSO_ALARMS]; /* oldest first */
} ypso_state_t;

typedef struct {
    void *ctx;
    int64_t (*now)(void *ctx);          /* Unix seconds */
    int32_t (*utc_offset)(void *ctx);   /* the board's local zone, seconds east of UTC */
    void (*save)(void *ctx, const ypso_state_t *st);
    void (*log)(void *ctx, const char *msg);
} ypso_io_t;

typedef struct {
    ypso_state_t st;
    const ypso_io_t *io;
    /* The connection. */
    bool connected;
    bool authorized;
    bool open;                   /* let any phone in: no password needed (not like a real pump) */
    int32_t setting_id;          /* selected by SettingID */
    int32_t counter_id;          /* selected by CounterID */
    int32_t index[4];            /* each log's entry index: event, alarm, system, complaint */
    /* An encrypted write being reassembled, per characteristic. */
    ypso_chr_t rx_chr;
    uint8_t rx[YPSO_MSG_MAX];
    size_t rx_len;
    uint8_t rx_next;             /* the frame number expected next */
    /* An encrypted read's frames still to come, through ExtendedRead. */
    uint8_t tx[YPSO_MSG_MAX + 16];
    size_t tx_len, tx_off;
    uint8_t tx_frame, tx_frames;
    /* The key exchange: the challenge in the last NewPublicKey read. */
    uint8_t challenge[32];
    bool challenge_set;
    int64_t last_tick, last_save;
    bool dirty;
    bool ran_dry;                /* the reservoir reached 0 this tick */
} ypso_t;

/* A new pump: fresh serial, key and password; full reservoir; a flat 0.80 U/h profile. */
void ypso_state_fresh(ypso_state_t *st, ypso_fw_t fw);
void ypso_init(ypso_t *y, const ypso_state_t *st, const ypso_io_t *io);
/* About once a second: basal, boluses and temp basals run on. */
void ypso_tick(ypso_t *y);

void ypso_on_connect(ypso_t *y);
void ypso_on_disconnect(ypso_t *y);
/* A read: the value (frame 1 when sealed) into out. Returns an ATT status. */
int ypso_on_read(ypso_t *y, ypso_chr_t c, uint8_t *out, size_t cap, size_t *len);
/* A write (one frame when sealed). Returns an ATT status. */
int ypso_on_write(ypso_t *y, ypso_chr_t c, const uint8_t *p, size_t n);

/* The control panel. */
void ypso_set_reservoir(ypso_t *y, int32_t hundredths);
void ypso_set_battery(ypso_t *y, uint8_t level);
void ypso_set_suspended(ypso_t *y, bool on);
/* A cartridge change, done on the pump while it is stopped, in hundredths of a
 * unit: the new cartridge (a rewind), then filling the tubing (a line prime)
 * and the cannula. Each is a history entry the app reads, with its amount;
 * the primes come out of the reservoir. NULL, or why the pump refuses. */
const char *ypso_change_cartridge(ypso_t *y, int32_t hundredths);
const char *ypso_prime_line(ypso_t *y, int32_t hundredths);
const char *ypso_fill_cannula(ypso_t *y, int32_t hundredths);
/* Open: the pump lets the phone in without its password, or with any
 * password, so an app with no credentials for it works at once. Closed: like
 * a real pump, only its own password. */
void ypso_set_open(ypso_t *y, bool open);
/* Set the pump's clock to the board's local time, as a person would on the
 * pump: a date and a time change in its history (stamped on the clock as it
 * was), only if it is a minute or more out. True if it moved. */
bool ypso_clock_to_local(ypso_t *y);
void ypso_reboot(ypso_t *y);   /* the pump restarts: its reboot counter goes up */
size_t ypso_status_json(ypso_t *y, char *out, size_t cap);
/* The advertised name: "YpsoPump_<serial>". */
void ypso_name(const ypso_state_t *st, char out[24]);
/* The pump's X25519 public key (the second half of its NewPublicKey offer). */
void ypso_pump_public(const ypso_state_t *st, uint8_t out[32]);

/* ---- exposed for tests -------------------------------------------------------- */

uint16_t ypso_crc16(const uint8_t *p, size_t n);  /* genCRC16 */
/* XChaCha20-Poly1305 with the pump's layout: ciphertext ‖ tag ‖ nonce(24), over
 * payload ‖ reboot(4 LE) ‖ counter(8 LE). */
size_t ypso_seal(const uint8_t key[32], const uint8_t *p, size_t n, uint32_t reboot, uint64_t counter,
                 const uint8_t nonce[24], uint8_t *out);
/* Returns the payload length, or -1; the trailer's counters come back through reboot/counter. */
int ypso_open(const uint8_t key[32], const uint8_t *in, size_t n, uint8_t *out, uint32_t *reboot, uint64_t *counter);
