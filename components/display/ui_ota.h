#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Create OTA page UI elements.
 *
 * @param parent Parent container (screen or tab)
 * @return Created page object
 */
lv_obj_t *ui_ota_create(lv_obj_t *parent);

/**
 * Update OTA page with current progress and state.
 *
 * @param progress Download progress (0-100)
 * @param state OTA state string ("idle", "downloading", "success", "failed")
 */
void ui_ota_update(uint8_t progress, const char *state);

#ifdef __cplusplus
}
#endif