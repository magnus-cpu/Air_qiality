idf_component_register(
    SRCS
        "main.c"
        "wifi_manager.c"
        "web_server.c"
        "sensor.c"
        "heater.c"
        "sd_card.c"
        "time_manager.c"
        "mqtt_manager.c"
        "telemetry_pipeline.c"
    INCLUDE_DIRS "."
)
