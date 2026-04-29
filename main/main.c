#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "heater.h"
#include "sensor.h"
#include "web_server.h"
#include "wifi_manager.h"

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    heater_init();
    sensor_init();
    wifi_manager_init();
    web_server_start();
}
