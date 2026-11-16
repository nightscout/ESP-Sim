/*
 * The captive portal: a DNS hijack and DHCP option 114, so that a phone
 * joining the board's access point opens the control panel by itself.
 *
 * Every A query is answered with the access point's own address, so the
 * phone's connectivity probe lands on the panel. The panel answers that probe
 * with a redirect (the 404 handler in panel.c) and the phone shows its "sign
 * in to network" sheet. Option 114 says the same thing in the DHCP lease,
 * which is what iOS, Android and Windows prefer when they have it.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <errno.h>
#include <string.h>

#include "app.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

static const char *TAG = "captive";

#define DNS_MAX_QUERY 512

/* What DHCP option 114 hands out. esp_netif keeps the pointer rather than a
 * copy of the string, so this has to outlive the call: see esp_netif.h. */
static char portal_uri[32];
static bool portal_started;

/* The reply: the query's header and question, then one A record pointing at
 * the access point. The record's name is a pointer back to the question's name
 * at offset 12 -- the first byte after the header -- which is why those two
 * bytes are the same for every query. Returns the length, or 0 to stay quiet. */
static size_t build_answer(uint8_t *out, const uint8_t *query, size_t qlen, uint32_t ip)
{
    if (qlen > DNS_MAX_QUERY || qlen < 12 + 5) {
        return 0;
    }
    if (query[4] == 0 && query[5] == 0) {
        return 0;   /* no question to answer */
    }

    /* Walk the question's labels. A question never uses name compression, so a
     * byte with its top bits set is malformed and the bounds checks catch it. */
    size_t q = 12;
    while (q < qlen && query[q] != 0) {
        q += (size_t)query[q] + 1;
    }
    if (q >= qlen || query[q] != 0 || q + 5 > qlen) {
        return 0;
    }
    size_t end = q + 5;   /* the root label, then QTYPE and QCLASS */

    uint16_t qtype = (uint16_t)((query[end - 4] << 8) | query[end - 3]);
    bool a_record = qtype == 1 || qtype == 255;   /* A, or ANY */

    memcpy(out, query, end);
    out[2] = (uint8_t)(0x80 | (query[2] & 0x01) | 0x04);   /* QR, the caller's RD, AA */
    out[3] = 0x80;                                        /* RA, rcode 0 */
    out[4] = 0;
    out[5] = 1;                                           /* one question */
    out[6] = 0;
    out[7] = a_record ? 1 : 0;                            /* one answer, or none */
    out[8] = 0;
    out[9] = 0;
    out[10] = 0;
    out[11] = 0;
    if (!a_record) {
        /* An AAAA query: no answer of that type, and the phone asks for A next. */
        return end;
    }

    uint8_t *a = out + end;
    a[0] = 0xc0;
    a[1] = 0x0c;                               /* the name, from the question */
    a[2] = 0x00;
    a[3] = 0x01;                               /* TYPE  A */
    a[4] = 0x00;
    a[5] = 0x01;                               /* CLASS IN */
    a[6] = 0;
    a[7] = 0;
    a[8] = 0;
    a[9] = 60;                                 /* TTL, 60 s */
    a[10] = 0x00;
    a[11] = 0x04;                              /* RDLENGTH */
    memcpy(a + 12, &ip, sizeof ip);
    return end + 16;
}

static void dns_task(void *arg)
{
    uint8_t query[DNS_MAX_QUERY];
    uint8_t answer[DNS_MAX_QUERY + 16];
    struct sockaddr_in from;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "no socket for the DNS hijack: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (const struct sockaddr *)&addr, sizeof addr) < 0) {
        ESP_LOGE(TAG, "could not bind UDP 53: errno %d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "every name resolves to the control panel");

    for (;;) {
        socklen_t from_len = sizeof from;
        int n = recvfrom(sock, query, sizeof query, 0, (struct sockaddr *)&from, &from_len);
        if (n <= 0) {
            continue;
        }
        /* Read the address per query: the task can outlive an address change,
         * and the call is a plain struct read. */
        esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
        esp_netif_ip_info_t ip;
        if (ap == NULL || esp_netif_get_ip_info(ap, &ip) != ESP_OK) {
            continue;
        }
        size_t len = build_answer(answer, query, (size_t)n, ip.ip.addr);
        if (len != 0) {
            sendto(sock, answer, len, 0, (const struct sockaddr *)&from, from_len);
        }
    }
}

void captive_portal_start(void)
{
    if (portal_started) {
        return;
    }
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    esp_netif_ip_info_t ip;
    if (ap == NULL || esp_netif_get_ip_info(ap, &ip) != ESP_OK) {
        return;   /* the access point is not up yet; the next AP_START retries */
    }
    portal_started = true;

    snprintf(portal_uri, sizeof portal_uri, "http://" IPSTR "/", IP2STR(&ip.ip));

    /* DHCP option 114: where the panel is, so the phone does not have to work
     * it out from its connectivity probe. Not running yet is fine, which is
     * why the stop and start results are not checked. */
    (void)esp_netif_dhcps_stop(ap);
    esp_err_t err = esp_netif_dhcps_option(ap, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
                                           portal_uri, strlen(portal_uri));
    (void)esp_netif_dhcps_start(ap);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "could not put the portal in the DHCP lease: %s", esp_err_to_name(err));
    }

    ESP_LOGW(TAG, "captive portal: joining the access point opens %s", portal_uri);
    xTaskCreate(dns_task, "dns_hijack", 4096, NULL, 4, NULL);
}
