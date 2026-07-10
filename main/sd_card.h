#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "esp_err.h"

#define SD_CARD_MOUNT_POINT "/sdcard"

void sd_card_init(void);

esp_err_t sd_card_ensure_mounted(void);
esp_err_t sd_card_format(void);
esp_err_t sd_card_begin_io(void);
void sd_card_end_io(void);
esp_err_t sd_card_sync_file(FILE *f);
bool sd_card_write_allowed(void);
void sd_card_get_mode(char *buf, size_t len);

bool sd_card_is_mounted(void);

void sd_card_get_status(char *buf, size_t len);
