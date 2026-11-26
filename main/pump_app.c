/* The pump page: one page for whichever pump the board is, with the pod,
 * YpsoPump and Medtrum panels over the commands the panel already serves.
 * /pod and /medtrum are old URLs: they redirect here.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "esp_log.h"

#include "pages.h"
#include "pump_app.h"

static const char *TAG = "pump";

extern const char pump_html_start[] asm("_binary_pump_html_start");
extern const char pump_html_end[] asm("_binary_pump_html_end");

static esp_err_t get_pump(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, pump_html_start, (ssize_t)(pump_html_end - pump_html_start));
}

static esp_err_t redirect_pod(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/pump");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "moved to /pump\n");
}

static esp_err_t redirect_medtrum(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/pump");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "moved to /pump\n");
}

esp_err_t pump_app_register(httpd_handle_t server)
{
    const httpd_uri_t routes[] = {
        {.uri = "/pump", .method = HTTP_GET, .handler = get_pump},
        {.uri = "/pod", .method = HTTP_GET, .handler = redirect_pod},
        {.uri = "/medtrum", .method = HTTP_GET, .handler = redirect_medtrum},
    };
    for (size_t i = 0; i < sizeof routes / sizeof routes[0]; i++) {
        esp_err_t err = httpd_register_uri_handler(server, &routes[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "registering %s: %s", routes[i].uri, esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}
