#include "device_registry.h"

#include <stdio.h>
#include <string.h>

#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs.h"

#include "site_config.h"

#define DEVICE_REGISTRY_NAMESPACE "device_reg"
#define DEVICE_REGISTRY_TOKEN_KEY "api_token"

static const char *TAG = "device_registry";

static char s_device_id[32] = "";
static char s_api_token[96] = "";
static bool s_initialized = false;

typedef struct {
    char *buffer;
    size_t capacity;
    size_t length;
} http_response_buffer_t;

static void set_status(char *status, size_t status_len, const char *value)
{
    if (!status || status_len == 0) {
        return;
    }

    strlcpy(status, value ? value : "", status_len);
}

static bool normalize_url(const char *input, char *output, size_t output_len)
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

static bool extract_json_string(const char *json, const char *key, char *out, size_t out_len)
{
    if (!json || !key || !out || out_len == 0) {
        return false;
    }

    char needle[48];
    int needle_len = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (needle_len <= 0 || (size_t)needle_len >= sizeof(needle)) {
        return false;
    }

    const char *start = strstr(json, needle);
    if (!start) {
        return false;
    }

    start += needle_len;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') {
        start++;
    }
    if (*start != ':') {
        return false;
    }
    start++;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') {
        start++;
    }
    if (*start != '"') {
        return false;
    }
    start++;

    size_t len = 0;
    while (start[len] != '\0' && start[len] != '"' && start[len] != '\r' && start[len] != '\n') {
        len++;
    }

    if (len == 0) {
        return false;
    }

    size_t copy_len = len < (out_len - 1) ? len : (out_len - 1);
    memcpy(out, start, copy_len);
    out[copy_len] = '\0';
    return true;
}

static bool build_base_url(const char *upload_url, char *base_url, size_t base_url_len)
{
    if (!upload_url || upload_url[0] == '\0' || !base_url || base_url_len == 0) {
        return false;
    }

    char normalized_url[192];
    if (!normalize_url(upload_url, normalized_url, sizeof(normalized_url))) {
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

static bool build_endpoint_url(const char *upload_url, const char *endpoint, char *url, size_t url_len)
{
    char base_url[160];
    if (!build_base_url(upload_url, base_url, sizeof(base_url))) {
        return false;
    }

    int written = snprintf(url, url_len, "%s%s", base_url, endpoint);
    return written > 0 && (size_t)written < url_len;
}

static void load_token_from_nvs(void)
{
    s_api_token[0] = '\0';

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(DEVICE_REGISTRY_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return;
    }

    size_t token_len = sizeof(s_api_token);
    err = nvs_get_str(nvs, DEVICE_REGISTRY_TOKEN_KEY, s_api_token, &token_len);
    if (err != ESP_OK) {
        s_api_token[0] = '\0';
    }
    nvs_close(nvs);
}

static bool save_token_to_nvs(const char *token)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(DEVICE_REGISTRY_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return false;
    }

    err = nvs_set_str(nvs, DEVICE_REGISTRY_TOKEN_KEY, token ? token : "");
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err == ESP_OK;
}

static esp_err_t http_event_handler(esp_http_client_event_t *event)
{
    if (!event || !event->user_data) {
        return ESP_OK;
    }

    http_response_buffer_t *resp = (http_response_buffer_t *)event->user_data;
    if (event->event_id != HTTP_EVENT_ON_DATA || !event->data || event->data_len <= 0 ||
        !resp->buffer || resp->capacity == 0) {
        return ESP_OK;
    }

    size_t remaining = resp->capacity - resp->length;
    if (remaining <= 1) {
        return ESP_OK;
    }

    size_t copy_len = (size_t)event->data_len;
    if (copy_len > remaining - 1) {
        copy_len = remaining - 1;
    }

    memcpy(resp->buffer + resp->length, event->data, copy_len);
    resp->length += copy_len;
    resp->buffer[resp->length] = '\0';
    return ESP_OK;
}

static void clear_token(void)
{
    s_api_token[0] = '\0';
    save_token_to_nvs("");
}

static bool http_post_json(const char *url,
                           const char *authorization,
                           const char *body,
                           char *response,
                           size_t response_len,
                           int *status_code)
{
    if (status_code) {
        *status_code = 0;
    }
    if (response && response_len > 0) {
        response[0] = '\0';
    }

    http_response_buffer_t response_buffer = {
        .buffer = response,
        .capacity = response_len,
        .length = 0,
    };

    esp_http_client_config_t config = {
        .url = NULL,
        .timeout_ms = 15000,
        .event_handler = http_event_handler,
        .user_data = &response_buffer,
    };

    char normalized_url[192];
    if (!normalize_url(url, normalized_url, sizeof(normalized_url))) {
        return false;
    }
    config.url = normalized_url;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return false;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    if (authorization && authorization[0] != '\0') {
        esp_http_client_set_header(client, "Authorization", authorization);
    }
    esp_http_client_set_post_field(client, body, (int)strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    int local_status = esp_http_client_get_status_code(client);

    esp_http_client_cleanup(client);

    if (status_code) {
        *status_code = local_status;
    }
    if (err != ESP_OK || local_status < 200 || local_status >= 300) {
        ESP_LOGW(TAG, "HTTP POST %s failed: err=%s http=%d response=%s",
                 normalized_url, esp_err_to_name(err), local_status,
                 response && response[0] != '\0' ? response : "<empty>");
    }
    return err == ESP_OK;
}

static bool use_fallback_token(const char *fallback_api_key)
{
    if (!fallback_api_key || strncasecmp(fallback_api_key, "Bearer ", 7) != 0) {
        return false;
    }

    const char *token = fallback_api_key + 7;
    if (token[0] == '\0') {
        return false;
    }

    strlcpy(s_api_token, token, sizeof(s_api_token));
    save_token_to_nvs(s_api_token);
    return true;
}

static bool authenticate_existing_token(const char *upload_url,
                                        char *status,
                                        size_t status_len,
                                        bool *token_rejected)
{
    if (token_rejected) {
        *token_rejected = false;
    }

    if (!upload_url || upload_url[0] == '\0' || s_api_token[0] == '\0') {
        return false;
    }

    char url[192];
    if (!build_endpoint_url(upload_url, "/api/devices/auth", url, sizeof(url))) {
        set_status(status, status_len, "device auth url invalid");
        return false;
    }

    char body[192];
    snprintf(body, sizeof(body),
             "{\"device_id\":\"%s\",\"api_token\":\"%s\"}",
             s_device_id, s_api_token);

    ESP_LOGD(TAG, "Authenticating device %s at %s", s_device_id, url);

    char response[256];
    int status_code = 0;
    if (!http_post_json(url, NULL, body, response, sizeof(response), &status_code)) {
        ESP_LOGW(TAG, "Device auth transport failed for %s", s_device_id);
        set_status(status, status_len, "device auth request failed");
        return false;
    }

    if (status_code >= 200 && status_code < 300) {
        ESP_LOGD(TAG, "Device auth OK for %s", s_device_id);
        set_status(status, status_len, "device authenticated");
        return true;
    }

    if (status_code == 401) {
        ESP_LOGW(TAG, "Device token rejected for %s", s_device_id);
        clear_token();
        if (token_rejected) {
            *token_rejected = true;
        }
        set_status(status, status_len, "device token rejected");
        return false;
    }

    ESP_LOGW(TAG, "Device auth server error for %s: http=%d response=%s",
             s_device_id, status_code, response[0] != '\0' ? response : "<empty>");
    set_status(status, status_len, "device auth server error");
    return false;
}

static bool register_device(const char *upload_url,
                            const char *registration_secret,
                            char *status,
                            size_t status_len)
{
    if (!upload_url || upload_url[0] == '\0') {
        set_status(status, status_len, "upload url missing");
        return false;
    }
    if (!registration_secret || registration_secret[0] == '\0') {
        set_status(status, status_len, "registration secret missing");
        return false;
    }

    char url[192];
    if (!build_endpoint_url(upload_url, "/api/devices/register", url, sizeof(url))) {
        set_status(status, status_len, "device register url invalid");
        return false;
    }

    site_config_t site = {0};
    site_config_load(&site);
    const char *device_name = site.location_name[0] != '\0' ? site.location_name : s_device_id;
    const char *location_name = site.location_name[0] != '\0' ? site.location_name : s_device_id;

    char body[384];
    snprintf(body, sizeof(body),
             "{\"device_id\":\"%s\",\"device_name\":\"%s\",\"location_name\":\"%s\",\"registration_secret\":\"%s\"}",
             s_device_id, device_name, location_name, registration_secret);

    ESP_LOGI(TAG, "Registering device %s at %s", s_device_id, url);

    char response[256];
    int status_code = 0;
    if (!http_post_json(url, NULL, body, response, sizeof(response), &status_code)) {
        ESP_LOGW(TAG, "Device register transport failed for %s", s_device_id);
        set_status(status, status_len, "device register request failed");
        return false;
    }

    ESP_LOGI(TAG, "Device register response for %s: http=%d body=%s",
             s_device_id, status_code, response[0] != '\0' ? response : "<empty>");

    if (status_code < 200 || status_code >= 300) {
        ESP_LOGW(TAG, "Device registration failed for %s: http=%d response=%s",
                 s_device_id, status_code, response[0] != '\0' ? response : "<empty>");
        set_status(status, status_len, status_code == 401 ? "device registration denied" : "device registration failed");
        return false;
    }

    char token[96];
    if (!extract_json_string(response, "api_token", token, sizeof(token))) {
        ESP_LOGW(TAG, "Device registration response missing api_token for %s", s_device_id);
        set_status(status, status_len, "device token missing in response");
        return false;
    }

    strlcpy(s_api_token, token, sizeof(s_api_token));
    save_token_to_nvs(s_api_token);
    set_status(status, status_len, "device registered");
    ESP_LOGI(TAG, "Registered device %s and saved token", s_device_id);
    return true;
}

void device_registry_init(void)
{
    if (s_initialized) {
        return;
    }

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_device_id, sizeof(s_device_id), "AQ-%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    load_token_from_nvs();
    s_initialized = true;
    ESP_LOGI(TAG, "Device ID: %s", s_device_id);
}

void device_registry_get_device_id(char *buf, size_t len)
{
    if (!buf || len == 0) {
        return;
    }

    if (!s_initialized) {
        device_registry_init();
    }
    strlcpy(buf, s_device_id, len);
}

bool device_registry_get_api_token(char *buf, size_t len)
{
    if (!buf || len == 0) {
        return false;
    }

    if (!s_initialized) {
        device_registry_init();
    }
    if (s_api_token[0] == '\0') {
        buf[0] = '\0';
        return false;
    }

    strlcpy(buf, s_api_token, len);
    return true;
}

bool device_registry_ensure_authenticated(const char *upload_url,
                                          const char *fallback_api_key,
                                          const char *registration_secret,
                                          char *status,
                                          size_t status_len)
{
    if (!s_initialized) {
        device_registry_init();
    }

    if (s_api_token[0] == '\0') {
        use_fallback_token(fallback_api_key);
    }

    bool had_token = s_api_token[0] != '\0';
    bool token_rejected = false;
    ESP_LOGD(TAG, "Ensure auth start: device=%s had_token=%d", s_device_id, had_token ? 1 : 0);
    if (authenticate_existing_token(upload_url, status, status_len, &token_rejected)) {
        return true;
    }

    if (had_token && !token_rejected) {
        ESP_LOGW(TAG, "Keeping existing token for %s after non-rejection auth failure", s_device_id);
        return false;
    }

    if (register_device(upload_url, registration_secret, status, status_len)) {
        set_status(status, status_len, "device registered");
        return true;
    }

    return false;
}
