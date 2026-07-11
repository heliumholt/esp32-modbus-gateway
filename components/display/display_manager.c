#include "display_manager.h"
#include "lcd_driver.h"
#include "touch_driver.h"
#include "ui_main.h"
#include "ui_modbus.h"
#include "ui_system.h"
#include "ui_ota.h"
#include "gateway_status.h"
#include "nvs_config.h"
#include "wifi_manager.h"
#include "app_mqtt.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include <stdio.h>
#include "lvgl.h"

static const char *TAG = "display";

/* Gateway status (updated periodically) */
static gateway_status_t s_gateway_status = {0};

/* UI page objects */
static lv_obj_t *s_page_main = NULL;
static lv_obj_t *s_page_modbus = NULL;
static lv_obj_t *s_page_system = NULL;
static lv_obj_t *s_page_ota = NULL;
static lv_obj_t *s_active_page = NULL;

/* Display task handle */
static TaskHandle_t s_display_task = NULL;

/* Start time for uptime calculation */
static uint64_t s_start_time_ms = 0;

/* ================================================================
 * Forward declarations
 * ================================================================ */

static void btn_main_cb(lv_event_t *e);
static void btn_modbus_cb(lv_event_t *e);
static void btn_system_cb(lv_event_t *e);
static void btn_ota_cb(lv_event_t *e);

/* ================================================================
 * Touch input driver for LVGL
 * ================================================================ */

static void touch_driver_read(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    uint16_t x, y;
    esp_err_t ret = touch_driver_read_point(&x, &y);

    if (ret == ESP_OK) {
        /* Map FT6336 raw coords to landscape screen:
         *   screen_x = ft_y
         *   screen_y = (LCD_WIDTH-1) - ft_x
         * (calibrated from screen corners) */
        data->point.x = y;
        data->point.y = (LCD_WIDTH - 1) - x;
        data->state = LV_INDEV_STATE_PR;  /* Touch pressed */
    } else {
        data->state = LV_INDEV_STATE_REL;  /* Touch released */
    }

    /* No buffering required */
    data->continue_reading = false;
}

/* ================================================================
 * Status update (internal)
 * ================================================================ */

static void update_gateway_status(void)
{
    /* WiFi status */
    s_gateway_status.wifi_state = wifi_manager_get_state();

    /* Real IP address from the default netif (works for both STA and AP) */
    s_gateway_status.ip_addr[0] = '\0';
    esp_netif_t *netif = esp_netif_get_default_netif();
    if (netif) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            snprintf(s_gateway_status.ip_addr, sizeof(s_gateway_status.ip_addr),
                     IPSTR, IP2STR(&ip_info.ip));
        }
    }

    /* MQTT status */
    s_gateway_status.mqtt_connected = mqtt_client_is_connected();

    /* System status */
    s_gateway_status.free_heap = xPortGetFreeHeapSize();
    s_gateway_status.uptime_sec = (esp_timer_get_time() - s_start_time_ms) / 1000000ULL;

    /* Real WiFi RSSI (STA mode only; meaningless in AP mode) */
    s_gateway_status.wifi_rssi = 0;
    if (s_gateway_status.wifi_state == WIFI_STATE_STA_READY) {
        int rssi = 0;
        if (esp_wifi_sta_get_rssi(&rssi) == ESP_OK) {
            s_gateway_status.wifi_rssi = (int8_t)rssi;
        }
    }
}

/* ================================================================
 * Page switching
 * ================================================================ */

static void switch_page(lv_obj_t *new_page)
{
    if (s_active_page && s_active_page != new_page) {
        lv_obj_add_flag(s_active_page, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_clear_flag(new_page, LV_OBJ_FLAG_HIDDEN);
    s_active_page = new_page;
}

/* ================================================================
 * Display task
 * ================================================================ */

static void display_task(void *arg)
{
    ESP_LOGI(TAG, "Display task started");

    /* Initialize LCD driver */
    const config_t *cfg = nvs_config_get();
    esp_err_t ret = lcd_driver_init(
        cfg->lcd_spi_cs,
        cfg->lcd_spi_sck,
        cfg->lcd_spi_mosi,
        cfg->lcd_spi_miso,
        cfg->lcd_dc,
        cfg->lcd_reset,
        cfg->lcd_backlight
    );
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LCD driver init failed: %d", ret);
        vTaskDelete(NULL);
        return;
    }

    /* Initialize touch driver */
    ret = touch_driver_init(
        cfg->touch_i2c_sda,
        cfg->touch_i2c_scl,
        cfg->touch_int,
        cfg->touch_reset
    );
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Touch driver init failed: %d", ret);
        /* Continue anyway - touch is optional */
    }

    /* Initialize LVGL via esp_lvgl_port */
    ESP_LOGI(TAG, "Initializing LVGL...");

    /* Initialize LVGL port core first (calls lv_init() + builds the LVGL
     * memory pool + spawns the LVGL task/timer). Without this, the later
     * lv_mem_alloc inside lv_disp_drv_register dereferences a NULL pool. */
    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_cfg.task_stack = 12288;  /* default 7168 overflowed when rendering styled objects */
    ret = lvgl_port_init(&lvgl_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LVGL port init failed: %d", ret);
        vTaskDelete(NULL);
        return;
    }

    /* Get LCD panel handle */
    esp_lcd_panel_handle_t lcd_panel = (esp_lcd_panel_handle_t)lcd_driver_get_panel_handle();
    if (!lcd_panel) {
        ESP_LOGE(TAG, "LCD panel handle is NULL");
        vTaskDelete(NULL);
        return;
    }

    /* Configure LVGL display buffer (partial buffer in PSRAM) */
    lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = lcd_driver_get_io_handle(),
        .panel_handle = lcd_panel,
        .hres = LCD_HEIGHT,  /* 320 px — landscape */
        .vres = LCD_WIDTH,   /* 240 px — landscape */
        .buffer_size = LCD_HEIGHT * 10,
        .double_buffer = false,
        .rotation = {
            .swap_xy = true,    /* rotate panel to landscape */
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = true,    /* Internal DMA-capable RAM — PSRAM buffer caused vertical-line artifacts (S3 SPI DMA can't cleanly read octal PSRAM) */
            .buff_spiram = false,
            /* swap_bytes only available in LVGL 9+ */
        },
    };

    lv_disp_t *disp = lvgl_port_add_disp(&disp_cfg);
    if (!disp) {
        ESP_LOGE(TAG, "LVGL display init failed");
        vTaskDelete(NULL);
        return;
    }

    /* Add touch input device.
     * indev_drv MUST be static — LVGL 8 keeps a *pointer* to the driver
     * (indev->driver), not a copy, so the struct must outlive this scope.
     * A stack-local driver here caused a use-after-scope crash (PC=0x00ffffff
     * in lv_indev_read_timer_cb) once the stack slot got reused. */
    static lv_indev_drv_t indev_drv;
    if (touch_driver_get_i2c_handle()) {
        lv_indev_drv_init(&indev_drv);
        indev_drv.type = LV_INDEV_TYPE_POINTER;
        indev_drv.disp = disp;
        indev_drv.read_cb = touch_driver_read;

        lv_indev_t *indev = lv_indev_drv_register(&indev_drv);
        if (!indev) {
            ESP_LOGW(TAG, "Touch input registration failed");
        } else {
            ESP_LOGI(TAG, "Touch input registered");
        }
    }

    /* Create UI pages */
    ESP_LOGI(TAG, "Creating UI pages...");

    /* Hold the LVGL lock while building the UI. esp_lvgl_port already
     * launched the LVGL task in lvgl_port_init(), so touching LVGL objects
     * from this task without the lock races the LVGL timer handler. */
    lvgl_port_lock(portMAX_DELAY);

    /* Create main screen container */
    lv_obj_t *scr = lv_disp_get_scr_act(disp);

    /* Create 4 page containers */
    s_page_main = ui_main_create(scr);
    s_page_modbus = ui_modbus_create(scr);
    s_page_system = ui_system_create(scr);
    s_page_ota = ui_ota_create(scr);

    /* Hide all pages except main */
    lv_obj_add_flag(s_page_modbus, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_page_system, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_page_ota, LV_OBJ_FLAG_HIDDEN);

    /* Set main page as active */
    s_active_page = s_page_main;

    /* Create navigation buttons (rounded, larger, moved up 20px, black centered text) */
    lv_obj_t *btn_main = lv_btn_create(scr);
    lv_obj_set_size(btn_main, 75, 45);
    lv_obj_align(btn_main, LV_ALIGN_BOTTOM_LEFT, 4, -25);
    lv_obj_set_style_radius(btn_main, 12, 0);
    lv_obj_add_event_cb(btn_main, btn_main_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *label_main = lv_label_create(btn_main);
    lv_label_set_text(label_main, "Main");
    lv_obj_center(label_main);
    lv_obj_set_style_bg_color(btn_main, lv_color_hex(0x0000FF), 0);   /* solid blue */
    lv_obj_set_style_bg_color(btn_main, lv_color_darken(lv_color_hex(0x0000FF), 60), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn_main, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(label_main, lv_color_black(), 0);

    lv_obj_t *btn_modbus = lv_btn_create(scr);
    lv_obj_set_size(btn_modbus, 75, 45);
    lv_obj_align(btn_modbus, LV_ALIGN_BOTTOM_LEFT, 83, -25);
    lv_obj_set_style_radius(btn_modbus, 12, 0);
    lv_obj_add_event_cb(btn_modbus, btn_modbus_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *label_modbus = lv_label_create(btn_modbus);
    lv_label_set_text(label_modbus, "MB");
    lv_obj_center(label_modbus);
    lv_obj_set_style_bg_color(btn_modbus, lv_color_hex(0x00FF00), 0); /* solid green */
    lv_obj_set_style_bg_color(btn_modbus, lv_color_darken(lv_color_hex(0x00FF00), 60), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn_modbus, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(label_modbus, lv_color_black(), 0);

    lv_obj_t *btn_system = lv_btn_create(scr);
    lv_obj_set_size(btn_system, 75, 45);
    lv_obj_align(btn_system, LV_ALIGN_BOTTOM_LEFT, 162, -25);
    lv_obj_set_style_radius(btn_system, 12, 0);
    lv_obj_add_event_cb(btn_system, btn_system_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *label_system = lv_label_create(btn_system);
    lv_label_set_text(label_system, "Sys");
    lv_obj_center(label_system);
    lv_obj_set_style_bg_color(btn_system, lv_color_hex(0xFF0000), 0); /* solid red */
    lv_obj_set_style_bg_color(btn_system, lv_color_darken(lv_color_hex(0xFF0000), 60), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn_system, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(label_system, lv_color_black(), 0);

    lv_obj_t *btn_ota = lv_btn_create(scr);
    lv_obj_set_size(btn_ota, 75, 45);
    lv_obj_align(btn_ota, LV_ALIGN_BOTTOM_LEFT, 241, -25);
    lv_obj_set_style_radius(btn_ota, 12, 0);
    lv_obj_add_event_cb(btn_ota, btn_ota_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *label_ota = lv_label_create(btn_ota);
    lv_label_set_text(label_ota, "OTA");
    lv_obj_center(label_ota);
    lv_obj_set_style_bg_color(btn_ota, lv_color_hex(0xFFFF00), 0);    /* solid yellow */
    lv_obj_set_style_bg_color(btn_ota, lv_color_darken(lv_color_hex(0xFFFF00), 60), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn_ota, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(label_ota, lv_color_black(), 0);      /* black text on yellow */

    lvgl_port_unlock();

    ESP_LOGI(TAG, "UI created, starting LVGL task loop...");

    s_start_time_ms = esp_timer_get_time();

    /* LVGL task loop (handled by esp_lvgl_port internally) */
    while (1) {
        /* Update gateway status every 1s */
        update_gateway_status();

        /* Update active page UI */
        lvgl_port_lock(0);  /* Lock LVGL mutex */

        if (s_active_page == s_page_main) {
            ui_main_update(&s_gateway_status);
        } else if (s_active_page == s_page_modbus) {
            ui_modbus_update(&s_gateway_status);
        } else if (s_active_page == s_page_system) {
            ui_system_update(&s_gateway_status);
        }

        lvgl_port_unlock();  /* Unlock LVGL mutex */

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ================================================================
 * Navigation button callbacks
 * ================================================================ */

static void btn_main_cb(lv_event_t *e)
{
    switch_page(s_page_main);
}

static void btn_modbus_cb(lv_event_t *e)
{
    switch_page(s_page_modbus);
}

static void btn_system_cb(lv_event_t *e)
{
    switch_page(s_page_system);
}

static void btn_ota_cb(lv_event_t *e)
{
    switch_page(s_page_ota);
}

/* ================================================================
 * Public API
 * ================================================================ */

esp_err_t display_manager_init(void)
{
    /* Create display task on Core 0, Priority 3 */
    BaseType_t ret = xTaskCreatePinnedToCore(
        display_task,
        "display",
        16384,  /* Stack size — UI creation with styles needs headroom */
        NULL,
        3,     /* Priority */
        &s_display_task,
        0      /* Core 0 */
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create display task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Display manager initialized");
    return ESP_OK;
}

void display_manager_update_status(const gateway_status_t *status)
{
    /* This is called externally - just trigger update */
    /* Actual update happens in display_task loop */
}

void display_manager_update_ota(uint8_t progress, const char *state)
{
    s_gateway_status.ota_progress = progress;

    if (strcmp(state, "downloading") == 0) {
        s_gateway_status.ota_state = OTA_STATE_DOWNLOADING;
    } else if (strcmp(state, "success") == 0) {
        s_gateway_status.ota_state = OTA_STATE_SUCCESS;
    } else if (strcmp(state, "failed") == 0) {
        s_gateway_status.ota_state = OTA_STATE_FAILED;
    } else {
        s_gateway_status.ota_state = OTA_STATE_IDLE;
    }

    /* Update OTA page if active */
    if (s_active_page == s_page_ota) {
        ui_ota_update(progress, state);
    }
}

void display_manager_set_backlight(uint8_t brightness)
{
    lcd_driver_set_backlight(brightness);
}