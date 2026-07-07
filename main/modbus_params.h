#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MODBUS_MAX_ENTRIES  32

/* MODBUS function code type */
typedef enum {
    MODBUS_FC_TYPE_READ_HOLDING  = 3,
    MODBUS_FC_TYPE_READ_INPUT    = 4,
} modbus_fc_type_t;

/**
 * A single register polling entry.
 */
typedef struct {
    uint8_t  slave_addr;   /* Slave device address (1-247) */
    uint8_t  fc;           /* Function code: 3 or 4 */
    uint16_t start_reg;    /* Start register address (0-65535) */
    uint16_t count;        /* Number of consecutive registers (1-125) */
} register_entry_t;

/**
 * Parse the register list string from NVS config.
 * Format: "slave,fc,start,count;slave,fc,start,count;..."
 * Example: "1,3,0,10;1,4,0,5;2,3,100,4"
 *
 * @param config_str  The raw config string from NVS.
 * @param entries_out Array to store parsed entries.
 * @param max_entries Maximum number of entries the array can hold.
 * @return Number of parsed entries, or -1 on parse error.
 */
int modbus_params_parse(const char *config_str,
                         register_entry_t *entries_out,
                         int max_entries);

/**
 * Get the number of currently active register entries.
 */
int modbus_params_get_count(void);

/**
 * Get a pointer to a specific entry by index.
 */
const register_entry_t *modbus_params_get_entry(int idx);

#ifdef __cplusplus
}
#endif
