#pragma once

#include <stdbool.h>

#include "esp_err.h"

#define SITE_CONFIG_LOCATION_NAME_MAX 64
#define SITE_CONFIG_COORD_MAX 24

typedef struct {
    char location_name[SITE_CONFIG_LOCATION_NAME_MAX];
    char latitude[SITE_CONFIG_COORD_MAX];
    char longitude[SITE_CONFIG_COORD_MAX];
} site_config_t;

esp_err_t site_config_load(site_config_t *config);

esp_err_t site_config_save(const site_config_t *config);

bool site_config_has_location(const site_config_t *config);
