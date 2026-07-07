#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Callback type for incoming MQTT data messages.
 * @param topic  Topic the message arrived on.
 * @param data   Raw payload data.
 * @param len    Payload length in bytes.
 */
typedef void (*mqtt_data_cb_t)(const char *topic, const char *data, int len);

/**
 * Start MQTT client.
 * Connects to the configured broker, sets LWT, subscribes to write topic.
 * @param on_data  Callback for incoming write commands (on write topic).
 */
esp_err_t mqtt_client_start(mqtt_data_cb_t on_data);

/**
 * Stop MQTT client gracefully (publishes offline and disconnects).
 */
esp_err_t mqtt_client_stop(void);

/**
 * Publish a message to the configured data topic.
 */
esp_err_t mqtt_client_publish_data(const char *payload, int len);

/**
 * Publish a message to the configured status topic.
 */
esp_err_t mqtt_client_publish_status(const char *payload, int len);

/**
 * Check if MQTT is currently connected.
 */
bool mqtt_client_is_connected(void);

#ifdef __cplusplus
}
#endif
