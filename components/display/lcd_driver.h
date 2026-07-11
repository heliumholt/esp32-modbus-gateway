#pragma once

#include "esp_err.h"
#include "driver/spi_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ILI9341V LCD driver configuration */

/* Default SPI pins (can be overridden via NVS config) */
#define LCD_SPI_CS_DEFAULT      10
#define LCD_SPI_SCK_DEFAULT     12
#define LCD_SPI_MOSI_DEFAULT    11
#define LCD_SPI_MISO_DEFAULT    13
#define LCD_DC_DEFAULT          7
#define LCD_RESET_DEFAULT       4
#define LCD_BACKLIGHT_DEFAULT   21

/* LCD resolution */
#define LCD_WIDTH   240
#define LCD_HEIGHT  320

/* SPI configuration */
#define LCD_SPI_HOST         SPI2_HOST
#define LCD_SPI_CLOCK_MHZ    40  /* ILI9341 SPI clock — vertical lines were gradient banding, not signal */

/**
 * Initialize ILI9341V LCD driver.
 *
 * @param spi_cs      SPI CS pin (default: GPIO 10)
 * @param spi_sck     SPI SCK pin (default: GPIO 12)
 * @param spi_mosi    SPI MOSI pin (default: GPIO 11)
 * @param spi_miso    SPI MISO pin (default: GPIO 13)
 * @param dc          Data/Command pin (default: GPIO 7)
 * @param reset       Reset pin (default: GPIO 4)
 * @param backlight   Backlight pin (default: GPIO 21)
 *
 * @return ESP_OK on success
 */
esp_err_t lcd_driver_init(
    uint8_t spi_cs,
    uint8_t spi_sck,
    uint8_t spi_mosi,
    uint8_t spi_miso,
    uint8_t dc,
    uint8_t reset,
    uint8_t backlight
);

/**
 * Get LCD panel handle for LVGL integration.
 *
 * @return Pointer to esp_lcd_panel_handle_t
 */
void *lcd_driver_get_panel_handle(void);

/**
 * Get LCD panel IO handle (SPI) for LVGL integration.
 *
 * @return Pointer to esp_lcd_panel_io_handle_t
 */
void *lcd_driver_get_io_handle(void);

/**
 * Set backlight brightness (0-100).
 *
 * @param brightness 0=off, 100=full brightness
 */
void lcd_driver_set_backlight(uint8_t brightness);

#ifdef __cplusplus
}
#endif