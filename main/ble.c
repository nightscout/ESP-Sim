/*
 * Bluetooth on NimBLE. One board, two devices: the pump (an Omnipod pod, or
 * a YpsoPump), from the chip's public address, and (when one is switched on)
 * a Dexcom CGM, from a random static address of its own. Each is a separate advertising set, so the phone
 * sees two peripherals and can be connected to both at once.
 *
 * They share one GATT database (NimBLE has one per host), so each device's
 * services are visible on the other's connection too: the apps pick their
 * device by advertisement and name, and writes to the other device's
 * characteristics are ignored. The GAP device name is answered per
 * connection, because the apps read it after connecting.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <string.h>

#include "app.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gatt/ble_svc_gatt.h"

#include "dash_internal.h"
#include "ypso.h"

void ble_store_config_init(void);
int ble_store_config_read(int obj_type, const union ble_store_key *key, union ble_store_value *val);
int ble_store_config_write(int obj_type, const union ble_store_value *val);
int ble_store_config_delete(int obj_type, const union ble_store_key *key);

static const char *TAG = "ble";

#define POD_INSTANCE 0
#define CGM_INSTANCE 1

/* 1a7e4024-e3ed-4464-8b7e-751e03d0dc5f and its two characteristics, as
 * NimBLE wants 128-bit UUIDs: least significant byte first. */
#define DASH_UUID(b12, b13) \
    BLE_UUID128_INIT(0x5f, 0xdc, 0xd0, 0x03, 0x1e, 0x75, 0x7e, 0x8b, 0x64, 0x44, 0xed, 0xe3, b12, b13, 0x7e, 0x1a)

static const ble_uuid128_t svc_uuid = DASH_UUID(0x24, 0x40);
static const ble_uuid128_t cmd_uuid = DASH_UUID(0x41, 0x24);
static const ble_uuid128_t data_uuid = DASH_UUID(0x42, 0x24);    /* DASH */
static const ble_uuid128_t o5_data_uuid = DASH_UUID(0x43, 0x24); /* Omnipod 5 */

/* Medtrum: 669A9001-0008-968F-E311-6050405558B3 with READ 669a9120-... and
 * WRITE 669a9101-... . Both notify: responses go on WRITE, the status
 * notifications on READ. */
#define MEDTRUM_UUID(b12, b13) \
    BLE_UUID128_INIT(0xb3, 0x58, 0x55, 0x40, 0x50, 0x60, 0x11, 0xe3, 0x8f, 0x96, 0x08, 0x00, b12, b13, \
                     0x9a, 0x66)

static const ble_uuid128_t medtrum_svc_uuid = MEDTRUM_UUID(0x01, 0x90);
static const ble_uuid128_t medtrum_read_uuid = MEDTRUM_UUID(0x20, 0x91);
static const ble_uuid128_t medtrum_write_uuid = MEDTRUM_UUID(0x01, 0x91);

/* Dexcom: F808xxxx-849E-531C-C594-30F1F86A4EA5. */
#define DEX_UUID(b12, b13) \
    BLE_UUID128_INIT(0xa5, 0x4e, 0x6a, 0xf8, 0xf1, 0x30, 0x94, 0xc5, 0x1c, 0x53, 0x9e, 0x84, b12, b13, 0x08, 0xf8)

static const ble_uuid128_t dex_svc_uuid = DEX_UUID(0x32, 0x35);
static const ble_uuid128_t dex_chr_uuid[CGM_CHR_COUNT] = {
    [CGM_CHR_COMM] = DEX_UUID(0x33, 0x35),
    [CGM_CHR_CONTROL] = DEX_UUID(0x34, 0x35),
    [CGM_CHR_AUTH] = DEX_UUID(0x35, 0x35),
    [CGM_CHR_BACKFILL] = DEX_UUID(0x36, 0x35),
};
static const ble_uuid16_t dex_adv_uuid = BLE_UUID16_INIT(0xfebc);

static const ble_uuid16_t gap_svc_uuid = BLE_UUID16_INIT(0x1800);
static const ble_uuid16_t gap_name_uuid = BLE_UUID16_INIT(0x2a00);
static const ble_uuid16_t gap_appearance_uuid = BLE_UUID16_INIT(0x2a01);

static QueueHandle_t events;
static QueueHandle_t medtrum_events;
static bool ypso_mode;
static bool medtrum_mode;
static uint8_t ypso_fw_id;
static char ypso_adv_name[24];
static int32_t passkey_shown = -1; /* -1: none waiting */
static const char *const cgm_chr_names[CGM_CHR_COUNT] = {"comm", "control", "auth", "backfill"};

static uint16_t cmd_handle, data_handle, o5_data_handle;
static uint16_t dex_handle[CGM_CHR_COUNT];
static uint16_t medtrum_read_handle, medtrum_write_handle;
static volatile uint16_t pod_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile uint16_t medtrum_conn = BLE_HS_CONN_HANDLE_NONE;

/* The CGM's links: one core session per central, up to CGM_MAX_SESSIONS. The
 * core owns each central's login, deliveries and backfill cursor; the radio
 * maps a session token (the link's slot) to a connection handle. */
typedef struct {
    bool used;
    uint16_t handle;
    uint32_t gen;
    int pending;                  /* frames queued by the core but not yet sent */
    bool drop;                    /* the core asked to hang up; held for the queue */
    uint16_t subs[CGM_CHR_COUNT]; /* the CCCD value per characteristic */
} cgm_link_t;
static cgm_link_t cgm_links[CGM_MAX_SESSIONS];
static uint32_t cgm_gen_counter;
static SemaphoreHandle_t cgm_mu; /* the links and the queue below */

/* The CGM's notifications go through a queue and a task of their own: the core
 * calls ble_cgm_send_session from inside its own mutex, so buffer pressure is
 * waited out here, never while the core is locked. A hang-up the core asked for
 * waits until this queue has drained. */
typedef struct {
    cgm_session_t session;
    uint16_t handle;
    uint32_t gen;
    uint8_t chr;
    uint8_t len;
    uint8_t data[32];
} cgm_out_t;
static QueueHandle_t cgm_out;
static void cgm_send_task(void *arg);
static volatile bool medtrum_status_notify;
static uint8_t own_addr_type;
static volatile bool synced;

static SemaphoreHandle_t adv_mu; /* the advertisements' contents below */
static dash_pod_type_t adv_type;
static bool adv_has_id, adv_has_pdm_id;
static uint8_t adv_id[4], adv_pdm_id[4], adv_fault, adv_alerts;
static bool cgm_adv_on;
static char cgm_adv_name[16];
static uint8_t cgm_adv_addr[6];
/* What the radio last applied. NimBLE stops an extended advertising instance
 * when a connection completes, so the set must be re-armed even when the
 * core's window and payload have not changed. */
static bool cgm_adv_applied_on;
static bool cgm_adv_applied_valid;
static char cgm_adv_applied_name[16];
static uint8_t cgm_adv_applied_addr[6];
static bool medtrum_adv_on;
static uint8_t medtrum_adv_sn[4];

static void push(uint8_t type, const uint8_t *data, size_t n)
{
    dash_ev_t ev = {.type = type, .len = (uint8_t)n};
    if (n) {
        memcpy(ev.data, data, n);
    }
    if (xQueueSend(events, &ev, 0) != pdTRUE) {
        ESP_LOGW(TAG, "event queue full: a packet from the phone was dropped");
    }
}

static void push_medtrum(uint8_t type, const uint8_t *data, size_t n)
{
    medtrum_ev_t ev = {.type = type, .len = (uint8_t)n};
    if (n) {
        memcpy(ev.data, data, n);
    }
    if (xQueueSend(medtrum_events, &ev, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Medtrum event queue full: a packet from the phone was dropped");
    }
}

/* ---- the CGM's links (cgm_mu) ------------------------------------------- */

static int cgm_slot_for_handle(uint16_t h)
{
    for (int i = 0; i < CGM_MAX_SESSIONS; i++) {
        if (cgm_links[i].used && cgm_links[i].handle == h) {
            return i;
        }
    }
    return -1;
}

static cgm_link_t *cgm_link_for_session(cgm_session_t session)
{
    return session < CGM_MAX_SESSIONS && cgm_links[session].used ? &cgm_links[session] : NULL;
}

/* The session token a CGM connection is on, or CGM_MAX_SESSIONS when the
 * handle is not a CGM link (the pod's or the patch's). */
static cgm_session_t cgm_session_for(uint16_t h)
{
    cgm_session_t s = CGM_MAX_SESSIONS;
    xSemaphoreTake(cgm_mu, portMAX_DELAY);
    int i = cgm_slot_for_handle(h);
    if (i >= 0) {
        s = (cgm_session_t)i;
    }
    xSemaphoreGive(cgm_mu);
    return s;
}

/* How many CGM links are in use. The advertising set stays up while a slot
 * remains. */
static int cgm_live_links(void)
{
    int n = 0;
    xSemaphoreTake(cgm_mu, portMAX_DELAY);
    for (int i = 0; i < CGM_MAX_SESSIONS; i++) {
        n += cgm_links[i].used ? 1 : 0;
    }
    xSemaphoreGive(cgm_mu);
    return n;
}

static void cgm_link_up(uint16_t h)
{
    int slot = -1;
    xSemaphoreTake(cgm_mu, portMAX_DELAY);
    for (int i = 0; i < CGM_MAX_SESSIONS; i++) {
        if (!cgm_links[i].used) {
            slot = i;
            break;
        }
    }
    if (slot >= 0) {
        memset(&cgm_links[slot], 0, sizeof cgm_links[slot]);
        cgm_links[slot].used = true;
        cgm_links[slot].handle = h;
        cgm_links[slot].gen = ++cgm_gen_counter;
    }
    xSemaphoreGive(cgm_mu);
    if (slot < 0) {
        ESP_LOGW(TAG, "CGM: a third central; two links at a time, closing it");
        ble_gap_terminate(h, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }
    ESP_LOGI(TAG, "cgm: session %d connected (handle 0x%04x)", slot, h);
    cgm_app_on_connect_session((cgm_session_t)slot);
}

static void cgm_link_down(uint16_t h)
{
    cgm_session_t session = CGM_MAX_SESSIONS;
    xSemaphoreTake(cgm_mu, portMAX_DELAY);
    int slot = cgm_slot_for_handle(h);
    if (slot >= 0) {
        session = (cgm_session_t)slot;
    }
    xSemaphoreGive(cgm_mu);
    /* Close the core session before the radio link goes: nothing new can be
     * queued for a link the core no longer knows. */
    if (session < CGM_MAX_SESSIONS) {
        cgm_app_on_disconnect_session(session);
    }
    xSemaphoreTake(cgm_mu, portMAX_DELAY);
    slot = cgm_slot_for_handle(h);
    if (slot >= 0) {
        memset(&cgm_links[slot], 0, sizeof cgm_links[slot]);
    }
    xSemaphoreGive(cgm_mu);
}

static void cgm_subscribe(uint16_t h, cgm_chr_t chr, uint16_t sub)
{
    cgm_session_t session = CGM_MAX_SESSIONS;
    xSemaphoreTake(cgm_mu, portMAX_DELAY);
    int slot = cgm_slot_for_handle(h);
    if (slot >= 0) {
        cgm_links[slot].subs[chr] = sub;
        session = (cgm_session_t)slot;
    }
    xSemaphoreGive(cgm_mu);
    if (session < CGM_MAX_SESSIONS) {
        ESP_LOGI(TAG, "cgm: %s subscribe %s (handle 0x%04x)", cgm_chr_names[chr], sub ? "on" : "off", h);
        cgm_app_on_subscribe_session(session, chr, sub != 0);
    }
}

static int flat(struct os_mbuf *om, uint8_t *buf, size_t cap, uint16_t *len)
{
    if (OS_MBUF_PKTLEN(om) > cap) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    return ble_hs_mbuf_to_flat(om, buf, (uint16_t)cap, len) == 0 ? 0 : BLE_ATT_ERR_UNLIKELY;
}

/* ---- the pod's characteristics ------------------------------------------- */

static int pod_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
    }
    uint8_t buf[DASH_PKT_MAX];
    uint16_t len = 0;
    int rc = flat(ctxt->om, buf, sizeof buf, &len);
    if (rc != 0 || conn_handle != pod_conn) {
        return rc; /* another device's connection: ignored */
    }
    push(attr_handle == cmd_handle ? DASH_EV_CMD : DASH_EV_DATA, buf, len);
    return 0;
}

/* ---- the Medtrum patch's characteristics --------------------------------- */

static int medtrum_read_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt,
                               void *arg)
{
    /* The READ characteristic only carries notifications; a read is empty. */
    return ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR ? 0 : BLE_ATT_ERR_REQ_NOT_SUPPORTED;
}

static int medtrum_write_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt,
                                void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
    }
    uint8_t buf[MEDTRUM_FRAME_MAX];
    uint16_t len = 0;
    int rc = flat(ctxt->om, buf, sizeof buf, &len);
    if (rc != 0 || conn_handle != medtrum_conn) {
        return rc; /* another device's connection: ignored */
    }
    push_medtrum(MEDTRUM_EV_WRITE, buf, len);
    return 0;
}

/* ---- the CGM's characteristics ------------------------------------------- */

static int dex_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    cgm_chr_t chr = (cgm_chr_t)(intptr_t)arg;
    cgm_session_t session = cgm_session_for(conn_handle);
    bool ours = session < CGM_MAX_SESSIONS;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        uint8_t buf[32];
        size_t n = ours ? cgm_app_on_read_session(session, chr, buf, sizeof buf) : 0;
        return os_mbuf_append(ctxt->om, buf, n) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
    }
    uint8_t buf[32];
    uint16_t len = 0;
    int rc = flat(ctxt->om, buf, sizeof buf, &len);
    if (rc == 0 && ours) {
        if (chr == CGM_CHR_AUTH && len > 0 && buf[0] == 0x07) {
            /* BondRequestTx: the transmitter starts BLE pairing. Just Works;
             * without it the app waits at "bonding" for a pairing that never
             * begins. */
            ESP_LOGI(TAG, "cgm: bond request: starting Just Works pairing (handle 0x%04x)", conn_handle);
            ble_gap_security_initiate(conn_handle);
        }
        ESP_LOGI(TAG, "cgm: %s write %u bytes (handle 0x%04x)", cgm_chr_names[chr], (unsigned)len, conn_handle);
        cgm_app_on_write_session(session, chr, buf, len);
    }
    return rc;
}

/* ---- GAP: the name depends on which device the phone connected to ---------- */

static int gap_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
    }
    if (ble_uuid_cmp(ctxt->chr->uuid, &gap_appearance_uuid.u) == 0) {
        uint16_t appearance = 0;
        return os_mbuf_append(ctxt->om, &appearance, 2) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    char name[16];
    bool cgm_link = cgm_session_for(conn_handle) < CGM_MAX_SESSIONS;
    xSemaphoreTake(adv_mu, portMAX_DELAY);
    if (cgm_link) {
        strcpy(name, cgm_adv_name);
    } else if (ypso_mode) {
        strcpy(name, ypso_adv_name);
    } else if (medtrum_mode) {
        strcpy(name, "MT");
    } else {
        strcpy(name, DASH_ADVERTISED_NAME);
    }
    xSemaphoreGive(adv_mu);
    return os_mbuf_append(ctxt->om, name, strlen(name)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}


/* ---- the YpsoPump's services, built at boot for the chosen firmware ---------- */

static ble_uuid128_t y_svc_uuid[YS_COUNT], y_chr_uuid[YC_COUNT];
static uint16_t ypso_handle[YC_COUNT];
static struct ble_gatt_chr_def ypso_chrs[YS_COUNT][24];
static struct ble_gatt_svc_def ypso_svcs[YS_COUNT + 1];

static int ypso_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    ypso_chr_t c = (ypso_chr_t)(intptr_t)arg;
    if (conn_handle != pod_conn) {
        return BLE_ATT_ERR_UNLIKELY; /* the CGM's connection */
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        uint8_t buf[64];
        size_t n = 0;
        int rc = ypso_app_on_read(c, buf, sizeof buf, &n);
        ESP_LOGI(TAG, "ypso read  chr %d (%s): %s %u bytes", (int)c, ypso_chr(c)->uuid + 24, rc ? "refused" : "ok",
                 rc ? (unsigned)rc : (unsigned)n);
        if (rc != 0) {
            return rc;
        }
        return os_mbuf_append(ctxt->om, buf, n) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
    }
    uint8_t buf[YPSO_PKT + 8];
    uint16_t len = 0;
    int rc = flat(ctxt->om, buf, sizeof buf, &len);
    if (rc == 0) {
        rc = ypso_app_on_write(c, buf, len);
    }
    ESP_LOGI(TAG, "ypso write chr %d (%s): %u bytes, %s 0x%02x", (int)c, ypso_chr(c)->uuid + 24, (unsigned)len,
             rc ? "refused" : "ok", (unsigned)rc);
    return rc;
}

static void ypso_build(void)
{
    int k = 0;
    for (int s = 0; s < YS_COUNT; s++) {
        if (!ypso_has_svc((ypso_fw_t)ypso_fw_id, (ypso_svc_t)s)) {
            continue;
        }
        y_svc_uuid[s].u.type = BLE_UUID_TYPE_128;
        ypso_uuid_le(ypso_svc_uuid((ypso_svc_t)s), y_svc_uuid[s].value);
        int j = 0;
        for (int c = 0; c < YC_COUNT; c++) {
            const ypso_chr_info_t *ci = ypso_chr((ypso_chr_t)c);
            if (ci->svc != s || !ypso_has_chr((ypso_fw_t)ypso_fw_id, (ypso_chr_t)c)) {
                continue;
            }
            y_chr_uuid[c].u.type = BLE_UUID_TYPE_128;
            ypso_uuid_le(ci->uuid, y_chr_uuid[c].value);
            /* Like the pump: nothing but its version strings until the phone has
             * bonded (an unencrypted link gets ATT 0x05 and pairs). */
            bool open = ci->clear && c != YC_AUTH_PASSWORD && c != YC_EXTENDED_READ && c != YC_WRITE_CHALLENGE;
            ble_gatt_chr_flags fl = 0;
            if (ci->read) {
                fl |= BLE_GATT_CHR_F_READ | (open ? 0 : BLE_GATT_CHR_F_READ_ENC);
            }
            if (ci->write) {
                fl |= BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC;
            }
            ypso_chrs[s][j++] = (struct ble_gatt_chr_def){
                .uuid = &y_chr_uuid[c].u,
                .access_cb = ypso_access,
                .arg = (void *)(intptr_t)c,
                .flags = fl,
                .val_handle = &ypso_handle[c],
            };
        }
        ypso_chrs[s][j] = (struct ble_gatt_chr_def){0};
        ypso_svcs[k++] = (struct ble_gatt_svc_def){
            .type = BLE_GATT_SVC_TYPE_PRIMARY,
            .uuid = &y_svc_uuid[s].u,
            .characteristics = ypso_chrs[s],
        };
    }
    ypso_svcs[k] = (struct ble_gatt_svc_def){0};
}

#define DEX_CHR(c, fl) \
    {.uuid = &dex_chr_uuid[c].u, .access_cb = dex_access, .arg = (void *)(intptr_t)(c), .flags = (fl), \
     .val_handle = &dex_handle[c]}

static const struct ble_gatt_svc_def gap_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &gap_svc_uuid.u,
        .characteristics =
            (struct ble_gatt_chr_def[]){
                {.uuid = &gap_name_uuid.u, .access_cb = gap_access, .flags = BLE_GATT_CHR_F_READ},
                {.uuid = &gap_appearance_uuid.u, .access_cb = gap_access, .flags = BLE_GATT_CHR_F_READ},
                {0},
            },
    },
    {0},
};

static const struct ble_gatt_svc_def pod_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &svc_uuid.u,
        .characteristics =
            (struct ble_gatt_chr_def[]){
                {
                    .uuid = &cmd_uuid.u,
                    .access_cb = pod_access,
                    .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY,
                    .val_handle = &cmd_handle,
                },
                {
                    .uuid = &data_uuid.u,
                    .access_cb = pod_access,
                    .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY,
                    .val_handle = &data_handle,
                },
                {
                    .uuid = &o5_data_uuid.u,
                    .access_cb = pod_access,
                    .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY,
                    .val_handle = &o5_data_handle,
                },
                {0},
            },
    },
    {0},
};

static const struct ble_gatt_svc_def medtrum_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &medtrum_svc_uuid.u,
        .characteristics =
            (struct ble_gatt_chr_def[]){
                {
                    .uuid = &medtrum_read_uuid.u,
                    .access_cb = medtrum_read_access,
                    .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                    .val_handle = &medtrum_read_handle,
                },
                {
                    .uuid = &medtrum_write_uuid.u,
                    .access_cb = medtrum_write_access,
                    .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY,
                    .val_handle = &medtrum_write_handle,
                },
                {0},
            },
    },
    {0},
};

static const struct ble_gatt_svc_def cgm_services[] = {
    {
        /* The apps subscribe with setNotifyValue: they take notifications
         * or indications alike, and notifications need no queueing here. */
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &dex_svc_uuid.u,
        .characteristics =
            (struct ble_gatt_chr_def[]){
                DEX_CHR(CGM_CHR_COMM, BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY),
                DEX_CHR(CGM_CHR_CONTROL, BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_INDICATE),
                DEX_CHR(CGM_CHR_AUTH, BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY |
                                          BLE_GATT_CHR_F_INDICATE),
                DEX_CHR(CGM_CHR_BACKFILL, BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY),
                {0},
            },
    },
    {0},
};

/* ---- advertising ------------------------------------------------------------ */

static int pod_gap_event(struct ble_gap_event *event, void *arg);
static int cgm_gap_event(struct ble_gap_event *event, void *arg);
static int medtrum_gap_event(struct ble_gap_event *event, void *arg);

/* (Re)starts an advertising set with these fields, from addr (NULL: public). */
static void adv_start(uint8_t instance, const struct ble_hs_adv_fields *fields, const struct ble_hs_adv_fields *rsp,
                      const uint8_t *addr, ble_gap_event_fn *cb)
{
    if (ble_gap_ext_adv_active(instance)) {
        ble_gap_ext_adv_stop(instance);
    }
    struct ble_gap_ext_adv_params params = {
        .connectable = 1,
        .scannable = 1,
        .legacy_pdu = 1,
        .itvl_min = 0x00f4, /* 152.5 ms, as the Go simulator */
        .itvl_max = 0x00f4,
        .channel_map = 0x07,
        .own_addr_type = addr ? BLE_OWN_ADDR_RANDOM : own_addr_type,
        .primary_phy = BLE_HCI_LE_PHY_1M,
        .secondary_phy = BLE_HCI_LE_PHY_1M,
        .tx_power = 127,
        .sid = instance,
    };
    int rc = ble_gap_ext_adv_configure(instance, &params, NULL, cb, NULL);
    if (rc == 0 && addr) {
        ble_addr_t a = {.type = BLE_ADDR_RANDOM};
        memcpy(a.val, addr, 6);
        rc = ble_gap_ext_adv_set_addr(instance, &a);
    }
    struct os_mbuf *d = NULL;
    if (rc == 0) {
        d = os_msys_get_pkthdr(BLE_HS_ADV_MAX_SZ, 0);
        rc = d ? ble_hs_adv_set_fields_mbuf(fields, d) : BLE_HS_ENOMEM;
        rc = rc == 0 ? ble_gap_ext_adv_set_data(instance, d) : (os_mbuf_free_chain(d), rc);
    }
    if (rc == 0) {
        d = os_msys_get_pkthdr(BLE_HS_ADV_MAX_SZ, 0);
        rc = d ? ble_hs_adv_set_fields_mbuf(rsp, d) : BLE_HS_ENOMEM;
        rc = rc == 0 ? ble_gap_ext_adv_rsp_set_data(instance, d) : (os_mbuf_free_chain(d), rc);
    }
    if (rc == 0) {
        rc = ble_gap_ext_adv_start(instance, 0, 0);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "could not advertise (set %u): %d", instance, rc);
    }
}

static void pod_advertise_now(void)
{
    if (!synced || pod_conn != BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    static ble_uuid16_t uuids[9];
    static ble_uuid128_t o5_uuid;
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    if (ypso_mode) {
        /* "YpsoPump_<serial>", as the pump. */
        fields.name = (const uint8_t *)ypso_adv_name;
        fields.name_len = strlen(ypso_adv_name);
        fields.name_is_complete = 1;
        struct ble_hs_adv_fields rsp = {0};
        rsp.name = fields.name;
        rsp.name_len = fields.name_len;
        rsp.name_is_complete = 1;
        adv_start(POD_INSTANCE, &fields, &rsp, NULL, pod_gap_event);
        return;
    }

    xSemaphoreTake(adv_mu, portMAX_DELAY);
    if (adv_type == DASH_POD_O5) {
        /* One 128-bit UUID with the paired controller's id in it. */
        o5_uuid = (ble_uuid128_t)BLE_UUID128_INIT(0);
        dash_o5_advertised_uuid(adv_has_pdm_id ? adv_pdm_id : NULL, o5_uuid.value);
        fields.uuids128 = &o5_uuid;
        fields.num_uuids128 = 1;
        fields.uuids128_is_complete = 1;
    } else {
        uint16_t svc[9];
        dash_advertised_services(adv_has_id ? adv_id : NULL, adv_fault, adv_alerts, svc);
        for (int i = 0; i < 9; i++) {
            uuids[i] = (ble_uuid16_t)BLE_UUID16_INIT(0);
            uuids[i].value = svc[i];
        }
        fields.uuids16 = uuids;
        fields.num_uuids16 = 9;
        fields.uuids16_is_complete = 1;
    }
    xSemaphoreGive(adv_mu);

    struct ble_hs_adv_fields rsp = {0};
    rsp.name = (const uint8_t *)DASH_ADVERTISED_NAME;
    rsp.name_len = strlen(DASH_ADVERTISED_NAME);
    rsp.name_is_complete = 1;
    adv_start(POD_INSTANCE, &fields, &rsp, NULL, pod_gap_event);
}

/* The core owns the CGM's advertising window (G6 continuous, G7 window, signal
 * loss, CGM off), but the radio decides whether a set can still accept a
 * central: with two links it stays up while a slot remains, and an app that
 * has just ended its session reconnects at once. NimBLE stops the instance
 * when a connection completes, so this is re-armed even when the window and
 * payload have not changed. */
static void cgm_advertise_now(void)
{
    if (!synced) {
        return;
    }
    int live = cgm_live_links();
    xSemaphoreTake(adv_mu, portMAX_DELAY);
    bool on = cgm_adv_on && live < CGM_MAX_SESSIONS;
    char name[16];
    uint8_t addr[6];
    strcpy(name, cgm_adv_name);
    memcpy(addr, cgm_adv_addr, 6);
    bool same = cgm_adv_applied_valid && on == cgm_adv_applied_on &&
                strcmp(name, cgm_adv_applied_name) == 0 && memcmp(addr, cgm_adv_applied_addr, 6) == 0;
    xSemaphoreGive(adv_mu);

    if (on) {
        if (same && ble_gap_ext_adv_active(CGM_INSTANCE)) {
            return; /* already advertising this */
        }
        /* Like a Dexcom: the FEBC service and the name, in the advertisement. */
        struct ble_hs_adv_fields fields = {0};
        fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
        fields.uuids16 = &dex_adv_uuid;
        fields.num_uuids16 = 1;
        fields.uuids16_is_complete = 1;
        fields.name = (const uint8_t *)name;
        fields.name_len = strlen(name);
        fields.name_is_complete = 1;
        struct ble_hs_adv_fields rsp = {0};
        rsp.name = (const uint8_t *)name;
        rsp.name_len = strlen(name);
        rsp.name_is_complete = 1;
        adv_start(CGM_INSTANCE, &fields, &rsp, addr, cgm_gap_event);
    } else if (ble_gap_ext_adv_active(CGM_INSTANCE)) {
        ble_gap_ext_adv_stop(CGM_INSTANCE);
    } else if (same) {
        return; /* already off and unchanged */
    }

    xSemaphoreTake(adv_mu, portMAX_DELAY);
    cgm_adv_applied_on = on;
    cgm_adv_applied_valid = true;
    strcpy(cgm_adv_applied_name, name);
    memcpy(cgm_adv_applied_addr, addr, 6);
    xSemaphoreGive(adv_mu);
}

/* The Medtrum base: the app scans for the name "MT" and reads the base
 * serial, device type and version out of the manufacturer data. It replaces
 * the pod (they are mutually exclusive), so it uses the pod's advertising
 * instance. */
static void medtrum_advertise_now(void)
{
    if (!synced || medtrum_conn != BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    xSemaphoreTake(adv_mu, portMAX_DELAY);
    bool on = medtrum_adv_on;
    uint8_t sn[4];
    memcpy(sn, medtrum_adv_sn, 4);
    xSemaphoreGive(adv_mu);
    if (!on) {
        if (ble_gap_ext_adv_active(POD_INSTANCE)) {
            ble_gap_ext_adv_stop(POD_INSTANCE);
        }
        return;
    }
    /* Manufacturer data: Medtrum's company id 0x4781, the serial (reversed),
     * then the device type and version. A real base sends this. */
    uint8_t mfg[8] = {0x81, 0x47, sn[0], sn[1], sn[2], sn[3], 0x01, 0x01};
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (const uint8_t *)"MT";
    fields.name_len = 2;
    fields.name_is_complete = 1;
    fields.mfg_data = mfg;
    fields.mfg_data_len = sizeof mfg;
    struct ble_hs_adv_fields rsp = {0};
    rsp.name = (const uint8_t *)"MT";
    rsp.name_len = 2;
    rsp.name_is_complete = 1;
    adv_start(POD_INSTANCE, &fields, &rsp, NULL, medtrum_gap_event);
}

/* ---- connections -------------------------------------------------------------- */

/* Pairing differs per link, and NimBLE reads it from ble_hs_cfg when the
 * phone asks to pair (just after connecting). A YpsoPump shows a 6-digit
 * passkey that the person types on the phone (passkey entry, MITM); a
 * Dexcom has no screen and bonds Just Works. */
static void security_for(bool pump_link)
{
    bool display = pump_link && ypso_mode;
    ble_hs_cfg.sm_io_cap = display ? BLE_HS_IO_DISPLAY_ONLY : BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_mitm = display;
    /* The pump's Pairing Response, from a phone's capture (rockit
     * docs/ble-pair.md): it gives only its long-term key and asks for the
     * central's identity (initiator 0x02, responder 0x01). */
    ble_hs_cfg.sm_our_key_dist = display ? BLE_SM_PAIR_KEY_DIST_ENC : BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = display ? BLE_SM_PAIR_KEY_DIST_ID : BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
}

/* ---- one bond store, two devices --------------------------------------------------------
 *
 * The board is two Bluetooth devices, the pump and the CGM, but NimBLE keeps
 * one bond per remote device, found by its address alone. A watch bonded to
 * both (the pump by passkey, the G6 Just Works) would have one bond replace
 * the other, and NimBLE refuses to replace a passkey bond with a weaker one.
 * So the CGM's bonds are kept under the remote address with a mark in its
 * type byte: the store holds both, and each link finds its own. Which device
 * a lookup is for: the link to that remote device that is not yet encrypted
 * (pairing and reconnecting both happen there), else the only link to it. */
#define CGM_NS 0x10
static int store_ns_force = -1; /* -1: from the links; 0: the pump's; 1: the CGM's */

static bool conn_peer(uint16_t conn, const ble_addr_t *a, bool *encrypted)
{
    struct ble_gap_conn_desc d;
    if (conn == BLE_HS_CONN_HANDLE_NONE || ble_gap_conn_find(conn, &d) != 0) {
        return false;
    }
    *encrypted = d.sec_state.encrypted;
    return ble_addr_cmp(&d.peer_id_addr, a) == 0 || ble_addr_cmp(&d.peer_ota_addr, a) == 0;
}

static bool for_cgm(const ble_addr_t *a)
{
    if (store_ns_force >= 0) {
        return store_ns_force == 1;
    }
    bool pe = false, ce = false, c = false;
    bool p = conn_peer(pod_conn, a, &pe);
    /* The CGM may hold two links (one per central): the device is the CGM's
     * if any of them is to it. */
    uint16_t links[CGM_MAX_SESSIONS];
    xSemaphoreTake(cgm_mu, portMAX_DELAY);
    for (int i = 0; i < CGM_MAX_SESSIONS; i++) {
        links[i] = cgm_links[i].used ? cgm_links[i].handle : BLE_HS_CONN_HANDLE_NONE;
    }
    xSemaphoreGive(cgm_mu);
    for (int i = 0; i < CGM_MAX_SESSIONS && !c; i++) {
        bool e = false;
        if (conn_peer(links[i], a, &e)) {
            c = true;
            ce = e;
        }
    }
    if (p && c) {
        return !ce && pe; /* both: the one still being secured */
    }
    return c;
}

static bool keyed(int t)
{
    return t == BLE_STORE_OBJ_TYPE_OUR_SEC || t == BLE_STORE_OBJ_TYPE_PEER_SEC || t == BLE_STORE_OBJ_TYPE_CCCD;
}

static ble_addr_t *key_addr(int t, union ble_store_key *k)
{
    return t == BLE_STORE_OBJ_TYPE_CCCD ? &k->cccd.peer_addr : &k->sec.peer_addr;
}

static ble_addr_t *val_addr(int t, union ble_store_value *v)
{
    return t == BLE_STORE_OBJ_TYPE_CCCD ? &v->cccd.peer_addr : &v->sec.peer_addr;
}

static int split_read(int t, const union ble_store_key *key, union ble_store_value *val)
{
    if (!keyed(t)) {
        return ble_store_config_read(t, key, val);
    }
    union ble_store_key k = *key;
    ble_addr_t *a = key_addr(t, &k);
    if (ble_addr_cmp(a, BLE_ADDR_ANY) != 0 && for_cgm(a)) {
        a->type |= CGM_NS;
    }
    int rc = ble_store_config_read(t, &k, val);
    if (t != BLE_STORE_OBJ_TYPE_CCCD && ble_addr_cmp(key_addr(t, (union ble_store_key *)key), BLE_ADDR_ANY) != 0) {
        ESP_LOGI(TAG, "bond lookup (%s) for ..%02X in the %s's: %s%s", t == BLE_STORE_OBJ_TYPE_OUR_SEC ? "ours" : "theirs",
                 a->val[0], (a->type & CGM_NS) ? "CGM" : "pump", rc == 0 ? "found" : "none",
                 rc == 0 && val->sec.authenticated ? ", authenticated" : "");
    }
    if (rc == 0) {
        val_addr(t, val)->type &= ~CGM_NS;
    }
    return rc;
}

static int split_write(int t, const union ble_store_value *val)
{
    if (!keyed(t)) {
        return ble_store_config_write(t, val);
    }
    union ble_store_value v = *val;
    ble_addr_t *a = val_addr(t, &v);
    if (for_cgm(a)) {
        a->type |= CGM_NS;
    }
    if (t != BLE_STORE_OBJ_TYPE_CCCD) {
        ESP_LOGI(TAG, "bond kept (%s) for ..%02X in the %s's", t == BLE_STORE_OBJ_TYPE_OUR_SEC ? "ours" : "theirs",
                 a->val[0], (a->type & CGM_NS) ? "CGM" : "pump");
    }
    return ble_store_config_write(t, &v);
}

static int split_delete(int t, const union ble_store_key *key)
{
    if (!keyed(t)) {
        return ble_store_config_delete(t, key);
    }
    union ble_store_key k = *key;
    ble_addr_t *a = key_addr(t, &k);
    if (ble_addr_cmp(a, BLE_ADDR_ANY) != 0 && for_cgm(a)) {
        a->type |= CGM_NS;
    }
    return ble_store_config_delete(t, &k);
}

/* Like the real pump: bonds with up to 5 devices, the oldest forgotten when a
 * sixth pairs (the CGM's bonds are its own and not counted here). */
#define YPSO_MAX_PEERS 5

static void ypso_note_peer(uint16_t conn)
{
    struct ble_gap_conn_desc d;
    if (ble_gap_conn_find(conn, &d) != 0 || !d.sec_state.bonded) {
        return;
    }
    uint8_t list[7 * (YPSO_MAX_PEERS + 1)];
    size_t n = store_load_ypso_peers(list, 7 * YPSO_MAX_PEERS) / 7;
    uint8_t me[7] = {d.peer_id_addr.type};
    memcpy(me + 1, d.peer_id_addr.val, 6);
    for (size_t i = 0; i < n; i++) {
        if (memcmp(list + 7 * i, me, 7) == 0) { /* known: move it to the newest */
            memmove(list + 7 * i, list + 7 * (i + 1), 7 * (n - i - 1));
            n--;
            break;
        }
    }
    memcpy(list + 7 * n++, me, 7);
    while (n > YPSO_MAX_PEERS) {
        ble_addr_t old = {.type = list[0]};
        memcpy(old.val, list + 1, 6);
        store_ns_force = 0; /* the pump's bond only */
        ble_store_util_delete_peer(&old);
        store_ns_force = -1;
        ESP_LOGI(TAG, "a sixth device bonded: the oldest bond forgotten");
        memmove(list, list + 7, 7 * --n);
    }
    store_save_ypso_peers(list, 7 * n);
    ESP_LOGI(TAG, "YpsoPump bonded devices: %u of %d", (unsigned)n, YPSO_MAX_PEERS);
}

static int pod_gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            pod_conn = event->connect.conn_handle;
            security_for(true);
            ESP_LOGI(TAG, "phone connected to the %s", ypso_mode ? "YpsoPump" : "pod");
            if (ypso_mode) {
                ypso_app_on_connect();
            } else {
                push(DASH_EV_CONNECTED, NULL, 0);
            }
        } else {
            ESP_LOGW(TAG, "a connection to the %s did not come up (status %d)", ypso_mode ? "YpsoPump" : "pod",
                     event->connect.status);
            pod_advertise_now();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "phone disconnected from the %s (reason 0x%x)", ypso_mode ? "YpsoPump" : "pod",
                 event->disconnect.reason);
        pod_conn = BLE_HS_CONN_HANDLE_NONE;
        passkey_shown = -1;
        if (ypso_mode) {
            ypso_app_on_disconnect();
        } else {
            push(DASH_EV_DISCONNECTED, NULL, 0);
        }
        pod_advertise_now();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (pod_conn == BLE_HS_CONN_HANDLE_NONE) {
            pod_advertise_now();
        }
        break;
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "pump link MTU %u", event->mtu.value);
        break;
    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "pump link encryption %s (status %d)", event->enc_change.status == 0 ? "on" : "failed",
                 event->enc_change.status);
        passkey_shown = -1;
        if (event->enc_change.status == 0) {
            struct ble_gap_conn_desc d;
            if (ble_gap_conn_find(event->enc_change.conn_handle, &d) == 0) {
                ESP_LOGI(TAG, "pump link security: authenticated %d, bonded %d, key %u bytes, device ..%02X",
                         d.sec_state.authenticated, d.sec_state.bonded, (unsigned)d.sec_state.key_size,
                         d.peer_id_addr.val[0]);
            }
        }
        if (ypso_mode && event->enc_change.status == 0) {
            ypso_note_peer(event->enc_change.conn_handle);
        }
        break;
    case BLE_GAP_EVENT_PASSKEY_ACTION:
        if (event->passkey.params.action == BLE_SM_IOACT_DISP) {
            struct ble_sm_io io = {.action = BLE_SM_IOACT_DISP};
            uint32_t r;
            dash_random((uint8_t *)&r, sizeof r);
            io.passkey = r % 1000000;
            passkey_shown = (int32_t)io.passkey;
            ESP_LOGW(TAG, "pairing passkey %06u: type it on the phone (also on the panel)", (unsigned)io.passkey);
            ble_sm_inject_io(event->passkey.conn_handle, &io);
        } else {
            ESP_LOGW(TAG, "pairing asked for action %d, which a YpsoPump does not do", event->passkey.params.action);
        }
        break;
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        /* The phone lost its bond and pairs again: forget the old one. */
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == cmd_handle || event->subscribe.attr_handle == data_handle ||
            event->subscribe.attr_handle == o5_data_handle) {
            ESP_LOGI(TAG, "%s notifications %s", event->subscribe.attr_handle == cmd_handle ? "CMD" : "DATA",
                     event->subscribe.cur_notify ? "on" : "off");
        } else if (ypso_mode) {
            ESP_LOGI(TAG, "pump link: notifications on handle %u %s", event->subscribe.attr_handle,
                     event->subscribe.cur_notify || event->subscribe.cur_indicate ? "on" : "off");
        }
        break;
    default:
        if (ypso_mode) {
            ESP_LOGI(TAG, "pump link event %d", event->type);
        }
        break;
    }
    return 0;
}

static int cgm_gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            security_for(false);
            ESP_LOGI(TAG, "CGM link security config: io %u, mitm %u, sc %u, sc_only %u, level %u, bonding %u",
                     ble_hs_cfg.sm_io_cap, ble_hs_cfg.sm_mitm, ble_hs_cfg.sm_sc, ble_hs_cfg.sm_sc_only,
                     ble_hs_cfg.sm_sec_lvl, ble_hs_cfg.sm_bonding);
            cgm_link_up(event->connect.conn_handle);
        } else {
            ESP_LOGW(TAG, "a connection to the CGM did not come up (status %d)", event->connect.status);
            cgm_advertise_now();
        }
        break;
    case BLE_GAP_EVENT_ENC_CHANGE:
        /* 0x205 later means the device's key is not the one kept here: it must pair again. */
        ESP_LOGI(TAG, "CGM link encryption %s (status %d)", event->enc_change.status == 0 ? "on" : "failed",
                 event->enc_change.status);
        break;
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        /* A device bonded to the CGM pairs again (its key was lost): forget the
         * CGM's old bond for it, not its pump bond (split_delete), and go on. */
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        ESP_LOGI(TAG, "a device pairs with the CGM again: its old CGM bond forgotten");
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "CGM link closed (handle 0x%04x, reason 0x%x)", event->disconnect.conn.conn_handle,
                 event->disconnect.reason);
        cgm_link_down(event->disconnect.conn.conn_handle);
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        for (int c = 0; c < CGM_CHR_COUNT; c++) {
            if (event->subscribe.attr_handle == dex_handle[c]) {
                uint16_t sub = (uint16_t)((event->subscribe.cur_notify ? 1 : 0) |
                                          (event->subscribe.cur_indicate ? 2 : 0));
                cgm_subscribe(event->subscribe.conn_handle, (cgm_chr_t)c, sub);
            }
        }
        break;
    default:
        break;
    }
    return 0;
}

static int medtrum_gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            medtrum_conn = event->connect.conn_handle;
            ESP_LOGI(TAG, "phone connected to the Medtrum base");
            push_medtrum(MEDTRUM_EV_CONNECTED, NULL, 0);
        } else {
            medtrum_advertise_now();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "phone disconnected from the Medtrum base (reason 0x%x)", event->disconnect.reason);
        medtrum_conn = BLE_HS_CONN_HANDLE_NONE;
        medtrum_status_notify = false;
        push_medtrum(MEDTRUM_EV_DISCONNECTED, NULL, 0);
        medtrum_advertise_now();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (medtrum_conn == BLE_HS_CONN_HANDLE_NONE) {
            medtrum_advertise_now();
        }
        break;
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "Medtrum link MTU %u", event->mtu.value);
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == medtrum_read_handle) {
            medtrum_status_notify = event->subscribe.cur_notify || event->subscribe.cur_indicate;
            ESP_LOGI(TAG, "Medtrum status notifications %s", medtrum_status_notify ? "on" : "off");
        }
        break;
    default:
        break;
    }
    return 0;
}

static void on_sync(void)
{
    /* The bond store split (split_read). Here, not at init: the host's
     * privacy setup runs ble_store_config_init again as it starts, which puts
     * the plain store back. */
    ble_hs_cfg.store_read_cb = split_read;
    ble_hs_cfg.store_write_cb = split_write;
    ble_hs_cfg.store_delete_cb = split_delete;
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &own_addr_type);
    uint8_t addr[6] = {0};
    ble_hs_id_copy_addr(own_addr_type, addr, NULL);
    ESP_LOGI(TAG, "pod's Bluetooth address %02X:%02X:%02X:%02X:%02X:%02X", addr[5], addr[4], addr[3], addr[2],
             addr[1], addr[0]);
    synced = true;
    if (medtrum_mode) {
        medtrum_advertise_now();
    } else {
        pod_advertise_now();
    }
    cgm_advertise_now();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "Bluetooth host reset: %d", reason);
    synced = false;
}

static void host_task(void *arg)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_start(uint8_t pump_mode, uint8_t fw, const char *ypso_name)
{
    events = xQueueCreate(64, sizeof(dash_ev_t));
    medtrum_events = xQueueCreate(16, sizeof(medtrum_ev_t));
    adv_mu = xSemaphoreCreateMutex();
    cgm_mu = xSemaphoreCreateMutex();
    /* Deep enough for a backfill burst (a dozen frames plus the records). */
    cgm_out = xQueueCreate(64, sizeof(cgm_out_t));
    if (!cgm_out || xTaskCreate(cgm_send_task, "cgmsend", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGW(TAG, "cgm: notification task unavailable; notifications will be dropped");
    }
    ypso_mode = pump_mode == PUMP_YPSO;
    medtrum_mode = pump_mode == PUMP_MEDTRUM;
    ypso_fw_id = fw;
    if (ypso_name) {
        strncpy(ypso_adv_name, ypso_name, sizeof ypso_adv_name - 1);
    }
    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    /* Bonding, keys kept in flash: LE legacy pairing. The YpsoPump's link
     * pairs by passkey, the CGM's Just Works (security_for); the pod never
     * asks to pair. */
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_bonding = 1;
    security_for(true);
    ble_hs_cfg.sm_sc = 0;
    /* Not NimBLE's GAP service: ours answers the name per connection. Only
     * the chosen pump's services: the pod's, or the YpsoPump's. */
    ble_svc_gatt_init();
    const struct ble_gatt_svc_def *pump = pod_services;
    if (ypso_mode) {
        ypso_build();
        pump = ypso_svcs;
    } else if (medtrum_mode) {
        pump = medtrum_services;
    }
    const struct ble_gatt_svc_def *all[] = {gap_services, pump, cgm_services};
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) {
        ESP_ERROR_CHECK(ble_gatts_count_cfg(all[i]));
        ESP_ERROR_CHECK(ble_gatts_add_svcs(all[i]));
    }
    ble_store_config_init();
    nimble_port_freertos_init(host_task);
}

int32_t ble_passkey(void)
{
    return passkey_shown;
}

/* The pump's bonds only: the CGM's are kept (the panel's YpsoPump card). */
void ble_forget_bonds(void)
{
    uint8_t list[7 * YPSO_MAX_PEERS];
    size_t n = store_load_ypso_peers(list, sizeof list) / 7;
    store_ns_force = 0;
    for (size_t i = 0; i < n; i++) {
        ble_addr_t a = {.type = list[7 * i]};
        memcpy(a.val, list + 7 * i + 1, 6);
        ble_store_util_delete_peer(&a);
    }
    store_ns_force = -1;
    store_save_ypso_peers(NULL, 0);
}

bool ble_connected(void)
{
    return pod_conn != BLE_HS_CONN_HANDLE_NONE;
}

static void notify(uint16_t c, uint16_t handle, const uint8_t *p, size_t n)
{
    if (c == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    /* With a dozen fragments queued, NimBLE can run short of buffers for a
     * moment: wait for them rather than lose a fragment. */
    for (int tries = 0; tries < 100; tries++) {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(p, (uint16_t)n);
        if (om) {
            int rc = ble_gatts_notify_custom(c, handle, om);
            if (rc == 0) {
                return;
            }
            if (rc != BLE_HS_ENOMEM) {
                ESP_LOGW(TAG, "notification failed: %d", rc);
                return;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    ESP_LOGW(TAG, "notification dropped: no buffers");
}

/* ---- the pod: the radio half of dash_io_t ------------------------------------- */

bool ble_next_event(void *ctx, dash_ev_t *ev, uint32_t timeout_ms)
{
    TickType_t t = timeout_ms == DASH_WAIT_FOREVER ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    if (t == 0 && timeout_ms > 0) {
        t = 1;
    }
    return xQueueReceive(events, ev, t) == pdTRUE;
}

void ble_notify_cmd(void *ctx, const uint8_t *p, size_t n)
{
    notify(pod_conn, cmd_handle, p, n);
}

void ble_notify_data(void *ctx, const uint8_t *p, size_t n)
{
    notify(pod_conn, adv_type == DASH_POD_O5 ? o5_data_handle : data_handle, p, n);
}

void ble_disconnect(void *ctx)
{
    uint16_t c = pod_conn;
    if (c != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(c, BLE_ERR_REM_USER_CONN_TERM);
    }
}

void ble_advertise(void *ctx, const dash_adv_t *adv)
{
    xSemaphoreTake(adv_mu, portMAX_DELAY);
    adv_type = (dash_pod_type_t)adv->pod_type;
    adv_has_id = adv->id != NULL;
    if (adv->id) {
        memcpy(adv_id, adv->id, 4);
    }
    adv_has_pdm_id = adv->pdm_id != NULL;
    if (adv->pdm_id) {
        memcpy(adv_pdm_id, adv->pdm_id, 4);
    }
    adv_fault = adv->fault;
    adv_alerts = adv->alerts;
    bool paired = adv_type == DASH_POD_O5 ? adv_has_pdm_id : adv_has_id;
    xSemaphoreGive(adv_mu);
    ESP_LOGI(TAG, "advertising as %s %s pod (fault 0x%02x, alerts 0x%02x)", paired ? "a paired" : "an unpaired",
             adv_type == DASH_POD_O5 ? "Omnipod 5" : "DASH", adv->fault, adv->alerts);
    pod_advertise_now(); /* while a phone is connected, it takes effect when it leaves */
}

/* ---- the CGM: the radio half of cgm_io_t ---------------------------------------- */

/* Sends one queued notification to the one session's link. The core emits one
 * send per session, so a phone and a watch each get their own frame. With a
 * dozen fragments queued, NimBLE can run short of buffers for a moment: wait
 * for one rather than lose a frame, without holding cgm_mu across the wait. */
static void cgm_send_task(void *arg)
{
    cgm_out_t m;
    for (;;) {
        if (xQueueReceive(cgm_out, &m, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        bool gone = false, not_sub = false, failed = false, no_buf = false;
        for (int tries = 0; tries < 100; tries++) {
            bool retry = false;
            xSemaphoreTake(cgm_mu, portMAX_DELAY);
            cgm_link_t *lk = cgm_link_for_session(m.session);
            if (!lk || lk->gen != m.gen) {
                gone = true;
            } else if (lk->subs[m.chr] == 0) {
                not_sub = true;
            } else {
                struct os_mbuf *om = ble_hs_mbuf_from_flat(m.data, m.len);
                if (!om) {
                    retry = true;
                } else {
                    int rc = (lk->subs[m.chr] & 1)
                                 ? ble_gatts_notify_custom(m.handle, dex_handle[m.chr], om)
                                 : ble_gatts_indicate_custom(m.handle, dex_handle[m.chr], om);
                    if (rc == BLE_HS_ENOMEM) {
                        retry = true;
                    } else if (rc != 0) {
                        failed = true;
                    }
                }
            }
            xSemaphoreGive(cgm_mu);
            if (!retry) {
                break;
            }
            if (tries == 99) {
                no_buf = true;
            } else {
                vTaskDelay(pdMS_TO_TICKS(5));
            }
        }
        /* Release this frame's hold on the session's hang-up exactly once,
         * whether it was sent, dropped or exhausted. */
        xSemaphoreTake(cgm_mu, portMAX_DELAY);
        cgm_link_t *lk = cgm_link_for_session(m.session);
        if (lk && lk->gen == m.gen && lk->pending > 0) {
            lk->pending--;
        }
        xSemaphoreGive(cgm_mu);
        if (gone) {
            continue;
        }
        if (not_sub) {
            ESP_LOGI(TAG, "cgm: %s dropped: handle 0x%04x is no longer subscribed", cgm_chr_names[m.chr],
                     m.handle);
        } else if (no_buf) {
            ESP_LOGW(TAG, "cgm: notification dropped: no buffers");
        } else if (failed) {
            ESP_LOGW(TAG, "cgm: notification to handle 0x%04x failed", m.handle);
        }
    }
}

/* Called by the core from inside its own mutex, with the session that asked for
 * this send: queue it for that session's link and return. If the session's link
 * has gone, or never subscribed, there is nobody to send to. A queued frame
 * counts against the session's hang-up until it has actually been sent. */
void ble_cgm_send_session(cgm_session_t session, cgm_chr_t chr, const uint8_t *p, size_t n)
{
    if (!cgm_out) {
        return;
    }
    if (n > sizeof(((cgm_out_t *)0)->data)) {
        ESP_LOGW(TAG, "cgm: a %u-byte notification does not fit the queue; dropped", (unsigned)n);
        return;
    }
    cgm_out_t m = {.session = session, .chr = (uint8_t)chr, .len = (uint8_t)n};
    if (n) {
        memcpy(m.data, p, n);
    }
    xSemaphoreTake(cgm_mu, portMAX_DELAY);
    cgm_link_t *lk = cgm_link_for_session(session);
    if (!lk || lk->subs[chr] == 0) {
        xSemaphoreGive(cgm_mu);
        return;
    }
    m.handle = lk->handle;
    m.gen = lk->gen;
    lk->pending++;
    xSemaphoreGive(cgm_mu);
    if (xQueueSend(cgm_out, &m, 0) != pdTRUE) {
        xSemaphoreTake(cgm_mu, portMAX_DELAY);
        lk = cgm_link_for_session(session);
        if (lk && lk->gen == m.gen && lk->pending > 0) {
            lk->pending--;
        }
        xSemaphoreGive(cgm_mu);
        ESP_LOGW(TAG, "cgm: notification queue full: a message was dropped");
    }
}

/* The core asks for a hang-up from inside its own mutex; mark it and let
 * ble_cgm_tick() carry it out once the queue has drained. */
void ble_cgm_disconnect_session(cgm_session_t session)
{
    xSemaphoreTake(cgm_mu, portMAX_DELAY);
    cgm_link_t *lk = cgm_link_for_session(session);
    if (lk) {
        lk->drop = true;
    }
    xSemaphoreGive(cgm_mu);
}

/* Drain-before-hangup: terminate a session only once the frames queued for it
 * have been sent, so an injected delay cannot close the link before its
 * reading has left the queue. Called from the core's tick, outside its lock. */
void ble_cgm_tick(void)
{
    xSemaphoreTake(cgm_mu, portMAX_DELAY);
    for (int i = 0; i < CGM_MAX_SESSIONS; i++) {
        cgm_link_t *lk = &cgm_links[i];
        if (!lk->used || !lk->drop || lk->pending > 0) {
            continue;
        }
        uint16_t h = lk->handle;
        lk->drop = false;
        ESP_LOGI(TAG, "cgm: hanging up a drained session (handle 0x%04x)", h);
        ble_gap_terminate(h, BLE_ERR_REM_USER_CONN_TERM);
    }
    xSemaphoreGive(cgm_mu);
    /* Apply the core's window (io->advertise only records it, because it runs
     * under the core mutex) and re-arm the set after a connect let NimBLE stop
     * it. */
    cgm_advertise_now();
}

void ble_cgm_advertise(void *ctx, bool on, const char *name, const uint8_t addr[6])
{
    xSemaphoreTake(adv_mu, portMAX_DELAY);
    cgm_adv_on = on;
    strncpy(cgm_adv_name, name, sizeof cgm_adv_name - 1);
    cgm_adv_name[sizeof cgm_adv_name - 1] = 0;
    memcpy(cgm_adv_addr, addr, 6);
    xSemaphoreGive(adv_mu);
    ESP_LOGI(TAG, "cgm: %s as %s", on ? "advertising" : "not advertising", name);
}

/* ---- the Medtrum base: the radio half of medtrum_io_t --------------------------- */

bool ble_medtrum_next_event(void *ctx, medtrum_ev_t *ev, uint32_t timeout_ms)
{
    TickType_t t = timeout_ms == MEDTRUM_WAIT_FOREVER ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    if (t == 0 && timeout_ms > 0) {
        t = 1;
    }
    return xQueueReceive(medtrum_events, ev, t) == pdTRUE;
}

/* A command response, on the WRITE characteristic. */
void ble_medtrum_notify(void *ctx, const uint8_t *p, size_t n)
{
    notify(medtrum_conn, medtrum_write_handle, p, n);
}

/* A status notification, on the READ characteristic. */
void ble_medtrum_notify_status(void *ctx, const uint8_t *p, size_t n)
{
    if (medtrum_status_notify) {
        notify(medtrum_conn, medtrum_read_handle, p, n);
    }
}

void ble_medtrum_disconnect(void *ctx)
{
    uint16_t c = medtrum_conn;
    if (c != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(c, BLE_ERR_REM_USER_CONN_TERM);
    }
}

bool ble_medtrum_connected(void)
{
    return medtrum_conn != BLE_HS_CONN_HANDLE_NONE;
}

void ble_medtrum_advertise(void *ctx, bool on, const uint8_t sn[4])
{
    xSemaphoreTake(adv_mu, portMAX_DELAY);
    medtrum_adv_on = on;
    if (sn) {
        memcpy(medtrum_adv_sn, sn, 4);
    }
    xSemaphoreGive(adv_mu);
    ESP_LOGI(TAG, "Medtrum %s, serial %02X%02X%02X%02X", on ? "advertising" : "not advertising",
             medtrum_adv_sn[0], medtrum_adv_sn[1], medtrum_adv_sn[2], medtrum_adv_sn[3]);
    medtrum_advertise_now();
}
