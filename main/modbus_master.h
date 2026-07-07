#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* MODBUS function codes */
#define MODBUS_FC_READ_HOLDING    0x03
#define MODBUS_FC_READ_INPUT      0x04
#define MODBUS_FC_WRITE_SINGLE    0x06
#define MODBUS_FC_WRITE_MULTI     0x10

/* MODBUS exception codes */
#define MODBUS_EXC_ILLEGAL_FC     0x01
#define MODBUS_EXC_ILLEGAL_ADDR   0x02
#define MODBUS_EXC_ILLEGAL_DATA   0x03
#define MODBUS_EXC_SLAVE_FAIL     0x04

/* Timing */
#define MODBUS_DEFAULT_TIMEOUT_MS 200
#define MODBUS_TURNAROUND_US      2000  /* 2ms after response before next request */

/**
 * Initialize MODBUS master (UART must be initialized first).
 */
esp_err_t modbus_master_init(void);

/**
 * Read holding registers (FC03).
 * @param slave_addr  MODBUS slave address (1-247)
 * @param start_reg   Starting register address (0-65535)
 * @param count       Number of registers to read (1-125)
 * @param results_out Buffer for results (must be at least count*sizeof(uint16_t))
 * @return ESP_OK on success
 */
esp_err_t modbus_read_holding_registers(uint8_t slave_addr, uint16_t start_reg,
                                         uint16_t count, uint16_t *results_out);

/**
 * Read input registers (FC04).
 */
esp_err_t modbus_read_input_registers(uint8_t slave_addr, uint16_t start_reg,
                                       uint16_t count, uint16_t *results_out);

/**
 * Write single register (FC06).
 */
esp_err_t modbus_write_single_register(uint8_t slave_addr, uint16_t reg_addr,
                                        uint16_t value);

/**
 * Write multiple registers (FC16).
 * @param values  Array of register values
 * @param count   Number of registers to write (1-123)
 */
esp_err_t modbus_write_multiple_registers(uint8_t slave_addr, uint16_t start_reg,
                                           const uint16_t *values, uint16_t count);

#ifdef __cplusplus
}
#endif
