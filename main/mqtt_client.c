#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "mqtt_client.h"       /* ESP-MQTT library (managed component) */
#include "app_mqtt.h"          /* local function declarations */
#include "nvs_config.h"
#include "led_indicator.h"

static const char *TAG = "mqtt";

static esp_mqtt_client_handle_t s_client = NULL;
static SemaphoreHandle_t s_client_mutex = NULL;
static mqtt_data_cb_t s_data_cb = NULL;
static mqtt_ota_cb_t  s_ota_cb  = NULL;
static volatile bool s_connected = false;

/* Resolved topic strings (with {dev} replaced) */
static char s_topic_data[64];
static char s_topic_write[64];
static char s_topic_status[64];
static char s_topic_ota[64];

/* Cached topic lengths — set once after resolve_topic() */
static size_t s_topic_write_len;
static size_t s_topic_ota_len;

/* OTA topic template — can be overridden via Kconfig */
#define OTA_TOPIC_TEMPLATE  "{dev}/ota"

/* ================================================================
 * Topic substitution
 * ================================================================ */

static void resolve_topic(const char *template, char *out, size_t out_len)
{
    const config_t *cfg = nvs_config_get();
    const char *pos = template;
    char *dst = out;
    char *end = out + out_len - 1;

    while (*pos && dst < end) {
        if (strncmp(pos, "{dev}", 5) == 0) {
            size_t n = strlen(cfg->dev_name);
            if (dst + n > end) n = end - dst;
            memcpy(dst, cfg->dev_name, n);
            dst += n;
            pos += 5;
        } else {
            *dst++ = *pos++;
        }
    }
    *dst = '\0';
}

/* ================================================================
 * MQTT event handler
 * ================================================================ */

static void mqtt_event_handler(void *arg, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)event_data;

    switch (event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT connected to broker");
        s_connected = true;
        led_indicator_set(LED_STATE_MQTT_CONNECTED);

        /* Publish birth message */
        mqtt_client_publish_status("online", 6);

        /* Subscribe to write topic */
        ESP_LOGI(TAG, "Subscribing to: %s", s_topic_write);
        esp_mqtt_client_subscribe(s_client, s_topic_write, 1);

        /* Subscribe to OTA topic */
        ESP_LOGI(TAG, "Subscribing to: %s", s_topic_ota);
        esp_mqtt_client_subscribe(s_client, s_topic_ota, 1);
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT disconnected");
        s_connected = false;
        led_indicator_set(LED_STATE_STA_READY);  /* back to WiFi-only state */
        break;

    case MQTT_EVENT_DATA:
        if (ev->data_len > 0) {
            ESP_LOGI(TAG, "MQTT received: topic=%.*s, data=%.*s",
                     ev->topic_len, ev->topic,
                     ev->data_len, ev->data);

            /* Check if this is a write command */
            if ((size_t)ev->topic_len == s_topic_write_len &&
                memcmp(ev->topic, s_topic_write, s_topic_write_len) == 0) {
                if (s_data_cb) {
                    s_data_cb(ev->topic, ev->data, ev->data_len);
                }
            }

            /* Check if this is an OTA command */
            if ((size_t)ev->topic_len == s_topic_ota_len &&
                memcmp(ev->topic, s_topic_ota, s_topic_ota_len) == 0) {
                if (s_ota_cb) {
                    s_ota_cb(ev->topic, ev->data, ev->data_len);
                }
            }
        }
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT error");
        break;

    default:
        break;
    }
}

/* ================================================================
 * Public API
 * ================================================================ */

esp_err_t mqtt_client_start(mqtt_data_cb_t on_data, mqtt_ota_cb_t on_ota)
{
    /* Create client mutex on first call (persists across stop/start cycles) */
    if (!s_client_mutex) {
        s_client_mutex = xSemaphoreCreateMutex();
        if (!s_client_mutex) {
            ESP_LOGE(TAG, "Failed to create client mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    if (s_client) {
        ESP_LOGW(TAG, "MQTT client already started");
        return ESP_OK;
    }

    const config_t *cfg = nvs_config_get();

    /* Resolve topics */
    resolve_topic(cfg->mqtt_data_t, s_topic_data, sizeof(s_topic_data));
    resolve_topic(cfg->mqtt_write_t, s_topic_write, sizeof(s_topic_write));
    resolve_topic(cfg->mqtt_stat_t, s_topic_status, sizeof(s_topic_status));
    resolve_topic(OTA_TOPIC_TEMPLATE, s_topic_ota, sizeof(s_topic_ota));

    /* Warn if any resolved topic was truncated */
    size_t max_topic = sizeof(s_topic_data);
    if (strlen(s_topic_data) >= max_topic - 1 ||
        strlen(s_topic_write) >= max_topic - 1 ||
        strlen(s_topic_status) >= max_topic - 1 ||
        strlen(s_topic_ota) >= max_topic - 1) {
        ESP_LOGW(TAG, "Topic truncated — device name or template too long (max %d chars)", max_topic - 1);
    }

    s_data_cb = on_data;
    s_ota_cb  = on_ota;

    /* Cache topic lengths for fast dispatch */
    s_topic_write_len = strlen(s_topic_write);
    s_topic_ota_len  = strlen(s_topic_ota);

    /* Build broker URI — auto-prepend mqtt:// if no scheme present.
     * Also strip an embedded :port from the hostname to avoid double-port. */
    char uri[256];
    const char *raw = cfg->mqtt_uri;
    char host[224];
    /* Copy raw to host, stripping trailing ":port" if present */
    strncpy(host, raw, sizeof(host) - 1);
    host[sizeof(host) - 1] = '\0';
    char *colon = strrchr(host, ':');
    if (colon && colon != host) {
        /* Check preceding char isn't ':' (IPv6 like [::1]) or '/' (scheme://) */
        char prev = *(colon - 1);
        if (prev != ':' && prev != '/') {
            *colon = '\0';  /* strip :port — use mqtt_port from config instead */
        }
    }

    if (strncmp(host, "mqtt://", 7) == 0 ||
        strncmp(host, "mqtts://", 8) == 0 ||
        strncmp(host, "tcp://", 6) == 0 ||
        strncmp(host, "ssl://", 6) == 0 ||
        strncmp(host, "ws://", 5) == 0 ||
        strncmp(host, "wss://", 6) == 0) {
        /* Already has scheme — don't double it */
        snprintf(uri, sizeof(uri), "%s:%d", host, cfg->mqtt_port);
    } else {
        snprintf(uri, sizeof(uri), "mqtt://%s:%d", host, cfg->mqtt_port);
    }
    ESP_LOGI(TAG, "MQTT broker URI: %s", uri);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = uri,
        .credentials.username = (strlen(cfg->mqtt_user) > 0) ? cfg->mqtt_user : NULL,
        .credentials.authentication.password = (strlen(cfg->mqtt_pass) > 0) ? cfg->mqtt_pass : NULL,
        .credentials.client_id = (strlen(cfg->mqtt_client_id) > 0) ? cfg->mqtt_client_id : NULL,
        .session.keepalive = 60,
        .session.disable_clean_session = false,
        .network.disable_auto_reconnect = false,
        .network.reconnect_timeout_ms = 1000,
        .session.last_will = {
            .topic = s_topic_status,
            .msg = "offline",
            .msg_len = 7,
            .qos = 1,
            .retain = 1,
        },
    };

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!s_client) {
        ESP_LOGE(TAG, "Failed to init MQTT client");
        return ESP_FAIL;
    }

    esp_err_t reg_err = esp_mqtt_client_register_event(s_client, MQTT_EVENT_ANY,
                                                        mqtt_event_handler, NULL);
    if (reg_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register MQTT event handler: %d", reg_err);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        return reg_err;
    }

    esp_err_t ret = esp_mqtt_client_start(s_client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT client: %d", ret);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        return ret;
    }

    ESP_LOGI(TAG, "MQTT client started: uri=%s, data=%s, write=%s, ota=%s",
             uri, s_topic_data, s_topic_write, s_topic_ota);
    return ESP_OK;
}

esp_err_t mqtt_client_stop(void)
{
    if (!s_client_mutex) return ESP_OK;
    xSemaphoreTake(s_client_mutex, portMAX_DELAY);

    if (!s_client) {
        xSemaphoreGive(s_client_mutex);
        return ESP_OK;
    }

    /* Publish offline (publish_status locks internally — release first) */
    xSemaphoreGive(s_client_mutex);
    mqtt_client_publish_status("offline", 7);
    vTaskDelay(pdMS_TO_TICKS(200));

    xSemaphoreTake(s_client_mutex, portMAX_DELAY);
    esp_mqtt_client_stop(s_client);
    esp_mqtt_client_destroy(s_client);
    s_client = NULL;
    s_connected = false;
    xSemaphoreGive(s_client_mutex);

    ESP_LOGI(TAG, "MQTT client stopped");
    return ESP_OK;
}

esp_err_t mqtt_client_publish_data(const char *payload, int len)
{
    if (!s_client_mutex) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_client_mutex, portMAX_DELAY);
    if (!s_client || !s_connected) {
        xSemaphoreGive(s_client_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    int msg_id = esp_mqtt_client_publish(s_client, s_topic_data, payload, len, 1, 0);
    xSemaphoreGive(s_client_mutex);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "Failed to publish data: %d", msg_id);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t mqtt_client_publish_status(const char *payload, int len)
{
    if (!s_client_mutex) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_client_mutex, portMAX_DELAY);
    if (!s_client || !s_connected) {
        xSemaphoreGive(s_client_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    int msg_id = esp_mqtt_client_publish(s_client, s_topic_status, payload, len, 1, 0);
    xSemaphoreGive(s_client_mutex);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "Failed to publish status: %d", msg_id);
        return ESP_FAIL;
    }
    return ESP_OK;
}

bool mqtt_client_is_connected(void)
{
    return s_connected;
}
