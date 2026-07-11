#include "ui_main.h"
#include "gateway_status.h"
#include "wifi_manager.h"
#include "esp_log.h"

static const char *TAG = "ui_main";

/* UI elements */
static lv_obj_t *s_label_dev_name = NULL;
static lv_obj_t *s_label_wifi_state = NULL;
static lv_obj_t *s_label_ip_addr = NULL;
static lv_obj_t *s_label_mqtt_state = NULL;
static lv_obj_t *s_label_uptime = NULL;

/* ================================================================
 * Public API
 * ================================================================ */

lv_obj_t *ui_main_create(lv_obj_t *parent)
{
    /* Create main page container */
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_center(page);

    /* Device name */
    s_label_dev_name = lv_label_create(page);
    lv_label_set_text(s_label_dev_name, "Device: [loading]");
    lv_obj_align(s_label_dev_name, LV_ALIGN_TOP_LEFT, 10, 10);

    /* WiFi state */
    s_label_wifi_state = lv_label_create(page);
    lv_label_set_text(s_label_wifi_state, "WiFi: [checking]");
    lv_obj_align(s_label_wifi_state, LV_ALIGN_TOP_LEFT, 10, 40);

    /* IP address */
    s_label_ip_addr = lv_label_create(page);
    lv_label_set_text(s_label_ip_addr, "IP: N/A");
    lv_obj_align(s_label_ip_addr, LV_ALIGN_TOP_LEFT, 10, 70);

    /* MQTT state */
    s_label_mqtt_state = lv_label_create(page);
    lv_label_set_text(s_label_mqtt_state, "MQTT: [checking]");
    lv_obj_align(s_label_mqtt_state, LV_ALIGN_TOP_LEFT, 10, 100);

    /* Uptime */
    s_label_uptime = lv_label_create(page);
    lv_label_set_text(s_label_uptime, "Uptime: 0s");
    lv_obj_align(s_label_uptime, LV_ALIGN_TOP_LEFT, 10, 130);

    ESP_LOGI(TAG, "Main page created");
    return page;
}

void ui_main_update(const gateway_status_t *status)
{
    if (!s_label_dev_name) return;

    /* Update WiFi state text */
    const char *wifi_state_str;
    switch (status->wifi_state) {
        case WIFI_STATE_AP_MODE:
            wifi_state_str = "WiFi: AP Mode";
            break;
        case WIFI_STATE_STA_START:
            wifi_state_str = "WiFi: Starting";
            break;
        case WIFI_STATE_STA_CONNECTING:
            wifi_state_str = "WiFi: Connecting";
            break;
        case WIFI_STATE_STA_READY:
            wifi_state_str = "WiFi: Connected";
            break;
        case WIFI_STATE_STA_LOST:
            wifi_state_str = "WiFi: Lost";
            break;
        default:
            wifi_state_str = "WiFi: Unknown";
            break;
    }
    lv_label_set_text(s_label_wifi_state, wifi_state_str);

    /* Update IP address */
    if (status->wifi_state == WIFI_STATE_STA_READY && strlen(status->ip_addr) > 0) {
        lv_label_set_text_fmt(s_label_ip_addr, "IP: %s", status->ip_addr);
    } else {
        lv_label_set_text(s_label_ip_addr, "IP: N/A");
    }

    /* Update MQTT state */
    if (status->mqtt_connected) {
        lv_label_set_text(s_label_mqtt_state, "MQTT: Connected");
    } else {
        lv_label_set_text(s_label_mqtt_state, "MQTT: Disconnected");
    }

    /* Update uptime */
    uint32_t hours = status->uptime_sec / 3600;
    uint32_t minutes = (status->uptime_sec % 3600) / 60;
    uint32_t seconds = status->uptime_sec % 60;
    lv_label_set_text_fmt(s_label_uptime, "Uptime: %02lu:%02lu:%02lu", hours, minutes, seconds);
}