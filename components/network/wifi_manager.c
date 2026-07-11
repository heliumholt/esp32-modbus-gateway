#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "lwip/ip4_addr.h"
#include "wifi_manager.h"
#include "nvs_config.h"
#include "web_server.h"
#include "led_indicator.h"

static const char *TAG = "wifi_mgr";

#define MAX_STA_RETRIES      5

static EventGroupHandle_t s_wifi_events;
static wifi_state_t s_state = WIFI_STATE_AP_MODE;
static int s_sta_retry_count = 0;
static TimerHandle_t s_mode_switch_timer = NULL;
static TimerHandle_t s_reconnect_timer = NULL;

/* Forward declarations */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data);
static void ip_event_handler(void *arg, esp_event_base_t base,
                             int32_t event_id, void *event_data);
static void start_ap_mode(void);
static void start_sta_mode(void);
static void mode_switch_phase1_cb(TimerHandle_t timer);
static void mode_switch_phase2_cb(TimerHandle_t timer);
static void reconnect_timer_cb(TimerHandle_t timer);

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

    /* Create reconnect timer (one-shot) */
    s_reconnect_timer = xTimerCreate("reconnect", pdMS_TO_TICKS(1000),
                                      pdFALSE, NULL, reconnect_timer_cb);

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
        led_indicator_set(LED_STATE_AP_MODE);
        start_ap_mode();
    }
}

void wifi_manager_on_config_saved(void)
{
    ESP_LOGI(TAG, "Config saved, transitioning to STA mode");
    xEventGroupSetBits(s_wifi_events, WIFI_EVENT_CONFIG_SAVED);

    /* Phase 1: stop web server + WiFi, then schedule phase 2 after 300ms */
    if (!s_mode_switch_timer) {
        s_mode_switch_timer = xTimerCreate("mode_sw1", pdMS_TO_TICKS(300),
                                            pdFALSE, NULL, mode_switch_phase1_cb);
    }
    if (s_mode_switch_timer) {
        xTimerStart(s_mode_switch_timer, 0);
    }
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
    led_indicator_set(LED_STATE_AP_MODE);

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
    s_sta_retry_count = 0;
    ESP_LOGI(TAG, "STA connecting to SSID: %s", config->wifi_ssid);

    esp_wifi_connect();
}

/* ================================================================
 * Timer callbacks (non-blocking — no vTaskDelay)
 * ================================================================ */

/* Phase 1: Stop web server + WiFi, schedule phase 2 */
static void mode_switch_phase1_cb(TimerHandle_t timer)
{
    (void)timer;
    ESP_LOGI(TAG, "Mode switch phase 1: stopping web server + WiFi");
    web_server_stop();
    esp_wifi_stop();

    /* Schedule phase 2 after 500ms settling time */
    static TimerHandle_t s_phase2_timer = NULL;
    if (!s_phase2_timer) {
        s_phase2_timer = xTimerCreate("mode_sw2", pdMS_TO_TICKS(500),
                                       pdFALSE, NULL, mode_switch_phase2_cb);
    }
    if (s_phase2_timer) {
        xTimerStart(s_phase2_timer, 0);
    }
}

/* Phase 2: Start STA mode */
static void mode_switch_phase2_cb(TimerHandle_t timer)
{
    (void)timer;
    ESP_LOGI(TAG, "Mode switch phase 2: starting STA");
    start_sta_mode();
}

/* Reconnect timer: called after exponential backoff delay */
static void reconnect_timer_cb(TimerHandle_t timer)
{
    (void)timer;
    if (s_state == WIFI_STATE_STA_CONNECTING || s_state == WIFI_STATE_STA_LOST) {
        ESP_LOGI(TAG, "Reconnect timer fired — attempting connect");
        esp_wifi_connect();
    }
}

/* ================================================================
 * Event handlers (NON-BLOCKING — no vTaskDelay!)
 * ================================================================ */

static void schedule_reconnect(int delay_ms)
{
    if (s_reconnect_timer) {
        xTimerStop(s_reconnect_timer, 0);
        xTimerChangePeriod(s_reconnect_timer, pdMS_TO_TICKS(delay_ms), 0);
        xTimerStart(s_reconnect_timer, 0);
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    switch (event_id) {
    case WIFI_EVENT_STA_START:
        ESP_LOGI(TAG, "STA started");
        led_indicator_set(LED_STATE_STA_CONNECTING);
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
                schedule_reconnect(delay);
            } else {
                ESP_LOGW(TAG, "Max retries reached, falling back to AP mode");
                esp_wifi_stop();
                start_ap_mode();
            }
        } else if (s_state == WIFI_STATE_STA_CONNECTING) {
            s_sta_retry_count++;
            if (s_sta_retry_count < MAX_STA_RETRIES || !nvs_config_get()->ap_fallback) {
                int delay = (1 << s_sta_retry_count) * 1000;
                if (delay > 15000) delay = 15000;
                ESP_LOGI(TAG, "Connect failed, retry in %d ms (%d/%d)",
                         delay, s_sta_retry_count, MAX_STA_RETRIES);
                schedule_reconnect(delay);
            } else {
                ESP_LOGW(TAG, "Max connect retries reached, falling back to AP mode");
                esp_wifi_stop();
                start_ap_mode();
            }
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
        s_state = WIFI_STATE_STA_READY;
        s_sta_retry_count = 0;
        xEventGroupSetBits(s_wifi_events, WIFI_EVENT_GOT_IP);
        led_indicator_set(LED_STATE_STA_READY);

        web_server_start();
        ESP_LOGI(TAG, "Web config available at http://" IPSTR, IP2STR(&ev->ip_info.ip));
    }
}
