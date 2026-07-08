#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * Configuration structure — cached in RAM after init
 * All strings are fixed-length char arrays (no dynamic allocation)
 * ================================================================ */

#define CONFIG_STR_LEN_DEV_NAME    32
#define CONFIG_STR_LEN_WIFI_SSID   32
#define CONFIG_STR_LEN_WIFI_PASS   64
#define CONFIG_STR_LEN_MQTT_URI   128
#define CONFIG_STR_LEN_MQTT_USER   32
#define CONFIG_STR_LEN_MQTT_PASS   32
#define CONFIG_STR_LEN_MQTT_CID    64
#define CONFIG_STR_LEN_TOPIC       64
#define CONFIG_STR_LEN_REG_LIST  1024
#define CONFIG_STR_LEN_CUSTOM     128

typedef struct {
    /* Configured flag */
    uint8_t  configd;

    /* Device */
    char     dev_name[CONFIG_STR_LEN_DEV_NAME];

    /* WiFi STA */
    char     wifi_ssid[CONFIG_STR_LEN_WIFI_SSID];
    char     wifi_pass[CONFIG_STR_LEN_WIFI_PASS];
    uint8_t  ap_fallback;

    /* MQTT broker */
    char     mqtt_uri[CONFIG_STR_LEN_MQTT_URI];
    uint16_t mqtt_port;
    char     mqtt_user[CONFIG_STR_LEN_MQTT_USER];
    char     mqtt_pass[CONFIG_STR_LEN_MQTT_PASS];
    char     mqtt_client_id[CONFIG_STR_LEN_MQTT_CID];

    /* MQTT topics (with {dev} placeholder) */
    char     mqtt_data_t[CONFIG_STR_LEN_TOPIC];
    char     mqtt_write_t[CONFIG_STR_LEN_TOPIC];
    char     mqtt_stat_t[CONFIG_STR_LEN_TOPIC];

    /* MODBUS / UART */
    uint32_t baudrate;
    uint8_t  data_bits;
    uint8_t  stop_bits;
    uint8_t  parity;       /* 0=none, 1=odd, 2=even */
    uint8_t  tx_pin;
    uint8_t  rx_pin;
    uint8_t  de_pin;
    char     reg_list[CONFIG_STR_LEN_REG_LIST];

    /* Polling */
    uint32_t poll_intv;    /* milliseconds */

    /* Custom parameters */
    char     custom1[CONFIG_STR_LEN_CUSTOM];
    char     custom2[CONFIG_STR_LEN_CUSTOM];
    char     custom3[CONFIG_STR_LEN_CUSTOM];
} config_t;

/* ================================================================
 * Public API
 * ================================================================ */

/**
 * Initialize NVS and load all config into RAM cache.
 * Returns true if device has been configured (configd == 1).
 * If NVS is empty/corrupted, loads defaults and configd remains 0.
 */
bool nvs_config_init(void);

/**
 * Write current RAM cache to NVS. Sets configd = 1.
 * Call this after web form submission.
 */
esp_err_t nvs_config_save(void);

/**
 * Erase the entire config namespace and reboot.
 */
void nvs_config_reset(void);

/**
 * Get pointer to the read-only RAM-cached config.
 */
const config_t *nvs_config_get(void);

/**
 * Individual setters — update RAM cache and immediately write to NVS.
 * Used by the web server POST handler.
 */
esp_err_t nvs_config_set_string(const char *key, const char *value);
esp_err_t nvs_config_set_u8(const char *key, uint8_t value);
esp_err_t nvs_config_set_u16(const char *key, uint16_t value);
esp_err_t nvs_config_set_u32(const char *key, uint32_t value);

#ifdef __cplusplus
}
#endif
