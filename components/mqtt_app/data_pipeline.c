#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "data_pipeline.h"

static const char *TAG = "pipeline";

#define REPORT_QUEUE_LEN  20
#define CMD_QUEUE_LEN     10

static QueueHandle_t s_report_queue = NULL;
static QueueHandle_t s_cmd_queue = NULL;

/* ================================================================
 * Init
 * ================================================================ */

esp_err_t pipeline_init(void)
{
    s_report_queue = xQueueCreate(REPORT_QUEUE_LEN, sizeof(reg_value_t));
    if (!s_report_queue) {
        ESP_LOGE(TAG, "Failed to create report_queue");
        return ESP_ERR_NO_MEM;
    }

    s_cmd_queue = xQueueCreate(CMD_QUEUE_LEN, sizeof(write_cmd_t));
    if (!s_cmd_queue) {
        ESP_LOGE(TAG, "Failed to create cmd_queue");
        vQueueDelete(s_report_queue);
        s_report_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Pipeline initialized (report=%d, cmd=%d)", REPORT_QUEUE_LEN, CMD_QUEUE_LEN);
    return ESP_OK;
}

/* ================================================================
 * Submit
 * ================================================================ */

esp_err_t pipeline_submit_reg_value(uint8_t slave, uint16_t reg, uint16_t val)
{
    reg_value_t item = {
        .slave_addr   = slave,
        .reg_addr     = reg,
        .value        = val,
        .timestamp_ms = esp_timer_get_time() / 1000,
    };

    if (xQueueSend(s_report_queue, &item, 0) != pdTRUE) {
        /* Queue full — discard oldest to make room */
        reg_value_t discard;
        xQueueReceive(s_report_queue, &discard, 0);
        xQueueSend(s_report_queue, &item, 0);
    }
    return ESP_OK;
}

esp_err_t pipeline_submit_write_cmd(uint8_t slave, uint16_t reg, uint16_t val)
{
    write_cmd_t cmd = {
        .slave_addr = slave,
        .reg_addr   = reg,
        .value      = val,
    };

    if (xQueueSend(s_cmd_queue, &cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "cmd_queue full, dropping write s%d:%d=%d", slave, reg, val);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* ================================================================
 * Build JSON report
 * ================================================================ */

int pipeline_build_report_json(char *json_out, int max_len, int timeout_ms)
{
    reg_value_t item;
    int count = 0;

    /* Wait for first item */
    if (xQueueReceive(s_report_queue, &item, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return 0;  /* nothing to report */
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "ts", item.timestamp_ms);

    cJSON *regs = cJSON_CreateObject();

    /* Add first item */
    char key[32];
    snprintf(key, sizeof(key), "s%d:%d", item.slave_addr, item.reg_addr);
    cJSON_AddNumberToObject(regs, key, item.value);
    count++;

    /* Drain remaining items (non-blocking, collect a batch) */
    while (count < 100 && xQueueReceive(s_report_queue, &item, 0) == pdTRUE) {
        snprintf(key, sizeof(key), "s%d:%d", item.slave_addr, item.reg_addr);
        cJSON_AddNumberToObject(regs, key, item.value);
        count++;
    }

    cJSON_AddItemToObject(root, "regs", regs);

    char *json_str = cJSON_PrintUnformatted(root);
    if (!json_str) {
        cJSON_Delete(root);
        return -1;
    }

    int json_len = strlen(json_str);
    if (json_len < max_len) {
        memcpy(json_out, json_str, json_len + 1);
    } else {
        ESP_LOGW(TAG, "JSON report truncated: %d > %d", json_len, max_len);
        memcpy(json_out, json_str, max_len - 1);
        json_out[max_len - 1] = '\0';
        json_len = max_len - 1;
    }

    cJSON_free(json_str);
    cJSON_Delete(root);

    return count;
}

/* ================================================================
 * Parse write JSON
 * ================================================================ */

int pipeline_parse_write_json(const char *json_str, int len)
{
    if (!json_str || len <= 0) return -1;

    cJSON *root = cJSON_ParseWithLength(json_str, len);
    if (!root) {
        ESP_LOGW(TAG, "Failed to parse write JSON: %s",
                 cJSON_GetErrorPtr() ? cJSON_GetErrorPtr() : "unknown");
        return -1;
    }

    if (!cJSON_IsObject(root)) {
        ESP_LOGW(TAG, "Write JSON is not an object");
        cJSON_Delete(root);
        return -1;
    }

    int count = 0;
    cJSON *item = root->child;
    while (item) {
        /* Parse key: "s{slave}:{register}" */
        const char *key = item->string;
        int slave = 0, reg = 0;

        if (sscanf(key, "s%d:%d", &slave, &reg) == 2 &&
            slave >= 1 && slave <= 247 &&
            reg >= 0 && reg <= 65535 &&
            cJSON_IsNumber(item)) {

            uint16_t val = (uint16_t)item->valuedouble;
            pipeline_submit_write_cmd((uint8_t)slave, (uint16_t)reg, val);
            count++;
            ESP_LOGI(TAG, "Write cmd parsed: s%d:%d = %d", slave, reg, val);
        } else {
            ESP_LOGW(TAG, "Invalid write key/value: %s", key);
        }

        item = item->next;
    }

    cJSON_Delete(root);
    return count;
}

/* ================================================================
 * Dequeue
 * ================================================================ */

esp_err_t pipeline_dequeue_write_cmd(write_cmd_t *cmd_out)
{
    if (!cmd_out) return ESP_ERR_INVALID_ARG;
    if (xQueueReceive(s_cmd_queue, cmd_out, 0) == pdTRUE) {
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}
