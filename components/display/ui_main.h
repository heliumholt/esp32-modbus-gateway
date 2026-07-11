#pragma once

#include "lvgl.h"
#include "gateway_status.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Create main page UI elements.
 *
 * @param parent Parent container (screen or tab)
 * @return Created page object
 */
lv_obj_t *ui_main_create(lv_obj_t *parent);

/**
 * Update main page with current gateway status.
 *
 * @param status Pointer to gateway_status_t
 */
void ui_main_update(const gateway_status_t *status);

#ifdef __cplusplus
}
#endif