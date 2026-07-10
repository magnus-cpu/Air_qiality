#include "sd_card.h"

#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>
#include <string.h>
#include <unistd.h>

#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "wifi_manager.h"

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
#define SD_CARD_BUS_WIDTH 1
#define SD_CARD_MAX_FREQ_KHZ SDMMC_FREQ_PROBING
#define SD_CARD_WRITE_LOCKOUT_RECOVERY_MS 120000

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
static uint32_t s_active_io = 0;
static bool s_unmount_deferred = false;
static esp_reset_reason_t s_reset_reason = ESP_RST_UNKNOWN;
static bool s_write_lockout = false;
static TickType_t s_write_lockout_since_tick = 0;
static bool s_logged_fsync_fallback = false;

static void update_mounted_status(void);
static void maybe_release_write_lockout_locked(void);

static const char *reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_UNKNOWN: return "unknown";
    case ESP_RST_POWERON: return "poweron";
    case ESP_RST_EXT: return "external";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt_wdt";
    case ESP_RST_TASK_WDT: return "task_wdt";
    case ESP_RST_WDT: return "wdt";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "sdio";
    default: return "other";
    }
}

static void sanitize_boot_value(char *value)
{
    if (!value) {
        return;
    }

    for (size_t i = 0; value[i] != '\0'; i++) {
        if (value[i] == '\r' || value[i] == '\n') {
            value[i] = ' ';
        }
    }
}

static void write_boot_marker_locked(void)
{
    if (!s_mounted || !s_card) {
        return;
    }

    s_active_io++;
    update_mounted_status();

    FILE *f = fopen(SD_CARD_MOUNT_POINT "/boot.txt", "a");
    if (!f) {
        ESP_LOGW(TAG, "Failed to write boot marker");
        if (s_active_io > 0) {
            s_active_io--;
        }
        update_mounted_status();
        return;
    }

    char saved_ssid[33] = "";
    char saved_pass[65] = "";
    bool have_saved_wifi = wifi_manager_load_credentials(saved_ssid, sizeof(saved_ssid),
                                                         saved_pass, sizeof(saved_pass));
    sanitize_boot_value(saved_ssid);

    fprintf(f, "boot reset_reason=%s brownout=%d\n",
            reset_reason_name(s_reset_reason),
            s_reset_reason == ESP_RST_BROWNOUT ? 1 : 0);
    fprintf(f, "wifi saved=%d connected=%d ssid=%s\n",
            have_saved_wifi ? 1 : 0,
            wifi_manager_is_connected() ? 1 : 0,
            have_saved_wifi ? saved_ssid : "-");
    sd_card_sync_file(f);
    fclose(f);
    if (s_active_io > 0) {
        s_active_io--;
    }
    update_mounted_status();
}

static void update_mounted_status(void)
{
    if (s_write_lockout) {
        TickType_t now = xTaskGetTickCount();
        uint32_t remaining_ms = 0;
        if (s_write_lockout_since_tick != 0) {
            TickType_t elapsed = now - s_write_lockout_since_tick;
            uint32_t elapsed_ms = (uint32_t)(elapsed * portTICK_PERIOD_MS);
            remaining_ms = elapsed_ms >= SD_CARD_WRITE_LOCKOUT_RECOVERY_MS
                               ? 0
                               : (SD_CARD_WRITE_LOCKOUT_RECOVERY_MS - elapsed_ms);
        }

        snprintf(s_status, sizeof(s_status), "mounted read-only after brownout %lus io=%lu%s",
                 (unsigned long)(remaining_ms / 1000),
                 (unsigned long)s_active_io,
                 s_unmount_deferred ? " deferred-unmount" : "");
        return;
    }

    snprintf(s_status, sizeof(s_status), "mounted: %lluMB %u-bit %uKHz io=%lu%s",
             ((uint64_t)s_card->csd.capacity) * s_card->csd.sector_size / (1024ULL * 1024ULL),
             (unsigned)SD_CARD_BUS_WIDTH,
             (unsigned)SD_CARD_MAX_FREQ_KHZ,
             (unsigned long)s_active_io,
             s_unmount_deferred ? " deferred-unmount" : "");
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

static void maybe_release_write_lockout_locked(void)
{
    if (!s_write_lockout || s_write_lockout_since_tick == 0) {
        return;
    }

    TickType_t now = xTaskGetTickCount();
    TickType_t elapsed = now - s_write_lockout_since_tick;
    if ((elapsed * portTICK_PERIOD_MS) < SD_CARD_WRITE_LOCKOUT_RECOVERY_MS) {
        return;
    }

    s_write_lockout = false;
    s_write_lockout_since_tick = 0;
    ESP_LOGW(TAG, "SD card write protection auto-cleared after stable uptime");
    if (s_mounted && s_card) {
        update_mounted_status();
    }
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
    host.max_freq_khz = SD_CARD_MAX_FREQ_KHZ;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = SD_CARD_BUS_WIDTH;
    slot_config.clk = SD_CARD_PIN_CLK;
    slot_config.cmd = SD_CARD_PIN_CMD;
    slot_config.d0 = SD_CARD_PIN_D0;
#if SD_CARD_BUS_WIDTH == 4
    slot_config.d1 = SD_CARD_PIN_D1;
    slot_config.d2 = SD_CARD_PIN_D2;
    slot_config.d3 = SD_CARD_PIN_D3;
#else
    slot_config.d1 = GPIO_NUM_NC;
    slot_config.d2 = GPIO_NUM_NC;
    slot_config.d3 = GPIO_NUM_NC;
#endif
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
    s_unmount_deferred = false;
    update_mounted_status();
    if (log_success) {
        ESP_LOGI(TAG, "SD card %s at %s", s_status, SD_CARD_MOUNT_POINT);
        sdmmc_card_print_info(stdout, s_card);
        ESP_LOGW(TAG, "SD stability mode: %u-bit bus at %u KHz. Verify external pull-ups on CMD and DAT lines.",
                 (unsigned)SD_CARD_BUS_WIDTH, (unsigned)SD_CARD_MAX_FREQ_KHZ);
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
    s_unmount_deferred = false;
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

static esp_err_t sd_card_ensure_mounted_locked(void)
{
    TickType_t now = xTaskGetTickCount();
    esp_err_t err = ESP_OK;

    if (s_mounted && s_card && health_check_due(now)) {
        s_last_probe_tick = now;
        if (s_active_io > 0) {
            update_mounted_status();
            return ESP_OK;
        }

        err = sdmmc_get_status(s_card);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Card no longer responding: %s", esp_err_to_name(err));
            if (s_active_io > 0) {
                s_unmount_deferred = true;
                update_mounted_status();
            } else {
                sd_card_unmount_internal("card removed");
            }
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

    return s_mounted ? ESP_OK : err;
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
    s_reset_reason = esp_reset_reason();
    if (s_reset_reason == ESP_RST_BROWNOUT) {
        s_write_lockout = true;
        s_write_lockout_since_tick = xTaskGetTickCount();
        ESP_LOGW(TAG, "Last reset reason was brownout. Check 3V3 rail, SD supply dip, and card current spikes.");
        ESP_LOGW(TAG, "SD card write protection is enabled temporarily to preserve existing data.");
    } else {
        s_write_lockout = false;
        s_write_lockout_since_tick = 0;
    }
    esp_err_t err = sd_card_mount_internal(true, true);
    if (err == ESP_OK) {
        struct stat st = {0};
        if (stat(SD_CARD_MOUNT_POINT, &st) != 0) {
            ESP_LOGW(TAG, "Mount point %s stat failed after mount: errno=%d", SD_CARD_MOUNT_POINT, errno);
        }
        write_boot_marker_locked();
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

    maybe_release_write_lockout_locked();
    esp_err_t err = sd_card_ensure_mounted_locked();
    xSemaphoreGive(s_lock);
    return err;
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

    if (s_active_io > 0) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }

    if (s_write_lockout) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }

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
    write_boot_marker_locked();
    ESP_LOGI(TAG, "SD card formatted and remounted at %s", SD_CARD_MOUNT_POINT);

    xSemaphoreGive(s_lock);
    return ESP_OK;
}

bool sd_card_is_mounted(void)
{
    sd_card_ensure_mounted();
    return s_mounted;
}

esp_err_t sd_card_begin_io(void)
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

    maybe_release_write_lockout_locked();
    esp_err_t err = sd_card_ensure_mounted_locked();
    if (err == ESP_OK) {
        s_active_io++;
        update_mounted_status();
    }

    xSemaphoreGive(s_lock);
    return err;
}

void sd_card_end_io(void)
{
    if (!s_lock) {
        return;
    }

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return;
    }

    if (s_active_io > 0) {
        s_active_io--;
    }

    if (s_active_io == 0 && s_unmount_deferred) {
        sd_card_unmount_internal("card removed");
    } else if (s_mounted && s_card) {
        update_mounted_status();
    }

    xSemaphoreGive(s_lock);
}

esp_err_t sd_card_sync_file(FILE *f)
{
    if (!f) {
        return ESP_ERR_INVALID_ARG;
    }

    if (fflush(f) != 0) {
        return ESP_FAIL;
    }

    if (fsync(fileno(f)) == 0) {
        return ESP_OK;
    }

    int sync_errno = errno;
    if (sync_errno == EINVAL || sync_errno == ENOSYS) {
        if (!s_logged_fsync_fallback) {
            ESP_LOGW(TAG, "fsync unsupported by current FAT/VFS path, using fflush-only persistence");
            s_logged_fsync_fallback = true;
        }
        return ESP_OK;
    }

    ESP_LOGW(TAG, "fsync failed: errno=%d (%s)", sync_errno, strerror(sync_errno));
    return ESP_FAIL;
}

bool sd_card_write_allowed(void)
{
    if (!s_lock) {
        return !s_write_lockout;
    }

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return !s_write_lockout;
    }

    maybe_release_write_lockout_locked();
    bool allowed = !s_write_lockout;
    xSemaphoreGive(s_lock);
    return allowed;
}

void sd_card_get_mode(char *buf, size_t len)
{
    if (!buf || len == 0) {
        return;
    }

    snprintf(buf, len, "%u-bit @ %u KHz",
             (unsigned)SD_CARD_BUS_WIDTH,
             (unsigned)SD_CARD_MAX_FREQ_KHZ);
}

void sd_card_get_status(char *buf, size_t len)
{
    if (!buf || len == 0) {
        return;
    }
    sd_card_ensure_mounted();
    strlcpy(buf, s_status, len);
}
