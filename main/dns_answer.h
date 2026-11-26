/* The captive portal's DNS answer builder: pure, no socket and no ESP-IDF, so
 * test/host/test_dns.c can link it with plain cc.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* Build a reply to a DNS query. Every A question is answered with the access
 * point's address (192.168.4.1); other question types are left unanswered
 * (NOERROR, no record). The query id, question name and question type/class
 * are echoed. Returns the reply length, or 0 when the query is malformed,
 * truncated (including its question section) or the reply buffer is too
 * small. */
size_t dns_build_reply(const uint8_t *query, size_t query_len, uint8_t *reply, size_t reply_cap);
