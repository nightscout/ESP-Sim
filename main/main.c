/*
 * ESP-Sim: an Omnipod DASH pod simulator on the ESP32-S3, ported from the
 * Go simulator (github.com/jeremybarnum/pod, from LoopKit/pod).
 *
 * TEST EQUIPMENT ONLY: it delivers no insulin, and what it reports is made
 * up. Never use it, or an app paired to it, for therapy.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "app.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "esp-sim";

static dash_pod_t pod;
static dash_state_t restored;
static SemaphoreHandle_t pod_mu;

static int64_t now_ms(void *ctx)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static void lock(void *ctx)
{
    xSemaphoreTake(pod_mu, portMAX_DELAY);
}

static void unlock(void *ctx)
{
    xSemaphoreGive(pod_mu);
}

static void crash(void *ctx)
{
    ESP_LOGW(TAG, "rebooting, as the control panel asked");
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
}

static const dash_io_t io = {
    .next_event = ble_next_event,
    .notify_cmd = ble_notify_cmd,
    .notify_data = ble_notify_data,
    .disconnect = ble_disconnect,
    .advertise = ble_advertise,
    .now_ms = now_ms,
    .save = store_save_state,
    .lock = lock,
    .unlock = unlock,
    .crash = crash,
};

/* The firmware's build time, as a last resort for the clock. */
static time_t build_time(void)
{
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char mon[4] = {0};
    struct tm tm = {0};
    memcpy(mon, __DATE__, 3);
    tm.tm_mon = (int)((strstr(months, mon) - months) / 3);
    tm.tm_mday = atoi(__DATE__ + 4);
    tm.tm_year = atoi(__DATE__ + 7) - 1900;
    tm.tm_hour = atoi(__TIME__);
    tm.tm_min = atoi(__TIME__ + 3);
    tm.tm_sec = atoi(__TIME__ + 6);
    return mktime(&tm);
}

/* Without a network the clock starts at 1970. Pick up from the last save
 * (losing only the time the board was off) until NTP sets it. */
static void set_clock(bool have_state)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    if (tv.tv_sec > 1700000000) {
        return;
    }
    time_t t = build_time();
    if (have_state && restored.saved_ms / 1000 > t) {
        t = (time_t)(restored.saved_ms / 1000);
    }
    tv.tv_sec = t;
    tv.tv_usec = 0;
    settimeofday(&tv, NULL);
}

const dash_state_t *pod_state(void)
{
    return &pod.st;
}

static void pod_task(void *arg)
{
    dash_pod_run(&pod);
    vTaskDelete(NULL);
}

static void cgm_radio_task(void *arg)
{
    for (;;) {
        ble_cgm_tick();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void app_main(void)
{
    ESP_LOGW(TAG, "ESP-Sim DASH pod simulator. TEST EQUIPMENT: delivers nothing; never use for therapy.");

    store_init();
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    setenv("TZ", CONFIG_ESPSIM_TZ, 1);
    tzset();

    bool have = store_load_state(&restored);
    set_clock(have);
    pod_mu = xSemaphoreCreateMutex();
    dash_pod_init(&pod, &io, have ? &restored : NULL);
    if (have) {
        ESP_LOGI(TAG, "restored the %s pod: %s, progress %d, reservoir %.2f U",
                 pod.st.pod_type == DASH_POD_O5 ? "Omnipod 5" : "DASH", pod.st.paired ? "paired" : "not paired",
                 pod.st.progress, pod.st.reservoir * 0.05);
    } else {
        ESP_LOGI(TAG, "a new DASH pod: not paired, 150 U (the control panel switches it to Omnipod 5)");
        store_save_state(NULL, &pod.st);
    }

    /* The pump: an Omnipod pod, a YpsoPump or a Medtrum base. One at a time;
     * the panel switches between them, with a restart. */
    uint8_t pump = store_load_pump();
    if (pump == PUMP_YPSO) {
        ypso_app_start();
        char name[24];
        ypso_name(ypso_app_state(), name);
        ble_start(PUMP_YPSO, ypso_app_state()->fw, name);
    } else {
        ble_start(pump, 0, NULL); /* the pod, or the Medtrum base */
    }
    cgm_app_start();
    xTaskCreatePinnedToCore(cgm_radio_task, "cgmradio", 6144, NULL, 5, NULL, tskNO_AFFINITY);
    if (pump == PUMP_MEDTRUM) {
        medtrum_app_start();
    }
    panel_start(&pod);
    if (pump == PUMP_POD) {
        xTaskCreatePinnedToCore(pod_task, "pod", 12288, NULL, 5, NULL, tskNO_AFFINITY);
    }
}
