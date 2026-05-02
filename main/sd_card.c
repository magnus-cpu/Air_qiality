#include "sd_card.h"

#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#define SD_CARD_MOUNT_POINT "/sdcard"

// ESP32 HS2 / SDMMC slot wiring.
#define SD_CARD_PIN_CLK   GPIO_NUM_14
#define SD_CARD_PIN_CMD   GPIO_NUM_15
#define SD_CARD_PIN_D0    GPIO_NUM_2
#define SD_CARD_PIN_D1    GPIO_NUM_4
#define SD_CARD_PIN_D2    GPIO_NUM_12
#define SD_CARD_PIN_D3    GPIO_NUM_13
#define SD_CARD_HEALTH_CHECK_MS 3000
#define SD_CARD_REMOUNT_INITIAL_RETRY_MS 5000
#define SD_CARD_REMOUNT_MAX_RETRY_MS 60000

static const char *TAG = "sd_card";
static const char *SDMMC_COMMON_TAG = "sdmmc_common";
static const char *VFS_FAT_SDMMC_TAG = "vfs_fat_sdmmc";
static sdmmc_card_t *s_card = NULL;
static bool s_mounted = false;
static char s_status[96] = "not initialized";
static SemaphoreHandle_t s_lock = NULL;
static TickType_t s_last_probe_tick = 0;
static TickType_t s_next_retry_tick = 0;
static uint32_t s_retry_delay_ms = SD_CARD_REMOUNT_INITIAL_RETRY_MS;
static uint32_t s_failed_mounts = 0;

static void write_boot_marker(void)
{
    FILE *f = fopen(SD_CARD_MOUNT_POINT "/boot.txt", "a");
    if (!f) {
        ESP_LOGW(TAG, "Failed to write boot marker");
        return;
    }
    fprintf(f, "Air quality firmware booted with SD card mounted\n");
    fclose(f);
}

static void update_mounted_status(void)
{
    snprintf(s_status, sizeof(s_status), "mounted: %lluMB",
             ((uint64_t)s_card->csd.capacity) * s_card->csd.sector_size / (1024ULL * 1024ULL));
}

static uint32_t next_retry_delay_ms(uint32_t current_ms)
{
    if (current_ms >= SD_CARD_REMOUNT_MAX_RETRY_MS) {
        return SD_CARD_REMOUNT_MAX_RETRY_MS;
    }

    uint32_t next_ms = current_ms * 2;
    if (next_ms > SD_CARD_REMOUNT_MAX_RETRY_MS) {
        next_ms = SD_CARD_REMOUNT_MAX_RETRY_MS;
    }
    return next_ms;
}

static void schedule_next_retry(TickType_t now)
{
    s_next_retry_tick = now + pdMS_TO_TICKS(s_retry_delay_ms);
    s_retry_delay_ms = next_retry_delay_ms(s_retry_delay_ms);
}

static esp_err_t sd_card_mount_internal(bool log_success, bool log_failure)
{
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 10,
        .allocation_unit_size = 16 * 1024,
        .disk_status_check_enable = true,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 4;
    slot_config.clk = SD_CARD_PIN_CLK;
    slot_config.cmd = SD_CARD_PIN_CMD;
    slot_config.d0 = SD_CARD_PIN_D0;
    slot_config.d1 = SD_CARD_PIN_D1;
    slot_config.d2 = SD_CARD_PIN_D2;
    slot_config.d3 = SD_CARD_PIN_D3;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    TickType_t now = xTaskGetTickCount();
    s_last_probe_tick = now;
    if (s_mounted && s_card) {
        return ESP_OK;
    }

    esp_log_level_t sdmmc_common_level = ESP_LOG_INFO;
    esp_log_level_t vfs_fat_level = ESP_LOG_INFO;
    if (!log_failure) {
        sdmmc_common_level = esp_log_level_get(SDMMC_COMMON_TAG);
        vfs_fat_level = esp_log_level_get(VFS_FAT_SDMMC_TAG);
        esp_log_level_set(SDMMC_COMMON_TAG, ESP_LOG_NONE);
        esp_log_level_set(VFS_FAT_SDMMC_TAG, ESP_LOG_NONE);
    }

    esp_err_t err = esp_vfs_fat_sdmmc_mount(SD_CARD_MOUNT_POINT, &host, &slot_config, &mount_config, &s_card);
    if (!log_failure) {
        esp_log_level_set(SDMMC_COMMON_TAG, sdmmc_common_level);
        esp_log_level_set(VFS_FAT_SDMMC_TAG, vfs_fat_level);
    }

    if (err != ESP_OK) {
        s_card = NULL;
        s_mounted = false;
        snprintf(s_status, sizeof(s_status), "mount failed: %s", esp_err_to_name(err));
        s_failed_mounts++;
        uint32_t retry_in_ms = s_retry_delay_ms;
        schedule_next_retry(now);
        if (log_failure) {
            ESP_LOGW(TAG, "%s", s_status);
        } else {
            ESP_LOGI(TAG, "SD card not present, retrying in %lu s",
                     (unsigned long)(retry_in_ms / 1000));
        }
        return err;
    }

    s_mounted = true;
    s_failed_mounts = 0;
    s_retry_delay_ms = SD_CARD_REMOUNT_INITIAL_RETRY_MS;
    s_next_retry_tick = 0;
    update_mounted_status();
    if (log_success) {
        ESP_LOGI(TAG, "SD card %s at %s", s_status, SD_CARD_MOUNT_POINT);
        sdmmc_card_print_info(stdout, s_card);
    }
    return ESP_OK;
}

static void sd_card_unmount_internal(const char *reason)
{
    if (!s_mounted || !s_card) {
        s_mounted = false;
        s_card = NULL;
        return;
    }

    esp_err_t err = esp_vfs_fat_sdcard_unmount(SD_CARD_MOUNT_POINT, s_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "unmount failed: %s", esp_err_to_name(err));
    }

    s_mounted = false;
    s_card = NULL;
    snprintf(s_status, sizeof(s_status), "%s", reason ? reason : "card unavailable");
}

static bool remount_due(TickType_t now)
{
    return s_next_retry_tick == 0 || now >= s_next_retry_tick;
}

static bool health_check_due(TickType_t now)
{
    return (now - s_last_probe_tick) >= pdMS_TO_TICKS(SD_CARD_HEALTH_CHECK_MS);
}

void sd_card_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    if (!s_lock) {
        snprintf(s_status, sizeof(s_status), "lock create failed");
        ESP_LOGE(TAG, "%s", s_status);
        return;
    }

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "Timed out waiting for SD card lock");
        return;
    }

    s_next_retry_tick = 0;
    s_retry_delay_ms = SD_CARD_REMOUNT_INITIAL_RETRY_MS;
    esp_err_t err = sd_card_mount_internal(true, true);
    if (err == ESP_OK) {
        struct stat st = {0};
        if (stat(SD_CARD_MOUNT_POINT, &st) != 0) {
            ESP_LOGW(TAG, "Mount point %s stat failed after mount: errno=%d", SD_CARD_MOUNT_POINT, errno);
        }
        write_boot_marker();
    }
    xSemaphoreGive(s_lock);
}

esp_err_t sd_card_ensure_mounted(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    TickType_t now = xTaskGetTickCount();
    esp_err_t err = ESP_OK;

    if (s_mounted && s_card && health_check_due(now)) {
        s_last_probe_tick = now;
        err = sdmmc_get_status(s_card);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Card no longer responding: %s", esp_err_to_name(err));
            sd_card_unmount_internal("card removed");
        } else {
            update_mounted_status();
        }
    }

    if (!s_mounted && remount_due(now)) {
        err = sd_card_mount_internal(false, false);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "SD card remounted at %s", SD_CARD_MOUNT_POINT);
            sdmmc_card_print_info(stdout, s_card);
        }
    } else if (!s_mounted) {
        err = ESP_ERR_NOT_FOUND;
    }

    xSemaphoreGive(s_lock);
    return s_mounted ? ESP_OK : err;
}

esp_err_t sd_card_format(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(3000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = ESP_OK;
    TickType_t now = xTaskGetTickCount();

    if (!s_mounted || !s_card) {
        err = sd_card_mount_internal(false, true);
        if (err != ESP_OK) {
            xSemaphoreGive(s_lock);
            return err;
        }
    }

    ESP_LOGW(TAG, "Formatting SD card at %s as FAT filesystem", SD_CARD_MOUNT_POINT);
    snprintf(s_status, sizeof(s_status), "formatting...");

    err = esp_vfs_fat_sdcard_format(SD_CARD_MOUNT_POINT, s_card);
    if (err != ESP_OK) {
        snprintf(s_status, sizeof(s_status), "format failed: %s", esp_err_to_name(err));
        ESP_LOGW(TAG, "%s", s_status);
        s_next_retry_tick = now + pdMS_TO_TICKS(SD_CARD_REMOUNT_INITIAL_RETRY_MS);
        xSemaphoreGive(s_lock);
        return err;
    }

    s_last_probe_tick = now;
    s_retry_delay_ms = SD_CARD_REMOUNT_INITIAL_RETRY_MS;
    s_next_retry_tick = 0;
    s_failed_mounts = 0;
    update_mounted_status();
    write_boot_marker();
    ESP_LOGI(TAG, "SD card formatted and remounted at %s", SD_CARD_MOUNT_POINT);

    xSemaphoreGive(s_lock);
    return ESP_OK;
}

bool sd_card_is_mounted(void)
{
    sd_card_ensure_mounted();
    return s_mounted;
}

void sd_card_get_status(char *buf, size_t len)
{
    if (!buf || len == 0) {
        return;
    }
    sd_card_ensure_mounted();
    strlcpy(buf, s_status, len);
}
