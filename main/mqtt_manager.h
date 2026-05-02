#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#define MQTT_MANAGER_URI_MAX_LEN 128
#define MQTT_MANAGER_TOPIC_MAX_LEN 64

typedef struct {
    char broker_uri[MQTT_MANAGER_URI_MAX_LEN];
    char base_topic[MQTT_MANAGER_TOPIC_MAX_LEN];
    bool enabled;
} mqtt_manager_config_t;

void mqtt_manager_init(void);

esp_err_t mqtt_manager_save_config(const mqtt_manager_config_t *config);

bool mqtt_manager_load_config(mqtt_manager_config_t *config);

bool mqtt_manager_is_connected(void);

void mqtt_manager_get_status(char *buf, size_t len);

void mqtt_manager_publish_now(void);

