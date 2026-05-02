#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    SENSOR_GAS_NH3 = 0,
    SENSOR_GAS_RED = 1,
    SENSOR_GAS_OX = 2,
    SENSOR_GAS_COUNT
} sensor_gas_t;

typedef struct {
    int raw;
    int mv;
    float resistance_ohms;
    float ppm;
    bool calibrated;
    bool ppm_valid;
} sensor_channel_reading_t;

typedef struct {
    sensor_channel_reading_t nh3;
    sensor_channel_reading_t red;
    sensor_channel_reading_t ox;
} sensor_snapshot_t;

typedef struct {
    bool valid;
    float reference_resistance_ohms;
    float reference_ppm;
} sensor_calibration_point_t;

typedef struct {
    sensor_calibration_point_t nh3;
    sensor_calibration_point_t red;
    sensor_calibration_point_t ox;
} sensor_calibration_t;

void sensor_init(void);

bool sensor_read_all(sensor_snapshot_t *snapshot);

bool sensor_get_calibration(sensor_calibration_t *calibration);

bool sensor_capture_calibration(float nh3_ppm, float red_ppm, float ox_ppm);

bool sensor_clear_calibration(void);

int sensor_read_nh3(void);
int sensor_read_red(void);
int sensor_read_ox(void);
int sensor_read_nh3_mv(void);
int sensor_read_red_mv(void);
int sensor_read_ox_mv(void);
