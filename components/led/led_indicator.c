#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/rmt_tx.h"
#include "esp_log.h"
#include "led_indicator.h"

static const char *TAG = "led";

/* ---- WS2812 timing (10 MHz RMT clock = 100 ns / tick) ---- */
#define RMT_RES_HZ          (10 * 1000 * 1000)   /* 10 MHz */
#define WS2812_T0H          4                     /* 0.4 µs */
#define WS2812_T0L          8                     /* 0.8 µs */
#define WS2812_T1H          8                     /* 0.8 µs */
#define WS2812_T1L          4                     /* 0.4 µs */
#define WS2812_RESET_TICKS  600                   /* 60 µs > 50 µs minimum */

#define LED_TASK_STACK      2048
#define LED_TASK_PRIO       1                     /* lowest priority */
#define LED_REFRESH_MS      40                    /* ~25 Hz pattern update */

/* ---- Color definitions (GRB order for WS2812) ---- */
typedef struct { uint8_t g, r, b; } led_color_t;

static const led_color_t COLOR_BLACK   = {  0,   0,   0};
static const led_color_t COLOR_RED     = {  0, 120,   0};
static const led_color_t COLOR_GREEN   = {120,   0,   0};
static const led_color_t COLOR_BLUE    = {  0,   0, 120};
static const led_color_t COLOR_YELLOW  = { 80,  80,   0};

/* ---- Module state ---- */
static rmt_channel_handle_t s_rmt_chan = NULL;
static rmt_encoder_handle_t s_encoder = NULL;
static QueueHandle_t        s_cmd_queue = NULL;   /* holds led_state_t */
static led_state_t          s_current_state = LED_STATE_OFF;
static uint32_t             s_tick = 0;            /* pattern tick counter */

/* ---- Forward declarations ---- */
static void led_task(void *arg);
static void ws2812_send(const led_color_t *c);
static led_color_t eval_pattern(led_state_t state, uint32_t tick);

/* ================================================================
 * WS2812 low-level send (uses built-in RMT copy encoder)
 * ================================================================ */

static void ws2812_send(const led_color_t *c)
{
    if (!s_rmt_chan || !s_encoder) return;

    /* Build RMT symbol array: 24 bits (GRB) + reset */
    rmt_symbol_word_t symbols[25];   /* 24 data + 1 reset */
    uint8_t bytes[3] = { c->g, c->r, c->b };

    for (int i = 0; i < 24; i++) {
        int byte_idx = i / 8;
        int bit_idx  = 7 - (i % 8);
        if (bytes[byte_idx] & (1 << bit_idx)) {
            /* 1-bit: HIGH then LOW */
            symbols[i].level0 = 1;
            symbols[i].duration0 = WS2812_T1H;
            symbols[i].level1 = 0;
            symbols[i].duration1 = WS2812_T1L;
        } else {
            /* 0-bit: HIGH then LOW (shorter high) */
            symbols[i].level0 = 1;
            symbols[i].duration0 = WS2812_T0H;
            symbols[i].level1 = 0;
            symbols[i].duration1 = WS2812_T0L;
        }
    }
    /* Reset code: >50µs LOW */
    symbols[24].level0 = 0;
    symbols[24].duration0 = 0;
    symbols[24].level1 = 0;
    symbols[24].duration1 = WS2812_RESET_TICKS;

    rmt_transmit_config_t tx_cfg = { .loop_count = 0, .flags.eot_level = 0 };
    rmt_transmit(s_rmt_chan, s_encoder, symbols, sizeof(symbols), &tx_cfg);
    rmt_tx_wait_all_done(s_rmt_chan, pdMS_TO_TICKS(10));
}

/* ================================================================
 * Pattern generator — maps (state, tick) → color
 * ================================================================ */

static led_color_t eval_pattern(led_state_t state, uint32_t tick)
{
    uint32_t phase;
    uint8_t  b;

    switch (state) {

    case LED_STATE_AP_MODE:
        /* Yellow, 1s on / 1s off */
        return (tick % 50 < 25) ? COLOR_YELLOW : COLOR_BLACK;

    case LED_STATE_STA_CONNECTING:
        /* Blue, 200ms blink */
        return (tick % 10 < 5) ? COLOR_BLUE : COLOR_BLACK;

    case LED_STATE_STA_READY:
        /* Cyan, solid with subtle breathe */
        phase = tick % 75;
        b = (phase < 37) ? (uint8_t)(phase * 3) : (uint8_t)((75 - phase) * 3);
        return (led_color_t){ (uint8_t)(b * 100 / 111), 0, (uint8_t)(b * 100 / 111) };

    case LED_STATE_MQTT_CONNECTED:
        /* Green, solid */
        return COLOR_GREEN;

    case LED_STATE_MODBUS_ERR:
        /* Red, single 120ms flash every 2s */
        return (tick % 50 < 3) ? COLOR_RED : COLOR_BLACK;

    case LED_STATE_OTA_PROGRESS:
        /* Purple pulse: 1.5s cycle */
        phase = tick % 37;
        b = (phase < 18) ? (uint8_t)(phase * 14) : (uint8_t)((37 - phase) * 14);
        return (led_color_t){
            (uint8_t)(b * 40 / 255),
            (uint8_t)(b * 60 / 255),
            (uint8_t)(b * 80 / 255)
        };

    case LED_STATE_OTA_SUCCESS:
        /* Green triple-blink: 3x 100ms on/off then long pause */
        phase = tick % 75;
        if (phase < 5 || (phase >= 10 && phase < 15) || (phase >= 20 && phase < 25))
            return COLOR_GREEN;
        return COLOR_BLACK;

    case LED_STATE_FACTORY_RESET:
        /* Red fast 100ms blink */
        return (tick % 5 < 3) ? COLOR_RED : COLOR_BLACK;

    case LED_STATE_OFF:
    default:
        return COLOR_BLACK;
    }
}

/* ================================================================
 * LED task — runs at ~25 Hz, evaluates pattern each tick
 * ================================================================ */

static void led_task(void *arg)
{
    (void)arg;
    led_color_t prev_color = COLOR_BLACK;
    s_tick = 0;

    ESP_LOGI(TAG, "LED task started");

    while (1) {
        /* Check for new state command (non-blocking) */
        led_state_t new_state;
        while (xQueueReceive(s_cmd_queue, &new_state, 0) == pdTRUE) {
            s_current_state = new_state;
            s_tick = 0;  /* reset pattern phase on state change */
        }

        /* Map current state + tick to color */
        led_color_t color = eval_pattern(s_current_state, s_tick);

        /* Update LED only when color changes (saves RMT traffic) */
        if (color.g != prev_color.g || color.r != prev_color.r || color.b != prev_color.b) {
            ws2812_send(&color);
            prev_color = color;
        }

        s_tick++;
        vTaskDelay(pdMS_TO_TICKS(LED_REFRESH_MS));
    }
}

/* ================================================================
 * Public API
 * ================================================================ */

esp_err_t led_indicator_init(uint8_t gpio)
{
    /* Already initialized? */
    if (s_rmt_chan) return ESP_OK;

    /* ---- RMT TX channel ---- */
    rmt_tx_channel_config_t tx_cfg = {
        .gpio_num = gpio,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RMT_RES_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 4,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_cfg, &s_rmt_chan));
    ESP_ERROR_CHECK(rmt_enable(s_rmt_chan));

    /* ---- Copy encoder (passes pre-built symbols through) ---- */
    rmt_copy_encoder_config_t enc_cfg = {};
    ESP_ERROR_CHECK(rmt_new_copy_encoder(&enc_cfg, &s_encoder));

    /* ---- Command queue ---- */
    s_cmd_queue = xQueueCreate(8, sizeof(led_state_t));
    if (!s_cmd_queue) {
        ESP_LOGE(TAG, "Failed to create LED command queue");
        return ESP_ERR_NO_MEM;
    }

    /* ---- LED task ---- */
    BaseType_t ret = xTaskCreate(led_task, "led", LED_TASK_STACK,
                                  NULL, LED_TASK_PRIO, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create LED task");
        vQueueDelete(s_cmd_queue);
        s_cmd_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    /* Initial state: off */
    ws2812_send(&COLOR_BLACK);

    ESP_LOGI(TAG, "LED indicator initialized on GPIO %d", gpio);
    return ESP_OK;
}

void led_indicator_set(led_state_t state)
{
    if (s_cmd_queue) {
        xQueueSend(s_cmd_queue, &state, 0);
    }
}
