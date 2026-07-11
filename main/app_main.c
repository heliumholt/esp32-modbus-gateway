#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"

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
#include "led_indicator.h"

static const char *TAG = "main";

/* Task handles */
static TaskHandle_t s_modbus_task = NULL;
static TaskHandle_t s_pub_task = NULL;

/* Report timer — periodic JSON publish */
static TimerHandle_t s_report_timer = NULL;

/* Low memory warning threshold (bytes) */
#define LOW_MEM_WARN_THRESHOLD  (20 * 1024)

/* TWDT timeout for critical tasks */
#define TWDT_TIMEOUT_S  30

/* ================================================================
 * MQTT write-command callback
 * ================================================================ */

static void on_write_received(const char *topic, const char *data, int len)
{
    pipeline_parse_write_json(data, len);
}

/* ================================================================
 * OTA callbacks
 * ================================================================ */

static void ota_status_callback(const char *msg, bool error)
{
    mqtt_client_publish_status(msg, strlen(msg));
}

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
 * Report timer callback — builds JSON and publishes
 * ================================================================ */

static void report_timer_cb(TimerHandle_t timer)
{
    (void)timer;

    if (!mqtt_client_is_connected()) return;

    char json_buf[2048];
    int n = pipeline_build_report_json(json_buf, sizeof(json_buf), 0);  /* non-blocking */
    if (n > 0) {
        ESP_LOGI(TAG, "Publishing %d values", n);
        mqtt_client_publish_data(json_buf, strlen(json_buf));

        /* Periodic memory health check */
        size_t free_heap = xPortGetFreeHeapSize();
        if (free_heap < LOW_MEM_WARN_THRESHOLD) {
            ESP_LOGW(TAG, "Low heap warning: %u bytes free", (unsigned)free_heap);
        }
    }
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
                led_indicator_set(LED_STATE_FACTORY_RESET);
                vTaskDelay(pdMS_TO_TICKS(500));
                nvs_config_reset();
                /* nvs_config_reset() calls esp_restart() — never returns */
            }
            if (hold % 10 == 0) {
                ESP_LOGI(TAG, "Reset button held: %d/%d ticks", hold, RST_HOLD_THRESHOLD);
            }
        } else {
            hold = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(RST_POLL_MS));
    }
}

/* ================================================================
 * MQTT Publisher Task (Event-Driven)
 *
 * Sleeps until WiFi + MQTT connection events arrive.
 * A periodic FreeRTOS timer handles report publishing.
 * No busy-loop polling.
 * ================================================================ */

static void mqtt_publisher_task(void *arg)
{
    EventGroupHandle_t wifi_events = (EventGroupHandle_t)wifi_manager_get_event_group();
    EventGroupHandle_t mqtt_events = mqtt_client_get_event_group();
    bool mqtt_started = false;

    ESP_LOGI(TAG, "Publisher task waiting for WiFi...");

    while (1) {
        /* Wait for WiFi event — use timeout to periodically re-check state */
        EventBits_t wifi_bits = xEventGroupWaitBits(wifi_events,
            WIFI_EVENT_GOT_IP | WIFI_EVENT_AP_STARTED,
            pdFALSE, pdFALSE, pdMS_TO_TICKS(5000));

        if (wifi_bits & WIFI_EVENT_GOT_IP) {
            /* STA mode active — start MQTT if not already */
            if (!mqtt_started) {
                ESP_LOGI(TAG, "WiFi ready, starting MQTT...");
                if (mqtt_client_start(on_write_received, on_ota_received) == ESP_OK) {
                    mqtt_started = true;
                } else {
                    ESP_LOGW(TAG, "MQTT start failed, retrying in 10s...");
                    vTaskDelay(pdMS_TO_TICKS(10000));
                    continue;
                }
            }

            /* Wait for MQTT connection event */
            if (mqtt_events) {
                EventBits_t mqtt_bits = xEventGroupWaitBits(mqtt_events,
                    MQTT_EVENT_CONNECTED_BIT | MQTT_EVENT_DISCONNECTED_BIT,
                    pdTRUE, pdFALSE, pdMS_TO_TICKS(30000));

                if (mqtt_bits & MQTT_EVENT_CONNECTED_BIT) {
                    ESP_LOGI(TAG, "MQTT connected — starting periodic report timer");

                    /* Create and start periodic report timer */
                    const config_t *cfg = nvs_config_get();
                    TickType_t period = pdMS_TO_TICKS(cfg->poll_intv < 200 ? 200 : cfg->poll_intv);

                    if (!s_report_timer) {
                        s_report_timer = xTimerCreate("report", period, pdTRUE,
                                                       NULL, report_timer_cb);
                    }
                    if (s_report_timer) {
                        xTimerChangePeriod(s_report_timer, period, 0);
                        xTimerStart(s_report_timer, 0);
                    }

                    /* Now just wait for disconnect or WiFi loss */
                    while (mqtt_started && mqtt_client_is_connected() &&
                           wifi_manager_get_state() == WIFI_STATE_STA_READY) {
                        /* Sleep until an event occurs */
                        wifi_bits = xEventGroupWaitBits(wifi_events,
                            WIFI_EVENT_GOT_IP, /* dummy — just wait for any bit */
                            pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));

                        if (wifi_manager_get_state() != WIFI_STATE_STA_READY) {
                            break;  /* WiFi lost */
                        }

                        if (mqtt_events) {
                            mqtt_bits = xEventGroupWaitBits(mqtt_events,
                                MQTT_EVENT_DISCONNECTED_BIT,
                                pdTRUE, pdFALSE, 0);
                            if (mqtt_bits & MQTT_EVENT_DISCONNECTED_BIT) {
                                ESP_LOGW(TAG, "MQTT disconnected — stopping report timer");
                                break;
                            }
                        }
                    }

                    /* Stop report timer on disconnect */
                    if (s_report_timer) {
                        xTimerStop(s_report_timer, 0);
                    }

                    /* Trigger lightweight reconnect (no destroy) */
                    if (wifi_manager_get_state() == WIFI_STATE_STA_READY) {
                        mqtt_client_reconnect();
                    }
                }
            }
        } else if (wifi_bits & WIFI_EVENT_AP_STARTED) {
            /* AP mode — nothing to publish */
            vTaskDelay(pdMS_TO_TICKS(5000));
        } else {
            /* Timeout */
            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        /* Handle WiFi disconnect: mark MQTT as not started, let auto-reconnect handle rest */
        if (mqtt_started && wifi_manager_get_state() == WIFI_STATE_STA_LOST) {
            ESP_LOGW(TAG, "WiFi lost — MQTT will auto-reconnect when WiFi recovers");
            mqtt_started = false;
            if (s_report_timer) {
                xTimerStop(s_report_timer, 0);
            }
        }
    }
}

/* ================================================================
 * MODBUS Poll Task
 * ================================================================ */

static void modbus_poll_task(void *arg)
{
    EventGroupHandle_t wifi_events = (EventGroupHandle_t)wifi_manager_get_event_group();
    bool modbus_init_done = false;
    register_entry_t entries[MODBUS_MAX_ENTRIES];
    int entry_count = 0;

    ESP_LOGI(TAG, "MODBUS task waiting for system ready...");

    TickType_t last_wake = 0;

    while (1) {
        /* Wait for STA mode */
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
            modbus_uart_deinit();
            if (modbus_master_init() != ESP_OK) {
                ESP_LOGE(TAG, "MODBUS init failed, retrying...");
                vTaskDelay(pdMS_TO_TICKS(5000));
                continue;
            }

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

        /* Refresh config */
        const config_t *cfg = nvs_config_get();
        TickType_t period = pdMS_TO_TICKS(cfg->poll_intv < 200 ? 200 : cfg->poll_intv);

        /* Poll loop: iterate all register entries */
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
                led_indicator_set(LED_STATE_MODBUS_ERR);
            }

            /* MODBUS turnaround delay between requests */
            vTaskDelay(pdMS_TO_TICKS(5));
        }

        /* Process pending write commands (non-blocking drain) */
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
            vTaskDelay(pdMS_TO_TICKS(10));
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

    /* ------ 5. Initialize LED indicator (WS2812 on GPIO 38) ------ */
    ESP_ERROR_CHECK(led_indicator_init(38));

    /* ------ 6. Initialize OTA handler ------ */
    ESP_ERROR_CHECK(ota_handler_init(ota_status_callback));

    /* ------ 7. Log chip info ------ */
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);
    ESP_LOGI(TAG, "Chip: %d cores, rev %d, flash=%lu MB",
             chip_info.cores, chip_info.revision,
             (unsigned long)(flash_size / (1024 * 1024)));

    /* Log initial heap status */
    ESP_LOGI(TAG, "Free heap: %u bytes", (unsigned)xPortGetFreeHeapSize());

    /* ------ 8. Task Watchdog Timer (TWDT) ------ */
    esp_task_wdt_config_t twdt_config = {
        .timeout_ms = TWDT_TIMEOUT_S * 1000,
        .idle_core_mask = 0,       /* don't monitor idle tasks */
        .trigger_panic = true,     /* reboot on TWDT timeout */
    };
    ESP_ERROR_CHECK(esp_task_wdt_init(&twdt_config));

    /* ------ 9. Factory-reset button monitor ------ */
    xTaskCreatePinnedToCore(factory_reset_task, "rst_btn", 2048, NULL, 1,
                            NULL, 0);

    /* ------ 10. Start WiFi state machine ------ */
    wifi_manager_init();

    /* ------ 11. Create main tasks ------ */
    xTaskCreatePinnedToCore(modbus_poll_task, "modbus", 5120, NULL, 5,
                            &s_modbus_task, 1);

    xTaskCreatePinnedToCore(mqtt_publisher_task, "mqtt_pub", 5120, NULL, 4,
                            &s_pub_task, 0);

    /* Register TWDT subscribers */
    if (s_modbus_task) {
        esp_task_wdt_add(s_modbus_task);
    }
    if (s_pub_task) {
        esp_task_wdt_add(s_pub_task);
    }

    ESP_LOGI(TAG, "Init complete. System running.");
}
