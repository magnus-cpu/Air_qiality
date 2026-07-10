#include "site_config.h"

#include <string.h>

#include "nvs.h"

#define SITE_CONFIG_NAMESPACE "sitecfg"
#define SITE_CONFIG_LOCATION_NAME_KEY "loc_name"
#define SITE_CONFIG_LATITUDE_KEY "latitude"
#define SITE_CONFIG_LONGITUDE_KEY "longitude"

static void sanitize_csv_text(char *value)
{
    if (!value) {
        return;
    }

    for (size_t i = 0; value[i] != '\0'; i++) {
        if (value[i] == ',' || value[i] == '\r' || value[i] == '\n' ||
            value[i] == '"' || value[i] == '\\') {
            value[i] = ' ';
        }
    }
}

esp_err_t site_config_load(site_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(config, 0, sizeof(*config));

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(SITE_CONFIG_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    size_t location_name_len = sizeof(config->location_name);
    size_t latitude_len = sizeof(config->latitude);
    size_t longitude_len = sizeof(config->longitude);

    esp_err_t location_err = nvs_get_str(nvs, SITE_CONFIG_LOCATION_NAME_KEY,
                                         config->location_name, &location_name_len);
    esp_err_t latitude_err = nvs_get_str(nvs, SITE_CONFIG_LATITUDE_KEY,
                                         config->latitude, &latitude_len);
    esp_err_t longitude_err = nvs_get_str(nvs, SITE_CONFIG_LONGITUDE_KEY,
                                          config->longitude, &longitude_len);
    nvs_close(nvs);

    if (location_err == ESP_ERR_NVS_NOT_FOUND &&
        latitude_err == ESP_ERR_NVS_NOT_FOUND &&
        longitude_err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NVS_NOT_FOUND;
    }

    if (location_err != ESP_OK && location_err != ESP_ERR_NVS_NOT_FOUND) {
        return location_err;
    }
    if (latitude_err != ESP_OK && latitude_err != ESP_ERR_NVS_NOT_FOUND) {
        return latitude_err;
    }
    if (longitude_err != ESP_OK && longitude_err != ESP_ERR_NVS_NOT_FOUND) {
        return longitude_err;
    }

    sanitize_csv_text(config->location_name);
    sanitize_csv_text(config->latitude);
    sanitize_csv_text(config->longitude);
    return ESP_OK;
}

esp_err_t site_config_save(const site_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    site_config_t clean = *config;
    sanitize_csv_text(clean.location_name);
    sanitize_csv_text(clean.latitude);
    sanitize_csv_text(clean.longitude);

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(SITE_CONFIG_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(nvs, SITE_CONFIG_LOCATION_NAME_KEY, clean.location_name);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, SITE_CONFIG_LATITUDE_KEY, clean.latitude);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, SITE_CONFIG_LONGITUDE_KEY, clean.longitude);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

bool site_config_has_location(const site_config_t *config)
{
    return config &&
           (config->location_name[0] != '\0' ||
            config->latitude[0] != '\0' ||
            config->longitude[0] != '\0');
}
