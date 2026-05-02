#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void time_manager_init(void);

bool time_manager_is_synchronized(void);

bool time_manager_is_estimated(void);

uint64_t time_manager_get_epoch_ms(void);

void time_manager_get_status(char *buf, size_t len);
