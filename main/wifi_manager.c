#include "wifi_manager.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs.h"

static const char *TAG = "wifi_manager";

#define WIFI_NAMESPACE "wifi"
#define WIFI_SSID_KEY  "ssid"
#define WIFI_PASS_KEY  "pass"
#define WIFI_HISTORY_KEY "history"

#define AP_SSID_PREFIX "AirQuality"
#define AP_PASS        "airquality"
#define AP_CHANNEL     10
#define AP_MAX_CONN    4

#define WIFI_CONNECTED_BIT BIT0

static EventGroupHandle_t s_wifi_event_group;
static SemaphoreHandle_t s_wifi_op_lock = NULL;
static esp_netif_t *s_sta_netif = NULL;
static TaskHandle_t s_reconnect_task = NULL;
static int s_retry_count = 0;

typedef struct {
    char ssid[33];
    char pass[65];
    bool hidden;
} wifi_saved_credential_t;

static bool ssid_matches_record(const wifi_ap_record_t *record, const char *ssid)
{
    return strncmp((const char *)record->ssid, ssid, sizeof(record->ssid)) == 0;
}

static void build_ap_ssid(char *ssid, size_t ssid_len)
{
    uint8_t mac[6] = {0};
    if (!ssid || ssid_len == 0) {
        return;
    }

    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        snprintf(ssid, ssid_len, AP_SSID_PREFIX "-%02X%02X", mac[4], mac[5]);
        return;
    }

    strlcpy(ssid, AP_SSID_PREFIX "-SETUP", ssid_len);
}

static esp_err_t load_wifi_history(wifi_saved_credential_t *history, size_t max_history, size_t *loaded)
{
    if (loaded) {
        *loaded = 0;
    }
    if (!history || max_history == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(WIFI_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    size_t blob_size = sizeof(wifi_saved_credential_t) * max_history;
    err = nvs_get_blob(nvs, WIFI_HISTORY_KEY, history, &blob_size);
    if (err == ESP_OK) {
        size_t count = blob_size / sizeof(wifi_saved_credential_t);
        if (count > max_history) {
            count = max_history;
        }
        if (loaded) {
            *loaded = count;
        }
    }
    nvs_close(nvs);
    return err;
}

static esp_err_t store_wifi_history(const wifi_saved_credential_t *history, size_t count)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(WIFI_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_blob(nvs, WIFI_HISTORY_KEY, history, sizeof(wifi_saved_credential_t) * count);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

static bool load_legacy_wifi_creds(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(WIFI_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return false;
    }

    size_t ssid_size = ssid_len;
    size_t pass_size = pass_len;
    err = nvs_get_str(nvs, WIFI_SSID_KEY, ssid, &ssid_size);
    if (err == ESP_OK) {
        err = nvs_get_str(nvs, WIFI_PASS_KEY, pass, &pass_size);
    }
    nvs_close(nvs);
    return (err == ESP_OK);
}

static bool find_saved_password(const char *ssid, char *pass, size_t pass_len)
{
    wifi_saved_credential_t history[WIFI_MANAGER_MAX_NETWORKS] = {0};
    size_t count = 0;
    if (load_wifi_history(history, WIFI_MANAGER_MAX_NETWORKS, &count) == ESP_OK) {
        for (size_t i = 0; i < count; i++) {
            if (strcmp(history[i].ssid, ssid) == 0) {
                strlcpy(pass, history[i].pass, pass_len);
                return true;
            }
        }
    }

    char legacy_ssid[33] = {0};
    char legacy_pass[65] = {0};
    if (load_legacy_wifi_creds(legacy_ssid, sizeof(legacy_ssid), legacy_pass, sizeof(legacy_pass)) &&
        strcmp(legacy_ssid, ssid) == 0) {
        strlcpy(pass, legacy_pass, pass_len);
        return true;
    }

    return false;
}

static void apply_sta_config_and_connect(const wifi_saved_credential_t *credential)
{
    if (!credential || credential->ssid[0] == '\0') {
        return;
    }

    if (!s_wifi_op_lock || xSemaphoreTake(s_wifi_op_lock, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGW(TAG, "Timed out waiting for Wi-Fi operation lock");
        return;
    }

    wifi_config_t sta_config = {0};
    strncpy((char *)sta_config.sta.ssid, credential->ssid, sizeof(sta_config.sta.ssid));
    strncpy((char *)sta_config.sta.password, credential->pass, sizeof(sta_config.sta.password));
    sta_config.sta.scan_method = credential->hidden ? WIFI_ALL_CHANNEL_SCAN : WIFI_FAST_SCAN;
    sta_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    sta_config.sta.threshold.authmode = WIFI_AUTH_OPEN;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &sta_config);
    if (err == ESP_OK) {
        err = esp_wifi_connect();
    }
    xSemaphoreGive(s_wifi_op_lock);

    ESP_ERROR_CHECK(err);
}

static bool choose_visible_saved_network(wifi_saved_credential_t *selected)
{
    if (!selected) {
        return false;
    }

    wifi_saved_credential_t *history = calloc(WIFI_MANAGER_MAX_NETWORKS, sizeof(*history));
    wifi_ap_record_t *records = calloc(WIFI_MANAGER_MAX_SCAN_RESULTS, sizeof(*records));
    if (!history || !records) {
        free(records);
        free(history);
        ESP_LOGE(TAG, "Out of memory while scanning saved networks");
        return false;
    }

    size_t history_count = 0;
    if (load_wifi_history(history, WIFI_MANAGER_MAX_NETWORKS, &history_count) != ESP_OK || history_count == 0) {
        free(records);
        free(history);
        return false;
    }

    if (!s_wifi_op_lock || xSemaphoreTake(s_wifi_op_lock, pdMS_TO_TICKS(10000)) != pdTRUE) {
        free(records);
        free(history);
        ESP_LOGW(TAG, "Timed out waiting for Wi-Fi operation lock");
        return false;
    }

    uint16_t ap_count = WIFI_MANAGER_MAX_SCAN_RESULTS;
    wifi_scan_config_t scan_config = {
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };

    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err == ESP_OK) {
        err = esp_wifi_scan_get_ap_records(&ap_count, records);
    }
    xSemaphoreGive(s_wifi_op_lock);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Saved network scan failed: %s", esp_err_to_name(err));
        free(records);
        free(history);
        return false;
    }

    int best_history_index = -1;
    int best_rssi = -128;
    for (size_t hi = 0; hi < history_count; hi++) {
        if (history[hi].hidden) {
            continue;
        }
        for (uint16_t ai = 0; ai < ap_count; ai++) {
            if (ssid_matches_record(&records[ai], history[hi].ssid) && records[ai].rssi > best_rssi) {
                best_rssi = records[ai].rssi;
                best_history_index = (int)hi;
            }
        }
    }

    if (best_history_index < 0) {
        free(records);
        free(history);
        return false;
    }

    *selected = history[best_history_index];
    free(records);
    free(history);
    return true;
}

static void reconnect_task(void *arg)
{
    (void)arg;
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        wifi_saved_credential_t selected = {0};
        if (choose_visible_saved_network(&selected)) {
            ESP_LOGI(TAG, "Trying saved Wi-Fi network: %s", selected.ssid);
            s_retry_count = 0;
            apply_sta_config_and_connect(&selected);
        }
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        if (s_retry_count < 5) {
            s_retry_count++;
            esp_wifi_connect();
            ESP_LOGI(TAG, "Retrying Wi-Fi connection (%d)", s_retry_count);
        } else {
            ESP_LOGW(TAG, "Wi-Fi connect failed, checking saved networks");
            if (s_reconnect_task) {
                xTaskNotifyGive(s_reconnect_task);
            }
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t save_wifi_creds(const char *ssid, const char *pass)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(WIFI_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(nvs, WIFI_SSID_KEY, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, WIFI_PASS_KEY, pass);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

static esp_err_t save_wifi_creds_to_history(const char *ssid, const char *pass, bool hidden)
{
    wifi_saved_credential_t history[WIFI_MANAGER_MAX_NETWORKS] = {0};
    size_t count = 0;
    esp_err_t err = load_wifi_history(history, WIFI_MANAGER_MAX_NETWORKS, &count);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND && err != ESP_ERR_NVS_NOT_INITIALIZED) {
        return err;
    }

    wifi_saved_credential_t updated = {0};
    strncpy(updated.ssid, ssid, sizeof(updated.ssid) - 1);
    strncpy(updated.pass, pass ? pass : "", sizeof(updated.pass) - 1);
    updated.hidden = hidden;

    wifi_saved_credential_t next[WIFI_MANAGER_MAX_NETWORKS] = {0};
    next[0] = updated;
    size_t next_count = 1;

    for (size_t i = 0; i < count && next_count < WIFI_MANAGER_MAX_NETWORKS; i++) {
        if (strcmp(history[i].ssid, ssid) == 0) {
            continue;
        }
        next[next_count++] = history[i];
    }

    return store_wifi_history(next, next_count);
}

bool wifi_manager_load_credentials(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    wifi_saved_credential_t history[WIFI_MANAGER_MAX_NETWORKS] = {0};
    size_t history_count = 0;
    if (load_wifi_history(history, WIFI_MANAGER_MAX_NETWORKS, &history_count) == ESP_OK && history_count > 0) {
        strlcpy(ssid, history[0].ssid, ssid_len);
        strlcpy(pass, history[0].pass, pass_len);
        return true;
    }

    return load_legacy_wifi_creds(ssid, ssid_len, pass, pass_len);
}

static void wifi_start_apsta(const char *sta_ssid, const char *sta_pass)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t ap_config = {
        .ap = {
            .channel = AP_CHANNEL,
            .max_connection = AP_MAX_CONN,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK,
        }
    };
    char ap_ssid[33] = {0};
    build_ap_ssid(ap_ssid, sizeof(ap_ssid));
    strncpy((char *)ap_config.ap.ssid, ap_ssid, sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len = strlen(ap_ssid);
    strncpy((char *)ap_config.ap.password, AP_PASS, sizeof(ap_config.ap.password));

    if (strlen(AP_PASS) == 0) {
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    wifi_config_t sta_config = {0};
    bool have_sta = (sta_ssid && sta_ssid[0] != '\0');
    if (have_sta) {
        strncpy((char *)sta_config.sta.ssid, sta_ssid, sizeof(sta_config.sta.ssid));
        strncpy((char *)sta_config.sta.password, sta_pass ? sta_pass : "", sizeof(sta_config.sta.password));
        sta_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        sta_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
        sta_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));

    if (have_sta) {
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    }

    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "AP SSID: %s  PASS: %s", ap_ssid, AP_PASS);
    ESP_LOGI(TAG, "Open http://192.168.4.1/ for setup");
}

void wifi_manager_init(void)
{
    s_wifi_event_group = xEventGroupCreate();
    s_wifi_op_lock = xSemaphoreCreateMutex();
    s_sta_netif = esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    xTaskCreate(reconnect_task, "wifi_reconnect", 6144, NULL, 4, &s_reconnect_task);

    char ssid[33] = {0};
    char pass[65] = {0};
    bool have = wifi_manager_load_credentials(ssid, sizeof(ssid), pass, sizeof(pass));
    if (have) {
        ESP_LOGI(TAG, "Loaded saved Wi-Fi SSID: %s", ssid);
        wifi_saved_credential_t history[WIFI_MANAGER_MAX_NETWORKS] = {0};
        size_t history_count = 0;
        if (load_wifi_history(history, WIFI_MANAGER_MAX_NETWORKS, &history_count) != ESP_OK || history_count == 0) {
            save_wifi_creds_to_history(ssid, pass, false);
        }
    } else {
        ESP_LOGI(TAG, "No saved Wi-Fi credentials");
    }

    wifi_start_apsta(have ? ssid : NULL, pass);
}

esp_err_t wifi_manager_save_and_connect(const char *ssid, const char *pass)
{
    return wifi_manager_save_and_connect_ex(ssid, pass, false);
}

esp_err_t wifi_manager_save_and_connect_ex(const char *ssid, const char *pass, bool hidden)
{
    if (!ssid || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    char resolved_pass[65] = {0};
    if (pass && pass[0] != '\0') {
        strlcpy(resolved_pass, pass, sizeof(resolved_pass));
    } else {
        find_saved_password(ssid, resolved_pass, sizeof(resolved_pass));
    }

    esp_err_t err = save_wifi_creds(ssid, resolved_pass);
    if (err != ESP_OK) {
        return err;
    }
    err = save_wifi_creds_to_history(ssid, resolved_pass, hidden);
    if (err != ESP_OK) {
        return err;
    }

    wifi_saved_credential_t credential = {0};
    strncpy(credential.ssid, ssid, sizeof(credential.ssid) - 1);
    strncpy(credential.pass, resolved_pass, sizeof(credential.pass) - 1);
    credential.hidden = hidden;

    s_retry_count = 0;
    apply_sta_config_and_connect(&credential);

    return ESP_OK;
}

esp_err_t wifi_manager_scan(wifi_manager_ap_t *aps, uint16_t *count)
{
    if (!aps || !count) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t max_results = *count;
    if (max_results > WIFI_MANAGER_MAX_SCAN_RESULTS) {
        max_results = WIFI_MANAGER_MAX_SCAN_RESULTS;
    }
    *count = 0;

    wifi_ap_record_t *records = calloc(WIFI_MANAGER_MAX_SCAN_RESULTS, sizeof(*records));
    wifi_saved_credential_t *history = calloc(WIFI_MANAGER_MAX_NETWORKS, sizeof(*history));
    if (!records || !history) {
        free(history);
        free(records);
        return ESP_ERR_NO_MEM;
    }

    if (!s_wifi_op_lock || xSemaphoreTake(s_wifi_op_lock, pdMS_TO_TICKS(10000)) != pdTRUE) {
        free(history);
        free(records);
        return ESP_ERR_TIMEOUT;
    }

    wifi_scan_config_t scan_config = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };

    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        xSemaphoreGive(s_wifi_op_lock);
        free(history);
        free(records);
        return err;
    }

    uint16_t ap_count = max_results;
    err = esp_wifi_scan_get_ap_records(&ap_count, records);
    xSemaphoreGive(s_wifi_op_lock);
    if (err != ESP_OK) {
        free(history);
        free(records);
        return err;
    }

    size_t history_count = 0;
    load_wifi_history(history, WIFI_MANAGER_MAX_NETWORKS, &history_count);

    for (uint16_t i = 0; i < ap_count; i++) {
        strlcpy(aps[i].ssid, (const char *)records[i].ssid, sizeof(aps[i].ssid));
        aps[i].rssi = records[i].rssi;
        aps[i].authmode = records[i].authmode;
        aps[i].saved = false;
        for (size_t hi = 0; hi < history_count; hi++) {
            if (strcmp(history[hi].ssid, aps[i].ssid) == 0) {
                aps[i].saved = true;
                break;
            }
        }
    }

    *count = ap_count;
    free(history);
    free(records);
    return ESP_OK;
}

size_t wifi_manager_get_saved_networks(wifi_manager_saved_network_t *networks, size_t max_networks)
{
    if (!networks || max_networks == 0) {
        return 0;
    }

    wifi_saved_credential_t history[WIFI_MANAGER_MAX_NETWORKS] = {0};
    size_t history_count = 0;
    if (load_wifi_history(history, WIFI_MANAGER_MAX_NETWORKS, &history_count) != ESP_OK) {
        return 0;
    }

    if (history_count > max_networks) {
        history_count = max_networks;
    }

    for (size_t i = 0; i < history_count; i++) {
        strlcpy(networks[i].ssid, history[i].ssid, sizeof(networks[i].ssid));
        networks[i].hidden = history[i].hidden;
    }
    return history_count;
}

bool wifi_manager_is_connected(void)
{
    if (!s_wifi_event_group) {
        return false;
    }

    EventBits_t bits = xEventGroupGetBits(s_wifi_event_group);
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

void wifi_manager_get_ip(char *buf, size_t len)
{
    if (!buf || len == 0) {
        return;
    }

    snprintf(buf, len, "0.0.0.0");
    if (!s_sta_netif) {
        return;
    }

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(s_sta_netif, &ip_info) == ESP_OK) {
        snprintf(buf, len, IPSTR, IP2STR(&ip_info.ip));
    }
}
