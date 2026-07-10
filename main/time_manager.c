#include "time_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "nvs.h"

#include "telemetry_pipeline.h"
#include "wifi_manager.h"

#define TIME_SYNC_TASK_STACK 4096
#define TIME_SYNC_TASK_PRIORITY 4
#define TIME_SYNC_RETRY_INITIAL_MS 10000
#define TIME_SYNC_RETRY_MAX_MS 300000
#define TIME_SNAPSHOT_INTERVAL_MS (30 * 60 * 1000)
#define TIME_SYNC_REFRESH_MS (6 * 60 * 60 * 1000)
#define TIME_SYNC_WAIT_MS 30000
#define TIME_VALID_UNIX_EPOCH 1704067200LL
#define TIME_NAMESPACE "time"
#define TIME_SNAPSHOT_EPOCH_KEY "snap_epoch"

static const char *TAG = "time_manager";

static TaskHandle_t s_time_task = NULL;
static bool s_sntp_initialized = false;
static bool s_time_synced = false;
static bool s_time_estimated = false;
static uint32_t s_retry_delay_ms = TIME_SYNC_RETRY_INITIAL_MS;
static TickType_t s_next_retry_tick = 0;
static uint64_t s_last_sync_epoch_ms = 0;
static uint64_t s_last_snapshot_epoch_ms = 0;
static char s_status[96] = "not synchronized";

static uint32_t next_retry_delay_ms(uint32_t current_ms)
{
    if (current_ms >= TIME_SYNC_RETRY_MAX_MS) {
        return TIME_SYNC_RETRY_MAX_MS;
    }

    uint32_t next_ms = current_ms * 2;
    if (next_ms > TIME_SYNC_RETRY_MAX_MS) {
        next_ms = TIME_SYNC_RETRY_MAX_MS;
    }
    return next_ms;
}

static bool system_time_valid(void)
{
    time_t now = 0;
    time(&now);
    return now >= TIME_VALID_UNIX_EPOCH;
}

static bool save_time_snapshot(uint64_t epoch_ms)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(TIME_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return false;
    }

    err = nvs_set_u64(nvs, TIME_SNAPSHOT_EPOCH_KEY, epoch_ms);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err == ESP_OK;
}

static bool load_time_snapshot(uint64_t *epoch_ms)
{
    if (!epoch_ms) {
        return false;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(TIME_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return false;
    }

    err = nvs_get_u64(nvs, TIME_SNAPSHOT_EPOCH_KEY, epoch_ms);
    nvs_close(nvs);
    return err == ESP_OK && *epoch_ms >= ((uint64_t)TIME_VALID_UNIX_EPOCH * 1000ULL);
}

static void maybe_persist_snapshot(uint64_t epoch_ms)
{
    if (!s_time_synced) {
        return;
    }

    if (s_last_snapshot_epoch_ms != 0 &&
        (epoch_ms - s_last_snapshot_epoch_ms) < TIME_SNAPSHOT_INTERVAL_MS) {
        return;
    }

    if (save_time_snapshot(epoch_ms)) {
        s_last_snapshot_epoch_ms = epoch_ms;
        ESP_LOGI(TAG, "Saved trusted time snapshot");
    }
}

static void restore_time_from_snapshot_if_available(void)
{
    if (system_time_valid()) {
        return;
    }

    uint64_t snapshot_epoch_ms = 0;
    if (!load_time_snapshot(&snapshot_epoch_ms)) {
        return;
    }

    struct timeval tv = {
        .tv_sec = (time_t)(snapshot_epoch_ms / 1000ULL),
        .tv_usec = (suseconds_t)((snapshot_epoch_ms % 1000ULL) * 1000ULL),
    };
    settimeofday(&tv, NULL);

    s_time_synced = false;
    s_time_estimated = true;
    s_last_snapshot_epoch_ms = snapshot_epoch_ms;
    snprintf(s_status, sizeof(s_status), "estimated from saved snapshot");
    ESP_LOGI(TAG, "Restored estimated time from saved snapshot");
}

static void update_sync_state_from_system_time(bool trusted_sync)
{
    if (!system_time_valid()) {
        s_time_synced = false;
        return;
    }

    struct timeval tv = {0};
    gettimeofday(&tv, NULL);
    s_last_sync_epoch_ms = ((uint64_t)tv.tv_sec * 1000ULL) + (uint64_t)(tv.tv_usec / 1000ULL);
    s_time_synced = trusted_sync;
    s_time_estimated = !trusted_sync;

    struct tm tm_utc = {0};
    gmtime_r(&tv.tv_sec, &tm_utc);
    if (trusted_sync) {
        strftime(s_status, sizeof(s_status), "synced %Y-%m-%d %H:%M:%S UTC", &tm_utc);
        maybe_persist_snapshot(s_last_sync_epoch_ms);
    } else {
        strftime(s_status, sizeof(s_status), "estimated %Y-%m-%d %H:%M:%S UTC", &tm_utc);
    }
}

static void time_sync_notification_cb(struct timeval *tv)
{
    (void)tv;
    update_sync_state_from_system_time(true);
    ESP_LOGI(TAG, "System time synchronized");
    telemetry_pipeline_handle_time_sync();
}

static void init_sntp_once(void)
{
    if (s_sntp_initialized) {
        return;
    }

    setenv("TZ", "UTC0", 1);
    tzset();

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    config.start = false;
    config.wait_for_sync = true;
    config.sync_cb = time_sync_notification_cb;

    ESP_ERROR_CHECK(esp_netif_sntp_init(&config));
    s_sntp_initialized = true;
}

static void schedule_retry_from_now(uint32_t delay_ms)
{
    s_next_retry_tick = xTaskGetTickCount() + pdMS_TO_TICKS(delay_ms);
}

static bool retry_due(TickType_t now)
{
    return s_next_retry_tick == 0 || now >= s_next_retry_tick;
}

static bool refresh_due(uint64_t now_epoch_ms)
{
    return s_last_sync_epoch_ms == 0 || (now_epoch_ms - s_last_sync_epoch_ms) >= TIME_SYNC_REFRESH_MS;
}

static void time_sync_task(void *arg)
{
    (void)arg;

    init_sntp_once();
    restore_time_from_snapshot_if_available();
    update_sync_state_from_system_time(false);

    while (true) {
        TickType_t now_tick = xTaskGetTickCount();
        uint64_t now_epoch_ms = time_manager_get_epoch_ms();

        if (!wifi_manager_is_connected()) {
            if (!s_time_synced) {
                strlcpy(s_status, s_time_estimated ? "estimated time, waiting for Wi-Fi" : "waiting for Wi-Fi",
                        sizeof(s_status));
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if ((!s_time_synced || refresh_due(now_epoch_ms)) && retry_due(now_tick)) {
            strlcpy(s_status, "syncing time", sizeof(s_status));
            ESP_LOGI(TAG, "Starting SNTP time synchronization");

            esp_netif_sntp_start();
            esp_err_t err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(TIME_SYNC_WAIT_MS));
            if ((err == ESP_OK || err == ESP_ERR_NOT_FINISHED) && system_time_valid()) {
                update_sync_state_from_system_time(true);
                s_retry_delay_ms = TIME_SYNC_RETRY_INITIAL_MS;
                s_next_retry_tick = 0;
            } else {
                s_time_synced = false;
                snprintf(s_status, sizeof(s_status), "sync failed: %s", esp_err_to_name(err));
                ESP_LOGW(TAG, "%s", s_status);
                schedule_retry_from_now(s_retry_delay_ms);
                s_retry_delay_ms = next_retry_delay_ms(s_retry_delay_ms);
            }
        }

        if (s_time_synced) {
            maybe_persist_snapshot(now_epoch_ms);
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void time_manager_init(void)
{
    if (s_time_task) {
        return;
    }

    xTaskCreate(time_sync_task, "time_sync", TIME_SYNC_TASK_STACK, NULL,
                TIME_SYNC_TASK_PRIORITY, &s_time_task);
}

bool time_manager_is_synchronized(void)
{
    if (!s_time_synced && system_time_valid()) {
        update_sync_state_from_system_time(false);
    }
    return s_time_synced;
}

bool time_manager_is_estimated(void)
{
    return s_time_estimated && !s_time_synced;
}

uint64_t time_manager_get_epoch_ms(void)
{
    struct timeval tv = {0};
    gettimeofday(&tv, NULL);
    return ((uint64_t)tv.tv_sec * 1000ULL) + (uint64_t)(tv.tv_usec / 1000ULL);
}

void time_manager_get_status(char *buf, size_t len)
{
    if (!buf || len == 0) {
        return;
    }
    strlcpy(buf, s_status, len);
}
