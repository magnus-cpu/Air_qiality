#pragma once

#include <stdbool.h>

void heater_init(void);
void heater_set(bool on);
int heater_get(void);
int heater_is_warming(int warmup_seconds);
int heater_seconds_since_change(void);
