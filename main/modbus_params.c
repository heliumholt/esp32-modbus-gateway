#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "modbus_params.h"
#include "esp_log.h"

static const char *TAG = "mb_params";

static register_entry_t s_entries[MODBUS_MAX_ENTRIES];
static int s_entry_count = 0;

/* ================================================================
 * Parser
 * ================================================================ */

int modbus_params_parse(const char *config_str,
                         register_entry_t *entries_out,
                         int max_entries)
{
    if (!config_str || !entries_out || max_entries <= 0) return -1;

    int count = 0;
    char *work = strdup(config_str);
    if (!work) return -1;

    char *saveptr_entry;
    char *entry = strtok_r(work, ";", &saveptr_entry);

    while (entry && count < max_entries) {
        /* Trim leading whitespace */
        while (*entry == ' ') entry++;

        int slave, fc, start, reg_count;
        if (sscanf(entry, "%d,%d,%d,%d", &slave, &fc, &start, &reg_count) == 4) {
            if (slave >= 1 && slave <= 247 &&
                (fc == 3 || fc == 4) &&
                start >= 0 && start <= 65535 &&
                reg_count >= 1 && reg_count <= 125) {
                entries_out[count].slave_addr = (uint8_t)slave;
                entries_out[count].fc         = (uint8_t)fc;
                entries_out[count].start_reg  = (uint16_t)start;
                entries_out[count].count      = (uint16_t)reg_count;
                count++;
            } else {
                ESP_LOGW(TAG, "Invalid register entry: %s (slave=%d,fc=%d,start=%d,count=%d)",
                         entry, slave, fc, start, reg_count);
            }
        } else {
            ESP_LOGW(TAG, "Malformed register entry: %s", entry);
        }

        entry = strtok_r(NULL, ";", &saveptr_entry);
    }

    if (count >= max_entries && entry) {
        ESP_LOGW(TAG, "Max entries (%d) reached, remaining entries ignored", max_entries);
    }

    free(work);

    /* Update global cache */
    memcpy(s_entries, entries_out, count * sizeof(register_entry_t));
    s_entry_count = count;

    ESP_LOGI(TAG, "Parsed %d register entries", count);
    return count;
}

/* ================================================================
 * Accessors
 * ================================================================ */

int modbus_params_get_count(void)
{
    return s_entry_count;
}

const register_entry_t *modbus_params_get_entry(int idx)
{
    if (idx < 0 || idx >= s_entry_count) return NULL;
    return &s_entries[idx];
}
