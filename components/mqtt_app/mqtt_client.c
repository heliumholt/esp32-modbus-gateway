#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_crt_bundle.h"       /* CA certificate bundle for TLS */
#include "mqtt_client.h"           /* ESP-MQTT library (managed component) */
#include "app_mqtt.h"              /* local function declarations */
#include "nvs_config.h"
#include "led_indicator.h"

static const char *TAG = "mqtt";

static esp_mqtt_client_handle_t s_client = NULL;
static SemaphoreHandle_t s_client_mutex = NULL;
static EventGroupHandle_t s_mqtt_events = NULL;
static mqtt_data_cb_t s_data_cb = NULL;
static mqtt_ota_cb_t  s_ota_cb  = NULL;
static volatile bool s_connected = false;

/* Resolved topic strings (with {dev} replaced) */
static char s_topic_data[96];
static char s_topic_write[96];
static char s_topic_status[96];
static char s_topic_ota[96];

/* Cached topic lengths — set once after resolve_topic() */
static size_t s_topic_write_len;
static size_t s_topic_ota_len;

/* OTA topic template */
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

        /* Signal connected event */
        if (s_mqtt_events) {
            xEventGroupSetBits(s_mqtt_events, MQTT_EVENT_CONNECTED_BIT);
        }

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
        ESP_LOGW(TAG, "MQTT disconnected (auto-reconnect enabled)");
        s_connected = false;
        led_indicator_set(LED_STATE_STA_READY);

        /* Signal disconnected event */
        if (s_mqtt_events) {
            xEventGroupSetBits(s_mqtt_events, MQTT_EVENT_DISCONNECTED_BIT);
        }
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

    case MQTT_EVENT_ERROR: {
        /* Log detailed error information */
        esp_mqtt_error_codes_t *err = ev->error_handle;
        if (err) {
            ESP_LOGE(TAG, "MQTT error: type=%d, connect_as=%d, tls_stack=%d, tls_cert=%d",
                     err->error_type,
                     err->connect_return_code,
                     err->esp_tls_last_esp_err,
                     err->esp_tls_cert_verify_flags);
            if (err->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
                ESP_LOGE(TAG, "  Transport error: sock_errno=%d, esp_tls_err=0x%x",
                         err->esp_transport_sock_errno,
                         err->esp_tls_last_esp_err);
            }
        }
        break;
    }

    default:
        break;
    }
}

/* ================================================================
 * Public API
 * ================================================================ */

esp_err_t mqtt_client_start(mqtt_data_cb_t on_data, mqtt_ota_cb_t on_ota)
{
    /* Create mutex and event group on first call */
    if (!s_client_mutex) {
        s_client_mutex = xSemaphoreCreateMutex();
        if (!s_client_mutex) {
            ESP_LOGE(TAG, "Failed to create client mutex");
            return ESP_ERR_NO_MEM;
        }
    }
    if (!s_mqtt_events) {
        s_mqtt_events = xEventGroupCreate();
        if (!s_mqtt_events) {
            ESP_LOGE(TAG, "Failed to create MQTT event group");
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
    strncpy(host, raw, sizeof(host) - 1);
    host[sizeof(host) - 1] = '\0';
    char *colon = strrchr(host, ':');
    if (colon && colon != host) {
        char prev = *(colon - 1);
        if (prev != ':' && prev != '/') {
            *colon = '\0';
        }
    }

    bool use_tls = false;
    if (strncmp(host, "mqtt://", 7) == 0 ||
        strncmp(host, "tcp://", 6) == 0 ||
        strncmp(host, "ws://", 5) == 0) {
        snprintf(uri, sizeof(uri), "%s:%d", host, cfg->mqtt_port);
    } else if (strncmp(host, "mqtts://", 8) == 0 ||
               strncmp(host, "ssl://", 6) == 0 ||
               strncmp(host, "wss://", 6) == 0) {
        snprintf(uri, sizeof(uri), "%s:%d", host, cfg->mqtt_port);
        use_tls = true;
    } else {
        /* No scheme — default to mqtt:// */
        snprintf(uri, sizeof(uri), "mqtt://%s:%d", host, cfg->mqtt_port);
    }
    ESP_LOGI(TAG, "MQTT broker URI: %s (TLS: %s)", uri, use_tls ? "yes" : "no");

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = uri,
        .credentials.username = (strlen(cfg->mqtt_user) > 0) ? cfg->mqtt_user : NULL,
        .credentials.authentication.password = (strlen(cfg->mqtt_pass) > 0) ? cfg->mqtt_pass : NULL,
        .credentials.client_id = (strlen(cfg->mqtt_client_id) > 0) ? cfg->mqtt_client_id : NULL,
        .session.keepalive = 60,
        .session.disable_clean_session = false,
        .network.disable_auto_reconnect = false,
        .network.reconnect_timeout_ms = 2000,
        .session.last_will = {
            .topic = s_topic_status,
            .msg = "offline",
            .msg_len = 7,
            .qos = 1,
            .retain = 1,
        },
    };

    /* TLS configuration: use ESP-IDF CA certificate bundle */
    if (use_tls) {
        mqtt_cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
        mqtt_cfg.broker.verification.skip_cert_common_name_check = false;
    }

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

esp_err_t mqtt_client_reconnect(void)
{
    if (!s_client) return ESP_ERR_INVALID_STATE;

    /* Trigger reconnection — esp-mqtt handles the rest internally.
     * LWT automatically publishes "offline" when connection drops. */
    esp_err_t ret = esp_mqtt_client_reconnect(s_client);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "MQTT reconnect trigger failed: %d", ret);
        return ret;
    }
    ESP_LOGI(TAG, "MQTT reconnect triggered");
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

EventGroupHandle_t mqtt_client_get_event_group(void)
{
    return s_mqtt_events;
}
