idf_component_register(
    SRCS "wifi_manager.c" "web_server.c" "dns_responder.c"
    INCLUDE_DIRS "."
    EMBED_FILES "web_config.html"
    REQUIRES config modbus led
             esp_wifi esp_netif esp_http_server esp_event lwip esp_timer
             cjson
)

set_source_files_properties(
    wifi_manager.c web_server.c dns_responder.c
    PROPERTIES COMPILE_OPTIONS "-Os"
)
