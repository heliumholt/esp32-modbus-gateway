#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Queue item: a single register reading */
typedef struct {
    uint8_t  slave_addr;
    uint16_t reg_addr;
    uint16_t value;
    uint64_t timestamp_ms;
} reg_value_t;

/* Queue item: a single write command */
typedef struct {
    uint8_t  slave_addr;
    uint16_t reg_addr;
    uint16_t value;
} write_cmd_t;

/**
 * Initialize the data pipeline:
 * - Create report_queue (MODBUS readings → MQTT publisher)
 * - Create cmd_queue (MQTT write commands → MODBUS writer)
 */
esp_err_t pipeline_init(void);

/**
 * Submit a register reading into the pipeline.
 * Called by MODBUS poll task.
 */
esp_err_t pipeline_submit_reg_value(uint8_t slave, uint16_t reg, uint16_t val);

/**
 * Submit a write command into the pipeline.
 * Called by MQTT event handler (runs in MQTT library task context).
 */
esp_err_t pipeline_submit_write_cmd(uint8_t slave, uint16_t reg, uint16_t val);

/**
 * Build JSON report string from pending register values in report_queue.
 * Drains up to max_items from the queue, builds JSON.
 *
 * Format: {"ts":<epoch_ms>,"regs":{"s1:30001":1234,...}}
 *
 * @param json_out  Output buffer
 * @param max_len   Max buffer size
 * @param timeout_ms Max wait for first item (ms)
 * @return Number of items included in the report, or -1 on error
 */
int pipeline_build_report_json(char *json_out, int max_len, int timeout_ms);

/**
 * Parse a JSON write command and submit entries to cmd_queue.
 * Expected format: {"s1:40001":9999, "s2:40002":8888}
 *
 * Called by MQTT event handler.
 *
 * @param json_str  Raw JSON string from MQTT message
 * @param len       Length of json_str (may not be null-terminated)
 * @return Number of write commands parsed and queued, or -1 on error
 */
int pipeline_parse_write_json(const char *json_str, int len);

/**
 * Dequeue a pending write command (non-blocking).
 * Used by MODBUS poll task between poll cycles.
 *
 * @return ESP_OK if a command was dequeued, ESP_ERR_NOT_FOUND if queue is empty
 */
esp_err_t pipeline_dequeue_write_cmd(write_cmd_t *cmd_out);

#ifdef __cplusplus
}
#endif
