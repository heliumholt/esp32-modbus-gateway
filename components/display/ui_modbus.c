#include "ui_modbus.h"
#include "gateway_status.h"
#include "esp_log.h"

static const char *TAG = "ui_modbus";

/* UI elements */
static lv_obj_t *s_label_poll_intv = NULL;
static lv_obj_t *s_label_poll_count = NULL;
static lv_obj_t *s_label_error_count = NULL;
static lv_obj_t *s_label_last_slave = NULL;

/* ================================================================
 * Public API
 * ================================================================ */

lv_obj_t *ui_modbus_create(lv_obj_t *parent)
{
    /* Create MODBUS page container */
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_center(page);

    /* Poll interval */
    s_label_poll_intv = lv_label_create(page);
    lv_label_set_text(s_label_poll_intv, "Poll Interval: 5000ms");
    lv_obj_align(s_label_poll_intv, LV_ALIGN_TOP_LEFT, 10, 10);

    /* Poll count */
    s_label_poll_count = lv_label_create(page);
    lv_label_set_text(s_label_poll_count, "Polls: 0");
    lv_obj_align(s_label_poll_count, LV_ALIGN_TOP_LEFT, 10, 40);

    /* Error count */
    s_label_error_count = lv_label_create(page);
    lv_label_set_text(s_label_error_count, "Errors: 0");
    lv_obj_align(s_label_error_count, LV_ALIGN_TOP_LEFT, 10, 70);

    /* Last slave */
    s_label_last_slave = lv_label_create(page);
    lv_label_set_text(s_label_last_slave, "Last Slave: N/A");
    lv_obj_align(s_label_last_slave, LV_ALIGN_TOP_LEFT, 10, 100);

    ESP_LOGI(TAG, "MODBUS page created");
    return page;
}

void ui_modbus_update(const gateway_status_t *status)
{
    if (!s_label_poll_count) return;

    /* Update poll count */
    lv_label_set_text_fmt(s_label_poll_count, "Polls: %lu", status->modbus_poll_count);

    /* Update error count */
    lv_label_set_text_fmt(s_label_error_count, "Errors: %lu", status->modbus_error_count);

    /* Update last slave */
    if (status->modbus_last_slave > 0) {
        lv_label_set_text_fmt(s_label_last_slave, "Last Slave: %d", status->modbus_last_slave);
    } else {
        lv_label_set_text(s_label_last_slave, "Last Slave: N/A");
    }
}