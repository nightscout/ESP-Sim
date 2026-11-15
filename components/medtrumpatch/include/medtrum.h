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
    MEDTRUM_CMD_GET_RECORD = 99,       /* declared, unused by the client */
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

#ifdef __cplusplus
}
#endif
