#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "uart_config.h"
#include "nvs_config.h"

static const char *TAG = "uart";

#define MODBUS_UART_NUM      UART_NUM_1
#define UART_BUF_SIZE        (512)
#define UART_TX_BUF_SIZE     (256)
#define UART_RX_BUF_SIZE     (512)

/* ================================================================
 * Public
 * ================================================================ */

esp_err_t modbus_uart_init(void)
{
    const config_t *cfg = nvs_config_get();

    /* Validate pin numbers */
    if (cfg->tx_pin >= GPIO_NUM_MAX || cfg->rx_pin >= GPIO_NUM_MAX) {
        ESP_LOGE(TAG, "Invalid UART pins: TX=%d, RX=%d", cfg->tx_pin, cfg->rx_pin);
        return ESP_ERR_INVALID_ARG;
    }

    uart_config_t uart_cfg = {
        .baud_rate  = (int)cfg->baudrate,
        .data_bits  = (cfg->data_bits == 7) ? UART_DATA_7_BITS : UART_DATA_8_BITS,
        .stop_bits  = (cfg->stop_bits == 2) ? UART_STOP_BITS_2 : UART_STOP_BITS_1,
        .parity     = (cfg->parity == 1) ? UART_PARITY_ODD :
                      (cfg->parity == 2) ? UART_PARITY_EVEN : UART_PARITY_DISABLE,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_LOGI(TAG, "Init UART1: baud=%d, data=%d, stop=%d, parity=%d, tx=%d, rx=%d, de=%d",
             uart_cfg.baud_rate, uart_cfg.data_bits, uart_cfg.stop_bits,
             uart_cfg.parity, cfg->tx_pin, cfg->rx_pin, cfg->de_pin);

    ESP_ERROR_CHECK(uart_param_config(MODBUS_UART_NUM, &uart_cfg));

    /* Leave 30ms for RS485 and UART — matching MODBUS standard timing */
    ESP_ERROR_CHECK(uart_set_pin(MODBUS_UART_NUM,
                                 cfg->tx_pin, cfg->rx_pin,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    ESP_ERROR_CHECK(uart_driver_install(MODBUS_UART_NUM,
                                        UART_RX_BUF_SIZE, UART_TX_BUF_SIZE,
                                        0, NULL, 0));

    /* Initialize DE/RE pin as GPIO if configured */
    if (cfg->de_pin != 0xFF && cfg->de_pin < GPIO_NUM_MAX) {
        gpio_config_t io_cfg = {
            .pin_bit_mask = (1ULL << cfg->de_pin),
            .mode         = GPIO_MODE_OUTPUT,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_ENABLE,  /* pull low = receive mode */
            .intr_type    = GPIO_INTR_DISABLE,
        };
        gpio_config(&io_cfg);
        gpio_set_level(cfg->de_pin, 0);  /* default to RX mode */
    }

    /* Flush UART buffers */
    uart_flush(MODBUS_UART_NUM);

    ESP_LOGI(TAG, "UART1 initialized");
    return ESP_OK;
}

esp_err_t modbus_uart_deinit(void)
{
    const config_t *cfg = nvs_config_get();
    if (cfg->de_pin != 0xFF && cfg->de_pin < GPIO_NUM_MAX) {
        gpio_set_level(cfg->de_pin, 0);
    }
    uart_flush(MODBUS_UART_NUM);
    uart_driver_delete(MODBUS_UART_NUM);
    ESP_LOGI(TAG, "UART1 deinitialized");
    return ESP_OK;
}

void modbus_uart_set_rx_mode(void)
{
    const config_t *cfg = nvs_config_get();
    if (cfg->de_pin != 0xFF && cfg->de_pin < GPIO_NUM_MAX) {
        gpio_set_level(cfg->de_pin, 0);
    }
}

void modbus_uart_set_tx_mode(void)
{
    const config_t *cfg = nvs_config_get();
    if (cfg->de_pin != 0xFF && cfg->de_pin < GPIO_NUM_MAX) {
        gpio_set_level(cfg->de_pin, 1);
    }
}

int modbus_uart_get_port(void)
{
    return MODBUS_UART_NUM;
}
