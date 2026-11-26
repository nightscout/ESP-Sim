/*
 * The Medtrum patch's packet framing: MedtrumKit's MedtrumBasePacket encode
 * and decode, including fragmentation and the trailing CRC-8.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <string.h>

#include "medtrum.h"

/* The single-packet threshold: a header plus (content + CRC) of at most 15
 * bytes stays one packet. */
#define SINGLE_PACKET_MAX 15

int medtrum_encode(uint8_t command, uint8_t sequence, const uint8_t *content, size_t content_len,
                   medtrum_frag_t *frags, size_t max_frags)
{
    if (content_len > MEDTRUM_CONTENT_MAX || frags == NULL) {
        return -1;
    }

    /* header(4) + content + CRC-8 */
    uint8_t total[MEDTRUM_CONTENT_MAX + 5];
    total[0] = (uint8_t)(content_len + 5);
    total[1] = command;
    total[2] = sequence;
    total[3] = 0; /* pkgIndex */
    if (content_len > 0) {
        memcpy(total + 4, content, content_len);
    }
    total[4 + content_len] = medtrum_crc8(total, 4 + content_len);
    size_t total_len = 4 + content_len + 1;

    if (total_len - 4 <= SINGLE_PACKET_MAX) {
        if (max_frags < 1) {
            return -1;
        }
        memcpy(frags[0].data, total, total_len);
        frags[0].data[total_len] = 0x00; /* the single-packet marker */
        frags[0].len = total_len + 1;
        return 1;
    }

    size_t off = 4; /* the content and the CRC, after the header */
    size_t remaining = total_len - 4;
    size_t count = 0;
    uint8_t pkg_index = 1;

    while (remaining > SINGLE_PACKET_MAX) {
        if (count >= max_frags) {
            return -1;
        }
        uint8_t *out = frags[count].data;
        out[0] = total[0];
        out[1] = command;
        out[2] = sequence;
        out[3] = pkg_index;
        memcpy(out + 4, total + off, SINGLE_PACKET_MAX);
        out[4 + SINGLE_PACKET_MAX] = medtrum_crc8(out, 4 + SINGLE_PACKET_MAX);
        frags[count].len = 4 + SINGLE_PACKET_MAX + 1;
        count++;
        pkg_index++;
        off += SINGLE_PACKET_MAX;
        remaining -= SINGLE_PACKET_MAX;
    }

    if (count >= max_frags) {
        return -1;
    }
    uint8_t *out = frags[count].data;
    out[0] = total[0];
    out[1] = command;
    out[2] = sequence;
    out[3] = pkg_index;
    memcpy(out + 4, total + off, remaining);
    out[4 + remaining] = medtrum_crc8(out, 4 + remaining);
    frags[count].len = 4 + remaining + 1;
    count++;

    return (int)count;
}

void medtrum_decode_init(medtrum_packet_t *p, uint8_t command)
{
    memset(p, 0, sizeof *p);
    p->command = command;
}

void medtrum_decode(medtrum_packet_t *p, const uint8_t *frag, size_t n)
{
    if (!p->started) {
        /* header(4) + responseCode(2) + CRC(1) */
        if (n < 7) {
            p->failed = true;
            return;
        }
        if (frag[1] != p->command) {
            p->failed = true;
        }
        p->started = true;
        p->data_size = frag[0];
        p->sequence = frag[3];
        p->response_code = (uint16_t)(frag[4] | (frag[5] << 8));
        p->len = n - 1;
        if (p->len > MEDTRUM_FRAME_MAX) {
            p->len = MEDTRUM_FRAME_MAX;
            p->failed = true;
        }
        memcpy(p->data, frag, p->len);
        if (medtrum_crc8(frag, n - 1) != frag[n - 1]) {
            p->failed = true;
        }
        return;
    }

    /* fragment header(4) + CRC(1) */
    if (n < 5) {
        p->failed = true;
        return;
    }
    size_t add = n - 1 - 4;
    if (p->len + add > MEDTRUM_FRAME_MAX) {
        add = MEDTRUM_FRAME_MAX - p->len;
        p->failed = true;
    }
    memcpy(p->data + p->len, frag + 4, add);
    p->len += add;
    p->sequence++;

    if (medtrum_crc8(frag, n - 1) != frag[n - 1]) {
        p->failed = true;
    }
    if (p->sequence != frag[3]) {
        p->failed = true;
    }
}

int medtrum_encode_response(uint8_t command, uint8_t sequence, uint16_t response_code,
                            const uint8_t *payload, size_t payload_len, medtrum_frag_t *frags,
                            size_t max_frags)
{
    /* header(4) + responseCode(2) + payload must fit the size byte. */
    if (frags == NULL || payload_len > 249) {
        return -1;
    }

    uint8_t buf[6 + 250];
    size_t buf_len = 6 + payload_len;
    buf[0] = (uint8_t)buf_len;
    buf[1] = command;
    buf[2] = sequence;
    buf[3] = 0; /* pkgIndex */
    buf[4] = (uint8_t)response_code;
    buf[5] = (uint8_t)(response_code >> 8);
    if (payload_len > 0) {
        memcpy(buf + 6, payload, payload_len);
    }

    if (buf_len - 4 <= SINGLE_PACKET_MAX) {
        if (max_frags < 1) {
            return -1;
        }
        memcpy(frags[0].data, buf, buf_len);
        frags[0].data[buf_len] = medtrum_crc8(buf, buf_len);
        frags[0].len = buf_len + 1;
        return 1;
    }

    /* A response fragments (responseCode + payload): no inner CRC, and no
     * trailing single-packet marker. */
    size_t off = 4;
    size_t remaining = buf_len - 4;
    size_t count = 0;
    uint8_t pkg_index = 1;

    while (remaining > SINGLE_PACKET_MAX) {
        if (count >= max_frags) {
            return -1;
        }
        uint8_t *out = frags[count].data;
        out[0] = buf[0];
        out[1] = command;
        out[2] = sequence;
        out[3] = pkg_index;
        memcpy(out + 4, buf + off, SINGLE_PACKET_MAX);
        out[4 + SINGLE_PACKET_MAX] = medtrum_crc8(out, 4 + SINGLE_PACKET_MAX);
        frags[count].len = 4 + SINGLE_PACKET_MAX + 1;
        count++;
        pkg_index++;
        off += SINGLE_PACKET_MAX;
        remaining -= SINGLE_PACKET_MAX;
    }

    if (count >= max_frags) {
        return -1;
    }
    uint8_t *out = frags[count].data;
    out[0] = buf[0];
    out[1] = command;
    out[2] = sequence;
    out[3] = pkg_index;
    memcpy(out + 4, buf + off, remaining);
    out[4 + remaining] = medtrum_crc8(out, 4 + remaining);
    frags[count].len = 4 + remaining + 1;
    count++;

    return (int)count;
}

bool medtrum_complete(const medtrum_packet_t *p)
{
    return p->len == p->data_size;
}

bool medtrum_has_enough(const medtrum_packet_t *p, size_t minimum)
{
    return p->len >= minimum;
}
