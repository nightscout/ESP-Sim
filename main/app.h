/* The ESP32 side of ESP-Sim: radio, storage and control panel.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "cgm.h"
#include "dash.h"
#include "medtrum.h"
#include "ypso.h"

/* Which device the board is, as store_load_pump() persists it. One at a time;
 * the panel switches and restarts. */
#define PUMP_POD 0
#define PUMP_YPSO 1
#define PUMP_MEDTRUM 2

/* ble.c: the pump's (pod, YpsoPump or Medtrum base) and the CGM's GATT
 * services and advertising (NimBLE). The pump's services are fixed at boot. */
void ble_start(uint8_t pump, uint8_t ypso_fw, const char *ypso_name);
int32_t ble_passkey(void);    /* the pairing passkey waiting to be typed on the phone, -1 if none */
void ble_forget_bonds(void);
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

/* ble.c: the Medtrum patch's radio half, the medtrum_io_t side. */
bool ble_medtrum_next_event(void *ctx, medtrum_ev_t *ev, uint32_t timeout_ms);
void ble_medtrum_notify(void *ctx, const uint8_t *p, size_t n);        /* WRITE: responses */
void ble_medtrum_notify_status(void *ctx, const uint8_t *p, size_t n); /* READ: notifications */
void ble_medtrum_disconnect(void *ctx);
void ble_medtrum_advertise(void *ctx, bool on, const uint8_t sn[4]);
bool ble_medtrum_connected(void);

/* cgm_app.c: the simulated Dexcom G6 or G7. */
void cgm_app_start(void);
void cgm_app_on_connect(void);
void cgm_app_on_disconnect(void);
void cgm_app_on_subscribe(cgm_chr_t chr, bool on);
void cgm_app_on_write(cgm_chr_t chr, const uint8_t *p, size_t n);
size_t cgm_app_on_read(cgm_chr_t chr, uint8_t *out, size_t cap);
size_t cgm_app_status_json(char *out, size_t cap);
const char *cgm_app_command(const char *body); /* NULL, or what was wrong */

/* main.c: the pod's state, read-only (the CGM's virtual person reads its deliveries). */
const dash_state_t *pod_state(void);

/* medtrum_app.c: the simulated Medtrum Nano patch. */
#define MEDTRUM_STATE_MAGIC 0x4d445452u /* "MDTR" */
#define MEDTRUM_STATE_VERSION 1u

/* What survives a reboot: the base identity and the delivery model. The
 * session (auth, token) does not. */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint8_t sn[4]; /* the advertised serial, as the app reads it */
    uint8_t device_type;
    uint8_t sw_version[3];
    medtrum_status_t status;
    medtrum_delivery_t delivery;
} medtrum_saved_t;

void medtrum_app_start(void);
bool medtrum_app_state_valid(const medtrum_saved_t *st);
/* The control panel: GET /patch's JSON, and POST /patch's command. */
size_t medtrum_app_status_json(char *out, size_t cap);
const char *medtrum_app_command(const char *body);

/* ypso_app.c: the simulated YpsoPump. */
void ypso_app_start(void);
const ypso_state_t *ypso_app_state(void);
void ypso_app_on_connect(void);
void ypso_app_on_disconnect(void);
int ypso_app_on_read(ypso_chr_t c, uint8_t *out, size_t cap, size_t *len);
int ypso_app_on_write(ypso_chr_t c, const uint8_t *p, size_t n);
size_t ypso_app_status_json(char *out, size_t cap);
size_t ypso_app_history_text(char *out, size_t cap, int max); /* newest first, as text */
uint32_t ypso_app_delivered(void); /* hundredths of a unit, ever */
/* POST /ypso. Returns NULL, or what was wrong; *reply, when set, is the JSON answer. */
const char *ypso_app_command(const char *body, char *reply, size_t cap);

/* store.c: NVS. */
void store_init(void);
bool store_load_state(dash_state_t *st);
void store_save_state(void *ctx, const dash_state_t *st);
bool store_load_wifi(char ssid[33], char password[65]);
void store_save_wifi(const char *ssid, const char *password);
bool store_load_cgm(cgm_state_t *st);
/* Which pump the board is: 0 an Omnipod pod, 1 a YpsoPump, 2 a Medtrum base. */
uint8_t store_load_pump(void);
void store_save_pump(uint8_t pump);
bool store_load_medtrum(medtrum_saved_t *st);
void store_save_medtrum(void *ctx, const medtrum_saved_t *st);
bool store_load_ypso(ypso_state_t *st);
bool store_load_ypso_open(void);
size_t store_load_ypso_peers(uint8_t *out, size_t cap);
void store_save_ypso_peers(const uint8_t *p, size_t n);
void store_save_ypso_open(bool open);
void store_save_ypso(void *ctx, const ypso_state_t *st);
void store_save_cgm(void *ctx, const cgm_state_t *st);

/* panel.c: Wi-Fi (station or access point) and the HTTP control panel. */
void panel_start(dash_pod_t *pod);
/* Whether the station is on a joined network (it has an address). */
bool panel_sta_joined(void);
/* Whether the board is running its own access point, and its credentials. */
bool panel_ap_active(void);
void panel_ap_credentials(char ssid[33], char pass[17]);
/* The panel's restart-after-a-second path, shared by the page handlers. */
void panel_restart_soon(void);

/* dns.c: the captive portal's DNS responder, started and stopped with the AP. */
void dns_start(void);
void dns_stop(void);

/* remote.c: the log at GET /log and new firmware at POST /ota, over Wi-Fi. */
void remote_log_init(void); /* first thing at boot: keeps the log's last 32 KB */
void remote_start(void *httpd_server);
void ypso_app_clock_changed(void); /* the board's time or zone changed: the pump follows */
bool store_load_clock(int64_t *t);
void store_save_clock(int64_t t);
bool store_load_tz(char tz[64]);
void store_save_tz(const char *tz);
