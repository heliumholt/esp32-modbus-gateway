#include "ui_system.h"
#include "gateway_status.h"
#include "esp_log.h"

static const char *TAG = "ui_system";

/* UI elements */
static lv_obj_t *s_label_free_heap = NULL;
static lv_obj_t *s_label_uptime = NULL;
static lv_obj_t *s_label_wifi_rssi = NULL;

/* ================================================================
 * Public API
 * ================================================================ */

lv_obj_t *ui_system_create(lv_obj_t *parent)
{
    /* Create system page container */
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_center(page);

    /* Free heap */
    s_label_free_heap = lv_label_create(page);
    lv_label_set_text(s_label_free_heap, "Free Heap: 0 bytes");
    lv_obj_align(s_label_free_heap, LV_ALIGN_TOP_LEFT, 10, 10);

    /* Uptime */
    s_label_uptime = lv_label_create(page);
    lv_label_set_text(s_label_uptime, "Uptime: 0s");
    lv_obj_align(s_label_uptime, LV_ALIGN_TOP_LEFT, 10, 40);

    /* WiFi RSSI */
    s_label_wifi_rssi = lv_label_create(page);
    lv_label_set_text(s_label_wifi_rssi, "WiFi RSSI: N/A");
    lv_obj_align(s_label_wifi_rssi, LV_ALIGN_TOP_LEFT, 10, 70);

    ESP_LOGI(TAG, "System page created");
    return page;
}

void ui_system_update(const gateway_status_t *status)
{
    if (!s_label_free_heap) return;

    /* Update free heap */
    lv_label_set_text_fmt(s_label_free_heap, "Free Heap: %lu bytes", status->free_heap);

    /* Update uptime */
    uint32_t hours = status->uptime_sec / 3600;
    uint32_t minutes = (status->uptime_sec % 3600) / 60;
    uint32_t seconds = status->uptime_sec % 60;
    lv_label_set_text_fmt(s_label_uptime, "Uptime: %02lu:%02lu:%02lu", hours, minutes, seconds);

    /* Update WiFi RSSI */
    if (status->wifi_rssi != 0) {
        lv_label_set_text_fmt(s_label_wifi_rssi, "WiFi RSSI: %d dBm", status->wifi_rssi);
    } else {
        lv_label_set_text(s_label_wifi_rssi, "WiFi RSSI: N/A");
    }
}