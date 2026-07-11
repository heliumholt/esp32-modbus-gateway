idf_component_register(
    SRCS "display_manager.c"
         "lcd_driver.c"
         "touch_driver.c"
         "ui_main.c"
         "ui_modbus.c"
         "ui_system.c"
         "ui_ota.c"
    INCLUDE_DIRS "."
    REQUIRES config network mqtt_app modbus ota led
             nvs_flash esp_timer
             esp_driver_gpio    # GPIO driver (IDF 6.0+)
             esp_driver_ledc    # LEDC PWM driver (IDF 6.0+)
             esp_driver_spi     # SPI master driver (IDF 6.0+)
             esp_driver_i2c     # I2C master driver (IDF 6.0+)
             esp_lcd            # LCD panel interface
             esp_netif          # IP address for status display
             esp_wifi           # RSSI for status display
             lvgl               # LVGL graphics library
             esp_lvgl_port      # LVGL ESP-IDF porting layer
)

set_source_files_properties(
    display_manager.c lcd_driver.c touch_driver.c
    ui_main.c ui_modbus.c ui_system.c ui_ota.c
    PROPERTIES COMPILE_OPTIONS "-Os"
)