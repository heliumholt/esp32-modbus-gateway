idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES config network modbus mqtt_app ota led
             nvs_flash esp_event esp_netif esp_timer
             spi_flash cjson esp_driver_gpio
)

set_source_files_properties(app_main.c PROPERTIES COMPILE_OPTIONS "-Os")
