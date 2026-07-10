#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char sample_uuid[72];
    uint64_t sample_id;
    uint64_t timestamp_ms;
    uint64_t uptime_ms;
    char location_name[64];
    char latitude[24];
    char longitude[24];
    int nh3_raw;
    int red_raw;
    int ox_raw;
    int nh3_mv;
    int red_mv;
    int ox_mv;
    float nh3_res_ohms;
    float red_res_ohms;
    float ox_res_ohms;
    float nh3_ppm;
    float red_ppm;
    float ox_ppm;
    bool nh3_ppm_valid;
    bool red_ppm_valid;
    bool ox_ppm_valid;
    int heater_on;
    int warmup;
    int since_change;
    bool time_synced;
    bool wifi_connected;
} telemetry_sample_t;

void telemetry_pipeline_init(void);

void telemetry_pipeline_handle_time_sync(void);

bool telemetry_pipeline_get_latest(telemetry_sample_t *sample);

void telemetry_pipeline_get_status(char *buf, size_t len);

bool telemetry_pipeline_get_today_file(char *buf, size_t len);
