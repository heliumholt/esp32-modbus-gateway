#include "lcd_driver.h"
#include "esp_log.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_dev.h"  /* esp_lcd_panel_dev_config_t (IDF 6.0+) */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "lcd_drv";

/* Vendor-specific init sequence for the BOE 3.2" IPS ILI9341V panel.
 * Replaces esp_lcd_ili9341's default init — the default Power/VCOM/0xB6
 * values caused periodic vertical-line artifacts on this module.
 * 0x36 (MADCTL) is intentionally omitted: esp_lcd + esp_lvgl_port manage
 * orientation via swap_xy/mirror. Source: supplier reference code. */
static const ili9341_lcd_init_cmd_t s_vendor_init_cmds[] = {
    {0xCF, (uint8_t[]){0x00, 0x89, 0x30}, 3, 0},
    {0xED, (uint8_t[]){0x67, 0x03, 0x12, 0x81}, 4, 0},
    {0xE8, (uint8_t[]){0x85, 0x01, 0x78}, 3, 0},
    {0xCB, (uint8_t[]){0x39, 0x2C, 0x00, 0x34, 0x02}, 5, 0},
    {0xF7, (uint8_t[]){0x20}, 1, 0},
    {0xEA, (uint8_t[]){0x00, 0x00}, 2, 0},
    {0xC0, (uint8_t[]){0x25}, 1, 0},               /* Power control 1 */
    {0xC1, (uint8_t[]){0x10}, 1, 0},               /* Power control 2 */
    {0xC5, (uint8_t[]){0x55, 0x50}, 2, 0},         /* VCOM control 1 */
    {0xC7, (uint8_t[]){0xB0}, 1, 0},               /* VCOM control 2 */
    {0xB6, (uint8_t[]){0x0A, 0x82}, 2, 0},         /* Display function control */
    {0x3A, (uint8_t[]){0x55}, 1, 0},               /* COLMOD: RGB565 */
    {0xF2, (uint8_t[]){0x00}, 1, 0},               /* Enable 3G: off */
    {0x26, (uint8_t[]){0x01}, 1, 0},               /* Gamma set: curve 1 */
    {0xE0, (uint8_t[]){0x0F,0x27,0x23,0x0B,0x0F,0x05,0x54,0x74,0x45,0x0A,0x17,0x0A,0x1C,0x0E,0x08}, 15, 0},  /* Gamma positive */
    {0xE1, (uint8_t[]){0x08,0x1A,0x1E,0x03,0x0F,0x05,0x2E,0x25,0x3B,0x01,0x06,0x05,0x25,0x33,0x0F}, 15, 0},  /* Gamma negative */
    {0x21, NULL, 0, 0},                            /* Display inversion ON (IPS panel) */
    {0x11, NULL, 0, 120},                          /* Sleep out, 120ms */
    {0x29, NULL, 0, 0},                            /* Display on */
};

/* LCD panel & IO handles */
static esp_lcd_panel_handle_t s_lcd_panel = NULL;
static esp_lcd_panel_io_handle_t s_lcd_io = NULL;

/* Backlight GPIO */
static uint8_t s_backlight_gpio = 0;
static bool s_backlight_pwm_enabled = false;

/* ================================================================
 * Public API
 * ================================================================ */

esp_err_t lcd_driver_init(
    uint8_t spi_cs,
    uint8_t spi_sck,
    uint8_t spi_mosi,
    uint8_t spi_miso,
    uint8_t dc,
    uint8_t reset,
    uint8_t backlight
)
{
    ESP_LOGI(TAG, "Initializing ILI9341V LCD driver...");
    ESP_LOGI(TAG, "SPI pins: CS=%d, SCK=%d, MOSI=%d, MISO=%d, DC=%d, RST=%d, BL=%d",
             spi_cs, spi_sck, spi_mosi, spi_miso, dc, reset, backlight);

    s_backlight_gpio = backlight;

    /* Configure SPI bus */
    spi_bus_config_t buscfg = {
        .sclk_io_num     = spi_sck,
        .mosi_io_num     = spi_mosi,
        .miso_io_num     = spi_miso,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = LCD_WIDTH * LCD_HEIGHT * 2,  /* Max frame size */
    };

    esp_err_t ret = spi_bus_initialize(LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %d", ret);
        return ret;
    }

    /* Configure LCD panel IO (SPI) */
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num         = spi_cs,
        .dc_gpio_num         = dc,
        .spi_mode            = 0,  /* SPI mode 0 (baseline) */
        .pclk_hz             = LCD_SPI_CLOCK_MHZ * 1000000,
        .trans_queue_depth   = 10,
        .on_color_trans_done = NULL,
        .user_ctx            = NULL,
        .lcd_cmd_bits        = 8,   /* 8-bit command */
        .lcd_param_bits      = 8,   /* 8-bit parameter */
    };

    esp_lcd_panel_io_handle_t io_handle = NULL;
    ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_config, &io_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LCD IO SPI init failed: %d", ret);
        spi_bus_free(LCD_SPI_HOST);
        return ret;
    }

    /* Create ILI9341 panel (IDF 6.0+ API) with vendor init sequence */
    ili9341_vendor_config_t vendor_config = {
        .init_cmds = s_vendor_init_cmds,
        .init_cmds_size = sizeof(s_vendor_init_cmds) / sizeof(ili9341_lcd_init_cmd_t),
    };

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = reset,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_BGR,
        .data_endian    = LCD_RGB_DATA_ENDIAN_LITTLE,
        .bits_per_pixel = 16,
        .vendor_config  = &vendor_config,
    };

    ret = esp_lcd_new_panel_ili9341(io_handle, &panel_config, &s_lcd_panel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ILI9341 panel creation failed: %d", ret);
        esp_lcd_panel_io_del(io_handle);
        spi_bus_free(LCD_SPI_HOST);
        return ret;
    }

    /* Save IO handle — esp_lvgl_port needs it to flush color data over SPI */
    s_lcd_io = io_handle;

    /* Reset and initialize LCD.
     * ILI9341V needs ~120ms after hardware reset before it reliably accepts
     * commands; esp_lcd_panel_reset() only waits ~10ms, so add an explicit
     * settle delay to ensure every vendor init command is received correctly.
     * Missing/late commands were the likely cause of the residual vertical lines. */
    esp_lcd_panel_reset(s_lcd_panel);
    vTaskDelay(pdMS_TO_TICKS(120));
    esp_lcd_panel_init(s_lcd_panel);

    /* Panel orientation (swap_xy/mirror) is NOT set here — esp_lvgl_port's
     * lvgl_port_add_disp overwrites it from disp_cfg.rotation.
     * Display inversion (0x21) is already sent in the vendor init sequence,
     * so no extra invert_color() call is needed here. */

    /* Turn on display */
    esp_lcd_panel_disp_on_off(s_lcd_panel, true);

    /* Initialize backlight GPIO with LEDC PWM control */
    if (backlight < GPIO_NUM_MAX) {
        /* Configure LEDC timer */
        ledc_timer_config_t ledc_timer = {
            .speed_mode       = LEDC_LOW_SPEED_MODE,
            .duty_resolution  = LEDC_TIMER_8_BIT,  /* 8-bit resolution (0-255) */
            .timer_num        = LEDC_TIMER_0,
            .freq_hz          = 5000,  /* 5 kHz PWM frequency */
            .clk_cfg          = LEDC_AUTO_CLK,
        };
        /* Configure LEDC channel */
        ledc_channel_config_t ledc_channel = {
            .gpio_num       = backlight,
            .speed_mode     = LEDC_LOW_SPEED_MODE,
            .channel        = LEDC_CHANNEL_0,
            .intr_type      = LEDC_INTR_DISABLE,
            .timer_sel      = LEDC_TIMER_0,
            .duty           = 255,  /* Default: 100% brightness (max duty) */
            .hpoint         = 0,
        };

        /* Backlight is non-critical — don't abort the whole display system on
         * LEDC failure; fall back to simple GPIO full-on. */
        if (ledc_timer_config(&ledc_timer) == ESP_OK &&
            ledc_channel_config(&ledc_channel) == ESP_OK) {
            s_backlight_pwm_enabled = true;
            ESP_LOGI(TAG, "Backlight PWM enabled (LEDC channel 0)");
        } else {
            ESP_LOGW(TAG, "LEDC backlight config failed, falling back to GPIO on/off");
            gpio_config_t bl_conf = {
                .pin_bit_mask = (1ULL << backlight),
                .mode = GPIO_MODE_OUTPUT,
            };
            gpio_config(&bl_conf);
            gpio_set_level(backlight, 1);
        }
    }

    ESP_LOGI(TAG, "LCD driver initialized successfully");
    return ESP_OK;
}

void *lcd_driver_get_panel_handle(void)
{
    return s_lcd_panel;
}

void *lcd_driver_get_io_handle(void)
{
    return s_lcd_io;
}

void lcd_driver_set_backlight(uint8_t brightness)
{
    if (s_backlight_gpio < GPIO_NUM_MAX) {
        if (s_backlight_pwm_enabled) {
            /* Map brightness (0-100) to duty (0-255) */
            uint32_t duty = (brightness * 255) / 100;
            ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
            ESP_LOGD(TAG, "Backlight set to %d%% (duty=%d)", brightness, duty);
        } else {
            /* Simple on/off control */
            gpio_set_level(s_backlight_gpio, brightness > 50 ? 1 : 0);
            ESP_LOGD(TAG, "Backlight set to %d%%", brightness);
        }
    }
}