#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/* FT6336 Touch driver configuration */

/* Default I2C pins (can be overridden via NVS config) */
#define TOUCH_I2C_SDA_DEFAULT  40
#define TOUCH_I2C_SCL_DEFAULT  41
#define TOUCH_INT_DEFAULT      39
#define TOUCH_RESET_DEFAULT    42

/* FT6336 I2C address */
#define FT6336_I2C_ADDR        0x38

/* I2C configuration */
#define TOUCH_I2C_HOST         I2C_NUM_0
#define TOUCH_I2C_CLOCK_KHZ   400  /* 400 kHz */

/**
 * Initialize FT6336 touch driver.
 *
 * @param i2c_sda    I2C SDA pin (default: GPIO 40)
 * @param i2c_scl    I2C SCL pin (default: GPIO 41)
 * @param int_pin    Interrupt pin (default: GPIO 39)
 * @param reset_pin  Reset pin (default: GPIO 42)
 *
 * @return ESP_OK on success
 */
esp_err_t touch_driver_init(
    uint8_t i2c_sda,
    uint8_t i2c_scl,
    uint8_t int_pin,
    uint8_t reset_pin
);

/**
 * Read touch point coordinates.
 * FT6336 supports single touch point.
 *
 * @param x_out  X coordinate output (0-239 for 240-width LCD)
 * @param y_out  Y coordinate output (0-319 for 320-height LCD)
 *
 * @return ESP_OK if touch detected, ESP_ERR_NOT_FOUND if no touch
 */
esp_err_t touch_driver_read_point(uint16_t *x_out, uint16_t *y_out);

/**
 * Get I2C master bus handle for further operations.
 *
 * @return Pointer to i2c_master_bus_handle_t
 */
void *touch_driver_get_i2c_handle(void);

#ifdef __cplusplus
}
#endif