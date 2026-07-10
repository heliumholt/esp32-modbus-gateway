idf_component_register(
    SRCS "nvs_config.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash
)

set_source_files_properties(nvs_config.c PROPERTIES COMPILE_OPTIONS "-Os")
