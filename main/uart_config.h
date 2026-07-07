#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize UART for MODBUS RTU communication.
 * Pins and baud rate are read from NVS config.
 * Uses UART1 (UART0 is reserved for console/logging).
 */
esp_err_t modbus_uart_init(void);

/**
 * Deinitialize UART (call before reconfiguration).
 */
esp_err_t modbus_uart_deinit(void);

/**
 * Set RS485 transceiver to receive mode (DE low, RE low).
 */
void modbus_uart_set_rx_mode(void);

/**
 * Set RS485 transceiver to transmit mode (DE high, RE high).
 */
void modbus_uart_set_tx_mode(void);

/**
 * Get UART port number used by MODBUS.
 */
int modbus_uart_get_port(void);

#ifdef __cplusplus
}
#endif
