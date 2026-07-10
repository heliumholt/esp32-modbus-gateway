idf_component_register(
    SRCS "modbus_master.c" "modbus_params.c" "uart_config.c"
    INCLUDE_DIRS "."
    REQUIRES config driver esp_driver_uart esp_driver_gpio esp_timer esp_rom
)

set_source_files_properties(
    modbus_master.c modbus_params.c uart_config.c
    PROPERTIES COMPILE_OPTIONS "-Os"
)
