#include "ui_ota.h"
#include "esp_log.h"

static const char *TAG = "ui_ota";

/* UI elements */
static lv_obj_t *s_label_ota_state = NULL;
static lv_obj_t *s_bar_ota_progress = NULL;

/* ================================================================
 * Public API
 * ================================================================ */

lv_obj_t *ui_ota_create(lv_obj_t *parent)
{
    /* Create OTA page container */
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_center(page);

    /* OTA state */
    s_label_ota_state = lv_label_create(page);
    lv_label_set_text(s_label_ota_state, "OTA Status: Idle");
    lv_obj_align(s_label_ota_state, LV_ALIGN_TOP_LEFT, 10, 10);

    /* Progress bar */
    s_bar_ota_progress = lv_bar_create(page);
    lv_obj_set_size(s_bar_ota_progress, 200, 20);
    lv_obj_align(s_bar_ota_progress, LV_ALIGN_TOP_LEFT, 10, 40);
    lv_bar_set_value(s_bar_ota_progress, 0, LV_ANIM_OFF);

    ESP_LOGI(TAG, "OTA page created");
    return page;
}

void ui_ota_update(uint8_t progress, const char *state)
{
    /* Guard each widget independently in case one failed to create */
    if (s_label_ota_state) {
        lv_label_set_text_fmt(s_label_ota_state, "OTA Status: %s", state);
    }
    if (s_bar_ota_progress) {
        lv_bar_set_value(s_bar_ota_progress, progress, LV_ANIM_ON);
    }
}