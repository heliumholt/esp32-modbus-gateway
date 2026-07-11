#include "touch_driver.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "touch_drv";

/* I2C master bus handle */
static i2c_master_bus_handle_t s_i2c_bus = NULL;

/* I2C device handle */
static i2c_master_dev_handle_t s_i2c_dev = NULL;

/* FT6336 registers (from datasheet) */
#define FT6336_REG_GESTURE_ID      0x01
#define FT6336_REG_TD_STATUS       0x02  /* Touch point count (0-2) */
#define FT6336_REG_TOUCH1_XH       0x03  /* Touch 1 X high byte (contains event flag) */
#define FT6336_REG_TOUCH1_XL       0x04  /* Touch 1 X low byte */
#define FT6336_REG_TOUCH1_YH       0x05  /* Touch 1 Y high byte */
#define FT6336_REG_TOUCH1_YL       0x06  /* Touch 1 Y low byte */
#define FT6336_REG_TOUCH2_XH       0x09  /* Touch 2 X high byte */
#define FT6336_REG_TOUCH2_XL       0x0A  /* Touch 2 X low byte */
#define FT6336_REG_TOUCH2_YH       0x0B  /* Touch 2 Y high byte */
#define FT6336_REG_TOUCH2_YL       0x0C  /* Touch 2 Y low byte */

/* ================================================================
 * Public API
 * ================================================================ */

esp_err_t touch_driver_init(
    uint8_t i2c_sda,
    uint8_t i2c_scl,
    uint8_t int_pin,
    uint8_t reset_pin
)
{
    ESP_LOGI(TAG, "Initializing FT6336 touch driver...");
    ESP_LOGI(TAG, "I2C pins: SDA=%d, SCL=%d, INT=%d, RST=%d",
             i2c_sda, i2c_scl, int_pin, reset_pin);

    /* Configure I2C master bus */
    i2c_master_bus_config_t i2c_bus_config = {
        .i2c_port          = TOUCH_I2C_HOST,
        .sda_io_num        = i2c_sda,
        .scl_io_num        = i2c_scl,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t ret = i2c_new_master_bus(&i2c_bus_config, &s_i2c_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed: %d", ret);
        return ret;
    }

    /* Configure I2C device */
    i2c_device_config_t i2c_dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = FT6336_I2C_ADDR,
        .scl_speed_hz    = TOUCH_I2C_CLOCK_KHZ * 1000,
    };

    ret = i2c_master_bus_add_device(s_i2c_bus, &i2c_dev_config, &s_i2c_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C device add failed: %d", ret);
        i2c_del_master_bus(s_i2c_bus);
        s_i2c_bus = NULL;
        return ret;
    }

    /* Reset FT6336 (optional) */
    if (reset_pin < GPIO_NUM_MAX) {
        gpio_config_t rst_conf = {
            .pin_bit_mask = (1ULL << reset_pin),
            .mode         = GPIO_MODE_OUTPUT,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        gpio_config(&rst_conf);

        gpio_set_level(reset_pin, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(reset_pin, 1);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    /* Configure INT pin (optional, for interrupt-based touch detection) */
    if (int_pin < GPIO_NUM_MAX) {
        gpio_config_t int_conf = {
            .pin_bit_mask = (1ULL << int_pin),
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_NEGEDGE,  /* Touch active on falling edge */
        };
        gpio_config(&int_conf);
    }

    ESP_LOGI(TAG, "Touch driver initialized successfully");
    return ESP_OK;
}

esp_err_t touch_driver_read_point(uint16_t *x_out, uint16_t *y_out)
{
    if (!s_i2c_dev) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Read touch status register (TD_STATUS) to check touch count */
    uint8_t touch_count = 0;
    uint8_t reg_addr = FT6336_REG_TD_STATUS;
    esp_err_t ret = i2c_master_transmit_receive(
        s_i2c_dev,
        &reg_addr,
        1,
        &touch_count,
        1,
        pdMS_TO_TICKS(100)
    );

    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "I2C read failed: %d", ret);
        return ret;
    }

    /* No touch detected */
    if (touch_count == 0 || touch_count > 2) {
        return ESP_ERR_NOT_FOUND;
    }

    /* Read touch coordinates (6 bytes: XH, XL, YH, YL + weight + misc) */
    uint8_t touch_data[6];
    reg_addr = FT6336_REG_TOUCH1_XH;
    ret = i2c_master_transmit_receive(
        s_i2c_dev,
        &reg_addr,
        1,
        touch_data,
        sizeof(touch_data),
        pdMS_TO_TICKS(100)
    );

    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "Touch coordinate read failed: %d", ret);
        return ret;
    }

    /* Parse touch data:
     * XH[7:4] = Event flag (0: no event, 1-3: touch event)
     * XH[3:0] + XL = X coordinate (12-bit)
     * YH[7:4] + YL = Y coordinate (12-bit)
     * Format: (XH & 0x0F) << 8 | XL, (YH & 0x0F) << 8 | YL
     */
    uint16_t x = ((touch_data[0] & 0x0F) << 8) | touch_data[1];
    uint16_t y = ((touch_data[2] & 0x0F) << 8) | touch_data[3];

    /* FT6336 typically reports coordinates in 0-239 for X, 0-319 for Y
     * For portrait mode (240x320), these are already correct
     * For landscape, we would need to swap: x = y, y = 240 - x
     */

    *x_out = x;
    *y_out = y;

    ESP_LOGD(TAG, "Touch detected: count=%d, X=%d, Y=%d", touch_count, *x_out, *y_out);
    return ESP_OK;
}

void *touch_driver_get_i2c_handle(void)
{
    return s_i2c_bus;
}