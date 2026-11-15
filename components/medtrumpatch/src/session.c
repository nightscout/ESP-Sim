/*
 * The connect session: AUTH_REQ, SYNCHRONIZE and SUBSCRIBE, as
 * MedtrumKit's PeripheralManager drives them, plus the READ-characteristic
 * status notification.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <string.h>

#include "medtrum.h"

void medtrum_patch_init(medtrum_patch_t *p, const uint8_t sn[4])
{
    memset(p, 0, sizeof *p);
    if (sn != NULL) {
        memcpy(p->sn, sn, 4);
    }
    p->status.state = MEDTRUM_STATE_NONE;
    p->delivery.last_ms = INT64_MIN;
}

void medtrum_patch_set_state(medtrum_patch_t *p, uint8_t state)
{
    p->status.state = state;
}

size_t medtrum_patch_notification(const medtrum_patch_t *p, uint8_t *out, size_t cap)
{
    return medtrum_status_payload(&p->status, out, cap);
}

/* The authorization key is genKey of the serial reversed, as AuthorizePacket
 * does before it builds the request. */
static bool key_matches(const medtrum_patch_t *p, const uint8_t key[4])
{
    uint8_t reversed[4] = {p->sn[3], p->sn[2], p->sn[1], p->sn[0]};
    uint8_t want[4];
    medtrum_gen_key(reversed, want);
    return memcmp(key, want, 4) == 0;
}

int medtrum_patch_handle(medtrum_patch_t *p, uint8_t command, uint8_t sequence, const uint8_t *content,
                         size_t content_len, medtrum_frag_t *frags, size_t max_frags)
{
    switch (command) {
    case MEDTRUM_CMD_AUTH_REQ: {
        /* role(1) + sessionToken(4) + key(4) */
        if (content == NULL || content_len < 9) {
            return -1;
        }
        const uint8_t *token = content + 1;
        const uint8_t *key = content + 5;
        if (!key_matches(p, key)) {
            p->authenticated = false;
            return medtrum_encode_response(command, sequence, MEDTRUM_RESPONSE_BAD_TOKEN, NULL, 0, frags,
                                           max_frags);
        }
        memcpy(p->session_token, token, 4);
        p->authenticated = true;
        /* The first payload byte is the patch state, as every response's is. */
        uint8_t payload[5] = {p->status.state, p->device_type, p->sw_version[0], p->sw_version[1],
                              p->sw_version[2]};
        return medtrum_encode_response(command, sequence, MEDTRUM_RESPONSE_OK, payload, sizeof payload, frags,
                                       max_frags);
    }
    case MEDTRUM_CMD_SYNCHRONIZE: {
        if (!p->authenticated) {
            return medtrum_encode_response(command, sequence, MEDTRUM_RESPONSE_BAD_TOKEN, NULL, 0, frags,
                                           max_frags);
        }
        uint8_t payload[MEDTRUM_STATUS_MAX + 1];
        size_t n = medtrum_status_payload(&p->status, payload, MEDTRUM_STATUS_MAX);
        if (n == 0) {
            return -1;
        }
        payload[n++] = p->sync_tail;
        return medtrum_encode_response(command, sequence, MEDTRUM_RESPONSE_OK, payload, n, frags, max_frags);
    }
    case MEDTRUM_CMD_SUBSCRIBE: {
        if (!p->authenticated) {
            return medtrum_encode_response(command, sequence, MEDTRUM_RESPONSE_BAD_TOKEN, NULL, 0, frags,
                                           max_frags);
        }
        if (content != NULL && content_len >= 2) {
            p->subscribe_mask = (uint16_t)(content[0] | (content[1] << 8));
        }
        p->subscribed = true;
        uint8_t payload[1] = {p->status.state};
        return medtrum_encode_response(command, sequence, MEDTRUM_RESPONSE_OK, payload, 1, frags, max_frags);
    }
    default:
        return medtrum_patch_command(p, command, sequence, content, content_len, frags, max_frags);
    }
}
