#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Max length of an OTA firmware URL.
 */
#define OTA_URL_MAX_LEN  256

/**
 * Callback type for OTA status messages (e.g. "OTA starting",
 * "OTA download failed: xxx").  The OTA module itself only logs;
 * if a callback is registered the callee can forward to MQTT, etc.
 *
 * @param status  Human-readable status string (not owned by callee).
 * @param error   true → this is an error message, false → informational.
 */
typedef void (*ota_status_cb_t)(const char *status, bool error);

/**
 * Initialize OTA subsystem (creates task + semaphore).
 * Must be called once after FreeRTOS scheduler starts.
 *
 * @param on_status  Optional callback for status/progress messages
 *                   (may be NULL if only ESP_LOG output is desired).
 */
esp_err_t ota_handler_init(ota_status_cb_t on_status);

/**
 * Request an OTA firmware update from the given HTTPS/HTTP URL.
 * Non-blocking — posts to internal queue, processed by OTA task.
 * Safe to call from any task context (including MQTT event handler).
 *
 * The request is silently ignored if an OTA is already in progress.
 *
 * @param url  Full URL to firmware binary (https://... or http://...)
 * @return ESP_OK if request was accepted
 */
esp_err_t ota_request(const char *url);

/**
 * Check if an OTA update is currently in progress.
 */
bool ota_is_in_progress(void);

#ifdef __cplusplus
}
#endif
