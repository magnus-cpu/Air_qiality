#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "heater.h"
#include "mqtt_manager.h"
#include "sd_card.h"
#include "sensor.h"
#include "telemetry_pipeline.h"
#include "time_manager.h"
#include "web_server.h"
#include "wifi_manager.h"

static const char *reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_UNKNOWN: return "unknown";
    case ESP_RST_POWERON: return "poweron";
    case ESP_RST_EXT: return "external";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt_wdt";
    case ESP_RST_TASK_WDT: return "task_wdt";
    case ESP_RST_WDT: return "wdt";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "sdio";
    default: return "other";
    }
}

void app_main(void)
{
    esp_reset_reason_t reset_reason = esp_reset_reason();

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    if (reset_reason == ESP_RST_BROWNOUT) {
        ESP_LOGW("main", "Boot after brownout reset. Check regulator headroom, SD card supply, and wiring.");
    } else {
        ESP_LOGI("main", "Boot reset reason: %s", reset_reason_name(reset_reason));
    }

    heater_init();
    sensor_init();
    sd_card_init();
    wifi_manager_init();
    time_manager_init();
    mqtt_manager_init();
    telemetry_pipeline_init();
    web_server_start();
}
