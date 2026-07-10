#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Callback type for incoming MQTT write-command messages.
 * @param topic  Topic the message arrived on.
 * @param data   Raw payload data.
 * @param len    Payload length in bytes.
 */
typedef void (*mqtt_data_cb_t)(const char *topic, const char *data, int len);

/**
 * Callback type for incoming MQTT OTA messages.
 * Same signature as data callback — receives topic + raw payload.
 * Typical payload: {"url": "https://ota.example.com/fw.bin"}
 */
typedef void (*mqtt_ota_cb_t)(const char *topic, const char *data, int len);

/**
 * Start MQTT client.
 * Connects to the configured broker, sets LWT, subscribes to write + OTA topics.
 *
 * @param on_data  Callback for incoming write commands (on write topic).
 * @param on_ota   Callback for incoming OTA commands (on OTA topic). May be NULL.
 */
esp_err_t mqtt_client_start(mqtt_data_cb_t on_data, mqtt_ota_cb_t on_ota);

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
