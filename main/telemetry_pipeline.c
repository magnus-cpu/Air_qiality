#include "telemetry_pipeline.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

#include "heater.h"
#include "sd_card.h"
#include "sensor.h"
#include "time_manager.h"
#include "wifi_manager.h"

#define TELEMETRY_SAMPLE_INTERVAL_MS 5000
#define TELEMETRY_STORAGE_RETRY_MS 5000
#define TELEMETRY_STORAGE_IDLE_WAIT_MS 250
#define TELEMETRY_UPLOAD_CHECK_MS 5000
#define TELEMETRY_QUEUE_LEN 64
#define TELEMETRY_RAM_BACKLOG_LEN 128
#define TELEMETRY_ACTIVE_FILE SD_CARD_MOUNT_POINT "/telact.csv"
#define TELEMETRY_UPLOAD_FILE SD_CARD_MOUNT_POINT "/telupl.csv"
#define TELEMETRY_UPLOAD_STATE_FILE SD_CARD_MOUNT_POINT "/telupl.sta"
#define TELEMETRY_UPLOAD_URL ""
#define TELEMETRY_UPLOAD_API_KEY ""
#define TELEMETRY_UPLOAD_BATCH_SIZE 20
#define TELEMETRY_UPLOAD_MAX_BODY 4096
#define TELEMETRY_WARN_INTERVAL_US (30LL * 1000LL * 1000LL)
#define HEATER_WARMUP_SECONDS 60

static const char *TAG = "telemetry_pipeline";

static QueueHandle_t s_sample_queue = NULL;
static SemaphoreHandle_t s_latest_lock = NULL;
static SemaphoreHandle_t s_file_lock = NULL;
static SemaphoreHandle_t s_backlog_lock = NULL;
static TaskHandle_t s_sampler_task = NULL;
static TaskHandle_t s_storage_task = NULL;
static TaskHandle_t s_upload_task = NULL;
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
static uint32_t s_uploaded_samples = 0;
static uint32_t s_upload_failures = 0;
static int64_t s_last_queue_warn_us = 0;
static int64_t s_last_backlog_warn_us = 0;
static int64_t s_last_no_sd_warn_us = 0;
static uint64_t s_next_sample_id = 1;
static uint32_t s_boot_id = 0;
static char s_upload_status[96] = "upload disabled";

typedef struct {
    uint64_t next_offset;
    size_t records_in_batch;
    bool eof;
} upload_batch_result_t;

static telemetry_sample_t make_sample(void)
{
    sensor_snapshot_t sensor_snapshot = {0};
    sensor_read_all(&sensor_snapshot);

    telemetry_sample_t sample = {
        .sample_id = (((uint64_t)s_boot_id) << 32) | s_next_sample_id++,
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
                   "sample_id,timestamp_ms,uptime_ms,time_synced,nh3_raw,red_raw,ox_raw,nh3_mv,red_mv,ox_mv,nh3_res_ohms,red_res_ohms,ox_res_ohms,nh3_ppm,red_ppm,ox_ppm,nh3_ppm_valid,red_ppm_valid,ox_ppm_valid,heater_on,warmup,since_change,wifi_connected\n") > 0;
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

static bool append_sample_to_sd(const telemetry_sample_t *sample)
{
    if (!sample) {
        return false;
    }

    if (sd_card_ensure_mounted() != ESP_OK) {
        return false;
    }

    errno = 0;
    FILE *f = fopen(TELEMETRY_ACTIVE_FILE, "a");
    if (!f) {
        int open_errno = errno;
        char sd_status[96] = {0};
        sd_card_get_status(sd_status, sizeof(sd_status));
        ESP_LOGW(TAG, "Failed to open %s: errno=%d (%s), sd=%s",
                 TELEMETRY_ACTIVE_FILE, open_errno, strerror(open_errno), sd_status);

        if (sd_card_ensure_mounted() == ESP_OK) {
            errno = 0;
            f = fopen(TELEMETRY_ACTIVE_FILE, "a");
            if (f) {
                ESP_LOGI(TAG, "Recovered telemetry file open after remount check");
            } else {
                open_errno = errno;
                ESP_LOGW(TAG, "Retry open failed for %s: errno=%d (%s)",
                         TELEMETRY_ACTIVE_FILE, open_errno, strerror(open_errno));
            }
        }
    }

    if (!f) {
        return false;
    }

    if (!ensure_csv_header(f)) {
        fclose(f);
        return false;
    }

    fprintf(f, "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%d,%d,%d,%d,%d,%d,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%d,%d,%d,%d,%d,%d\n",
            sample->sample_id,
            sample->timestamp_ms,
            sample->uptime_ms,
            sample->time_synced ? 1 : 0,
            sample->nh3_raw, sample->red_raw, sample->ox_raw,
            sample->nh3_mv, sample->red_mv, sample->ox_mv,
            sample->nh3_res_ohms, sample->red_res_ohms, sample->ox_res_ohms,
            sample->nh3_ppm, sample->red_ppm, sample->ox_ppm,
            sample->nh3_ppm_valid ? 1 : 0,
            sample->red_ppm_valid ? 1 : 0,
            sample->ox_ppm_valid ? 1 : 0,
            sample->heater_on, sample->warmup, sample->since_change,
            sample->wifi_connected ? 1 : 0);
    fclose(f);
    return true;
}

static bool load_upload_offset(uint64_t *offset)
{
    if (!offset) {
        return false;
    }

    FILE *f = fopen(TELEMETRY_UPLOAD_STATE_FILE, "r");
    if (!f) {
        *offset = 0;
        return true;
    }

    unsigned long long value = 0;
    int matched = fscanf(f, "%llu", &value);
    fclose(f);
    if (matched != 1) {
        *offset = 0;
        return false;
    }

    *offset = (uint64_t)value;
    return true;
}

static bool save_upload_offset(uint64_t offset)
{
    FILE *f = fopen(TELEMETRY_UPLOAD_STATE_FILE, "w");
    if (!f) {
        return false;
    }

    bool ok = fprintf(f, "%" PRIu64 "\n", offset) > 0;
    fclose(f);
    return ok;
}

static bool file_has_payload(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        return false;
    }

    char line[256];
    bool has_payload = false;
    if (fgets(line, sizeof(line), f) && fgets(line, sizeof(line), f)) {
        has_payload = true;
    }
    fclose(f);
    return has_payload;
}

static bool rotate_active_file_for_upload(void)
{
    if (access(TELEMETRY_UPLOAD_FILE, F_OK) == 0) {
        return true;
    }

    if (!file_has_payload(TELEMETRY_ACTIVE_FILE)) {
        return false;
    }

    unlink(TELEMETRY_UPLOAD_STATE_FILE);
    if (rename(TELEMETRY_ACTIVE_FILE, TELEMETRY_UPLOAD_FILE) != 0) {
        ESP_LOGW(TAG, "Failed to rotate telemetry file for upload");
        return false;
    }

    strlcpy(s_upload_status, "upload file prepared", sizeof(s_upload_status));
    return true;
}

static bool parse_sample_line(const char *line, telemetry_sample_t *sample)
{
    if (!line || !sample) {
        return false;
    }

    unsigned long long sample_id = 0;
    unsigned long long timestamp_ms = 0;
    unsigned long long uptime_ms = 0;
    int time_synced = 0;
    int nh3_ppm_valid = 0;
    int red_ppm_valid = 0;
    int ox_ppm_valid = 0;
    int wifi_connected = 0;
    int matched = sscanf(line,
                         "%llu,%llu,%llu,%d,%d,%d,%d,%d,%d,%d,%f,%f,%f,%f,%f,%f,%d,%d,%d,%d,%d,%d,%d",
                         &sample_id,
                         &timestamp_ms,
                         &uptime_ms,
                         &time_synced,
                         &sample->nh3_raw, &sample->red_raw, &sample->ox_raw,
                         &sample->nh3_mv, &sample->red_mv, &sample->ox_mv,
                         &sample->nh3_res_ohms, &sample->red_res_ohms, &sample->ox_res_ohms,
                         &sample->nh3_ppm, &sample->red_ppm, &sample->ox_ppm,
                         &nh3_ppm_valid, &red_ppm_valid, &ox_ppm_valid,
                         &sample->heater_on, &sample->warmup, &sample->since_change,
                         &wifi_connected);
    if (matched != 23) {
        return false;
    }

    sample->sample_id = (uint64_t)sample_id;
    sample->timestamp_ms = (uint64_t)timestamp_ms;
    sample->uptime_ms = (uint64_t)uptime_ms;
    sample->time_synced = time_synced != 0;
    sample->nh3_ppm_valid = nh3_ppm_valid != 0;
    sample->red_ppm_valid = red_ppm_valid != 0;
    sample->ox_ppm_valid = ox_ppm_valid != 0;
    sample->wifi_connected = wifi_connected != 0;
    return true;
}

static bool build_upload_batch_json(char *body, size_t body_len, upload_batch_result_t *result)
{
    if (!body || !result) {
        return false;
    }

    uint64_t offset = 0;
    if (!load_upload_offset(&offset)) {
        return false;
    }

    FILE *f = fopen(TELEMETRY_UPLOAD_FILE, "r");
    if (!f) {
        return false;
    }

    if (fseek(f, (long)offset, SEEK_SET) != 0) {
        fclose(f);
        return false;
    }

    if (offset == 0) {
        char header[256];
        if (!fgets(header, sizeof(header), f)) {
            fclose(f);
            return false;
        }
    }

    size_t pos = 0;
    pos += snprintf(body + pos, body_len - pos, "{\"records\":[");

    char line[256];
    size_t count = 0;
    while (count < TELEMETRY_UPLOAD_BATCH_SIZE && fgets(line, sizeof(line), f)) {
        telemetry_sample_t sample = {0};
        if (!parse_sample_line(line, &sample)) {
            continue;
        }

        int written = snprintf(
            body + pos, body_len - pos,
            "%s{\"sample_id\":%" PRIu64 ",\"timestamp_ms\":%" PRIu64 ",\"uptime_ms\":%" PRIu64
            ",\"time_synced\":%s,\"nh3_raw\":%d,\"red_raw\":%d,\"ox_raw\":%d,"
            "\"nh3_mv\":%d,\"red_mv\":%d,\"ox_mv\":%d,"
            "\"nh3_res_ohms\":%.3f,\"red_res_ohms\":%.3f,\"ox_res_ohms\":%.3f,"
            "\"nh3_ppm\":%.3f,\"red_ppm\":%.3f,\"ox_ppm\":%.3f,"
            "\"nh3_ppm_valid\":%s,\"red_ppm_valid\":%s,\"ox_ppm_valid\":%s,"
            "\"heater_on\":%d,"
            "\"warmup\":%d,\"since_change\":%d,\"wifi_connected\":%s}",
            count ? "," : "",
            sample.sample_id, sample.timestamp_ms, sample.uptime_ms,
            sample.time_synced ? "true" : "false",
            sample.nh3_raw, sample.red_raw, sample.ox_raw,
            sample.nh3_mv, sample.red_mv, sample.ox_mv,
            sample.nh3_res_ohms, sample.red_res_ohms, sample.ox_res_ohms,
            sample.nh3_ppm, sample.red_ppm, sample.ox_ppm,
            sample.nh3_ppm_valid ? "true" : "false",
            sample.red_ppm_valid ? "true" : "false",
            sample.ox_ppm_valid ? "true" : "false",
            sample.heater_on, sample.warmup, sample.since_change,
            sample.wifi_connected ? "true" : "false");
        if (written < 0 || (size_t)written >= (body_len - pos)) {
            break;
        }

        pos += (size_t)written;
        count++;
        offset = (uint64_t)ftell(f);
    }

    pos += snprintf(body + pos, body_len - pos, "]}");
    result->next_offset = offset;
    result->records_in_batch = count;
    result->eof = feof(f);
    fclose(f);
    return count > 0;
}

static bool post_upload_batch(const char *body, int *status_code)
{
    if (status_code) {
        *status_code = 0;
    }

    esp_http_client_config_t config = {
        .url = TELEMETRY_UPLOAD_URL,
        .timeout_ms = 15000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return false;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    if (TELEMETRY_UPLOAD_API_KEY[0] != '\0') {
        esp_http_client_set_header(client, "Authorization", TELEMETRY_UPLOAD_API_KEY);
    }
    esp_http_client_set_post_field(client, body, (int)strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    int local_status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (status_code) {
        *status_code = local_status;
    }
    return err == ESP_OK && local_status >= 200 && local_status < 300;
}

static void uploader_task(void *arg)
{
    (void)arg;

    if (!upload_enabled()) {
        strlcpy(s_upload_status, "upload disabled", sizeof(s_upload_status));
    }

    while (true) {
        if (!upload_enabled()) {
            vTaskDelay(pdMS_TO_TICKS(TELEMETRY_UPLOAD_CHECK_MS));
            continue;
        }

        if (!wifi_manager_is_connected() || !time_manager_is_synchronized()) {
            strlcpy(s_upload_status, "waiting for internet/time sync", sizeof(s_upload_status));
            vTaskDelay(pdMS_TO_TICKS(TELEMETRY_UPLOAD_CHECK_MS));
            continue;
        }

        if (xSemaphoreTake(s_file_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        bool have_upload_file = rotate_active_file_for_upload();
        char body[TELEMETRY_UPLOAD_MAX_BODY];
        upload_batch_result_t batch = {0};
        bool have_batch = have_upload_file && build_upload_batch_json(body, sizeof(body), &batch);
        xSemaphoreGive(s_file_lock);

        if (!have_batch) {
            strlcpy(s_upload_status, have_upload_file ? "upload file empty" : "no pending upload data", sizeof(s_upload_status));
            vTaskDelay(pdMS_TO_TICKS(TELEMETRY_UPLOAD_CHECK_MS));
            continue;
        }

        int status_code = 0;
        if (!post_upload_batch(body, &status_code)) {
            s_upload_failures++;
            snprintf(s_upload_status, sizeof(s_upload_status), "upload failed (HTTP %d)", status_code);
            ESP_LOGW(TAG, "%s", s_upload_status);
            vTaskDelay(pdMS_TO_TICKS(TELEMETRY_UPLOAD_CHECK_MS));
            continue;
        }

        if (xSemaphoreTake(s_file_lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
            save_upload_offset(batch.next_offset);
            s_uploaded_samples += (uint32_t)batch.records_in_batch;

            if (batch.eof) {
                unlink(TELEMETRY_UPLOAD_FILE);
                unlink(TELEMETRY_UPLOAD_STATE_FILE);
                strlcpy(s_upload_status, "upload cycle complete", sizeof(s_upload_status));
            } else {
                snprintf(s_upload_status, sizeof(s_upload_status), "uploaded %u records", (unsigned)batch.records_in_batch);
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

        if (!sd_card_is_mounted()) {
            s_skipped_no_sd_samples++;
            log_rate_limited(&s_last_no_sd_warn_us, "SD unavailable, telemetry persistence paused");
            vTaskDelay(pdMS_TO_TICKS(TELEMETRY_SAMPLE_INTERVAL_MS));
            continue;
        }

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

void telemetry_pipeline_init(void)
{
    if (s_sample_queue) {
        return;
    }

    s_sample_queue = xQueueCreate(TELEMETRY_QUEUE_LEN, sizeof(telemetry_sample_t));
    s_latest_lock = xSemaphoreCreateMutex();
    s_file_lock = xSemaphoreCreateMutex();
    s_backlog_lock = xSemaphoreCreateMutex();
    s_boot_id = esp_random();
    if (!s_sample_queue || !s_latest_lock || !s_file_lock || !s_backlog_lock) {
        ESP_LOGE(TAG, "Failed to initialize telemetry pipeline");
        return;
    }

    xTaskCreate(sampler_task, "telemetry_sample", 4096, NULL, 4, &s_sampler_task);
    xTaskCreate(storage_task, "telemetry_store", 6144, NULL, 4, &s_storage_task);
    xTaskCreate(uploader_task, "telemetry_upload", 8192, NULL, 4, &s_upload_task);
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

    snprintf(buf, len, "queued=%lu backlog=%lu buffered=%lu persisted=%lu skipped_no_sd=%lu uploaded=%lu dropped=%lu upload=%s",
             (unsigned long)queued,
             (unsigned long)ram_backlog,
             (unsigned long)s_buffered_samples,
             (unsigned long)s_persisted_samples,
             (unsigned long)s_skipped_no_sd_samples,
             (unsigned long)s_uploaded_samples,
             (unsigned long)s_dropped_samples,
             s_upload_status);
}
