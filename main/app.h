/* The ESP32 side of ESP-Sim: radio, storage and control panel.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "cgm.h"
#include "dash.h"
#include "medtrum.h"

/* ble.c: the pod's GATT service and advertising (NimBLE). */
void ble_start(void);
/* The dash_io_t radio half: events from the phone, notifications to it. */
bool ble_next_event(void *ctx, dash_ev_t *ev, uint32_t timeout_ms);
void ble_notify_cmd(void *ctx, const uint8_t *p, size_t n);
void ble_notify_data(void *ctx, const uint8_t *p, size_t n);
void ble_disconnect(void *ctx);
void ble_advertise(void *ctx, const dash_adv_t *adv);
bool ble_connected(void);
/* The cgm_io_t radio half, for the CGM's own advertising set and connection. */
void ble_cgm_send(void *ctx, cgm_chr_t chr, const uint8_t *p, size_t n);
void ble_cgm_disconnect(void *ctx);
void ble_cgm_advertise(void *ctx, bool on, const char *name, const uint8_t addr[6]);

/* cgm_app.c: the simulated Dexcom G6 or G7. */
void cgm_app_start(void);
void cgm_app_on_connect(void);
void cgm_app_on_disconnect(void);
void cgm_app_on_subscribe(cgm_chr_t chr, bool on);
void cgm_app_on_write(cgm_chr_t chr, const uint8_t *p, size_t n);
size_t cgm_app_on_read(cgm_chr_t chr, uint8_t *out, size_t cap);
size_t cgm_app_status_json(char *out, size_t cap);
const char *cgm_app_command(const char *body); /* NULL, or what was wrong */

/* medtrum_app.c: the simulated Medtrum Nano patch. */
#define MEDTRUM_STATE_MAGIC 0x4d445452u /* "MDTR" */
#define MEDTRUM_STATE_VERSION 1u

/* What survives a reboot: the base identity and the delivery model. The
 * session (auth, token) does not. */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint8_t sn[4];          /* the advertised serial, as the app reads it */
    uint8_t device_type;
    uint8_t sw_version[3];
    medtrum_status_t status;
    medtrum_delivery_t delivery;
} medtrum_saved_t;

void medtrum_app_start(void);
bool medtrum_app_state_valid(const medtrum_saved_t *st);
/* The control panel: GET /medtrum's JSON, and POST /medtrum's command. */
size_t medtrum_app_status_json(char *out, size_t cap);
const char *medtrum_app_command(const char *body);

/* ble.c: the Medtrum patch's radio half. */
bool ble_medtrum_next_event(void *ctx, medtrum_ev_t *ev, uint32_t timeout_ms);
void ble_medtrum_notify(void *ctx, const uint8_t *p, size_t n);        /* WRITE: responses */
void ble_medtrum_notify_status(void *ctx, const uint8_t *p, size_t n); /* READ: notifications */
void ble_medtrum_disconnect(void *ctx);
void ble_medtrum_advertise(void *ctx, bool on, const uint8_t sn[4], uint8_t device_type);
bool ble_medtrum_connected(void);

/* store.c: NVS. */
void store_init(void);
bool store_load_state(dash_state_t *st);
void store_save_state(void *ctx, const dash_state_t *st);
bool store_load_wifi(char ssid[33], char password[65]);
void store_save_wifi(const char *ssid, const char *password);
bool store_load_cgm(cgm_state_t *st);
void store_save_cgm(void *ctx, const cgm_state_t *st);
bool store_load_medtrum(medtrum_saved_t *st);
void store_save_medtrum(void *ctx, const medtrum_saved_t *st);

/* panel.c: Wi-Fi (station or access point) and the HTTP control panel. */
void panel_start(dash_pod_t *pod);

/* captive.c: the DNS hijack and DHCP option that make the access point a
 * captive portal, so joining it opens the control panel. Called on AP_START. */
void captive_portal_start(void);
