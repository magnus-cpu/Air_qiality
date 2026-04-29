#include "sensor.h"

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"
#include "esp_log.h"

#define NH3_ADC_CH     ADC_CHANNEL_6  // IO34
#define RED_ADC_CH     ADC_CHANNEL_0  // SENSOR_VP / IO36
#define OX_ADC_CH      ADC_CHANNEL_7  // IO35

static const char *TAG = "sensor";
static adc_oneshot_unit_handle_t s_adc_handle;
static adc_cali_handle_t s_cali_handle;
static bool s_cali_enabled = false;

static void adc_calibration_init(void)
{
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    esp_err_t err = adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali_handle);
    if (err == ESP_OK) {
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
    if (err2 == ESP_OK) {
        s_cali_enabled = true;
        return;
    }
    ESP_LOGW(TAG, "Line fitting not available (%d)", err2);
#endif

    s_cali_enabled = false;
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
}

static int read_channel(adc_channel_t ch)
{
    int value = 0;
    esp_err_t err = adc_oneshot_read(s_adc_handle, ch, &value);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ADC read failed (%d)", err);
        return -1;
    }
    return value;
}

static int read_channel_mv(adc_channel_t ch)
{
    int raw = read_channel(ch);
    if (raw < 0) {
        return -1;
    }
    if (!s_cali_enabled) {
        return -1;
    }
    int mv = 0;
    esp_err_t err = adc_cali_raw_to_voltage(s_cali_handle, raw, &mv);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ADC cali failed (%d)", err);
        return -1;
    }
    return mv;
}

int sensor_read_nh3(void)
{
    return read_channel(NH3_ADC_CH);
}

int sensor_read_red(void)
{
    return read_channel(RED_ADC_CH);
}

int sensor_read_ox(void)
{
    return read_channel(OX_ADC_CH);
}

int sensor_read_nh3_mv(void)
{
    return read_channel_mv(NH3_ADC_CH);
}

int sensor_read_red_mv(void)
{
    return read_channel_mv(RED_ADC_CH);
}

int sensor_read_ox_mv(void)
{
    return read_channel_mv(OX_ADC_CH);
}
