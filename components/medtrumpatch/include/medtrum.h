/*
 * ESP-Sim Medtrum patch: the wire framing and crypto of MedtrumKit.
 *
 * TEST EQUIPMENT ONLY. This pretends to be a Medtrum patch so that
 * controller apps can be developed and tested without a real one. It
 * delivers nothing, and nothing it reports reflects a real patch. Never use
 * it for therapy.
 *
 * Portable C: no ESP-IDF, no mbedTLS. The platform supplies a medtrum_io_t
 * (radio, clock, randomness). The framing mirrors MedtrumKit's
 * MedtrumBasePacket encode/decode and Crc8, and Crypto.simpleCrypt /
 * simpleDecrypt / randomGen / genKey.
 *
 * The CRC-8 lookup table and the Rijndael S-box / inverse S-box tables are
 * taken verbatim from MedtrumKit (https://github.com/nightscout/MedtrumKit),
 * MIT License, Copyright (c) 2024 J. Bunt. See LICENSE.medtrumkit.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The largest request/response body MedtrumKit can carry: the size byte is
 * body + 5, so a body of 250 already fills it. */
#define MEDTRUM_CONTENT_MAX 250
/* A reassembled packet: the 4-byte header plus the payload (data_size bytes). */
#define MEDTRUM_FRAME_MAX 256
/* One fragment on the wire: 4-byte header + up to 15 payload + CRC-8. */
#define MEDTRUM_FRAG_MAX 20
/* Enough fragments for the largest packet. */
#define MEDTRUM_FRAGS_MAX 32
#define MEDTRUM_WAIT_FOREVER UINT32_MAX

/* ---- command types (MedtrumKit/Packets/CommandType.swift) -------------- */

typedef enum {
    MEDTRUM_CMD_SYNCHRONIZE = 3,
    MEDTRUM_CMD_SUBSCRIBE = 4,
    MEDTRUM_CMD_AUTH_REQ = 5,
    MEDTRUM_CMD_GET_DEVICE_TYPE = 6, /* declared, unused by the client */
    MEDTRUM_CMD_SET_TIME = 10,
    MEDTRUM_CMD_GET_TIME = 11,
    MEDTRUM_CMD_SET_TIME_ZONE = 12,
    MEDTRUM_CMD_PRIME = 16,
    MEDTRUM_CMD_ACTIVATE = 18,
    MEDTRUM_CMD_SET_BOLUS = 19,
    MEDTRUM_CMD_CANCEL_BOLUS = 20,
    MEDTRUM_CMD_SET_BASAL_PROFILE = 21,
    MEDTRUM_CMD_SET_TEMP_BASAL = 24,
    MEDTRUM_CMD_CANCEL_TEMP_BASAL = 25,
    MEDTRUM_CMD_SUSPEND_PUMP = 28,
    MEDTRUM_CMD_RESUME_PUMP = 29,
    MEDTRUM_CMD_POLL_PATCH = 30,      /* declared, unused by the client */
    MEDTRUM_CMD_STOP_PATCH = 31,
    MEDTRUM_CMD_READ_BOLUS_STATE = 34, /* declared, unused by the client */
    MEDTRUM_CMD_SET_PATCH = 35,
    MEDTRUM_CMD_SET_BOLUS_MOTOR = 36,  /* declared, unused by the client */
    MEDTRUM_CMD_GET_RECORD = 99,       /* history: the TDD answer the simulator gives */
    MEDTRUM_CMD_CLEAR_ALARM = 115,
} medtrum_cmd_t;

/* The responseCode in a response's bytes 4-5 (little-endian). */
enum {
    MEDTRUM_RESPONSE_OK = 0x0000,
    /* The client drops a response with this code (PeripheralManager). */
    MEDTRUM_RESPONSE_SKIP = 0x4000,
    MEDTRUM_RESPONSE_BAD_TOKEN = 7,
    MEDTRUM_RESPONSE_BAD_STATE = 8,
};

/* ---- what the platform provides ---------------------------------------- */

typedef enum {
    MEDTRUM_EV_CONNECTED = 0,
    MEDTRUM_EV_DISCONNECTED,
    MEDTRUM_EV_WRITE, /* the phone wrote the command characteristic */
    MEDTRUM_EV_STOP,  /* host tests: make the run loop return */
} medtrum_ev_type_t;

typedef struct {
    uint8_t type; /* medtrum_ev_type_t */
    uint8_t len;
    uint8_t data[MEDTRUM_FRAME_MAX];
} medtrum_ev_t;

typedef struct {
    void *ctx;
    /* Waits up to timeout_ms (or MEDTRUM_WAIT_FOREVER) for the radio's next
     * event. Returns false on timeout. */
    bool (*next_event)(void *ctx, medtrum_ev_t *ev, uint32_t timeout_ms);
    /* Sends a notification or a command response to the connected phone. */
    void (*notify)(void *ctx, const uint8_t *p, size_t n);
    void (*disconnect)(void *ctx);
    /* Advertises (on) as a Medtrum patch with this serial, or stops. */
    void (*advertise)(void *ctx, bool on, const uint8_t sn[4]);
    int64_t (*now_ms)(void *ctx);
    void (*random)(void *ctx, uint8_t *p, size_t n);
    void (*log)(void *ctx, const char *msg);
} medtrum_io_t;

/* ---- CRC-8 (MedtrumKit/Encryption/Crc8.swift) -------------------------- */

uint8_t medtrum_crc8(const uint8_t *p, size_t n);
extern const uint8_t medtrum_crc8_table[256];

/* ---- crypto (MedtrumKit/Encryption/Crypto.swift) ----------------------- */

/* Park-Miller minimal-standard generator: x <- 16807 * x mod (2^31 - 1). */
uint32_t medtrum_random_gen(uint32_t x);
/* 32 rounds of byte substitution and rotation over a little-endian word. */
uint32_t medtrum_simple_crypt(uint32_t x);
uint32_t medtrum_simple_decrypt(uint32_t x);
/* genKey: the 4-byte key for a serial that is already in wire order (the
 * caller reverses the advertised serial first, as AuthorizePacket does). */
void medtrum_gen_key(const uint8_t pump_sn[4], uint8_t key[4]);
/* genSessionToken: four random bytes from the platform. */
void medtrum_gen_session_token(const medtrum_io_t *io, uint8_t out[4]);

extern const uint8_t medtrum_sbox[256];
extern const uint8_t medtrum_inv_sbox[256];

/* ---- packet framing (MedtrumKit/Packets/BasePacket.swift) -------------- */

typedef struct {
    uint8_t data[MEDTRUM_FRAG_MAX];
    size_t len;
} medtrum_frag_t;

typedef struct {
    uint8_t command;       /* the command this packet is being decoded as */
    uint8_t data_size;     /* the size byte: header + payload, excluding CRC */
    uint8_t sequence;      /* the echoed sequence number */
    uint16_t response_code; /* bytes 4-5 of a response, little-endian */
    uint8_t data[MEDTRUM_FRAME_MAX]; /* the reassembled packet, CRC stripped */
    size_t len;            /* bytes in data */
    bool started;          /* a first fragment has been seen */
    bool failed;           /* command mismatch, short fragment, or bad CRC */
} medtrum_packet_t;

/* Encodes a request. `content` is the request body (getRequestBytes). Writes
 * the fragments into `frags` and returns their count, or -1 if content is
 * too long or max_frags is too small. A single-packet request carries the
 * trailing 0x00 marker; a fragmented one does not. */
int medtrum_encode(uint8_t command, uint8_t sequence, const uint8_t *content, size_t content_len,
                   medtrum_frag_t *frags, size_t max_frags);

/* Starts decoding a response for `command`. */
void medtrum_decode_init(medtrum_packet_t *p, uint8_t command);
/* Feeds one fragment. The first must be at least 7 bytes; later ones 5. */
void medtrum_decode(medtrum_packet_t *p, const uint8_t *frag, size_t n);
/* Whether the reassembled packet has reached its declared size. */
bool medtrum_complete(const medtrum_packet_t *p);
/* Whether at least `minimum` bytes have been reassembled (MedtrumKit's
 * hasEnoughData). */
bool medtrum_has_enough(const medtrum_packet_t *p, size_t minimum);

/* Encodes a response: header(4) + responseCode(2) + payload, with the size
 * byte counting the header, responseCode and payload but not the CRC (unlike
 * a request, whose size byte includes the CRC). A single-packet response has
 * no trailing marker. Fragments are 15-byte chunks of (responseCode +
 * payload); the reassembled packet carries no inner CRC. Returns the number
 * of fragments, or -1. */
int medtrum_encode_response(uint8_t command, uint8_t sequence, uint16_t response_code,
                            const uint8_t *payload, size_t payload_len, medtrum_frag_t *frags,
                            size_t max_frags);

/* Reassembles a request from the phone's writes. A request is header(4) +
 * content + CRC-8, with a trailing 0x00 marker on a single packet; a
 * fragmented request ends with its own CRC instead of a marker. */
typedef struct {
    uint8_t command;
    uint8_t sequence;
    uint8_t content[MEDTRUM_CONTENT_MAX];
    size_t content_len;
    uint8_t buf[MEDTRUM_CONTENT_MAX + 1]; /* content + CRC while fragmenting */
    size_t len;
    bool active;
} medtrum_request_t;

void medtrum_request_init(medtrum_request_t *r);
/* Feeds one write. Returns true once a complete request is assembled (its
 * command, sequence, content and content_len are then valid), or false while
 * more fragments are needed or on a bad frame. An empty-content single packet
 * is the 6-byte size + cmd + seq + pkg + CRC + 0x00 form. */
bool medtrum_request_feed(medtrum_request_t *r, const uint8_t *frag, size_t n);

/* ---- patch state (MedtrumKit/Packets/Enums/PatchState.swift) ----------- */

typedef enum {
    MEDTRUM_STATE_NONE = 0,
    MEDTRUM_STATE_IDLE = 1,
    MEDTRUM_STATE_FILLED = 2,
    MEDTRUM_STATE_PRIMING = 3,
    MEDTRUM_STATE_PRIMED = 4,
    MEDTRUM_STATE_EJECTING = 5,
    MEDTRUM_STATE_EJECTED = 6,
    MEDTRUM_STATE_ACTIVE = 32,
    MEDTRUM_STATE_ACTIVE_ALT = 33,
    MEDTRUM_STATE_LOW_BG_SUSPENDED = 64,
    MEDTRUM_STATE_LOW_BG_SUSPENDED2 = 65,
    MEDTRUM_STATE_AUTO_SUSPENDED = 66,
    MEDTRUM_STATE_HOURLY_MAX_SUSPENDED = 67,
    MEDTRUM_STATE_DAILY_MAX_SUSPENDED = 68,
    MEDTRUM_STATE_SUSPENDED = 69,
    MEDTRUM_STATE_PAUSED = 70,
    MEDTRUM_STATE_OCCLUSION = 96,
    MEDTRUM_STATE_EXPIRED = 97,
    MEDTRUM_STATE_RESERVOIR_EMPTY = 98,
    MEDTRUM_STATE_PATCH_FAULT = 99,
    MEDTRUM_STATE_PATCH_FAULT2 = 100,
    MEDTRUM_STATE_BASE_FAULT = 101,
    MEDTRUM_STATE_BATTERY_OUT = 102,
    MEDTRUM_STATE_NO_CALIBRATION = 103,
    MEDTRUM_STATE_STOPPED = 128,
} medtrum_state_t;

/* The classifications PatchState computes (phase, isSuspended, isFault, ...). */
bool medtrum_state_is_setup(uint8_t state);
bool medtrum_state_is_running(uint8_t state);
bool medtrum_state_is_terminated(uint8_t state);
bool medtrum_state_is_suspended(uint8_t state);
bool medtrum_state_is_fault(uint8_t state);
bool medtrum_state_is_delivery_halted(uint8_t state);
bool medtrum_state_before_priming(uint8_t state);
bool medtrum_state_completed_priming(uint8_t state);

/* ---- status fields (MedtrumKit/Packets/NotificationPacket.swift) ------- */

/* The field mask: a field is present in the payload iff its bit is set. The
 * payload order is fixed by MedtrumKit's `fields`, not by the bit order. */
enum {
    MEDTRUM_MASK_SUSPEND = 0x0001,
    MEDTRUM_MASK_NORMAL_BOLUS = 0x0002,
    MEDTRUM_MASK_EXTENDED_BOLUS = 0x0004,
    MEDTRUM_MASK_BASAL = 0x0008,
    MEDTRUM_MASK_SETUP = 0x0010,
    MEDTRUM_MASK_RESERVOIR = 0x0020,
    MEDTRUM_MASK_START_TIME = 0x0040,
    MEDTRUM_MASK_BATTERY = 0x0080,
    MEDTRUM_MASK_STORAGE = 0x0100,
    MEDTRUM_MASK_ALARM = 0x0200,
    MEDTRUM_MASK_AGE = 0x0400,
    MEDTRUM_MASK_MAGNETO_PLACE = 0x0800,
    MEDTRUM_MASK_UNUSED_CGM = 0x1000,
    MEDTRUM_MASK_UNUSED_COMMAND_CONFIRM = 0x2000,
    MEDTRUM_MASK_UNUSED_AUTO_STATUS = 0x4000,
    MEDTRUM_MASK_UNUSED_LEGACY = 0x8000,
};

/* The largest status payload: state + mask + every field. */
#define MEDTRUM_STATUS_MAX 64

/* A patch's reported status, in the wire representation: times are Medtrum
 * seconds, insulin values are pulses of 0.05 U. Only the fields whose mask
 * bit is set are emitted, in MedtrumKit's fixed order. */
typedef struct {
    uint8_t state;  /* medtrum_state_t */
    uint16_t mask;  /* MEDTRUM_MASK_* bits */

    uint32_t suspend_time;      /* MASK_SUSPEND */
    uint8_t bolus_type;         /* MASK_NORMAL_BOLUS: low 7 bits */
    bool bolus_completed;       /* MASK_NORMAL_BOLUS: bit 7 */
    uint16_t bolus_delivered;   /* pulses */
    uint8_t extended_bolus[3];  /* MASK_EXTENDED_BOLUS, raw */
    uint8_t basal_type;         /* MASK_BASAL */
    uint16_t basal_sequence;
    uint16_t basal_patch_id;
    uint32_t basal_start_time;  /* Medtrum seconds */
    uint16_t basal_rate;        /* pulses of 0.05 U/h, low 12 bits */
    uint16_t basal_delivery;    /* pulses, high bits */
    uint8_t prime_progress;     /* MASK_SETUP */
    uint16_t reservoir;         /* pulses of 0.05 U */
    uint32_t start_time;        /* MASK_START_TIME, Medtrum seconds */
    uint32_t battery;           /* MASK_BATTERY, raw 24 bits */
    uint16_t storage_sequence;  /* MASK_STORAGE */
    uint16_t storage_patch_id;
    uint16_t alarm_flags;       /* MASK_ALARM */
    uint8_t alarm_unused[2];
    uint32_t patch_age;         /* MASK_AGE, seconds */
    uint16_t magneto_placement; /* MASK_MAGNETO_PLACE */
    uint8_t unused_cgm[5];      /* MASK_UNUSED_CGM */
    uint8_t unused_command_confirm[2];
    uint8_t unused_auto_status[2];
    uint8_t unused_legacy[2];
} medtrum_status_t;

/* Builds the notification payload: state, mask (little-endian), then each
 * masked field in MedtrumKit's order. Returns the length, or 0 if out is too
 * small. This is both the READ-characteristic notification and the body of a
 * SYNCHRONIZE response. */
size_t medtrum_status_payload(const medtrum_status_t *st, uint8_t *out, size_t cap);

/* ---- delivery ---------------------------------------------------------- */

/* Alarm flags: the low three bits of the MASK_ALARM field. */
enum {
    MEDTRUM_ALARM_LOW_BATTERY = 0x0001,
    MEDTRUM_ALARM_LOW_RESERVOIR = 0x0002,
    MEDTRUM_ALARM_EXPIRES_SOON = 0x0004,
};

/* Clear-alarm alert types (MedtrumKit's AlertType). */
enum {
    MEDTRUM_ALERT_HOURLY = 4,
    MEDTRUM_ALERT_DAILY = 5,
};

/* Basal types the client sends and parses. */
enum {
    MEDTRUM_BASAL_STANDARD = 1,
    MEDTRUM_BASAL_ABSOLUTE_TEMP = 6,
};

/* The largest basal profile MedtrumKit sends: count + entries of 3 bytes. */
#define MEDTRUM_BASAL_PROFILE_MAX 150

/* The delivery model. The commands project it into `status`, and
 * medtrum_patch_tick keeps the clock, the bolus progress, the temp basal and
 * the patch age moving. Delivery consumes the reservoir in `status`: each tick
 * subtracts the pulses it newly delivered (bolus and basal), and an empty
 * reservoir stops the bolus short of its total. */
typedef struct {
    uint8_t patch_id[4];      /* the active patch's id */
    uint32_t patch_time;      /* the patch's clock, Medtrum seconds */
    uint32_t patch_start;     /* activation time, Medtrum seconds */
    int32_t timezone_minutes; /* from SET_TIME_ZONE */
    int64_t last_ms;          /* wall clock of the last tick */
    uint16_t scheduled_rate;  /* scheduled basal, pulses of 0.05 U/h */
    uint16_t temp_rate;       /* active temp basal, pulses */
    uint32_t temp_end;        /* Medtrum seconds; 0 = no temp basal */
    uint16_t basal_sequence;
    uint16_t bolus_total;     /* pulses */
    uint32_t bolus_start;     /* Medtrum seconds */
    bool bolus_active;
    bool was_halted;          /* the previous tick saw a halted state */
    uint16_t storage_sequence;
    uint16_t max_hourly;      /* pulses */
    uint16_t max_daily;       /* pulses */
    uint8_t alarm_setting;
    uint8_t expiry_timer;
    uint16_t alarm_flags;
    uint8_t basal_profile[MEDTRUM_BASAL_PROFILE_MAX];
    size_t basal_profile_len;
} medtrum_delivery_t;

/* ---- the patch and its connect session --------------------------------- */

typedef struct {
    uint8_t sn[4];          /* the logical serial, as the app knows it */
    uint8_t device_type;
    uint8_t sw_version[3];
    uint8_t session_token[4]; /* the last token the app authenticated with */
    bool authenticated;
    bool subscribed;
    uint16_t subscribe_mask;
    /* The captured SYNCHRONIZE responses carry one extra trailing byte that
     * the client ignores; a real patch always seems to send it. */
    uint8_t sync_tail;
    medtrum_status_t status;
    medtrum_delivery_t delivery;
} medtrum_patch_t;

void medtrum_patch_init(medtrum_patch_t *p, const uint8_t sn[4]);
void medtrum_patch_set_state(medtrum_patch_t *p, uint8_t state);

/* Builds the READ-characteristic notification for the current status. */
size_t medtrum_patch_notification(const medtrum_patch_t *p, uint8_t *out, size_t cap);

/* Handles one session request. `content` is the request body (a decoded
 * request's bytes after the 4-byte header, with the trailing marker and CRC
 * already stripped). Writes the response fragments into `frags` and returns
 * their count, 0 for no response, or -1 if the command is not part of the
 * session. */
int medtrum_patch_handle(medtrum_patch_t *p, uint8_t command, uint8_t sequence, const uint8_t *content,
                         size_t content_len, medtrum_frag_t *frags, size_t max_frags);

/* Handles a delivery command (everything outside the connect session).
 * Returns the fragment count, or -1 if the command is not implemented. */
int medtrum_patch_command(medtrum_patch_t *p, uint8_t command, uint8_t sequence, const uint8_t *content,
                          size_t content_len, medtrum_frag_t *frags, size_t max_frags);

/* Advances the patch clock, the bolus progress, the temp basal and the patch
 * age. `now_ms` is the platform's wall clock; the first call only records it.
 * Delivery drains `status.reservoir` in pulses of 0.05 U, charging only the
 * pulses each tick newly delivers (never the running total): a running bolus at
 * 1.5 U/min, and the basal at the temp rate or the schedule. Both clamp at
 * zero. An empty reservoir stops a running bolus without setting
 * `bolus_completed` or changing `state` (this simulator changes numbers, not
 * the base's frames), so the app sees a bolus that neither progresses nor
 * completes. A halted patch (suspended or stopped) draws no basal, and the
 * first tick after it resumes starts a fresh basal segment rather than charging
 * the whole halt. */
void medtrum_patch_tick(medtrum_patch_t *p, int64_t now_ms);

/* Direct setters, for the control panel and for tests. */
void medtrum_patch_set_reservoir(medtrum_patch_t *p, uint16_t pulses);
void medtrum_patch_set_battery(medtrum_patch_t *p, uint32_t raw);
void medtrum_patch_set_patch_id(medtrum_patch_t *p, const uint8_t id[4]);
void medtrum_patch_set_alarm(medtrum_patch_t *p, uint16_t flags);
void medtrum_patch_set_fault(medtrum_patch_t *p, uint8_t state);

#ifdef __cplusplus
}
#endif
