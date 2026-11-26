/* The captive portal's request decision and its shared responses.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "esp_err.h"
#include "esp_http_server.h"
#include "portal.h"

/* Run the portal decision for this request with the board's current state:
 * the station link and the setup=done cookie. */
PortalAction captive_action(httpd_req_t *req, const char *path, const char *method);

/* 302 to the setup sheet, as the portal and the root both answer. */
esp_err_t captive_redirect_setup(httpd_req_t *req);

/* Set the setup=done dismissal cookie on this response. */
void captive_set_dismissed(httpd_req_t *req);
