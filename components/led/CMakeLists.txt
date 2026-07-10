idf_component_register(
    SRCS "led_indicator.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_driver_rmt
)

set_source_files_properties(led_indicator.c PROPERTIES COMPILE_OPTIONS "-Os")
