#pragma once

#include "esp_err.h"
#include "gateway_status.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize display system:
 * - LCD driver (ILI9341V via SPI)
 * - Touch driver (FT6336 via I2C)
 * - LVGL integration (esp_lvgl_port)
 * - UI pages
 *
 * Creates display_task on Core 0, Priority 3.
 */
esp_err_t display_manager_init(void);

/**
 * Update gateway status in UI.
 * Called periodically (every 1s) by display_task.
 */
void display_manager_update_status(const gateway_status_t *status);

/**
 * Update OTA progress in UI.
 * Called by OTA handler when download progress changes.
 *
 * @param progress 0-100 percentage
 * @param state OTA state string ("downloading", "success", "failed")
 */
void display_manager_update_ota(uint8_t progress, const char *state);

/**
 * Set LCD backlight brightness.
 *
 * @param brightness 0-100 percentage
 */
void display_manager_set_backlight(uint8_t brightness);

#ifdef __cplusplus
}
#endif