/* The captive portal: the setup sheet, the form handlers, and the wildcard
 * catch-all that answers each OS probe the way its captive-portal check
 * expects. The decision itself is components/portalsim.
 *
 * The catch-all is registered last: the first matching handler wins, so the
 * board's own exact routes are unaffected.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <string.h>

#include "app.h"
#include "captive.h"
#include "esp_log.h"
#include "pages.h"
#include "portal.h"

static const char *TAG = "captive";

extern const char setup_html_start[] asm("_binary_setup_html_start");
extern const char setup_html_end[] asm("_binary_setup_html_end");

#define SETUP_COOKIE "setup=done; Path=/; Max-Age=604800"

static bool setup_done(httpd_req_t *req)
{
    char value[8];
    size_t len = sizeof value;
    return httpd_req_get_cookie_val(req, "setup", value, &len) == ESP_OK && strcmp(value, "done") == 0;
}

PortalAction captive_action(httpd_req_t *req, const char *path, const char *method)
{
    return portalDecide(path, method, panel_sta_joined() ? 1 : 0, setup_done(req) ? 1 : 0);
}

esp_err_t captive_redirect_setup(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/setup");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "", 0);
}

static esp_err_t send_setup_sheet(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, setup_html_start, (ssize_t)(setup_html_end - setup_html_start));
}

void captive_set_dismissed(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Set-Cookie", SETUP_COOKIE);
}

static esp_err_t get_setup(httpd_req_t *req)
{
    return send_setup_sheet(req);
}

static esp_err_t post_setup_wifi(httpd_req_t *req)
{
    /* The shared save handler sets the cookie only after the body validates, so
     * a rejected password does not dismiss the sheet for a week. */
    return panel_post_setup_wifi(req);
}

static esp_err_t post_setup_dismiss(httpd_req_t *req)
{
    captive_set_dismissed(req);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "dismissed\n");
}

/* Any path no other route claimed: the portal decision. */
static esp_err_t portal_catch_all(httpd_req_t *req)
{
    const char *method = req->method == HTTP_GET ? "GET" : "OTHER";
    switch (captive_action(req, req->uri, method)) {
    case PORTAL_SERVE_SETUP:
    case PORTAL_PROBE_APPLE_HTML:
        return send_setup_sheet(req);
    case PORTAL_REDIRECT_SETUP:
    case PORTAL_PROBE_ANDROID_REDIRECT:
    case PORTAL_PROBE_WINDOWS_REDIRECT:
    case PORTAL_PROBE_FWLINK:
        return captive_redirect_setup(req);
    case PORTAL_PROBE_APPLE_SUCCESS:
        httpd_resp_set_type(req, "text/html");
        return httpd_resp_sendstr(req, "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>");
    case PORTAL_PROBE_ANDROID_204:
        httpd_resp_set_status(req, "204 No Content");
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_send(req, "", 0);
    case PORTAL_PROBE_CONNECTTEST:
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_sendstr(req, "Microsoft Connect Test");
    case PORTAL_PROBE_NCSI:
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_sendstr(req, "Microsoft NCSI");
    case PORTAL_SERVE_APP:
    default:
        /* The fork's onNotFound: no route matched, so the app has no page. */
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_sendstr(req, "not found\n");
    }
}

esp_err_t captive_register(httpd_handle_t server)
{
    const httpd_uri_t routes[] = {
        {.uri = "/setup", .method = HTTP_GET, .handler = get_setup},
        {.uri = "/setup/wifi", .method = HTTP_POST, .handler = post_setup_wifi},
        {.uri = "/setup/dismiss", .method = HTTP_POST, .handler = post_setup_dismiss},
        /* Last: the wildcard shadows every route registered after it. */
        {.uri = "/*", .method = HTTP_ANY, .handler = portal_catch_all},
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
