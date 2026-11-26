/* The settings page: its routes and the JSON the page polls.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

esp_err_t settings_app_register(httpd_handle_t server);
