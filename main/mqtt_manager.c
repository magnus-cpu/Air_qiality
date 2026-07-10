#include "mqtt_manager.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "mqtt_client.h"
#include "nvs.h"

#include "heater.h"
#include "sd_card.h"
#include "sensor.h"
#include "site_config.h"
#include "telemetry_pipeline.h"
#include "time_manager.h"
#include "wifi_manager.h"

#define MQTT_NAMESPACE "mqtt"
#define MQTT_URI_KEY "uri"
#define MQTT_TOPIC_KEY "topic"
#define MQTT_ENABLED_KEY "enabled"
#define MQTT_DEFAULT_BASE_TOPIC "air_quality"
#define MQTT_PUBLISH_INTERVAL_MS 2000
#define HEATER_WARMUP_SECONDS 60

static const char *TAG = "mqtt_manager";

static esp_mqtt_client_handle_t s_client = NULL;
static TaskHandle_t s_publish_task = NULL;
static mqtt_manager_config_t s_config = {0};
static bool s_connected = false;
static char s_device_id[18] = "unknown";
static char s_status[96] = "not configured";

static void make_device_id(void)
{
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        snprintf(s_device_id, sizeof(s_device_id), "%02x%02x%02x%02x%02x%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
}

static void topic_for(char *buf, size_t len, const char *suffix)
{
    snprintf(buf, len, "%s/%s/%s", s_config.base_topic, s_device_id, suffix);
}

static void build_telemetry(char *buf, size_t len)
{
    char ip_str[16] = "0.0.0.0";
    char sd_status[96] = {0};
    char location_name[96] = {0};
    telemetry_sample_t sample = {0};
    bool have_sample = telemetry_pipeline_get_latest(&sample);
    sensor_snapshot_t sensor_snapshot = {0};
    site_config_t site = {0};
    if (!have_sample) {
        sensor_read_all(&sensor_snapshot);
    }
    if (!have_sample && site_config_load(&site) == ESP_OK) {
        strlcpy(location_name, site.location_name, sizeof(location_name));
    } else if (have_sample) {
        strlcpy(location_name, sample.location_name, sizeof(location_name));
    }
    wifi_manager_get_ip(ip_str, sizeof(ip_str));
    sd_card_get_status(sd_status, sizeof(sd_status));

    snprintf(buf, len,
             "{\"device_id\":\"%s\",\"nh3_raw\":%d,\"red_raw\":%d,\"ox_raw\":%d,"
             "\"location_name\":\"%s\",\"latitude\":\"%s\",\"longitude\":\"%s\","
             "\"nh3_mv\":%d,\"red_mv\":%d,\"ox_mv\":%d,"
             "\"nh3_res_ohms\":%.3f,\"red_res_ohms\":%.3f,\"ox_res_ohms\":%.3f,"
             "\"nh3_ppm\":%.3f,\"red_ppm\":%.3f,\"ox_ppm\":%.3f,"
             "\"nh3_ppm_valid\":%s,\"red_ppm_valid\":%s,\"ox_ppm_valid\":%s,"
             "\"heater_on\":%d,\"warmup\":%d,\"since_change\":%d,"
             "\"wifi_connected\":%s,\"time_synced\":%s,\"timestamp_ms\":%" PRIu64 ",\"ip\":\"%s\",\"sd_card\":\"%s\"}",
             s_device_id,
             have_sample ? sample.nh3_raw : sensor_snapshot.nh3.raw,
             have_sample ? sample.red_raw : sensor_snapshot.red.raw,
             have_sample ? sample.ox_raw : sensor_snapshot.ox.raw,
             have_sample ? sample.location_name : location_name,
             have_sample ? sample.latitude : site.latitude,
             have_sample ? sample.longitude : site.longitude,
             have_sample ? sample.nh3_mv : sensor_snapshot.nh3.mv,
             have_sample ? sample.red_mv : sensor_snapshot.red.mv,
             have_sample ? sample.ox_mv : sensor_snapshot.ox.mv,
             have_sample ? sample.nh3_res_ohms : sensor_snapshot.nh3.resistance_ohms,
             have_sample ? sample.red_res_ohms : sensor_snapshot.red.resistance_ohms,
             have_sample ? sample.ox_res_ohms : sensor_snapshot.ox.resistance_ohms,
             have_sample ? sample.nh3_ppm : sensor_snapshot.nh3.ppm,
             have_sample ? sample.red_ppm : sensor_snapshot.red.ppm,
             have_sample ? sample.ox_ppm : sensor_snapshot.ox.ppm,
             (have_sample ? sample.nh3_ppm_valid : sensor_snapshot.nh3.ppm_valid) ? "true" : "false",
             (have_sample ? sample.red_ppm_valid : sensor_snapshot.red.ppm_valid) ? "true" : "false",
             (have_sample ? sample.ox_ppm_valid : sensor_snapshot.ox.ppm_valid) ? "true" : "false",
             have_sample ? sample.heater_on : heater_get(),
             have_sample ? sample.warmup : heater_is_warming(HEATER_WARMUP_SECONDS),
             have_sample ? sample.since_change : heater_seconds_since_change(),
             (have_sample ? sample.wifi_connected : wifi_manager_is_connected()) ? "true" : "false",
             (have_sample ? sample.time_synced : time_manager_is_synchronized()) ? "true" : "false",
             have_sample ? sample.timestamp_ms : time_manager_get_epoch_ms(),
             ip_str, sd_status);
}

static void publish_online_status(const char *state)
{
    if (!s_client || !s_connected) {
        return;
    }

    char topic[128];
    topic_for(topic, sizeof(topic), "status");
    esp_mqtt_client_publish(s_client, topic, state, 0, 1, 1);
}

void mqtt_manager_publish_now(void)
{
    if (!s_client || !s_connected) {
        return;
    }

    char topic[128];
    char payload[768];
    topic_for(topic, sizeof(topic), "telemetry");
    build_telemetry(payload, sizeof(payload));
    esp_mqtt_client_publish(s_client, topic, payload, 0, 1, 0);
}

static bool payload_equals(const char *data, int len, const char *value)
{
    size_t value_len = strlen(value);
    return len == (int)value_len && strncasecmp(data, value, value_len) == 0;
}

static bool payload_contains(const char *data, int len, const char *value)
{
    size_t value_len = strlen(value);
    if (value_len == 0 || len < (int)value_len) {
        return false;
    }
    for (int i = 0; i <= len - (int)value_len; i++) {
        if (strncasecmp(data + i, value, value_len) == 0) {
            return true;
        }
    }
    return false;
}

static void handle_command(const char *data, int len)
{
    if (!data || len <= 0) {
        return;
    }

    if (payload_equals(data, len, "heater:on") ||
        payload_equals(data, len, "heater=1") ||
        payload_equals(data, len, "1") ||
        payload_contains(data, len, "\"heater_on\":true")) {
        heater_set(true);
        mqtt_manager_publish_now();
        return;
    }

    if (payload_equals(data, len, "heater:off") ||
        payload_equals(data, len, "heater=0") ||
        payload_equals(data, len, "0") ||
        payload_contains(data, len, "\"heater_on\":false")) {
        heater_set(false);
        mqtt_manager_publish_now();
        return;
    }

    if (payload_equals(data, len, "publish") || payload_equals(data, len, "status")) {
        mqtt_manager_publish_now();
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED: {
        s_connected = true;
        snprintf(s_status, sizeof(s_status), "connected");
        char cmd_topic[128];
        topic_for(cmd_topic, sizeof(cmd_topic), "cmd");
        esp_mqtt_client_subscribe(s_client, cmd_topic, 1);
        publish_online_status("online");
        mqtt_manager_publish_now();
        ESP_LOGI(TAG, "MQTT connected, subscribed to %s", cmd_topic);
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        snprintf(s_status, sizeof(s_status), "disconnected");
        ESP_LOGW(TAG, "MQTT disconnected");
        break;
    case MQTT_EVENT_DATA:
        handle_command(event->data, event->data_len);
        break;
    case MQTT_EVENT_ERROR:
        snprintf(s_status, sizeof(s_status), "error");
        ESP_LOGW(TAG, "MQTT error");
        break;
    default:
        break;
    }
}

static void mqtt_publish_task(void *arg)
{
    (void)arg;
    while (true) {
        if (wifi_manager_is_connected()) {
            mqtt_manager_publish_now();
        }
        vTaskDelay(pdMS_TO_TICKS(MQTT_PUBLISH_INTERVAL_MS));
    }
}

static void stop_mqtt_client(void)
{
    if (!s_client) {
        return;
    }

    esp_mqtt_client_stop(s_client);
    esp_mqtt_client_destroy(s_client);
    s_client = NULL;
    s_connected = false;
}

static esp_err_t start_mqtt_client(void)
{
    if (!s_config.enabled || s_config.broker_uri[0] == '\0') {
        strlcpy(s_status, s_config.enabled ? "not configured" : "disabled", sizeof(s_status));
        return ESP_OK;
    }

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = s_config.broker_uri,
    };

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!s_client) {
        snprintf(s_status, sizeof(s_status), "init failed");
        return ESP_FAIL;
    }

    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_err_t err = esp_mqtt_client_start(s_client);
    if (err != ESP_OK) {
        snprintf(s_status, sizeof(s_status), "start failed: %s", esp_err_to_name(err));
        ESP_LOGW(TAG, "%s", s_status);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        return err;
    }

    snprintf(s_status, sizeof(s_status), "connecting");
    return ESP_OK;
}

bool mqtt_manager_load_config(mqtt_manager_config_t *config)
{
    if (!config) {
        return false;
    }

    memset(config, 0, sizeof(*config));
    strlcpy(config->base_topic, MQTT_DEFAULT_BASE_TOPIC, sizeof(config->base_topic));

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(MQTT_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return false;
    }

    uint8_t enabled = 0;
    size_t uri_len = sizeof(config->broker_uri);
    size_t topic_len = sizeof(config->base_topic);
    esp_err_t uri_err = nvs_get_str(nvs, MQTT_URI_KEY, config->broker_uri, &uri_len);
    esp_err_t topic_err = nvs_get_str(nvs, MQTT_TOPIC_KEY, config->base_topic, &topic_len);
    if (topic_err != ESP_OK || config->base_topic[0] == '\0') {
        strlcpy(config->base_topic, MQTT_DEFAULT_BASE_TOPIC, sizeof(config->base_topic));
    }
    if (nvs_get_u8(nvs, MQTT_ENABLED_KEY, &enabled) == ESP_OK) {
        config->enabled = enabled != 0;
    }
    nvs_close(nvs);

    return uri_err == ESP_OK && config->broker_uri[0] != '\0';
}

esp_err_t mqtt_manager_save_config(const mqtt_manager_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(MQTT_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(nvs, MQTT_URI_KEY, config->broker_uri);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, MQTT_TOPIC_KEY,
                          config->base_topic[0] ? config->base_topic : MQTT_DEFAULT_BASE_TOPIC);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, MQTT_ENABLED_KEY, config->enabled ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);

    if (err == ESP_OK) {
        stop_mqtt_client();
        s_config = *config;
        if (s_config.base_topic[0] == '\0') {
            strlcpy(s_config.base_topic, MQTT_DEFAULT_BASE_TOPIC, sizeof(s_config.base_topic));
        }
        err = start_mqtt_client();
    }
    return err;
}

void mqtt_manager_init(void)
{
    make_device_id();
    if (!s_publish_task) {
        xTaskCreate(mqtt_publish_task, "mqtt_publish", 4096, NULL, 4, &s_publish_task);
    }

    bool have_config = mqtt_manager_load_config(&s_config);
    if (!have_config || !s_config.enabled) {
        strlcpy(s_status, have_config ? "disabled" : "not configured", sizeof(s_status));
        return;
    }

    start_mqtt_client();
}

bool mqtt_manager_is_connected(void)
{
    return s_connected;
}

void mqtt_manager_get_status(char *buf, size_t len)
{
    if (!buf || len == 0) {
        return;
    }

    if (s_config.enabled && s_config.broker_uri[0] != '\0') {
        snprintf(buf, len, "%s (%s/%s)", s_status, s_config.base_topic, s_device_id);
    } else {
        strlcpy(buf, s_status, len);
    }
}
