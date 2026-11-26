/* The pump page and its aliases.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

esp_err_t pump_app_register(httpd_handle_t server);
