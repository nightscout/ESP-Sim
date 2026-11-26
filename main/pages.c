/* The page routes, in one place so registration order does not drift.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "pages.h"

esp_err_t pages_register(httpd_handle_t server)
{
    esp_err_t err = settings_app_register(server);
    if (err != ESP_OK) {
        return err;
    }
    err = pump_app_register(server);
    if (err != ESP_OK) {
        return err;
    }
    /* captive_register() installs the wildcard catch-all, so it goes last. */
    return captive_register(server);
}
