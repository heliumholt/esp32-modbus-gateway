#pragma once

#include "lvgl.h"
#include "gateway_status.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Create system page UI elements.
 *
 * @param parent Parent container (screen or tab)
 * @return Created page object
 */
lv_obj_t *ui_system_create(lv_obj_t *parent);

/**
 * Update system page with current status.
 *
 * @param status Pointer to gateway_status_t
 */
void ui_system_update(const gateway_status_t *status);

#ifdef __cplusplus
}
#endif