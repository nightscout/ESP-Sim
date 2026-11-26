/* The pages and the captive portal: one registration order, fixed here so the
 * page units only fill in their own files.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/* Each page registers its own routes; the captive portal registers the wildcard
 * catch-all and must stay last, because the first matching handler wins. */
esp_err_t settings_app_register(httpd_handle_t server);
esp_err_t pump_app_register(httpd_handle_t server);
esp_err_t captive_register(httpd_handle_t server);

/* panel.c: the one Wi-Fi save handler, shared by POST /wifi and the setup
 * form's POST /setup/wifi. The setup handler sets the dismissal cookie only
 * after the body validates. */
esp_err_t panel_post_wifi(httpd_req_t *req);
esp_err_t panel_post_setup_wifi(httpd_req_t *req);
/* panel.c: POST /ypso, including the pumpSelect switch; /settings/pump is an
 * alias so the pages share one code path. */
esp_err_t panel_post_ypso(httpd_req_t *req);

/* Registers the pages in that order: the specific pages first, the catch-all
 * last. Called from panel_start() after the panel's own routes. */
esp_err_t pages_register(httpd_handle_t server);
