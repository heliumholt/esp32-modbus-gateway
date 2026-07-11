#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "wifi_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Gateway status structure for UI display */

typedef enum {
    OTA_STATE_IDLE,
    OTA_STATE_DOWNLOADING,
    OTA_STATE_SUCCESS,
    OTA_STATE_FAILED,
} ota_state_t;

typedef struct {
    /* WiFi */
    wifi_state_t wifi_state;
    char ip_addr[16];           /* STA mode IP address */

    /* MQTT */
    bool mqtt_connected;

    /* MODBUS */
    uint32_t modbus_poll_count;   /* Successful poll count */
    uint32_t modbus_error_count;  /* Error count */
    uint8_t modbus_last_slave;    /* Last polled slave address */
    uint64_t modbus_last_poll_time; /* Last poll timestamp (ms) */

    /* System */
    uint32_t free_heap;           /* Free heap memory (bytes) */
    uint32_t uptime_sec;          /* Uptime in seconds */
    int8_t wifi_rssi;             /* WiFi signal strength (dBm) */

    /* OTA */
    uint8_t ota_progress;         /* OTA download progress (0-100) */
    ota_state_t ota_state;        /* OTA state */
} gateway_status_t;

#ifdef __cplusplus
}
#endif