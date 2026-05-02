#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#define SD_CARD_MOUNT_POINT "/sdcard"

void sd_card_init(void);

esp_err_t sd_card_ensure_mounted(void);
esp_err_t sd_card_format(void);

bool sd_card_is_mounted(void);

void sd_card_get_status(char *buf, size_t len);
