#include "wifi_manager.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs.h"

static const char *TAG = "wifi_manager";

#define WIFI_NAMESPACE "wifi"
#define WIFI_SSID_KEY  "ssid"
#define WIFI_PASS_KEY  "pass"

#define AP_SSID        "AirQuality-Setup"
#define AP_PASS        "airquality"
#define AP_CHANNEL     10
#define AP_MAX_CONN    4

#define WIFI_CONNECTED_BIT BIT0

static EventGroupHandle_t s_wifi_event_group;
static esp_netif_t *s_sta_netif = NULL;
static int s_retry_count = 0;

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
            ESP_LOGW(TAG, "Wi-Fi connect failed, staying on AP for config");
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

bool wifi_manager_load_credentials(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(WIFI_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return false;
    }

    size_t ssid_size = ssid_len;
    size_t pass_size = pass_len;
    err = nvs_get_str(nvs, WIFI_SSID_KEY, ssid, &ssid_size);
    if (err != ESP_OK) {
        nvs_close(nvs);
        return false;
    }

    err = nvs_get_str(nvs, WIFI_PASS_KEY, pass, &pass_size);
    nvs_close(nvs);
    return (err == ESP_OK);
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
    strncpy((char *)ap_config.ap.ssid, AP_SSID, sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len = strlen(AP_SSID);
    strncpy((char *)ap_config.ap.password, AP_PASS, sizeof(ap_config.ap.password));

    if (strlen(AP_PASS) == 0) {
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    wifi_config_t sta_config = {0};
    bool have_sta = (sta_ssid && sta_ssid[0] != '\0');
    if (have_sta) {
        strncpy((char *)sta_config.sta.ssid, sta_ssid, sizeof(sta_config.sta.ssid));
        strncpy((char *)sta_config.sta.password, sta_pass ? sta_pass : "", sizeof(sta_config.sta.password));
        sta_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));

    if (have_sta) {
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    }

    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "AP SSID: %s  PASS: %s", AP_SSID, AP_PASS);
    ESP_LOGI(TAG, "Open http://192.168.4.1/ for setup");
}

void wifi_manager_init(void)
{
    s_wifi_event_group = xEventGroupCreate();
    s_sta_netif = esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    char ssid[33] = {0};
    char pass[65] = {0};
    bool have = wifi_manager_load_credentials(ssid, sizeof(ssid), pass, sizeof(pass));
    if (have) {
        ESP_LOGI(TAG, "Loaded saved Wi-Fi SSID: %s", ssid);
    } else {
        ESP_LOGI(TAG, "No saved Wi-Fi credentials");
    }

    wifi_start_apsta(have ? ssid : NULL, pass);
}

esp_err_t wifi_manager_save_and_connect(const char *ssid, const char *pass)
{
    esp_err_t err = save_wifi_creds(ssid, pass);
    if (err != ESP_OK) {
        return err;
    }

    wifi_config_t sta_config = {0};
    strncpy((char *)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid));
    strncpy((char *)sta_config.sta.password, pass, sizeof(sta_config.sta.password));
    sta_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_connect());

    return ESP_OK;
}

bool wifi_manager_is_connected(void)
{
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
