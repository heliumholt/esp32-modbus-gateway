#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * Gateway LED state definitions
 *
 * Single WS2812 on GPIO 38 indicates overall system status via
 * color + blink pattern.  Call led_indicator_set() from any task.
 * ================================================================ */

typedef enum {
    LED_STATE_AP_MODE,           /* Yellow slow blink — AP active, awaiting config */
    LED_STATE_STA_CONNECTING,    /* Blue fast blink — connecting to WiFi STA */
    LED_STATE_STA_READY,         /* Cyan solid — WiFi connected, MQTT pending */
    LED_STATE_MQTT_CONNECTED,    /* Green solid — fully operational */
    LED_STATE_MODBUS_ERR,        /* Red 1-tick flash — last MODBUS poll had error */
    LED_STATE_OTA_PROGRESS,      /* Purple pulse — OTA firmware download */
    LED_STATE_OTA_SUCCESS,       /* Green fast blink x3 — OTA done, rebooting */
    LED_STATE_FACTORY_RESET,     /* Red fast blink — NVS erasing */
    LED_STATE_OFF,               /* LED off */
} led_state_t;

/**
 * Initialize WS2812 LED on the given GPIO pin.
 * Creates a low-priority background task for pattern generation.
 */
esp_err_t led_indicator_init(uint8_t gpio);

/**
 * Set the LED state.  May be called from any task or ISR-safe
 * context (posts to a lightweight queue internally).
 */
void led_indicator_set(led_state_t state);

#ifdef __cplusplus
}
#endif
