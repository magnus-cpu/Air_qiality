#include "sensor.h"

#include <math.h>
#include <string.h>

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"
#include "esp_log.h"
#include "nvs.h"

#define NH3_ADC_CH ADC_CHANNEL_6 // IO34
#define RED_ADC_CH ADC_CHANNEL_0 // SENSOR_VP / IO36
#define OX_ADC_CH ADC_CHANNEL_7  // IO35

#define SENSOR_NAMESPACE "sensor"
#define SENSOR_CAL_KEY "cal"

// These values depend on the sensor breakout and divider around each analog output.
// Adjust if the board uses a different analog supply or load resistor.
#define SENSOR_SUPPLY_MV 3300.0f           // 3.3V
#define SENSOR_LOAD_RESISTOR_OHMS 56000.0f // 56000 Ohms

static const char *TAG = "sensor";
static adc_oneshot_unit_handle_t s_adc_handle;
static adc_cali_handle_t s_cali_handle;
static bool s_cali_enabled = false;
static sensor_calibration_t s_calibration = {0};

static adc_channel_t channel_for_gas(sensor_gas_t gas)
{
    switch (gas)
    {
    case SENSOR_GAS_NH3:
        return NH3_ADC_CH;
    case SENSOR_GAS_RED:
        return RED_ADC_CH;
    case SENSOR_GAS_OX:
    default:
        return OX_ADC_CH;
    }
}

static sensor_channel_reading_t *reading_for_gas(sensor_snapshot_t *snapshot, sensor_gas_t gas)
{
    switch (gas)
    {
    case SENSOR_GAS_NH3:
        return &snapshot->nh3;
    case SENSOR_GAS_RED:
        return &snapshot->red;
    case SENSOR_GAS_OX:
    default:
        return &snapshot->ox;
    }
}

static const sensor_calibration_point_t *calibration_for_gas(const sensor_calibration_t *cal, sensor_gas_t gas)
{
    switch (gas)
    {
    case SENSOR_GAS_NH3:
        return &cal->nh3;
    case SENSOR_GAS_RED:
        return &cal->red;
    case SENSOR_GAS_OX:
    default:
        return &cal->ox;
    }
}

static sensor_calibration_point_t *mutable_calibration_for_gas(sensor_calibration_t *cal, sensor_gas_t gas)
{
    switch (gas)
    {
    case SENSOR_GAS_NH3:
        return &cal->nh3;
    case SENSOR_GAS_RED:
        return &cal->red;
    case SENSOR_GAS_OX:
    default:
        return &cal->ox;
    }
}

static bool load_calibration(sensor_calibration_t *calibration)
{
    if (!calibration)
    {
        return false;
    }

    memset(calibration, 0, sizeof(*calibration));

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(SENSOR_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK)
    {
        return false;
    }

    size_t size = sizeof(*calibration);
    err = nvs_get_blob(nvs, SENSOR_CAL_KEY, calibration, &size);
    nvs_close(nvs);
    return err == ESP_OK && size == sizeof(*calibration);
}

static bool store_calibration(const sensor_calibration_t *calibration)
{
    if (!calibration)
    {
        return false;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(SENSOR_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK)
    {
        return false;
    }

    err = nvs_set_blob(nvs, SENSOR_CAL_KEY, calibration, sizeof(*calibration));
    if (err == ESP_OK)
    {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err == ESP_OK;
}

static void adc_calibration_init(void)
{
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    esp_err_t err = adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali_handle);
    if (err == ESP_OK)
    {
        s_cali_enabled = true;
        return;
    }
    ESP_LOGW(TAG, "Curve fitting not available (%d)", err);
#endif

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t line_cfg = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    esp_err_t err2 = adc_cali_create_scheme_line_fitting(&line_cfg, &s_cali_handle);
    if (err2 == ESP_OK)
    {
        s_cali_enabled = true;
        return;
    }
    ESP_LOGW(TAG, "Line fitting not available (%d)", err2);
#endif

    s_cali_enabled = false;
}

static int read_channel_raw(adc_channel_t ch)
{
    int value = 0;
    esp_err_t err = adc_oneshot_read(s_adc_handle, ch, &value);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "ADC read failed (%d)", err);
        return -1;
    }
    return value;
}

static int raw_to_mv(int raw)
{
    if (raw < 0 || !s_cali_enabled)
    {
        return -1;
    }

    int mv = 0;
    esp_err_t err = adc_cali_raw_to_voltage(s_cali_handle, raw, &mv);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "ADC cali failed (%d)", err);
        return -1;
    }
    return mv;
}

static float mv_to_resistance_ohms(int mv)
{
    if (mv <= 0 || mv >= (int)SENSOR_SUPPLY_MV)
    {
        return -1.0f;
    }

    float vout = (float)mv;
    return SENSOR_LOAD_RESISTOR_OHMS * ((SENSOR_SUPPLY_MV - vout) / vout);
}

static float estimate_ppm(float resistance_ohms, const sensor_calibration_point_t *calibration, bool *valid)
{
    if (valid)
    {
        *valid = false;
    }

    if (!calibration || !calibration->valid || calibration->reference_resistance_ohms <= 0.0f ||
        calibration->reference_ppm <= 0.0f || resistance_ohms <= 0.0f)
    {
        return -1.0f;
    }

    // First-pass model for this board: higher gas concentration drives the measured
    // output voltage down, which makes the derived sensor resistance go up.
    // So ppm should scale with Rs/Rref instead of the inverse.
    float ratio = resistance_ohms / calibration->reference_resistance_ohms;
    float ppm = calibration->reference_ppm * ratio;
    if (ppm < 0.0f || !isfinite(ppm))
    {
        return -1.0f;
    }

    if (valid)
    {
        *valid = true;
    }
    return ppm;
}

static void fill_reading(sensor_channel_reading_t *reading, sensor_gas_t gas)
{
    if (!reading)
    {
        return;
    }

    memset(reading, 0, sizeof(*reading));
    reading->raw = read_channel_raw(channel_for_gas(gas));
    reading->mv = raw_to_mv(reading->raw);
    reading->resistance_ohms = mv_to_resistance_ohms(reading->mv);

    const sensor_calibration_point_t *cal = calibration_for_gas(&s_calibration, gas);
    reading->calibrated = cal->valid;
    reading->ppm = estimate_ppm(reading->resistance_ohms, cal, &reading->ppm_valid);
}

void sensor_init(void)
{
    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };

    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_cfg, &s_adc_handle));

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };

    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc_handle, NH3_ADC_CH, &chan_cfg));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc_handle, RED_ADC_CH, &chan_cfg));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc_handle, OX_ADC_CH, &chan_cfg));

    adc_calibration_init();
    load_calibration(&s_calibration);
}

bool sensor_read_all(sensor_snapshot_t *snapshot)
{
    if (!snapshot)
    {
        return false;
    }

    fill_reading(&snapshot->nh3, SENSOR_GAS_NH3);
    fill_reading(&snapshot->red, SENSOR_GAS_RED);
    fill_reading(&snapshot->ox, SENSOR_GAS_OX);
    return true;
}

bool sensor_get_calibration(sensor_calibration_t *calibration)
{
    if (!calibration)
    {
        return false;
    }

    *calibration = s_calibration;
    return true;
}

bool sensor_capture_calibration(float nh3_ppm, float red_ppm, float ox_ppm)
{
    sensor_snapshot_t snapshot = {0};
    if (!sensor_read_all(&snapshot))
    {
        return false;
    }

    sensor_calibration_t next = s_calibration;
    const float ref_ppm[SENSOR_GAS_COUNT] = {nh3_ppm, red_ppm, ox_ppm};

    for (int gas = 0; gas < SENSOR_GAS_COUNT; gas++)
    {
        sensor_channel_reading_t *reading = reading_for_gas(&snapshot, (sensor_gas_t)gas);
        sensor_calibration_point_t *point = mutable_calibration_for_gas(&next, (sensor_gas_t)gas);
        if (ref_ppm[gas] <= 0.0f || reading->resistance_ohms <= 0.0f)
        {
            return false;
        }

        point->valid = true;
        point->reference_ppm = ref_ppm[gas];
        point->reference_resistance_ohms = reading->resistance_ohms;
    }

    if (!store_calibration(&next))
    {
        return false;
    }

    s_calibration = next;
    ESP_LOGI(TAG, "Sensor calibration saved");
    return true;
}

bool sensor_clear_calibration(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(SENSOR_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK)
    {
        return false;
    }

    err = nvs_erase_key(nvs, SENSOR_CAL_KEY);
    if (err == ESP_OK)
    {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);

    if (err != ESP_OK)
    {
        return false;
    }

    memset(&s_calibration, 0, sizeof(s_calibration));
    return true;
}

int sensor_read_nh3(void)
{
    return read_channel_raw(NH3_ADC_CH);
}

int sensor_read_red(void)
{
    return read_channel_raw(RED_ADC_CH);
}

int sensor_read_ox(void)
{
    return read_channel_raw(OX_ADC_CH);
}

int sensor_read_nh3_mv(void)
{
    return raw_to_mv(sensor_read_nh3());
}

int sensor_read_red_mv(void)
{
    return raw_to_mv(sensor_read_red());
}

int sensor_read_ox_mv(void)
{
    return raw_to_mv(sensor_read_ox());
}
