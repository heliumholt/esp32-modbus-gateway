idf_component_register(
    SRCS "ota_handler.c"
    INCLUDE_DIRS "."
    REQUIRES led esp_https_ota app_update esp-tls
)

set_source_files_properties(ota_handler.c PROPERTIES COMPILE_OPTIONS "-Os")
