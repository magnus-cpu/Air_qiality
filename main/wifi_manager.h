#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_wifi_types.h"

#define WIFI_MANAGER_MAX_NETWORKS 8
#define WIFI_MANAGER_MAX_SCAN_RESULTS 16

typedef struct {
    char ssid[33];
    int rssi;
    wifi_auth_mode_t authmode;
    bool saved;
} wifi_manager_ap_t;

typedef struct {
    char ssid[33];
    bool hidden;
} wifi_manager_saved_network_t;

void wifi_manager_init(void);

bool wifi_manager_load_credentials(char *ssid, size_t ssid_len, char *pass, size_t pass_len);

esp_err_t wifi_manager_save_and_connect(const char *ssid, const char *pass);

esp_err_t wifi_manager_save_and_connect_ex(const char *ssid, const char *pass, bool hidden);

esp_err_t wifi_manager_scan(wifi_manager_ap_t *aps, uint16_t *count);

size_t wifi_manager_get_saved_networks(wifi_manager_saved_network_t *networks, size_t max_networks);

bool wifi_manager_is_connected(void);

void wifi_manager_get_ip(char *buf, size_t len);
