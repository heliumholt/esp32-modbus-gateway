#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"

#include "ota_handler.h"

static const char *TAG = "ota";

#define OTA_QUEUE_DEPTH   1        /* only one OTA at a time */
#define OTA_TASK_STACK    8192
#define OTA_TASK_PRIO     2        /* low priority — yield to everything else */

static QueueHandle_t   s_ota_queue   = NULL;
static ota_status_cb_t s_status_cb   = NULL;
static volatile bool   s_in_progress = false;

/* ================================================================
 * Status helper
 * ================================================================ */

static void ota_status(const char *msg, bool error)
{
    if (error) {
        ESP_LOGE(TAG, "%s", msg);
    } else {
        ESP_LOGI(TAG, "%s", msg);
    }
    if (s_status_cb) {
        s_status_cb(msg, error);
    }
}

/* ================================================================
 * OTA Task
 *
 * Blocks on s_ota_queue forever.
 * When a URL arrives:
 *   1. Report "starting"
 *   2. Call esp_https_ota() — blocking download + flash
 *   3. Report "success, rebooting" or "failed"
 *   4. On success → esp_restart()
 * ================================================================ */

static void ota_task(void *arg)
{
    char url[OTA_URL_MAX_LEN];

    ESP_LOGI(TAG, "OTA task started, waiting for update requests...");

    while (1) {
        /* Block until a URL is queued */
        if (xQueueReceive(s_ota_queue, url, portMAX_DELAY) != pdTRUE) {
            continue;  /* should never happen */
        }

        s_in_progress = true;

        ESP_LOGI(TAG, "OTA update requested: %s", url);
        ota_status("OTA update starting", false);

        /* Configure HTTPS OTA (also works for plain HTTP when
         * CONFIG_OTA_ALLOW_HTTP=y) */
        esp_http_client_config_t http_cfg = {
            .url = url,
            .timeout_ms = 30000,   /* 30s HTTP timeout per chunk */
            .skip_cert_common_name_check = true,
        };

        esp_https_ota_config_t ota_cfg = {
            .http_config = &http_cfg,
        };

        ESP_LOGI(TAG, "Downloading firmware...");
        esp_err_t ret = esp_https_ota(&ota_cfg);

        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "OTA successful! Rebooting in 2 seconds...");
            ota_status("OTA successful, rebooting", false);

            /* Give MQTT/WiFi a moment to flush before reboot */
            vTaskDelay(pdMS_TO_TICKS(2000));
            esp_restart();
        } else {
            /* Map common error codes to readable messages */
            const char *reason = "unknown error";
            switch (ret) {
            case ESP_ERR_OTA_VALIDATE_FAILED:
                reason = "firmware image validation failed"; break;
            case ESP_ERR_NO_MEM:
                reason = "out of memory"; break;
            case ESP_ERR_HTTPS_OTA_IN_PROGRESS:
                reason = "OTA already in progress"; break;
            case ESP_FAIL:
                reason = "connection or HTTP error"; break;
            default:
                break;
            }

            ESP_LOGE(TAG, "OTA failed: %s (err=0x%x)", reason, ret);

            char buf[128];
            snprintf(buf, sizeof(buf), "OTA failed: %s (0x%x)", reason, ret);
            ota_status(buf, true);
        }

        s_in_progress = false;
    }
}

/* ================================================================
 * Public API
 * ================================================================ */

esp_err_t ota_handler_init(ota_status_cb_t on_status)
{
    if (s_ota_queue) {
        ESP_LOGW(TAG, "OTA handler already initialized");
        return ESP_OK;
    }

    s_status_cb = on_status;

    s_ota_queue = xQueueCreate(OTA_QUEUE_DEPTH, OTA_URL_MAX_LEN);
    if (!s_ota_queue) {
        ESP_LOGE(TAG, "Failed to create OTA queue");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ret = xTaskCreate(ota_task, "ota", OTA_TASK_STACK,
                                 NULL, OTA_TASK_PRIO, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create OTA task");
        vQueueDelete(s_ota_queue);
        s_ota_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "OTA handler initialized (queue=%d, stack=%d)",
             OTA_QUEUE_DEPTH, OTA_TASK_STACK);
    return ESP_OK;
}

esp_err_t ota_request(const char *url)
{
    if (!s_ota_queue) {
        ESP_LOGE(TAG, "OTA handler not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (!url || strlen(url) == 0) {
        ESP_LOGE(TAG, "OTA request with empty URL");
        return ESP_ERR_INVALID_ARG;
    }

    if (s_in_progress) {
        ESP_LOGW(TAG, "OTA already in progress, ignoring new request");
        return ESP_ERR_INVALID_STATE;
    }

    char buf[OTA_URL_MAX_LEN];
    strncpy(buf, url, OTA_URL_MAX_LEN - 1);
    buf[OTA_URL_MAX_LEN - 1] = '\0';

    if (xQueueSend(s_ota_queue, buf, 0) != pdTRUE) {
        ESP_LOGW(TAG, "OTA queue full, request dropped");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "OTA request queued");
    return ESP_OK;
}

bool ota_is_in_progress(void)
{
    return s_in_progress;
}
