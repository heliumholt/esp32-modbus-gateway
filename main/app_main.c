#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_timer.h"

#include "nvs_config.h"
#include "wifi_manager.h"
#include "modbus_master.h"
#include "uart_config.h"
#include "modbus_params.h"
#include "app_mqtt.h"
#include "data_pipeline.h"
#include "ota_handler.h"
#include "cJSON.h"
#include "driver/gpio.h"

static const char *TAG = "main";

/* Task handles */
static TaskHandle_t s_modbus_task = NULL;
static TaskHandle_t s_pub_task = NULL;

/* ================================================================
 * MQTT write-command callback (bridges to pipeline)
 * ================================================================ */

/**
 * Wrapper that adapts mqtt_data_cb_t(topic, data, len) →
 * pipeline_parse_write_json(data).
 * Runs in MQTT library task context — must NOT block.
 */
static void on_write_received(const char *topic, const char *data, int len)
{
    pipeline_parse_write_json(data, len);
}

/* ================================================================
 * OTA callbacks
 * ================================================================ */

/**
 * Forward OTA status messages to MQTT so the cloud can monitor progress.
 */
static void ota_status_callback(const char *msg, bool error)
{
    mqtt_client_publish_status(msg, strlen(msg));
}

/**
 * Handle incoming MQTT OTA commands.
 * Expected JSON payload: {"url": "https://ota.example.com/fw.bin"}
 * Runs in MQTT library task context — must NOT block.
 */
static void on_ota_received(const char *topic, const char *data, int len)
{
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (!root) {
        ESP_LOGW(TAG, "OTA: invalid JSON payload");
        return;
    }

    cJSON *url_item = cJSON_GetObjectItem(root, "url");
    if (cJSON_IsString(url_item) && url_item->valuestring) {
        ESP_LOGI(TAG, "OTA request received: %s", url_item->valuestring);
        esp_err_t ret = ota_request(url_item->valuestring);
        if (ret != ESP_OK) {
            mqtt_client_publish_status("OTA request rejected", 20);
        }
    } else {
        ESP_LOGW(TAG, "OTA: missing or invalid 'url' field");
    }

    cJSON_Delete(root);
}

/* ================================================================
 * Factory Reset Button Monitor Task
 *
 * Hardwired to GPIO 9 (active-low, internal pull-up).
 * A continuous 5-second LOW (button held) triggers nvs_config_reset().
 * ================================================================ */

#define RST_BUTTON_GPIO     9
#define RST_HOLD_THRESHOLD  50      /* 5 seconds @ 100ms ticks */
#define RST_POLL_MS         100

static void factory_reset_task(void *arg)
{
    /* Configure GPIO 9 as input with internal pull-up */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << RST_BUTTON_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    ESP_LOGI(TAG, "Factory-reset button on GPIO %d (hold %d ms to trigger)",
             RST_BUTTON_GPIO, RST_HOLD_THRESHOLD * RST_POLL_MS);

    int hold = 0;

    while (1) {
        if (gpio_get_level(RST_BUTTON_GPIO) == 0) {
            hold++;
            if (hold >= RST_HOLD_THRESHOLD) {
                ESP_LOGW(TAG, "Factory-reset triggered — erasing config and rebooting...");
                nvs_config_reset();
                /* nvs_config_reset() calls esp_restart() — never returns */
            }
            /* Log progress every second */
            if (hold % 10 == 0) {
                ESP_LOGI(TAG, "Reset button held: %d/%d ticks", hold, RST_HOLD_THRESHOLD);
            }
        } else {
            hold = 0;   /* button released — reset counter */
        }
        vTaskDelay(pdMS_TO_TICKS(RST_POLL_MS));
    }
}

/* ================================================================
 * MQTT Publisher Task
 *
 * Waits for WiFi + MQTT connection, then:
 *  - Blocks on report_queue
 *  - Collects register values into a JSON batch
 *  - Publishes via MQTT
 * ================================================================ */

static void mqtt_publisher_task(void *arg)
{
    EventGroupHandle_t wifi_events = (EventGroupHandle_t)wifi_manager_get_event_group();
    bool mqtt_started = false;

    ESP_LOGI(TAG, "Publisher task waiting for WiFi...");

    while (1) {
        /* Wait for WiFi to be ready (or AP mode started) */
        EventBits_t bits = xEventGroupWaitBits(wifi_events,
                              WIFI_EVENT_GOT_IP | WIFI_EVENT_AP_STARTED,
                              pdFALSE, pdFALSE, pdMS_TO_TICKS(5000));

        if (bits & WIFI_EVENT_GOT_IP) {
            /* STA mode active — start MQTT if not already */
            if (!mqtt_started) {
                ESP_LOGI(TAG, "WiFi ready, starting MQTT...");
                if (mqtt_client_start(on_write_received, on_ota_received) == ESP_OK) {
                    mqtt_started = true;
                    /* Wait a bit for MQTT connection */
                    vTaskDelay(pdMS_TO_TICKS(3000));
                }
            }

            /* If MQTT is connected, build and publish report */
            if (mqtt_client_is_connected()) {
                char json_buf[2048];
                int n = pipeline_build_report_json(json_buf, sizeof(json_buf), 5000);
                if (n > 0) {
                    ESP_LOGI(TAG, "Publishing %d values: %s", n, json_buf);
                    mqtt_client_publish_data(json_buf, strlen(json_buf));
                }
            } else if (mqtt_started) {
                /* MQTT disconnected — wait and retry */
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        } else if (bits & WIFI_EVENT_AP_STARTED) {
            /* AP mode — nothing to publish. Just wait. */
            vTaskDelay(pdMS_TO_TICKS(1000));
        } else {
            /* Timeout — neither event received. Wait and retry. */
            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        /* Handle WiFi disconnect: stop MQTT */
        if (mqtt_started && wifi_manager_get_state() == WIFI_STATE_STA_LOST) {
            mqtt_client_stop();
            mqtt_started = false;
            ESP_LOGW(TAG, "WiFi lost, MQTT stopped");
        }
    }
}

/* ================================================================
 * MODBUS Poll Task
 *
 * Waits for WiFi STA_READY + MQTT connected, then:
 *  - Periodically polls all configured register entries
 *  - Submits readings to data pipeline
 *  - Checks cmd_queue for pending write commands
 * ================================================================ */

static void modbus_poll_task(void *arg)
{
    EventGroupHandle_t wifi_events = (EventGroupHandle_t)wifi_manager_get_event_group();
    bool modbus_init_done = false;
    register_entry_t entries[MODBUS_MAX_ENTRIES];
    int entry_count = 0;

    ESP_LOGI(TAG, "MODBUS task waiting for system ready...");

    const config_t *cfg = nvs_config_get();
    TickType_t last_wake = xTaskGetTickCount();
    TickType_t period = pdMS_TO_TICKS(cfg->poll_intv);

    while (1) {
        /* Wait for STA mode (GOT_IP) to be active */
        if (wifi_manager_get_state() != WIFI_STATE_STA_READY) {
            if (modbus_init_done) {
                modbus_uart_deinit();
                modbus_init_done = false;
                ESP_LOGI(TAG, "WiFi lost, MODBUS deinitialized");
            }
            xEventGroupWaitBits(wifi_events, WIFI_EVENT_GOT_IP, pdFALSE, pdFALSE,
                               pdMS_TO_TICKS(1000));
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        /* Initialize (or re-initialize) MODBUS after WiFi connected */
        if (!modbus_init_done) {
            modbus_uart_deinit();   /* safe no-op if not initialized */
            if (modbus_master_init() != ESP_OK) {
                ESP_LOGE(TAG, "MODBUS init failed, retrying...");
                vTaskDelay(pdMS_TO_TICKS(5000));
                continue;
            }

            /* Parse register list from config (always re-parse on re-init) */
            const config_t *c = nvs_config_get();
            if (strlen(c->reg_list) > 0) {
                entry_count = modbus_params_parse(c->reg_list, entries, MODBUS_MAX_ENTRIES);
                ESP_LOGI(TAG, "Loaded %d MODBUS register entries", entry_count);
            } else {
                entry_count = 0;
                ESP_LOGW(TAG, "No register list configured — idle");
            }

            modbus_init_done = true;
            last_wake = xTaskGetTickCount();
        }

        /* Refresh config (may have changed via web) */
        cfg = nvs_config_get();
        period = pdMS_TO_TICKS(cfg->poll_intv < 200 ? 200 : cfg->poll_intv);

        /* ============================================================
         * Poll loop: iterate all register entries
         * ============================================================ */
        for (int i = 0; i < entry_count && wifi_manager_get_state() == WIFI_STATE_STA_READY; i++) {
            register_entry_t *e = &entries[i];
            uint16_t results[125];
            esp_err_t err;

            if (e->fc == 3) {
                err = modbus_read_holding_registers(e->slave_addr, e->start_reg,
                                                     e->count, results);
            } else {
                err = modbus_read_input_registers(e->slave_addr, e->start_reg,
                                                   e->count, results);
            }

            if (err == ESP_OK) {
                for (int j = 0; j < e->count; j++) {
                    pipeline_submit_reg_value(e->slave_addr,
                                               e->start_reg + j, results[j]);
                }
            } else {
                ESP_LOGW(TAG, "MODBUS read failed: slave=%d, fc=%d, addr=%d, err=%d",
                         e->slave_addr, e->fc, e->start_reg, err);
                /* Continue to next entry — don't block the loop */
            }

            /* MODBUS turnaround delay between requests */
            vTaskDelay(pdMS_TO_TICKS(5));
        }

        /* ============================================================
         * Process pending write commands (non-blocking drain)
         * ============================================================ */
        write_cmd_t cmd;
        while (pipeline_dequeue_write_cmd(&cmd) == ESP_OK) {
            esp_err_t err = modbus_write_single_register(cmd.slave_addr,
                                                          cmd.reg_addr, cmd.value);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Write failed: s%d:%d=%d, err=%d",
                         cmd.slave_addr, cmd.reg_addr, cmd.value, err);
            } else {
                ESP_LOGI(TAG, "Written: s%d:%d=%d", cmd.slave_addr, cmd.reg_addr, cmd.value);
            }
            vTaskDelay(pdMS_TO_TICKS(10));  /* gap between writes */
        }

        /* Wait until next poll cycle */
        vTaskDelayUntil(&last_wake, period);
    }
}

/* ================================================================
 * Entry Point
 * ================================================================ */

void app_main(void)
{
    ESP_LOGI(TAG, "============================================");
    ESP_LOGI(TAG, "  ESP32S3 MODBUS-MQTT Gateway v1.0");
    ESP_LOGI(TAG, "============================================");

    /* ------ 1. NVS flash ------ */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS corrupted, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* ------ 2. TCP/IP stack ------ */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* ------ 3. Load config ------ */
    bool configured = nvs_config_init();
    ESP_LOGI(TAG, "Device configured: %s", configured ? "yes" : "no");

    /* ------ 4. Create data pipeline (queues) ------ */
    ESP_ERROR_CHECK(pipeline_init());

    /* ------ 5. Initialize OTA handler ------ */
    ESP_ERROR_CHECK(ota_handler_init(ota_status_callback));

    /* ------ 6. Log chip info ------ */
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);
    ESP_LOGI(TAG, "Chip: %d cores, rev %d, flash=%d MB",
             chip_info.cores, chip_info.revision,
             flash_size / (1024 * 1024));

    /* ------ 7. Start WiFi state machine FIRST — creates event group that tasks depend on ------ */
    wifi_manager_init();

    /* ------ 8. Factory-reset button monitor ------ */
    xTaskCreatePinnedToCore(factory_reset_task, "rst_btn", 2048, NULL, 1,
                            NULL, 0);

    /* ------ 9. Create main tasks ------ */
    /* MODBUS poll task — pinned to Core 1 for real-time UART timing */
    xTaskCreatePinnedToCore(modbus_poll_task, "modbus", 5120, NULL, 5,
                            &s_modbus_task, 1);

    /* MQTT publisher task — Core 0 (shares with WiFi + web server) */
    xTaskCreatePinnedToCore(mqtt_publisher_task, "mqtt_pub", 5120, NULL, 4,
                            &s_pub_task, 0);

    /* app_main returns — FreeRTOS scheduler keeps tasks running */
    ESP_LOGI(TAG, "Init complete. System running.");
}
