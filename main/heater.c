#include "heater.h"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#define HEATER_EN_GPIO GPIO_NUM_5
// Set to 1 if your heater enable pin is active-low.
#define HEATER_ACTIVE_LOW 1

static const char *TAG = "heater";
static int s_heater_on = 0;
static TickType_t s_last_change_tick = 0;

static int to_gpio_level(int logical_on)
{
    return HEATER_ACTIVE_LOW ? !logical_on : logical_on;
}

static int seconds_since_change(void)
{
    TickType_t now = xTaskGetTickCount();
    TickType_t diff = now - s_last_change_tick;
    return (int)(diff * portTICK_PERIOD_MS / 1000);
}

void heater_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << HEATER_EN_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(HEATER_EN_GPIO, to_gpio_level(0));
    s_heater_on = 0;
    s_last_change_tick = xTaskGetTickCount();
    // Default to heater ON for stable sensor readings.
    heater_set(true);
}

void heater_set(bool on)
{
    int logical_on = on ? 1 : 0;
    if (logical_on != s_heater_on) {
        s_heater_on = logical_on;
        s_last_change_tick = xTaskGetTickCount();
    }
    gpio_set_level(HEATER_EN_GPIO, to_gpio_level(s_heater_on));
    ESP_LOGI(TAG, "heater_set: logical=%d active_low=%d", s_heater_on, HEATER_ACTIVE_LOW);
}

int heater_get(void)
{
    return s_heater_on;
}

int heater_seconds_since_change(void)
{
    return seconds_since_change();
}

int heater_is_warming(int warmup_seconds)
{
    if (!s_heater_on) {
        return 0;
    }
    return seconds_since_change() < warmup_seconds;
}
