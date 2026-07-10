#include "telemetry_pipeline.h"

#include <errno.h>
#include <inttypes.h>
#include <dirent.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

#include "device_registry.h"
#include "heater.h"
#include "sd_card.h"
#include "sensor.h"
#include "site_config.h"
#include "time_manager.h"
#include "wifi_manager.h"

#define TELEMETRY_SAMPLE_INTERVAL_MS 5000
#define TELEMETRY_STORAGE_RETRY_MS 5000
#define TELEMETRY_STORAGE_IDLE_WAIT_MS 250
#define TELEMETRY_STORAGE_BATCH_SIZE 8
#define TELEMETRY_STORAGE_BATCH_WINDOW_MS 2000
#define TELEMETRY_FILE_REFRESH_MS 60000
#define TELEMETRY_UPLOAD_CHECK_MS 5000
#define TELEMETRY_QUEUE_LEN 64
#define TELEMETRY_RAM_BACKLOG_LEN 128
#define TELEMETRY_LIVE_UPLOAD_QUEUE_LEN 32
#define TELEMETRY_DEFAULT_FILE SD_CARD_MOUNT_POINT "/tm_unsyn.csv"
#define TELEMETRY_UPLOAD_STATE_FILE SD_CARD_MOUNT_POINT "/telupl.sta"
#define TELEMETRY_UPLOAD_URL "http://192.168.43.121:3000/api/telemetry"
#define TELEMETRY_UPLOAD_API_KEY ""
#define TELEMETRY_REGISTRATION_SECRET "52711f6eb839180f983c8ebe9f756d379f107798547c6fd02c18a70bdba7c60b"
#define TELEMETRY_UPLOAD_BATCH_SIZE 20
#define TELEMETRY_UPLOAD_MAX_BODY 4096
#define TELEMETRY_UPLOAD_STREAM_BUFFER 4096
#define TELEMETRY_HEALTH_CHECK_MS 15000
#define TELEMETRY_WARN_INTERVAL_US (30LL * 1000LL * 1000LL)
#define HEATER_WARMUP_SECONDS 60

static const char *TAG = "telemetry_pipeline";

static QueueHandle_t s_sample_queue = NULL;
static QueueHandle_t s_live_upload_queue = NULL;
static SemaphoreHandle_t s_latest_lock = NULL;
static SemaphoreHandle_t s_file_lock = NULL;
static SemaphoreHandle_t s_backlog_lock = NULL;
static TaskHandle_t s_sampler_task = NULL;
static TaskHandle_t s_storage_task = NULL;
static TaskHandle_t s_upload_task = NULL;
static TaskHandle_t s_file_manager_task = NULL;
static telemetry_sample_t s_ram_backlog[TELEMETRY_RAM_BACKLOG_LEN];
static size_t s_ram_backlog_head = 0;
static size_t s_ram_backlog_tail = 0;
static size_t s_ram_backlog_count = 0;
static telemetry_sample_t s_latest_sample = {0};
static bool s_have_latest_sample = false;
static uint32_t s_enqueued_samples = 0;
static uint32_t s_buffered_samples = 0;
static uint32_t s_persisted_samples = 0;
static uint32_t s_dropped_samples = 0;
static uint32_t s_skipped_no_sd_samples = 0;
static uint32_t s_skipped_write_locked_samples = 0;
static uint32_t s_uploaded_samples = 0;
static uint32_t s_upload_failures = 0;
static int64_t s_last_queue_warn_us = 0;
static int64_t s_last_backlog_warn_us = 0;
static int64_t s_last_no_sd_warn_us = 0;
static int64_t s_last_write_lock_warn_us = 0;
static uint64_t s_next_sample_id = 1;
static uint64_t s_boot_id = 0;
static char s_upload_status[96] = "upload disabled";
static char s_today_file_path[96] = TELEMETRY_DEFAULT_FILE;
static char s_telemetry_mode[24] = "startup";
static bool s_last_health_ok = false;
static TickType_t s_last_health_check_tick = 0;
static bool s_server_ready = false;

typedef struct {
    uint64_t start_offset;
    uint64_t next_offset;
    size_t records_in_batch;
    bool eof;
} upload_batch_result_t;

typedef struct {
    char path[96];
    uint64_t offset;
} upload_state_t;

static bool refresh_today_file_cache_locked(bool create_file);
static bool ensure_csv_header(FILE *f);
static bool is_telemetry_file_name(const char *name);
static bool select_upload_file_locked(char *path, size_t len);
static bool append_sample_to_sd(const telemetry_sample_t *sample);
static bool append_samples_to_sd_locked(const telemetry_sample_t *samples, size_t count);
static void discard_queued_samples(size_t *discarded_count);
static bool build_telemetry_file_path(char *path, size_t path_len, const struct tm *tm_now);
static bool build_upload_batch_json_from_samples(const telemetry_sample_t *samples, size_t count, char *body, size_t body_len);
static bool find_latest_dated_telemetry_file(char *path, size_t len);
static int compare_dated_tm_file_names(const char *lhs, const char *rhs);
static bool path_equals_ignore_case(const char *lhs, const char *rhs);
static bool file_has_records_after_offset(const char *path, uint64_t offset);
static bool truncate_telemetry_file_to_header_locked(const char *path);
static void clear_upload_state_file(void);
static bool post_upload_file_stream(const char *path,
                                    upload_batch_result_t *result,
                                    int *status_code);

static const char *path_basename(const char *path)
{
    if (!path) {
        return "";
    }

    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static bool normalize_upload_url(const char *input, char *output, size_t output_len)
{
    if (!input || input[0] == '\0' || !output || output_len == 0) {
        return false;
    }

    if (strstr(input, "://")) {
        strlcpy(output, input, output_len);
        return output[0] != '\0';
    }

    int written = snprintf(output, output_len, "http://%s", input);
    return written > 0 && (size_t)written < output_len;
}

static bool build_upload_base_url(const char *upload_url, char *base_url, size_t base_url_len)
{
    char normalized_url[192];
    if (!normalize_upload_url(upload_url, normalized_url, sizeof(normalized_url))) {
        return false;
    }

    const char *scheme = strstr(normalized_url, "://");
    if (!scheme) {
        return false;
    }

    const char *host_start = scheme + 3;
    const char *path_start = strchr(host_start, '/');
    size_t copy_len = path_start ? (size_t)(path_start - normalized_url) : strlen(normalized_url);
    if (copy_len >= base_url_len) {
        return false;
    }

    memcpy(base_url, normalized_url, copy_len);
    base_url[copy_len] = '\0';
    return true;
}

static bool build_healthcheck_url(char *health_url, size_t health_url_len)
{
    char base_url[160];
    if (!build_upload_base_url(TELEMETRY_UPLOAD_URL, base_url, sizeof(base_url))) {
        return false;
    }

    int written = snprintf(health_url, health_url_len, "%s/health", base_url);
    return written > 0 && (size_t)written < health_url_len;
}

static bool build_telemetry_file_path(char *path, size_t path_len, const struct tm *tm_now)
{
    if (!path || path_len == 0 || !tm_now) {
        return false;
    }

    int year = (tm_now->tm_year + 1900) % 100;
    int month = tm_now->tm_mon + 1;
    int day = tm_now->tm_mday;
    int written = snprintf(path, path_len, SD_CARD_MOUNT_POINT "/tm%02d%02d%02d.csv",
                           year, month, day);
    return written > 0 && (size_t)written < path_len;
}

static int compare_dated_tm_file_names(const char *lhs, const char *rhs)
{
    if (!lhs || !rhs) {
        return 0;
    }

    int date_cmp = strncmp(lhs + 2, rhs + 2, 6);
    if (date_cmp != 0) {
        return date_cmp;
    }

    return strcasecmp(lhs, rhs);
}

static bool path_equals_ignore_case(const char *lhs, const char *rhs)
{
    if (!lhs || !rhs) {
        return false;
    }

    return strcasecmp(lhs, rhs) == 0;
}

static bool find_latest_dated_telemetry_file(char *path, size_t len)
{
    if (!path || len == 0 || sd_card_ensure_mounted() != ESP_OK) {
        return false;
    }

    path[0] = '\0';
    if (sd_card_begin_io() != ESP_OK) {
        return false;
    }

    DIR *dir = opendir(SD_CARD_MOUNT_POINT);
    if (!dir) {
        sd_card_end_io();
        return false;
    }

    char best_name[64] = {0};
    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        const char *name = entry->d_name;
        size_t name_len = strlen(name);
        bool is_dated_tm_file = name_len == 12 &&
                                strncasecmp(name, "tm", 2) == 0 &&
                                strcasecmp(name + 8, ".csv") == 0 &&
                                isdigit((unsigned char)name[2]) &&
                                isdigit((unsigned char)name[3]) &&
                                isdigit((unsigned char)name[4]) &&
                                isdigit((unsigned char)name[5]) &&
                                isdigit((unsigned char)name[6]) &&
                                isdigit((unsigned char)name[7]);
        if (!is_dated_tm_file) {
            continue;
        }

        if (best_name[0] == '\0' || compare_dated_tm_file_names(name, best_name) > 0) {
            strlcpy(best_name, name, sizeof(best_name));
        }
    }

    closedir(dir);
    sd_card_end_io();

    if (best_name[0] == '\0') {
        return false;
    }

    char canonical_name[16];
    int written = snprintf(canonical_name, sizeof(canonical_name), "tm%.6s.csv", best_name + 2);
    if (written <= 0 || (size_t)written >= sizeof(canonical_name)) {
        return false;
    }

    strlcpy(path, SD_CARD_MOUNT_POINT "/", len);
    strlcat(path, canonical_name, len);
    return true;
}

static void sanitize_csv_field(char *value)
{
    if (!value) {
        return;
    }

    for (size_t i = 0; value[i] != '\0'; i++) {
        if (value[i] == ',' || value[i] == '\r' || value[i] == '\n') {
            value[i] = ' ';
        }
    }
}

static void fill_sample_location(telemetry_sample_t *sample)
{
    if (!sample) {
        return;
    }

    site_config_t site = {0};
    if (site_config_load(&site) != ESP_OK) {
        return;
    }

    strlcpy(sample->location_name, site.location_name, sizeof(sample->location_name));
    strlcpy(sample->latitude, site.latitude, sizeof(sample->latitude));
    strlcpy(sample->longitude, site.longitude, sizeof(sample->longitude));
    sanitize_csv_field(sample->location_name);
    sanitize_csv_field(sample->latitude);
    sanitize_csv_field(sample->longitude);
}

static uint64_t generate_boot_id(void)
{
    return ((uint64_t)esp_random() << 32) | (uint64_t)esp_random();
}

static void fill_sample_uuid(telemetry_sample_t *sample, uint64_t sequence)
{
    if (!sample) {
        return;
    }

    char device_id[32] = "unknown";
    device_registry_get_device_id(device_id, sizeof(device_id));
    snprintf(sample->sample_uuid, sizeof(sample->sample_uuid),
             "%s-%016" PRIX64 "-%016" PRIX64,
             device_id, s_boot_id, sequence);
}

static telemetry_sample_t make_sample(void)
{
    sensor_snapshot_t sensor_snapshot = {0};
    sensor_read_all(&sensor_snapshot);
    uint64_t sequence = s_next_sample_id++;

    telemetry_sample_t sample = {
        .sample_id = sequence,
        .timestamp_ms = time_manager_get_epoch_ms(),
        .uptime_ms = (uint64_t)(esp_timer_get_time() / 1000),
        .nh3_raw = sensor_snapshot.nh3.raw,
        .red_raw = sensor_snapshot.red.raw,
        .ox_raw = sensor_snapshot.ox.raw,
        .nh3_mv = sensor_snapshot.nh3.mv,
        .red_mv = sensor_snapshot.red.mv,
        .ox_mv = sensor_snapshot.ox.mv,
        .nh3_res_ohms = sensor_snapshot.nh3.resistance_ohms,
        .red_res_ohms = sensor_snapshot.red.resistance_ohms,
        .ox_res_ohms = sensor_snapshot.ox.resistance_ohms,
        .nh3_ppm = sensor_snapshot.nh3.ppm,
        .red_ppm = sensor_snapshot.red.ppm,
        .ox_ppm = sensor_snapshot.ox.ppm,
        .nh3_ppm_valid = sensor_snapshot.nh3.ppm_valid,
        .red_ppm_valid = sensor_snapshot.red.ppm_valid,
        .ox_ppm_valid = sensor_snapshot.ox.ppm_valid,
        .heater_on = heater_get(),
        .warmup = heater_is_warming(HEATER_WARMUP_SECONDS),
        .since_change = heater_seconds_since_change(),
        .time_synced = time_manager_is_synchronized(),
        .wifi_connected = wifi_manager_is_connected(),
    };
    fill_sample_uuid(&sample, sequence);
    fill_sample_location(&sample);
    return sample;
}

static bool upload_enabled(void)
{
    return TELEMETRY_UPLOAD_URL[0] != '\0';
}

static bool ensure_csv_header(FILE *f)
{
    if (!f) {
        return false;
    }

    long pos = ftell(f);
    if (pos != 0) {
        return true;
    }

    return fprintf(f,
                   "sample_uuid,sample_id,timestamp_ms,uptime_ms,location_name,latitude,longitude,time_synced,nh3_raw,red_raw,ox_raw,nh3_mv,red_mv,ox_mv,nh3_res_ohms,red_res_ohms,ox_res_ohms,nh3_ppm,red_ppm,ox_ppm,nh3_ppm_valid,red_ppm_valid,ox_ppm_valid,heater_on,warmup,since_change\n") > 0;
}

static bool create_current_day_file_locked(void)
{
    if (!sd_card_write_allowed()) {
        char sd_status[96] = {0};
        sd_card_get_status(sd_status, sizeof(sd_status));
        ESP_LOGW(TAG, "Skipping file create for %s because SD writes are disabled: %s",
                 s_today_file_path, sd_status);
        return false;
    }

    esp_err_t begin_err = sd_card_begin_io();
    if (begin_err != ESP_OK) {
        char sd_status[96] = {0};
        sd_card_get_status(sd_status, sizeof(sd_status));
        ESP_LOGW(TAG, "Failed to begin SD I/O for %s: %s, sd=%s",
                 s_today_file_path, esp_err_to_name(begin_err), sd_status);
        return false;
    }

    errno = 0;
    FILE *f = fopen(s_today_file_path, "a");
    if (!f) {
        int open_errno = errno;
        char sd_status[96] = {0};
        sd_card_get_status(sd_status, sizeof(sd_status));
        ESP_LOGW(TAG, "Failed to open/create %s: errno=%d (%s), sd=%s",
                 s_today_file_path, open_errno, strerror(open_errno), sd_status);
        sd_card_end_io();
        return false;
    }

    bool ok = ensure_csv_header(f);
    if (!ok) {
        ESP_LOGW(TAG, "Failed to write CSV header for %s", s_today_file_path);
    }
    if (ok) {
        ok = sd_card_sync_file(f) == ESP_OK;
        if (!ok) {
            ESP_LOGW(TAG, "Failed to sync created telemetry file %s", s_today_file_path);
        }
    }
    fclose(f);
    sd_card_end_io();
    return ok;
}

static bool truncate_telemetry_file_to_header_locked(const char *path)
{
    if (!path || path[0] == '\0' || !sd_card_write_allowed()) {
        return false;
    }

    if (sd_card_begin_io() != ESP_OK) {
        return false;
    }

    errno = 0;
    FILE *f = fopen(path, "w");
    if (!f) {
        int open_errno = errno;
        ESP_LOGW(TAG, "Failed to truncate %s: errno=%d (%s)",
                 path, open_errno, strerror(open_errno));
        sd_card_end_io();
        return false;
    }

    bool ok = ensure_csv_header(f);
    if (ok) {
        ok = sd_card_sync_file(f) == ESP_OK;
    }
    fclose(f);
    sd_card_end_io();

    if (!ok) {
        ESP_LOGW(TAG, "Failed to rewrite CSV header for %s", path);
    }
    return ok;
}

static void clear_upload_state_file(void)
{
    if (!sd_card_write_allowed()) {
        return;
    }

    if (sd_card_begin_io() != ESP_OK) {
        return;
    }

    unlink(TELEMETRY_UPLOAD_STATE_FILE);
    sd_card_end_io();
}

static bool refresh_today_file_cache_locked(bool create_file)
{
    char new_stamp[16] = "unsynced";
    char new_path[sizeof(s_today_file_path)] = TELEMETRY_DEFAULT_FILE;

    /*
     * Only rotate into a date-based file once SNTP has produced trusted time.
     * A restored snapshot can be stale across a day boundary, which would send
     * fresh samples into yesterday's file until Wi-Fi time sync finishes.
     */
    if (time_manager_is_synchronized()) {
        time_t now_secs = (time_t)(time_manager_get_epoch_ms() / 1000ULL);
        struct tm tm_now = {0};
        gmtime_r(&now_secs, &tm_now);
        if (strftime(new_stamp, sizeof(new_stamp), "%Y%m%d", &tm_now) > 0) {
            if (!build_telemetry_file_path(new_path, sizeof(new_path), &tm_now)) {
                strlcpy(new_path, TELEMETRY_DEFAULT_FILE, sizeof(new_path));
            }
        }
    } else if (!find_latest_dated_telemetry_file(new_path, sizeof(new_path))) {
        strlcpy(new_path, TELEMETRY_DEFAULT_FILE, sizeof(new_path));
    }

    bool changed = !path_equals_ignore_case(new_path, s_today_file_path);
    if (changed) {
        strlcpy(s_today_file_path, new_path, sizeof(s_today_file_path));
        ESP_LOGI(TAG, "Telemetry day file set to %s", s_today_file_path);
    }

    if (create_file) {
        return create_current_day_file_locked();
    }

    return true;
}

static void update_latest_sample(const telemetry_sample_t *sample)
{
    if (!sample || !s_latest_lock) {
        return;
    }

    if (xSemaphoreTake(s_latest_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return;
    }

    s_latest_sample = *sample;
    s_have_latest_sample = true;
    xSemaphoreGive(s_latest_lock);
}

static void log_rate_limited(int64_t *last_log_us, const char *msg)
{
    int64_t now_us = esp_timer_get_time();
    if (!last_log_us || (now_us - *last_log_us) < TELEMETRY_WARN_INTERVAL_US) {
        return;
    }

    *last_log_us = now_us;
    ESP_LOGW(TAG, "%s", msg);
}

static bool backlog_push(const telemetry_sample_t *sample, bool *overwrote_oldest)
{
    if (!sample || !s_backlog_lock) {
        return false;
    }

    if (overwrote_oldest) {
        *overwrote_oldest = false;
    }

    if (xSemaphoreTake(s_backlog_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return false;
    }

    if (s_ram_backlog_count == TELEMETRY_RAM_BACKLOG_LEN) {
        s_ram_backlog_head = (s_ram_backlog_head + 1) % TELEMETRY_RAM_BACKLOG_LEN;
        s_ram_backlog_count--;
        if (overwrote_oldest) {
            *overwrote_oldest = true;
        }
    }

    s_ram_backlog[s_ram_backlog_tail] = *sample;
    s_ram_backlog_tail = (s_ram_backlog_tail + 1) % TELEMETRY_RAM_BACKLOG_LEN;
    s_ram_backlog_count++;
    s_buffered_samples++;

    xSemaphoreGive(s_backlog_lock);
    return true;
}

static bool backlog_pop(telemetry_sample_t *sample)
{
    if (!sample || !s_backlog_lock) {
        return false;
    }

    if (xSemaphoreTake(s_backlog_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return false;
    }

    if (s_ram_backlog_count == 0) {
        xSemaphoreGive(s_backlog_lock);
        return false;
    }

    *sample = s_ram_backlog[s_ram_backlog_head];
    s_ram_backlog_head = (s_ram_backlog_head + 1) % TELEMETRY_RAM_BACKLOG_LEN;
    s_ram_backlog_count--;

    xSemaphoreGive(s_backlog_lock);
    return true;
}

static void discard_queued_samples(size_t *discarded_count)
{
    size_t discarded = 0;
    telemetry_sample_t sample = {0};

    while (xQueueReceive(s_sample_queue, &sample, 0) == pdTRUE) {
        discarded++;
    }
    while (backlog_pop(&sample)) {
        discarded++;
    }

    if (discarded_count) {
        *discarded_count = discarded;
    }
}

static bool append_sample_to_sd(const telemetry_sample_t *sample)
{
    if (!sample) {
        return false;
    }

    if (!sd_card_write_allowed()) {
        return false;
    }

    if (!refresh_today_file_cache_locked(true)) {
        ESP_LOGW(TAG, "Failed to prepare current telemetry file %s", s_today_file_path);
        return false;
    }

    errno = 0;
    if (sd_card_begin_io() != ESP_OK) {
        return false;
    }

    FILE *f = fopen(s_today_file_path, "a");
    if (!f) {
        int open_errno = errno;
        char sd_status[96] = {0};
        sd_card_get_status(sd_status, sizeof(sd_status));
        ESP_LOGW(TAG, "Failed to open %s: errno=%d (%s), sd=%s",
                 s_today_file_path, open_errno, strerror(open_errno), sd_status);

        if (refresh_today_file_cache_locked(true)) {
            errno = 0;
            f = fopen(s_today_file_path, "a");
            if (f) {
                ESP_LOGI(TAG, "Recovered telemetry file open after remount check");
            } else {
                open_errno = errno;
                ESP_LOGW(TAG, "Retry open failed for %s: errno=%d (%s)",
                         s_today_file_path, open_errno, strerror(open_errno));
            }
        }
    }

    if (!f) {
        sd_card_end_io();
        return false;
    }

    if (!ensure_csv_header(f)) {
        ESP_LOGW(TAG, "Failed to ensure CSV header for %s", s_today_file_path);
        fclose(f);
        sd_card_end_io();
        return false;
    }

    int written = fprintf(f, "%s,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%s,%s,%s,%d,%d,%d,%d,%d,%d,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%d,%d,%d,%d,%d\n",
            sample->sample_uuid,
            sample->sample_id,
            sample->timestamp_ms,
            sample->uptime_ms,
            sample->location_name,
            sample->latitude,
            sample->longitude,
            sample->time_synced ? 1 : 0,
            sample->nh3_raw, sample->red_raw, sample->ox_raw,
            sample->nh3_mv, sample->red_mv, sample->ox_mv,
            sample->nh3_res_ohms, sample->red_res_ohms, sample->ox_res_ohms,
            sample->nh3_ppm, sample->red_ppm, sample->ox_ppm,
            sample->nh3_ppm_valid ? 1 : 0,
            sample->red_ppm_valid ? 1 : 0,
            sample->ox_ppm_valid ? 1 : 0,
            sample->heater_on, sample->warmup, sample->since_change);
    bool ok = written > 0 && sd_card_sync_file(f) == ESP_OK;
    if (!ok) {
        ESP_LOGW(TAG, "Failed to sync telemetry sample to %s", s_today_file_path);
    }
    fclose(f);
    sd_card_end_io();
    return ok;
}

static bool append_samples_to_sd_locked(const telemetry_sample_t *samples, size_t count)
{
    if (!samples || count == 0) {
        return false;
    }

    for (size_t i = 0; i < count; i++) {
        if (!append_sample_to_sd(&samples[i])) {
            return false;
        }
    }

    return true;
}

static bool load_upload_state(upload_state_t *state)
{
    if (!state) {
        return false;
    }

    if (sd_card_begin_io() != ESP_OK) {
        return false;
    }

    FILE *f = fopen(TELEMETRY_UPLOAD_STATE_FILE, "r");
    if (!f) {
        memset(state, 0, sizeof(*state));
        sd_card_end_io();
        return true;
    }

    char path[sizeof(state->path)] = {0};
    unsigned long long value = 0;
    int matched = fscanf(f, "%95s\n%llu", path, &value);
    fclose(f);
    sd_card_end_io();
    if (matched != 2) {
        memset(state, 0, sizeof(*state));
        if (sd_card_begin_io() == ESP_OK) {
        unlink(TELEMETRY_UPLOAD_STATE_FILE);
            sd_card_end_io();
        }
        return true;
    }

    memset(state, 0, sizeof(*state));
    strlcpy(state->path, path, sizeof(state->path));
    state->offset = (uint64_t)value;
    return true;
}

static bool save_upload_state(const char *path, uint64_t offset)
{
    if (!path || path[0] == '\0') {
        return false;
    }

    if (!sd_card_write_allowed()) {
        return false;
    }

    if (sd_card_begin_io() != ESP_OK) {
        return false;
    }

    FILE *f = fopen(TELEMETRY_UPLOAD_STATE_FILE, "w");
    if (!f) {
        sd_card_end_io();
        return false;
    }

    bool ok = fprintf(f, "%s\n%" PRIu64 "\n", path, offset) > 0 &&
              sd_card_sync_file(f) == ESP_OK;
    fclose(f);
    sd_card_end_io();
    return ok;
}

static bool file_has_records_after_offset(const char *path, uint64_t offset)
{
    if (!path) {
        return false;
    }

    if (sd_card_begin_io() != ESP_OK) {
        return false;
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        sd_card_end_io();
        return false;
    }

    bool has_payload = false;
    if (fseek(f, (long)offset, SEEK_SET) == 0) {
        if (offset == 0) {
            char header[256];
            if (!fgets(header, sizeof(header), f)) {
                fclose(f);
                sd_card_end_io();
                return false;
            }
        }

        char line[768];
        has_payload = fgets(line, sizeof(line), f) != NULL;
    }

    fclose(f);
    sd_card_end_io();
    return has_payload;
}

static bool is_telemetry_file_name(const char *name)
{
    if (!name) {
        return false;
    }

    size_t len = strlen(name);
    if (len <= 4 || strcasecmp(name + len - 4, ".csv") != 0) {
        return false;
    }

    return strncasecmp(name, "tm", 2) == 0 ||
           strncasecmp(name, "telemetry_", 10) == 0;
}

static bool select_upload_file_locked(char *path, size_t len)
{
    if (!path || len == 0) {
        return false;
    }

    path[0] = '\0';
    if (!refresh_today_file_cache_locked(false) || sd_card_ensure_mounted() != ESP_OK) {
        return false;
    }

    upload_state_t state = {0};
    bool preserve_current_file_state = false;
    if (load_upload_state(&state) &&
        state.path[0] != '\0' &&
        access(state.path, F_OK) == 0 &&
        file_has_records_after_offset(state.path, state.offset)) {
        strlcpy(path, state.path, len);
        return true;
    }

    if (state.path[0] != '\0' &&
        access(state.path, F_OK) == 0 &&
        path_equals_ignore_case(state.path, s_today_file_path) &&
        !file_has_records_after_offset(state.path, state.offset)) {
        preserve_current_file_state = true;
    }

    if (state.path[0] != '\0' &&
        access(state.path, F_OK) == 0 &&
        !path_equals_ignore_case(state.path, s_today_file_path) &&
        !file_has_records_after_offset(state.path, state.offset)) {
        if (sd_card_write_allowed() && sd_card_begin_io() == ESP_OK) {
            unlink(state.path);
            unlink(TELEMETRY_UPLOAD_STATE_FILE);
            sd_card_end_io();
        }
    }

    if (sd_card_begin_io() != ESP_OK) {
        return false;
    }

    DIR *dir = opendir(SD_CARD_MOUNT_POINT);
    if (!dir) {
        sd_card_end_io();
        return false;
    }

    char best_name[64] = {0};
    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (!is_telemetry_file_name(entry->d_name)) {
            continue;
        }

        char full_path[96];
        strlcpy(full_path, SD_CARD_MOUNT_POINT "/", sizeof(full_path));
        strlcat(full_path, entry->d_name, sizeof(full_path));
        uint64_t candidate_offset = path_equals_ignore_case(state.path, full_path) ? state.offset : 0;
        if (!file_has_records_after_offset(full_path, candidate_offset)) {
            continue;
        }

        if (best_name[0] == '\0' || strcmp(entry->d_name, best_name) < 0) {
            strlcpy(best_name, entry->d_name, sizeof(best_name));
        }
    }
    closedir(dir);
    sd_card_end_io();

    if (best_name[0] == '\0') {
        if (preserve_current_file_state) {
            return false;
        }
        if (sd_card_write_allowed() && sd_card_begin_io() == ESP_OK) {
            unlink(TELEMETRY_UPLOAD_STATE_FILE);
            sd_card_end_io();
        }
        return false;
    }

    strlcpy(path, SD_CARD_MOUNT_POINT "/", len);
    strlcat(path, best_name, len);
    return true;
}

static bool post_upload_file_stream(const char *path,
                                    upload_batch_result_t *result,
                                    int *status_code)
{
    if (!path || !result) {
        return false;
    }

    if (status_code) {
        *status_code = 0;
    }

    upload_state_t state = {0};
    if (!load_upload_state(&state)) {
        return false;
    }
    uint64_t offset = 0;
    if (path_equals_ignore_case(state.path, path)) {
        offset = state.offset;
    }

    if (sd_card_begin_io() != ESP_OK) {
        return false;
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        sd_card_end_io();
        return false;
    }

    if (fseek(f, (long)offset, SEEK_SET) != 0) {
        fclose(f);
        sd_card_end_io();
        return false;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        sd_card_end_io();
        return false;
    }

    long end_pos = ftell(f);
    if (end_pos < 0 || (uint64_t)end_pos <= offset) {
        fclose(f);
        sd_card_end_io();
        return false;
    }

    if (fseek(f, (long)offset, SEEK_SET) != 0) {
        fclose(f);
        sd_card_end_io();
        return false;
    }

    esp_http_client_config_t config = {
        .url = NULL,
        .timeout_ms = 30000,
    };

    char normalized_url[192];
    if (!normalize_upload_url(TELEMETRY_UPLOAD_URL, normalized_url, sizeof(normalized_url))) {
        fclose(f);
        sd_card_end_io();
        return false;
    }
    config.url = normalized_url;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        fclose(f);
        sd_card_end_io();
        return false;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "text/csv");
    char bearer_token[96] = {0};
    if (device_registry_get_api_token(bearer_token, sizeof(bearer_token))) {
        char auth_header[128];
        snprintf(auth_header, sizeof(auth_header), "Bearer %s", bearer_token);
        esp_http_client_set_header(client, "Authorization", auth_header);
    } else if (TELEMETRY_UPLOAD_API_KEY[0] != '\0') {
        esp_http_client_set_header(client, "Authorization", TELEMETRY_UPLOAD_API_KEY);
    }

    esp_http_client_set_header(client, "X-Telemetry-File-Name", path_basename(path));
    char offset_header[32];
    snprintf(offset_header, sizeof(offset_header), "%" PRIu64, offset);
    esp_http_client_set_header(client, "X-Telemetry-Start-Offset", offset_header);

    size_t remaining_bytes = (size_t)((uint64_t)end_pos - offset);
    if (esp_http_client_open(client, (int)remaining_bytes) != ESP_OK) {
        esp_http_client_cleanup(client);
        fclose(f);
        sd_card_end_io();
        return false;
    }

    char *stream_buf = calloc(1, TELEMETRY_UPLOAD_STREAM_BUFFER);
    if (!stream_buf) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        fclose(f);
        sd_card_end_io();
        return false;
    }

    size_t total_written = 0;
    size_t records_sent = 0;
    result->start_offset = offset;
    bool saw_header = (offset > 0);
    while (total_written < remaining_bytes) {
        size_t to_read = remaining_bytes - total_written;
        if (to_read > TELEMETRY_UPLOAD_STREAM_BUFFER) {
            to_read = TELEMETRY_UPLOAD_STREAM_BUFFER;
        }

        size_t bytes_read = fread(stream_buf, 1, to_read, f);
        if (bytes_read == 0) {
            break;
        }

        size_t written_now = 0;
        while (written_now < bytes_read) {
            int wret = esp_http_client_write(client, stream_buf + written_now, (int)(bytes_read - written_now));
            if (wret < 0) {
                free(stream_buf);
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                fclose(f);
                sd_card_end_io();
                return false;
            }
            written_now += (size_t)wret;
        }

        for (size_t i = 0; i < bytes_read; i++) {
            if (stream_buf[i] != '\n') {
                continue;
            }
            if (!saw_header) {
                saw_header = true;
            } else {
                records_sent++;
            }
        }
        total_written += bytes_read;
    }

    int64_t header_ret = esp_http_client_fetch_headers(client);
    int local_status = esp_http_client_get_status_code(client);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(stream_buf);

    result->next_offset = offset + total_written;
    result->records_in_batch = records_sent;
    result->eof = (total_written == remaining_bytes);

    if (status_code) {
        *status_code = local_status;
    }

    fclose(f);
    sd_card_end_io();

    if (header_ret < 0 || local_status < 200 || local_status >= 300 || total_written != remaining_bytes) {
        ESP_LOGW(TAG, "Telemetry file POST failed: file=%s err_headers=%lld http=%d sent=%u/%u",
                 path_basename(path),
                 (long long)header_ret,
                 local_status,
                 (unsigned)total_written,
                 (unsigned)remaining_bytes);
        return false;
    }

    if (records_sent > 0) {
        ESP_LOGI(TAG, "Uploaded telemetry file: file=%s records=%u start_offset=%" PRIu64 " next_offset=%" PRIu64,
                 path_basename(path), (unsigned)records_sent, result->start_offset, result->next_offset);
    }
    return total_written == remaining_bytes;
}

static bool build_upload_batch_json_from_samples(const telemetry_sample_t *samples, size_t count, char *body, size_t body_len)
{
    if (!samples || count == 0 || !body || body_len == 0) {
        return false;
    }

    size_t pos = 0;
    pos += snprintf(body + pos, body_len - pos, "{\"records\":[");

    for (size_t i = 0; i < count; i++) {
        const telemetry_sample_t *sample = &samples[i];
        int written = snprintf(
            body + pos, body_len - pos,
            "%s{\"sample_uuid\":\"%s\",\"sample_id\":%" PRIu64 ",\"timestamp_ms\":%" PRIu64 ",\"uptime_ms\":%" PRIu64
            ",\"location_name\":\"%s\",\"latitude\":\"%s\",\"longitude\":\"%s\","
            "\"time_synced\":%s,\"nh3_raw\":%d,\"red_raw\":%d,\"ox_raw\":%d,"
            "\"nh3_mv\":%d,\"red_mv\":%d,\"ox_mv\":%d,"
            "\"nh3_res_ohms\":%.3f,\"red_res_ohms\":%.3f,\"ox_res_ohms\":%.3f,"
            "\"nh3_ppm\":%.3f,\"red_ppm\":%.3f,\"ox_ppm\":%.3f,"
            "\"nh3_ppm_valid\":%s,\"red_ppm_valid\":%s,\"ox_ppm_valid\":%s,"
            "\"heater_on\":%d,"
            "\"warmup\":%d,\"since_change\":%d}",
            i ? "," : "",
            sample->sample_uuid,
            sample->sample_id, sample->timestamp_ms, sample->uptime_ms,
            sample->location_name, sample->latitude, sample->longitude,
            sample->time_synced ? "true" : "false",
            sample->nh3_raw, sample->red_raw, sample->ox_raw,
            sample->nh3_mv, sample->red_mv, sample->ox_mv,
            sample->nh3_res_ohms, sample->red_res_ohms, sample->ox_res_ohms,
            sample->nh3_ppm, sample->red_ppm, sample->ox_ppm,
            sample->nh3_ppm_valid ? "true" : "false",
            sample->red_ppm_valid ? "true" : "false",
            sample->ox_ppm_valid ? "true" : "false",
            sample->heater_on, sample->warmup, sample->since_change);
        if (written < 0 || (size_t)written >= (body_len - pos)) {
            return false;
        }

        pos += (size_t)written;
    }

    pos += snprintf(body + pos, body_len - pos, "]}");
    return pos < body_len;
}

static bool post_upload_payload(const char *content_type,
                                const char *body,
                                size_t body_len,
                                const char *file_name,
                                uint64_t start_offset,
                                int *status_code)
{
    if (!content_type || !body) {
        return false;
    }

    if (status_code) {
        *status_code = 0;
    }

    esp_http_client_config_t config = {
        .url = NULL,
        .timeout_ms = 15000,
    };

    char normalized_url[192];
    if (!normalize_upload_url(TELEMETRY_UPLOAD_URL, normalized_url, sizeof(normalized_url))) {
        return false;
    }
    config.url = normalized_url;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return false;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", content_type);
    char bearer_token[96] = {0};
    if (device_registry_get_api_token(bearer_token, sizeof(bearer_token))) {
        char auth_header[128];
        snprintf(auth_header, sizeof(auth_header), "Bearer %s", bearer_token);
        esp_http_client_set_header(client, "Authorization", auth_header);
    } else if (TELEMETRY_UPLOAD_API_KEY[0] != '\0') {
        esp_http_client_set_header(client, "Authorization", TELEMETRY_UPLOAD_API_KEY);
    }

    if (file_name && file_name[0] != '\0') {
        esp_http_client_set_header(client, "X-Telemetry-File-Name", file_name);
        char offset_header[32];
        snprintf(offset_header, sizeof(offset_header), "%" PRIu64, start_offset);
        esp_http_client_set_header(client, "X-Telemetry-Start-Offset", offset_header);
    }

    esp_http_client_set_post_field(client, body, (int)body_len);

    esp_err_t err = esp_http_client_perform(client);
    int local_status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (status_code) {
        *status_code = local_status;
    }
    if (err != ESP_OK || local_status < 200 || local_status >= 300) {
        ESP_LOGW(TAG, "Telemetry POST failed: type=%s err=%s http=%d", content_type, esp_err_to_name(err), local_status);
    }
    return err == ESP_OK && local_status >= 200 && local_status < 300;
}

static bool server_is_reachable(void)
{
    TickType_t now = xTaskGetTickCount();
    if (s_last_health_check_tick != 0 &&
        (now - s_last_health_check_tick) < pdMS_TO_TICKS(TELEMETRY_HEALTH_CHECK_MS)) {
        return s_last_health_ok;
    }

    s_last_health_check_tick = now;

    char health_url[192];
    if (!build_healthcheck_url(health_url, sizeof(health_url))) {
        s_last_health_ok = false;
        return false;
    }

    esp_http_client_config_t config = {
        .url = health_url,
        .timeout_ms = 5000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        s_last_health_ok = false;
        return false;
    }

    esp_http_client_set_method(client, HTTP_METHOD_GET);
    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    s_last_health_ok = (err == ESP_OK && status_code >= 200 && status_code < 300);
    if (!s_last_health_ok) {
        ESP_LOGW(TAG, "Server health check failed: err=%s http=%d", esp_err_to_name(err), status_code);
    }
    return s_last_health_ok;
}

static void uploader_task(void *arg)
{
    (void)arg;

    char *body = calloc(1, TELEMETRY_UPLOAD_MAX_BODY);
    telemetry_sample_t *live_batch = calloc(TELEMETRY_UPLOAD_BATCH_SIZE, sizeof(telemetry_sample_t));
    if (!body || !live_batch) {
        ESP_LOGE(TAG, "Failed to allocate uploader buffers");
        free(live_batch);
        free(body);
        vTaskDelete(NULL);
        return;
    }

    if (!upload_enabled()) {
        strlcpy(s_upload_status, "upload disabled", sizeof(s_upload_status));
    }

    while (true) {
        bool uploaded_live_this_cycle = false;

        if (!upload_enabled()) {
            s_server_ready = false;
            strlcpy(s_telemetry_mode, "disabled", sizeof(s_telemetry_mode));
            vTaskDelay(pdMS_TO_TICKS(TELEMETRY_UPLOAD_CHECK_MS));
            continue;
        }

        if (!wifi_manager_is_connected()) {
            s_server_ready = false;
            strlcpy(s_telemetry_mode, "sd-fallback", sizeof(s_telemetry_mode));
            strlcpy(s_upload_status, "waiting for internet", sizeof(s_upload_status));
            vTaskDelay(pdMS_TO_TICKS(TELEMETRY_UPLOAD_CHECK_MS));
            continue;
        }

        size_t live_count = 0;
        while (s_live_upload_queue &&
               live_count < TELEMETRY_UPLOAD_BATCH_SIZE &&
               xQueueReceive(s_live_upload_queue, &live_batch[live_count], 0) == pdTRUE) {
            live_count++;
        }

        char auth_status[96] = {0};
        bool server_ready = server_is_reachable() &&
                            device_registry_ensure_authenticated(TELEMETRY_UPLOAD_URL,
                                                                 TELEMETRY_UPLOAD_API_KEY,
                                                                 TELEMETRY_REGISTRATION_SECRET,
                                                                 auth_status,
                                                                 sizeof(auth_status));
        s_server_ready = server_ready;
        if (!server_ready) {
            if (live_count > 0) {
                if (sd_card_write_allowed() &&
                    sd_card_is_mounted() &&
                    xSemaphoreTake(s_file_lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
                    if (append_samples_to_sd_locked(live_batch, live_count)) {
                        strlcpy(s_telemetry_mode, "sd-fallback", sizeof(s_telemetry_mode));
                        s_persisted_samples += (uint32_t)live_count;
                        strlcpy(s_upload_status, "server unreachable, storing locally", sizeof(s_upload_status));
                    } else {
                        strlcpy(s_telemetry_mode, "offline-drop", sizeof(s_telemetry_mode));
                        s_dropped_samples += (uint32_t)live_count;
                        strlcpy(s_upload_status, "server unreachable, SD store failed", sizeof(s_upload_status));
                    }
                    xSemaphoreGive(s_file_lock);
                } else {
                    strlcpy(s_telemetry_mode, "offline-drop", sizeof(s_telemetry_mode));
                    s_dropped_samples += (uint32_t)live_count;
                    strlcpy(s_upload_status, auth_status[0] != '\0' ? auth_status : "server unavailable, no sd fallback",
                            sizeof(s_upload_status));
                }
            } else {
                strlcpy(s_telemetry_mode, sd_card_is_mounted() ? "sd-fallback" : "offline-drop",
                        sizeof(s_telemetry_mode));
                strlcpy(s_upload_status, auth_status[0] != '\0' ? auth_status : "server unreachable, storing locally",
                        sizeof(s_upload_status));
            }
            vTaskDelay(pdMS_TO_TICKS(TELEMETRY_UPLOAD_CHECK_MS));
            continue;
        }

        if (live_count > 0) {
            if (!build_upload_batch_json_from_samples(live_batch, live_count, body, TELEMETRY_UPLOAD_MAX_BODY)) {
                strlcpy(s_telemetry_mode, "offline-drop", sizeof(s_telemetry_mode));
                s_dropped_samples += (uint32_t)live_count;
                strlcpy(s_upload_status, "live batch build failed", sizeof(s_upload_status));
                vTaskDelay(pdMS_TO_TICKS(TELEMETRY_UPLOAD_CHECK_MS));
                continue;
            }

            int status_code = 0;
            if (!post_upload_payload("application/json", body, strlen(body), NULL, 0, &status_code)) {
                if (sd_card_write_allowed() &&
                    sd_card_is_mounted() &&
                    xSemaphoreTake(s_file_lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
                    if (append_samples_to_sd_locked(live_batch, live_count)) {
                        strlcpy(s_telemetry_mode, "sd-fallback", sizeof(s_telemetry_mode));
                        s_persisted_samples += (uint32_t)live_count;
                        strlcpy(s_upload_status, "live upload failed, stored to sd", sizeof(s_upload_status));
                    } else {
                        strlcpy(s_telemetry_mode, "offline-drop", sizeof(s_telemetry_mode));
                        s_dropped_samples += (uint32_t)live_count;
                        snprintf(s_upload_status, sizeof(s_upload_status), "upload failed (HTTP %d), sd store failed", status_code);
                    }
                    xSemaphoreGive(s_file_lock);
                } else {
                    strlcpy(s_telemetry_mode, "offline-drop", sizeof(s_telemetry_mode));
                    s_dropped_samples += (uint32_t)live_count;
                    snprintf(s_upload_status, sizeof(s_upload_status), "upload failed (HTTP %d)", status_code);
                }
                s_upload_failures++;
                vTaskDelay(pdMS_TO_TICKS(TELEMETRY_UPLOAD_CHECK_MS));
                continue;
            }

            strlcpy(s_telemetry_mode, "live", sizeof(s_telemetry_mode));
            s_uploaded_samples += (uint32_t)live_count;
            snprintf(s_upload_status, sizeof(s_upload_status), "uploaded live batch (%u records)",
                     (unsigned)live_count);
            uploaded_live_this_cycle = true;
        }

        if (xSemaphoreTake(s_file_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        char upload_path[96] = {0};
        upload_batch_result_t batch = {0};
        bool have_upload_file = false;

        if (sd_card_is_mounted()) {
            have_upload_file = select_upload_file_locked(upload_path, sizeof(upload_path));
        }
        xSemaphoreGive(s_file_lock);

        if (!have_upload_file) {
            strlcpy(s_telemetry_mode, "live", sizeof(s_telemetry_mode));
            if (!uploaded_live_this_cycle) {
                strlcpy(s_upload_status, "no pending upload data", sizeof(s_upload_status));
            }
            vTaskDelay(pdMS_TO_TICKS(TELEMETRY_UPLOAD_CHECK_MS));
            continue;
        }

        int status_code = 0;
        if (!post_upload_file_stream(upload_path, &batch, &status_code)) {
            strlcpy(s_telemetry_mode, "sd-fallback", sizeof(s_telemetry_mode));
            s_upload_failures++;
            snprintf(s_upload_status, sizeof(s_upload_status), "upload failed (HTTP %d)", status_code);
            ESP_LOGW(TAG, "%s", s_upload_status);
            vTaskDelay(pdMS_TO_TICKS(TELEMETRY_UPLOAD_CHECK_MS));
            continue;
        }

        if (xSemaphoreTake(s_file_lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
            strlcpy(s_telemetry_mode, "live", sizeof(s_telemetry_mode));
            save_upload_state(upload_path, batch.next_offset);
            s_uploaded_samples += (uint32_t)batch.records_in_batch;

            refresh_today_file_cache_locked(false);
            bool is_current_file = path_equals_ignore_case(upload_path, s_today_file_path);

            if (batch.eof && !is_current_file) {
                if (sd_card_write_allowed() && sd_card_begin_io() == ESP_OK) {
                    unlink(upload_path);
                    unlink(TELEMETRY_UPLOAD_STATE_FILE);
                    sd_card_end_io();
                }
                snprintf(s_upload_status, sizeof(s_upload_status), "uploaded and deleted %.40s",
                         path_basename(upload_path));
            } else if (batch.eof && is_current_file) {
                if (truncate_telemetry_file_to_header_locked(upload_path)) {
                    clear_upload_state_file();
                    snprintf(s_upload_status, sizeof(s_upload_status), "uploaded and cleared %.40s",
                             path_basename(upload_path));
                } else {
                    snprintf(s_upload_status, sizeof(s_upload_status), "uploaded %u records from %.32s",
                             (unsigned)batch.records_in_batch,
                             path_basename(upload_path));
                }
            } else {
                snprintf(s_upload_status, sizeof(s_upload_status), "uploaded %u records from %.32s",
                         (unsigned)batch.records_in_batch,
                         path_basename(upload_path));
            }
            xSemaphoreGive(s_file_lock);
        }
    }
}

static void sampler_task(void *arg)
{
    (void)arg;

    while (true) {
        telemetry_sample_t sample = make_sample();
        update_latest_sample(&sample);

        if (wifi_manager_is_connected() && s_server_ready && s_live_upload_queue) {
            if (xQueueSend(s_live_upload_queue, &sample, 0) == pdTRUE) {
                strlcpy(s_telemetry_mode, "live", sizeof(s_telemetry_mode));
                vTaskDelay(pdMS_TO_TICKS(TELEMETRY_SAMPLE_INTERVAL_MS));
                continue;
            }
        }

        if (!sd_card_write_allowed()) {
            strlcpy(s_telemetry_mode, "offline-drop", sizeof(s_telemetry_mode));
            s_skipped_write_locked_samples++;
            s_dropped_samples++;
            log_rate_limited(&s_last_write_lock_warn_us,
                             "SD write protection active after brownout, telemetry persistence paused");
            vTaskDelay(pdMS_TO_TICKS(TELEMETRY_SAMPLE_INTERVAL_MS));
            continue;
        }

        if (!sd_card_is_mounted()) {
            strlcpy(s_telemetry_mode, "offline-drop", sizeof(s_telemetry_mode));
            s_skipped_no_sd_samples++;
            s_dropped_samples++;
            log_rate_limited(&s_last_no_sd_warn_us, "SD unavailable and server not ready, telemetry not persisted");
            vTaskDelay(pdMS_TO_TICKS(TELEMETRY_SAMPLE_INTERVAL_MS));
            continue;
        }

        strlcpy(s_telemetry_mode, "sd-fallback", sizeof(s_telemetry_mode));
        if (xQueueSend(s_sample_queue, &sample, 0) == pdTRUE) {
            s_enqueued_samples++;
        } else {
            bool overwrote_oldest = false;
            if (backlog_push(&sample, &overwrote_oldest)) {
                log_rate_limited(&s_last_queue_warn_us, "Sample queue full, buffering in RAM backlog");
                if (overwrote_oldest) {
                    s_dropped_samples++;
                    log_rate_limited(&s_last_backlog_warn_us, "RAM backlog full, overwriting oldest telemetry sample");
                }
            } else {
                s_dropped_samples++;
                log_rate_limited(&s_last_backlog_warn_us, "Telemetry sample lost: queue full and RAM backlog unavailable");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(TELEMETRY_SAMPLE_INTERVAL_MS));
    }
}

static void storage_task(void *arg)
{
    (void)arg;

    telemetry_sample_t pending_sample = {0};
    bool have_pending = false;

    while (true) {
        if (!have_pending) {
            if (xQueueReceive(s_sample_queue, &pending_sample, pdMS_TO_TICKS(TELEMETRY_STORAGE_IDLE_WAIT_MS)) == pdTRUE) {
                have_pending = true;
            } else if (backlog_pop(&pending_sample)) {
                have_pending = true;
            } else {
                continue;
            }
        }

        if (!sd_card_write_allowed()) {
            size_t discarded = 0;
            have_pending = false;
            discard_queued_samples(&discarded);
            s_dropped_samples += (uint32_t)discarded;
            vTaskDelay(pdMS_TO_TICKS(TELEMETRY_STORAGE_RETRY_MS));
            continue;
        }

        if (xSemaphoreTake(s_file_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (append_sample_to_sd(&pending_sample)) {
            s_persisted_samples++;
            have_pending = false;
            xSemaphoreGive(s_file_lock);
            continue;
        }
        xSemaphoreGive(s_file_lock);

        vTaskDelay(pdMS_TO_TICKS(TELEMETRY_STORAGE_RETRY_MS));
    }
}

static void file_manager_task(void *arg)
{
    (void)arg;

    while (true) {
        if (xSemaphoreTake(s_file_lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
            refresh_today_file_cache_locked(true);
            xSemaphoreGive(s_file_lock);
        }
        vTaskDelay(pdMS_TO_TICKS(TELEMETRY_FILE_REFRESH_MS));
    }
}

void telemetry_pipeline_init(void)
{
    if (s_sample_queue) {
        return;
    }

    s_sample_queue = xQueueCreate(TELEMETRY_QUEUE_LEN, sizeof(telemetry_sample_t));
    s_live_upload_queue = xQueueCreate(TELEMETRY_LIVE_UPLOAD_QUEUE_LEN, sizeof(telemetry_sample_t));
    s_latest_lock = xSemaphoreCreateMutex();
    s_file_lock = xSemaphoreCreateMutex();
    s_backlog_lock = xSemaphoreCreateMutex();
    s_boot_id = generate_boot_id();
    if (!s_sample_queue || !s_live_upload_queue || !s_latest_lock || !s_file_lock || !s_backlog_lock) {
        ESP_LOGE(TAG, "Failed to initialize telemetry pipeline");
        return;
    }

    device_registry_init();

    if (xSemaphoreTake(s_file_lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
        refresh_today_file_cache_locked(false);
        xSemaphoreGive(s_file_lock);
    }

    xTaskCreate(sampler_task, "telemetry_sample", 6144, NULL, 4, &s_sampler_task);
    xTaskCreate(storage_task, "telemetry_store", 6144, NULL, 4, &s_storage_task);
    xTaskCreate(uploader_task, "telemetry_upload", 10240, NULL, 4, &s_upload_task);
    xTaskCreate(file_manager_task, "telemetry_file", 4096, NULL, 4, &s_file_manager_task);
}

void telemetry_pipeline_handle_time_sync(void)
{
    if (!s_file_lock) {
        return;
    }

    if (xSemaphoreTake(s_file_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return;
    }

    refresh_today_file_cache_locked(true);
    xSemaphoreGive(s_file_lock);
}

bool telemetry_pipeline_get_latest(telemetry_sample_t *sample)
{
    if (!sample || !s_latest_lock) {
        return false;
    }

    if (xSemaphoreTake(s_latest_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return false;
    }

    bool have_sample = s_have_latest_sample;
    if (have_sample) {
        *sample = s_latest_sample;
    }
    xSemaphoreGive(s_latest_lock);
    return have_sample;
}

void telemetry_pipeline_get_status(char *buf, size_t len)
{
    if (!buf || len == 0) {
        return;
    }

    size_t queued = s_sample_queue ? (size_t)uxQueueMessagesWaiting(s_sample_queue) : 0;
    size_t ram_backlog = 0;
    if (s_backlog_lock && xSemaphoreTake(s_backlog_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        ram_backlog = s_ram_backlog_count;
        xSemaphoreGive(s_backlog_lock);
    }

    snprintf(buf, len, "mode=%s queued=%lu backlog=%lu buffered=%lu persisted=%lu skipped_no_sd=%lu skipped_write_locked=%lu uploaded=%lu dropped=%lu upload=%s",
             s_telemetry_mode,
             (unsigned long)queued,
             (unsigned long)ram_backlog,
             (unsigned long)s_buffered_samples,
             (unsigned long)s_persisted_samples,
             (unsigned long)s_skipped_no_sd_samples,
             (unsigned long)s_skipped_write_locked_samples,
             (unsigned long)s_uploaded_samples,
             (unsigned long)s_dropped_samples,
             s_upload_status);
}

bool telemetry_pipeline_get_today_file(char *buf, size_t len)
{
    if (!buf || len == 0 || !s_file_lock) {
        return false;
    }

    if (xSemaphoreTake(s_file_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return false;
    }

    refresh_today_file_cache_locked(false);
    strlcpy(buf, s_today_file_path, len);
    xSemaphoreGive(s_file_lock);
    return buf[0] != '\0';
}
