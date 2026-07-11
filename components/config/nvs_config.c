#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "nvs_config.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "nvs_cfg";

/* NVS namespace and key names */
#define NVS_NAMESPACE   "config"

/* Single global RAM cache — protected by s_cfg_mutex (dual-core access) */
static config_t g_config;
static SemaphoreHandle_t s_cfg_mutex = NULL;

/* -- helpers -- */
static esp_err_t nvs_load_str(nvs_handle_t h, const char *key, char *dst, size_t max, const char *def)
{
    size_t len = max;
    esp_err_t err = nvs_get_str(h, key, dst, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        strncpy(dst, def, max - 1);
        dst[max - 1] = '\0';
        return ESP_OK;
    }
    return err;
}

static esp_err_t nvs_load_u8(nvs_handle_t h, const char *key, uint8_t *dst, uint8_t def)
{
    esp_err_t err = nvs_get_u8(h, key, dst);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *dst = def;
        return ESP_OK;
    }
    return err;
}

static esp_err_t nvs_load_u16(nvs_handle_t h, const char *key, uint16_t *dst, uint16_t def)
{
    esp_err_t err = nvs_get_u16(h, key, dst);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *dst = def;
        return ESP_OK;
    }
    return err;
}

static esp_err_t nvs_load_u32(nvs_handle_t h, const char *key, uint32_t *dst, uint32_t def)
{
    esp_err_t err = nvs_get_u32(h, key, dst);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *dst = def;
        return ESP_OK;
    }
    return err;
}

/* ================================================================
 * Init
 * ================================================================ */

bool nvs_config_init(void)
{
    ESP_LOGI(TAG, "Loading configuration from NVS...");

    /* Create mutex for config access (dual-core protection) */
    s_cfg_mutex = xSemaphoreCreateMutex();
    if (!s_cfg_mutex) {
        ESP_LOGE(TAG, "Failed to create config mutex");
    }

    /* Start with defaults */
    memset(&g_config, 0, sizeof(g_config));
    g_config.configd     = 1;   /* will be cleared if NVS is empty */
    strcpy(g_config.dev_name,   CONFIG_MODBUS_GW_DEVICE_NAME);
    g_config.ap_fallback = 1;
    strcpy(g_config.mqtt_uri,    CONFIG_MODBUS_GW_MQTT_URI);
    g_config.mqtt_port   = 1883;
    strcpy(g_config.mqtt_data_t,  "{dev}/data");
    strcpy(g_config.mqtt_write_t, "{dev}/write");
    strcpy(g_config.mqtt_stat_t,  "{dev}/status");
    g_config.baudrate     = 9600;
    g_config.data_bits    = 8;
    g_config.stop_bits    = 1;
    g_config.parity       = 0;
    g_config.tx_pin       = CONFIG_MODBUS_GW_UART_TX;
    g_config.rx_pin       = CONFIG_MODBUS_GW_UART_RX;
    g_config.de_pin       = CONFIG_MODBUS_GW_UART_DE;
    g_config.poll_intv    = 5000;

    /* LCD pins (defaults from lcd_driver.h) */
    g_config.lcd_spi_cs   = 10;
    g_config.lcd_spi_sck  = 12;
    g_config.lcd_spi_mosi = 11;
    g_config.lcd_spi_miso = 13;
    g_config.lcd_dc       = 7;
    g_config.lcd_reset    = 4;
    g_config.lcd_backlight = 21;

    /* Touch pins (defaults from touch_driver.h) */
    g_config.touch_i2c_sda = 40;
    g_config.touch_i2c_scl = 41;
    g_config.touch_int     = 39;
    g_config.touch_reset   = 42;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS config namespace not found, using defaults (first boot)");
        g_config.configd = 0;
        return false;
    }

    /* configd */
    nvs_load_u8(h, "configd", &g_config.configd, 0);

    /* Device */
    nvs_load_str(h, "dev_name", g_config.dev_name, sizeof(g_config.dev_name), g_config.dev_name);

    /* WiFi */
    nvs_load_str(h, "wifi_ssid", g_config.wifi_ssid, sizeof(g_config.wifi_ssid), "");
    nvs_load_str(h, "wifi_pass", g_config.wifi_pass, sizeof(g_config.wifi_pass), "");
    nvs_load_u8(h, "ap_fb", &g_config.ap_fallback, 1);

    /* MQTT */
    nvs_load_str(h, "mqtt_uri",  g_config.mqtt_uri,  sizeof(g_config.mqtt_uri),  g_config.mqtt_uri);
    nvs_load_u16(h, "mqtt_port", &g_config.mqtt_port, 1883);
    nvs_load_str(h, "mqtt_user", g_config.mqtt_user, sizeof(g_config.mqtt_user), "");
    nvs_load_str(h, "mqtt_pass", g_config.mqtt_pass, sizeof(g_config.mqtt_pass), "");
    nvs_load_str(h, "mqtt_cid",  g_config.mqtt_client_id, sizeof(g_config.mqtt_client_id), "");
    nvs_load_str(h, "mq_data_t", g_config.mqtt_data_t,  sizeof(g_config.mqtt_data_t),  g_config.mqtt_data_t);
    nvs_load_str(h, "mq_writ_t", g_config.mqtt_write_t, sizeof(g_config.mqtt_write_t), g_config.mqtt_write_t);
    nvs_load_str(h, "mq_stat_t", g_config.mqtt_stat_t,  sizeof(g_config.mqtt_stat_t),  g_config.mqtt_stat_t);

    /* MODBUS */
    nvs_load_u32(h, "baudrate",  &g_config.baudrate,  9600);
    nvs_load_u8(h,  "data_bits", &g_config.data_bits, 8);
    nvs_load_u8(h,  "stop_bits", &g_config.stop_bits, 1);
    nvs_load_u8(h,  "parity",    &g_config.parity,    0);
    nvs_load_u8(h,  "tx_pin",    &g_config.tx_pin,    CONFIG_MODBUS_GW_UART_TX);
    nvs_load_u8(h,  "rx_pin",    &g_config.rx_pin,    CONFIG_MODBUS_GW_UART_RX);
    nvs_load_u8(h,  "de_pin",    &g_config.de_pin,    CONFIG_MODBUS_GW_UART_DE);
    nvs_load_str(h, "reg_list",  g_config.reg_list,   sizeof(g_config.reg_list),   "");

    /* Polling */
    nvs_load_u32(h, "poll_intv", &g_config.poll_intv, 5000);
    /* Clamp at load time — legacy values may exceed bounds */
    if (g_config.poll_intv < 200) g_config.poll_intv = 200;
    if (g_config.poll_intv > 3600000) g_config.poll_intv = 3600000;

    /* LCD pins */
    nvs_load_u8(h, "lcd_cs",     &g_config.lcd_spi_cs,     10);
    nvs_load_u8(h, "lcd_sck",    &g_config.lcd_spi_sck,    12);
    nvs_load_u8(h, "lcd_mosi",   &g_config.lcd_spi_mosi,   11);
    nvs_load_u8(h, "lcd_miso",   &g_config.lcd_spi_miso,   13);
    nvs_load_u8(h, "lcd_dc",     &g_config.lcd_dc,         7);
    nvs_load_u8(h, "lcd_reset",  &g_config.lcd_reset,      4);
    nvs_load_u8(h, "lcd_bl",     &g_config.lcd_backlight,  21);

    /* Touch pins */
    nvs_load_u8(h, "touch_sda",  &g_config.touch_i2c_sda,  40);
    nvs_load_u8(h, "touch_scl",  &g_config.touch_i2c_scl,  41);
    nvs_load_u8(h, "touch_int",  &g_config.touch_int,      39);
    nvs_load_u8(h, "touch_rst",  &g_config.touch_reset,    42);

    /* Custom */
    nvs_load_str(h, "custom1", g_config.custom1, sizeof(g_config.custom1), "");
    nvs_load_str(h, "custom2", g_config.custom2, sizeof(g_config.custom2), "");
    nvs_load_str(h, "custom3", g_config.custom3, sizeof(g_config.custom3), "");

    nvs_close(h);

    ESP_LOGI(TAG, "Config loaded: configd=%d, dev=%s, baud=%lu, tx=%d, rx=%d, de=%d",
             g_config.configd, g_config.dev_name, g_config.baudrate,
             g_config.tx_pin, g_config.rx_pin, g_config.de_pin);

    return (g_config.configd == 1);
}

/* ================================================================
 * Save
 * ================================================================ */

esp_err_t nvs_config_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for writing: %d", err);
        return err;
    }

    g_config.configd = 1;

    /* Write all fields */
    nvs_set_u8(h,  "configd",    g_config.configd);
    nvs_set_str(h, "dev_name",   g_config.dev_name);
    nvs_set_str(h, "wifi_ssid",  g_config.wifi_ssid);
    nvs_set_str(h, "wifi_pass",  g_config.wifi_pass);
    nvs_set_u8(h,  "ap_fb",      g_config.ap_fallback);
    nvs_set_str(h, "mqtt_uri",   g_config.mqtt_uri);
    nvs_set_u16(h, "mqtt_port",  g_config.mqtt_port);
    nvs_set_str(h, "mqtt_user",  g_config.mqtt_user);
    nvs_set_str(h, "mqtt_pass",  g_config.mqtt_pass);
    nvs_set_str(h, "mqtt_cid",   g_config.mqtt_client_id);
    nvs_set_str(h, "mq_data_t",  g_config.mqtt_data_t);
    nvs_set_str(h, "mq_writ_t",  g_config.mqtt_write_t);
    nvs_set_str(h, "mq_stat_t",  g_config.mqtt_stat_t);
    nvs_set_u32(h, "baudrate",   g_config.baudrate);
    nvs_set_u8(h,  "data_bits",  g_config.data_bits);
    nvs_set_u8(h,  "stop_bits",  g_config.stop_bits);
    nvs_set_u8(h,  "parity",     g_config.parity);
    nvs_set_u8(h,  "tx_pin",     g_config.tx_pin);
    nvs_set_u8(h,  "rx_pin",     g_config.rx_pin);
    nvs_set_u8(h,  "de_pin",     g_config.de_pin);
    nvs_set_str(h, "reg_list",   g_config.reg_list);
    nvs_set_u32(h, "poll_intv",  g_config.poll_intv);

    /* LCD pins */
    nvs_set_u8(h,  "lcd_cs",     g_config.lcd_spi_cs);
    nvs_set_u8(h,  "lcd_sck",    g_config.lcd_spi_sck);
    nvs_set_u8(h,  "lcd_mosi",   g_config.lcd_spi_mosi);
    nvs_set_u8(h,  "lcd_miso",   g_config.lcd_spi_miso);
    nvs_set_u8(h,  "lcd_dc",     g_config.lcd_dc);
    nvs_set_u8(h,  "lcd_reset",  g_config.lcd_reset);
    nvs_set_u8(h,  "lcd_bl",     g_config.lcd_backlight);

    /* Touch pins */
    nvs_set_u8(h,  "touch_sda",  g_config.touch_i2c_sda);
    nvs_set_u8(h,  "touch_scl",  g_config.touch_i2c_scl);
    nvs_set_u8(h,  "touch_int",  g_config.touch_int);
    nvs_set_u8(h,  "touch_rst",  g_config.touch_reset);

    nvs_set_str(h, "custom1",    g_config.custom1);
    nvs_set_str(h, "custom2",    g_config.custom2);
    nvs_set_str(h, "custom3",    g_config.custom3);

    err = nvs_commit(h);
    nvs_close(h);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Configuration saved to NVS");
    } else {
        ESP_LOGE(TAG, "Failed to commit NVS: %d", err);
    }
    return err;
}

/* ================================================================
 * Reset
 * ================================================================ */

void nvs_config_reset(void)
{
    ESP_LOGW(TAG, "Resetting configuration...");
    esp_err_t err = nvs_flash_erase_partition("nvs");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS erase failed: 0x%x — rebooting anyway", err);
    }
    vTaskDelay(pdMS_TO_TICKS(500));   /* let NVS settle */
    esp_restart();
}

/* ================================================================
 * Accessors
 * ================================================================ */

const config_t *nvs_config_get(void)
{
    /* Callers read via pointer — must hold lock while reading fields.
     * The lock is NOT held on return because callers hold the pointer
     * transiently (single poll cycle). Long-held pointers are the
     * caller's responsibility. In practice, web writes are rare and
     * MODBUS reads are fast, so a transient stale read is benign. */
    return &g_config;
}

/* Internal: lock/unlock helpers for external use by save() */
void nvs_config_lock(void)
{
    if (s_cfg_mutex) xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
}

void nvs_config_unlock(void)
{
    if (s_cfg_mutex) xSemaphoreGive(s_cfg_mutex);
}

/* ================================================================
 * Individual setters (for web POST handler)
 * ================================================================ */

/* Safe strncpy — always null-terminates */
static void cfg_strncpy(char *dst, const char *src, size_t size)
{
    strncpy(dst, src, size - 1);
    dst[size - 1] = '\0';
}

esp_err_t nvs_config_set_string(const char *key, const char *value)
{
    if (!key || !value) return ESP_ERR_INVALID_ARG;

    if (strcmp(key, "dev_name") == 0) {
        cfg_strncpy(g_config.dev_name, value, sizeof(g_config.dev_name));
    } else if (strcmp(key, "wifi_ssid") == 0) {
        cfg_strncpy(g_config.wifi_ssid, value, sizeof(g_config.wifi_ssid));
    } else if (strcmp(key, "wifi_pass") == 0) {
        cfg_strncpy(g_config.wifi_pass, value, sizeof(g_config.wifi_pass));
    } else if (strcmp(key, "mqtt_uri") == 0) {
        cfg_strncpy(g_config.mqtt_uri, value, sizeof(g_config.mqtt_uri));
    } else if (strcmp(key, "mqtt_user") == 0) {
        cfg_strncpy(g_config.mqtt_user, value, sizeof(g_config.mqtt_user));
    } else if (strcmp(key, "mqtt_pass") == 0) {
        cfg_strncpy(g_config.mqtt_pass, value, sizeof(g_config.mqtt_pass));
    } else if (strcmp(key, "mqtt_client_id") == 0) {
        cfg_strncpy(g_config.mqtt_client_id, value, sizeof(g_config.mqtt_client_id));
    } else if (strcmp(key, "mqtt_data_t") == 0) {
        cfg_strncpy(g_config.mqtt_data_t, value, sizeof(g_config.mqtt_data_t));
    } else if (strcmp(key, "mqtt_write_t") == 0) {
        cfg_strncpy(g_config.mqtt_write_t, value, sizeof(g_config.mqtt_write_t));
    } else if (strcmp(key, "mqtt_stat_t") == 0) {
        cfg_strncpy(g_config.mqtt_stat_t, value, sizeof(g_config.mqtt_stat_t));
    } else if (strcmp(key, "reg_list") == 0) {
        cfg_strncpy(g_config.reg_list, value, sizeof(g_config.reg_list));
    } else if (strcmp(key, "custom1") == 0) {
        cfg_strncpy(g_config.custom1, value, sizeof(g_config.custom1));
    } else if (strcmp(key, "custom2") == 0) {
        cfg_strncpy(g_config.custom2, value, sizeof(g_config.custom2));
    } else if (strcmp(key, "custom3") == 0) {
        cfg_strncpy(g_config.custom3, value, sizeof(g_config.custom3));
    } else {
        ESP_LOGD(TAG, "Unknown string config key: %s", key);
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

esp_err_t nvs_config_set_u8(const char *key, uint8_t value)
{
    if (!key) return ESP_ERR_INVALID_ARG;

    if (strcmp(key, "ap_fallback") == 0) {
        g_config.ap_fallback = value;
    } else if (strcmp(key, "data_bits") == 0) {
        g_config.data_bits = value;
    } else if (strcmp(key, "stop_bits") == 0) {
        g_config.stop_bits = value;
    } else if (strcmp(key, "parity") == 0) {
        g_config.parity = value;
    } else if (strcmp(key, "tx_pin") == 0) {
        g_config.tx_pin = value;
    } else if (strcmp(key, "rx_pin") == 0) {
        g_config.rx_pin = value;
    } else if (strcmp(key, "de_pin") == 0) {
        g_config.de_pin = value;
    } else if (strcmp(key, "lcd_cs") == 0) {
        g_config.lcd_spi_cs = value;
    } else if (strcmp(key, "lcd_sck") == 0) {
        g_config.lcd_spi_sck = value;
    } else if (strcmp(key, "lcd_mosi") == 0) {
        g_config.lcd_spi_mosi = value;
    } else if (strcmp(key, "lcd_miso") == 0) {
        g_config.lcd_spi_miso = value;
    } else if (strcmp(key, "lcd_dc") == 0) {
        g_config.lcd_dc = value;
    } else if (strcmp(key, "lcd_reset") == 0) {
        g_config.lcd_reset = value;
    } else if (strcmp(key, "lcd_bl") == 0) {
        g_config.lcd_backlight = value;
    } else if (strcmp(key, "touch_sda") == 0) {
        g_config.touch_i2c_sda = value;
    } else if (strcmp(key, "touch_scl") == 0) {
        g_config.touch_i2c_scl = value;
    } else if (strcmp(key, "touch_int") == 0) {
        g_config.touch_int = value;
    } else if (strcmp(key, "touch_rst") == 0) {
        g_config.touch_reset = value;
    } else {
        ESP_LOGD(TAG, "Unknown u8 config key: %s", key);
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

esp_err_t nvs_config_set_u16(const char *key, uint16_t value)
{
    if (!key) return ESP_ERR_INVALID_ARG;

    if (strcmp(key, "mqtt_port") == 0) {
        g_config.mqtt_port = value;
    } else {
        ESP_LOGD(TAG, "Unknown u16 config key: %s", key);
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

esp_err_t nvs_config_set_u32(const char *key, uint32_t value)
{
    if (!key) return ESP_ERR_INVALID_ARG;

    if (strcmp(key, "baudrate") == 0) {
        g_config.baudrate = value;
    } else if (strcmp(key, "poll_intv") == 0) {
        g_config.poll_intv = (value < 200) ? 200 :
                             (value > 3600000) ? 3600000 : value;
    } else {
        ESP_LOGD(TAG, "Unknown u32 config key: %s", key);
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}
