idf_component_register(
    SRCS "mqtt_client.c" "data_pipeline.c"
    INCLUDE_DIRS "."
    REQUIRES config led mqtt cjson esp_timer esp_event
)

set_source_files_properties(
    mqtt_client.c data_pipeline.c
    PROPERTIES COMPILE_OPTIONS "-Os"
)
