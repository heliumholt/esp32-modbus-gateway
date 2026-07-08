#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "lwip/ip4_addr.h"
#include "wifi_manager.h"
#include "nvs_config.h"
#include "web_server.h"

static const char *TAG = "wifi_mgr";

#define MAX_STA_RETRIES      5
#define STA_CONNECT_TIMEOUT_MS  30000

static EventGroupHandle_t s_wifi_events;
static wifi_state_t s_state = WIFI_STATE_AP_MODE;
static int s_sta_retry_count = 0;

/* Forward declarations */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data);
static void ip_event_handler(void *arg, esp_event_base_t base,
                             int32_t event_id, void *event_data);
static void start_ap_mode(void);
static void start_sta_mode(void);

/* ================================================================
 * Public
 * ================================================================ */

void wifi_manager_init(void)
{
    s_wifi_events = xEventGroupCreate();
    if (!s_wifi_events) {
        ESP_LOGE(TAG, "Failed to create WiFi event group");
        return;
    }

    /* Register event handlers */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                        &ip_event_handler, NULL, NULL));

    /* Initialize WiFi */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    const config_t *config = nvs_config_get();

    if (config->configd && strlen(config->wifi_ssid) > 0) {
        ESP_LOGI(TAG, "Config found, starting STA mode (SSID: %s)", config->wifi_ssid);
        start_sta_mode();
    } else {
        ESP_LOGI(TAG, "No config, starting AP mode for configuration");
        start_ap_mode();
    }
}

void wifi_manager_on_config_saved(void)
{
    ESP_LOGI(TAG, "Config saved, transitioning to STA mode");
    xEventGroupSetBits(s_wifi_events, WIFI_EVENT_CONFIG_SAVED);

    /* Stop AP services */
    web_server_stop();

    /* Stop WiFi before switching modes */
    esp_wifi_stop();
    esp_wifi_disconnect();

    /* Small delay to let WiFi settle */
    vTaskDelay(pdMS_TO_TICKS(500));

    start_sta_mode();
}

wifi_state_t wifi_manager_get_state(void)
{
    return s_state;
}

void *wifi_manager_get_event_group(void)
{
    return (void *)s_wifi_events;
}

void wifi_manager_stop_all(void)
{
    web_server_stop();
    esp_wifi_stop();
    s_state = WIFI_STATE_AP_MODE;
    start_ap_mode();
}

/* ================================================================
 * Private — AP mode
 * ================================================================ */

static void start_ap_mode(void)
{
    s_state = WIFI_STATE_AP_MODE;
    s_sta_retry_count = 0;

    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    if (!ap_netif) {
        ESP_LOGE(TAG, "Failed to create AP netif");
        return;
    }

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = "ESP32-MODBUS-CONFIG",
            .ssid_len = 0,
            .password = "",
            .max_connection = 2,
            .authmode = WIFI_AUTH_OPEN,
            .pmf_cfg = { .required = false },
        },
    };

    /* Append last 2 MAC bytes to SSID for uniqueness */
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf((char *)wifi_config.ap.ssid, sizeof(wifi_config.ap.ssid),
             "BOX_%02X%02X", mac[4], mac[5]);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "AP started: SSID=%s, IP=192.168.4.1", wifi_config.ap.ssid);

    /* Start captive portal */
    vTaskDelay(pdMS_TO_TICKS(500));
    web_server_start();

    xEventGroupSetBits(s_wifi_events, WIFI_EVENT_AP_STARTED);
}

/* ================================================================
 * Private — STA mode
 * ================================================================ */

static void start_sta_mode(void)
{
    s_state = WIFI_STATE_STA_START;

    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    if (!sta_netif) {
        ESP_LOGE(TAG, "Failed to create STA netif");
        start_ap_mode();
        return;
    }

    const config_t *config = nvs_config_get();

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, config->wifi_ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, config->wifi_pass, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_state = WIFI_STATE_STA_CONNECTING;
    ESP_LOGI(TAG, "STA connecting to SSID: %s", config->wifi_ssid);

    esp_wifi_connect();

    /* Wait for connection with timeout */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_events,
                         WIFI_EVENT_GOT_IP | WIFI_EVENT_DISCONNECTED,
                         pdTRUE, pdFALSE,
                         pdMS_TO_TICKS(STA_CONNECT_TIMEOUT_MS));

    if (bits & WIFI_EVENT_GOT_IP) {
        s_state = WIFI_STATE_STA_READY;
        s_sta_retry_count = 0;
        ESP_LOGI(TAG, "STA connected successfully");
    } else {
        ESP_LOGW(TAG, "STA connection timed out, retries: %d/%d",
                 s_sta_retry_count, MAX_STA_RETRIES);
        s_sta_retry_count++;
        if (s_sta_retry_count >= MAX_STA_RETRIES) {
            ESP_LOGW(TAG, "Max STA retries reached, falling back to AP mode");
            esp_wifi_stop();
            start_ap_mode();
        }
    }
}

/* ================================================================
 * Event handlers
 * ================================================================ */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    switch (event_id) {
    case WIFI_EVENT_STA_START:
        ESP_LOGI(TAG, "STA started");
        break;

    case WIFI_EVENT_STA_DISCONNECTED: {
        wifi_event_sta_disconnected_t *ev =
            (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(TAG, "STA disconnected (reason: %d)", ev->reason);
        xEventGroupSetBits(s_wifi_events, WIFI_EVENT_DISCONNECTED);

        if (s_state == WIFI_STATE_STA_READY) {
            s_state = WIFI_STATE_STA_LOST;
            s_sta_retry_count++;
            if (s_sta_retry_count < MAX_STA_RETRIES && nvs_config_get()->ap_fallback) {
                int delay = (1 << s_sta_retry_count) * 1000;
                if (delay > 30000) delay = 30000;
                ESP_LOGI(TAG, "Reconnecting in %d ms (attempt %d/%d)",
                         delay, s_sta_retry_count, MAX_STA_RETRIES);
                vTaskDelay(pdMS_TO_TICKS(delay));
                esp_wifi_connect();
            } else {
                ESP_LOGW(TAG, "Max retries reached, falling back to AP mode");
                esp_wifi_stop();
                start_ap_mode();
            }
        } else if (s_state == WIFI_STATE_STA_CONNECTING) {
            /* Connection attempt during initial STA start failed */
            esp_wifi_connect(); /* retry */
        }
        break;
    }

    case WIFI_EVENT_AP_STACONNECTED:
        ESP_LOGI(TAG, "Client connected to AP");
        break;

    case WIFI_EVENT_AP_STADISCONNECTED:
        ESP_LOGI(TAG, "Client disconnected from AP");
        break;

    default:
        break;
    }
}

static void ip_event_handler(void *arg, esp_event_base_t base,
                             int32_t event_id, void *event_data)
{
    if (event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        xEventGroupSetBits(s_wifi_events, WIFI_EVENT_GOT_IP);
    }
}
