/*
 * Request reassembly: the phone's writes are header(4) + content + CRC-8,
 * with a trailing 0x00 marker on a single packet. A fragmented request (the
 * ACTIVATE command, for one) ends with its own CRC instead of a marker.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <string.h>

#include "medtrum.h"

void medtrum_request_init(medtrum_request_t *r)
{
    memset(r, 0, sizeof *r);
}

/* The CRC of a fragmented request covers the logical header (pkgIndex 0) and
 * the content, so it spans two buffers. */
static uint8_t crc8_two(const uint8_t *a, size_t an, const uint8_t *b, size_t bn)
{
    uint8_t crc = 0;
    for (size_t i = 0; i < an; i++) {
        crc = medtrum_crc8_table[a[i] ^ crc];
    }
    for (size_t i = 0; i < bn; i++) {
        crc = medtrum_crc8_table[b[i] ^ crc];
    }
    return crc;
}

bool medtrum_request_feed(medtrum_request_t *r, const uint8_t *frag, size_t n)
{
    if (n < 5) {
        return false;
    }
    uint8_t size = frag[0];
    uint8_t pkg = frag[3];

    if (pkg == 0) {
        /* header(4) + content + CRC + the single-packet marker. The smallest
         * valid single packet is the 6-byte empty-content form, whose size is
         * 5; the size + 1 check below rejects any size below that too. */
        if (n < 6 || n != (size_t)size + 1 || frag[n - 1] != 0) {
            return false;
        }
        if (medtrum_crc8(frag, n - 2) != frag[n - 2]) {
            return false;
        }
        size_t clen = n - 6;
        if (clen > MEDTRUM_CONTENT_MAX) {
            return false;
        }
        memcpy(r->content, frag + 4, clen);
        r->content_len = clen;
        r->command = frag[1];
        r->sequence = frag[2];
        return true;
    }

    /* A continuation fragment: header(4) + chunk + CRC. */
    if (medtrum_crc8(frag, n - 1) != frag[n - 1]) {
        r->active = false;
        return false;
    }
    if (pkg == 1) {
        r->active = true;
        r->command = frag[1];
        r->sequence = frag[2];
        r->len = 0;
    } else if (!r->active || r->command != frag[1] || r->sequence != frag[2]) {
        return false;
    }
    size_t chunk = n - 5;
    if (r->len + chunk > sizeof r->buf) {
        r->active = false;
        return false;
    }
    memcpy(r->buf + r->len, frag + 4, chunk);
    r->len += chunk;
    if (r->len < (size_t)size - 4) {
        return false; /* more fragments coming */
    }
    if (r->len != (size_t)size - 4) {
        r->active = false;
        return false;
    }
    size_t clen = r->len - 1; /* the reassembled body ends with its CRC */
    if (clen > MEDTRUM_CONTENT_MAX) {
        r->active = false;
        return false;
    }
    uint8_t header[4] = {size, r->command, r->sequence, 0};
    if (crc8_two(header, sizeof header, r->buf, clen) != r->buf[clen]) {
        r->active = false;
        return false;
    }
    memcpy(r->content, r->buf, clen);
    r->content_len = clen;
    r->active = false;
    return true;
}
