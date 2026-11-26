/* The captive portal's DNS responder: every A query is answered with the
 * access point's address, so a phone that joins the AP resolves every name to
 * the board and shows its setup sheet. Started with the AP and stopped when
 * the AP stops.
 *
 * The answer builder is pure and lives in dns_answer.c; this file owns the
 * socket and the task. The socket is waited on with a timeout so dns_stop()
 * can end the task without resorting to vTaskDelete().
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <errno.h>
#include <string.h>

#include "app.h"
#include "dns_answer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#define DNS_PORT 53
#define DNS_SELECT_MS 200

static const char *TAG = "dns";

static volatile bool running;
static SemaphoreHandle_t done;

static esp_err_t bind_socket(int *out)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        return ESP_FAIL;
    }
    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(DNS_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (struct sockaddr *)&addr, sizeof addr) < 0) {
        close(sock);
        return ESP_FAIL;
    }
    *out = sock;
    return ESP_OK;
}

static void dns_task(void *arg)
{
    uint8_t query[512];
    uint8_t reply[768];
    int sock = -1;

    for (;;) {
        if (sock < 0) {
            if (bind_socket(&sock) != ESP_OK) {
                ESP_LOGW(TAG, "cannot bind UDP port %d: errno %d", DNS_PORT, errno);
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (!running) {
                    break;
                }
                continue;
            }
            ESP_LOGI(TAG, "answering every name with 192.168.4.1");
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sock, &rfds);
        struct timeval tv = {.tv_sec = 0, .tv_usec = DNS_SELECT_MS * 1000};
        int ready = select(sock + 1, &rfds, NULL, NULL, &tv);
        if (!running) {
            break;
        }
        if (ready < 0) {
            if (errno != EINTR) {
                ESP_LOGW(TAG, "select failed: errno %d", errno);
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            continue;
        }
        if (ready == 0) {
            continue;
        }

        struct sockaddr_storage from;
        socklen_t from_len = sizeof from;
        int len = recvfrom(sock, query, sizeof query, 0, (struct sockaddr *)&from, &from_len);
        if (len <= 0) {
            continue;
        }
        size_t reply_len = dns_build_reply(query, (size_t)len, reply, sizeof reply);
        if (reply_len == 0) {
            continue;
        }
        sendto(sock, reply, reply_len, 0, (struct sockaddr *)&from, from_len);
    }

    if (sock >= 0) {
        close(sock);
    }
    if (done) {
        xSemaphoreGive(done);
    }
    vTaskDelete(NULL);
}

void dns_start(void)
{
    if (running) {
        return;
    }
    if (!done && !(done = xSemaphoreCreateBinary())) {
        ESP_LOGE(TAG, "cannot create the stop semaphore");
        return;
    }
    /* A stop that timed out may have left a token; it is not a start signal. */
    xSemaphoreTake(done, 0);
    running = true;
    if (xTaskCreate(dns_task, "dns", 4096, NULL, 5, NULL) != pdPASS) {
        running = false;
        ESP_LOGE(TAG, "cannot start the DNS task");
    }
}

void dns_stop(void)
{
    if (!running) {
        return;
    }
    running = false;
    /* The task wakes within DNS_SELECT_MS, closes the socket and gives the
     * semaphore. Do not vTaskDelete() a task blocked in select(). */
    if (done && xSemaphoreTake(done, pdMS_TO_TICKS(2000)) != pdTRUE) {
        ESP_LOGW(TAG, "the DNS task did not stop in time");
    }
}
