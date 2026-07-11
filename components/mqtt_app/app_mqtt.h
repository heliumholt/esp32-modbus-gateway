#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Event bits for MQTT state signaling ---- */
#define MQTT_EVENT_CONNECTED_BIT    BIT0
#define MQTT_EVENT_DISCONNECTED_BIT BIT1

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
 * Supports both mqtt:// (TCP) and mqtts:// (TLS with CA cert bundle).
 *
 * @param on_data  Callback for incoming write commands (on write topic).
 * @param on_ota   Callback for incoming OTA commands (on OTA topic). May be NULL.
 */
esp_err_t mqtt_client_start(mqtt_data_cb_t on_data, mqtt_ota_cb_t on_ota);

/**
 * Trigger MQTT reconnection without destroying the client.
 * Uses esp-mqtt's built-in auto-reconnect mechanism.
 * LWT automatically publishes "offline" on disconnect.
 */
esp_err_t mqtt_client_reconnect(void);

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

/**
 * Get the MQTT event group handle.
 * Other tasks can wait on MQTT_EVENT_CONNECTED_BIT / MQTT_EVENT_DISCONNECTED_BIT.
 */
EventGroupHandle_t mqtt_client_get_event_group(void);

#ifdef __cplusplus
}
#endif
