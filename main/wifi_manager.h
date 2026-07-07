#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* WiFi state */
typedef enum {
    WIFI_STATE_AP_MODE,
    WIFI_STATE_STA_START,
    WIFI_STATE_STA_CONNECTING,
    WIFI_STATE_STA_READY,
    WIFI_STATE_STA_LOST,
} wifi_state_t;

/* Event group bits for WiFi state signaling */
#define WIFI_EVENT_GOT_IP      BIT0
#define WIFI_EVENT_DISCONNECTED BIT1
#define WIFI_EVENT_AP_STARTED   BIT2
#define WIFI_EVENT_CONFIG_SAVED BIT3

/**
 * Initialize WiFi subsystem.
 * If configd==true: starts STA mode immediately.
 * If configd==false: starts AP mode (waits for web config).
 */
void wifi_manager_init(void);

/**
 * Called by web server when config has been saved.
 * Triggers transition from AP to STA.
 */
void wifi_manager_on_config_saved(void);

/**
 * Get current WiFi state.
 */
wifi_state_t wifi_manager_get_state(void);

/**
 * Get event group handle (for other tasks to wait on WiFi events).
 */
void *wifi_manager_get_event_group(void);

/**
 * Stop WiFi and web server (called before reconfiguration).
 */
void wifi_manager_stop_all(void);

#ifdef __cplusplus
}
#endif
