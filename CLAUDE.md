# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ESP32S3 MODBUS-MQTT Gateway — firmware that bridges RS485 MODBUS-RTU slaves to MQTT over WiFi. The ESP32 acts as a MODBUS master polling slave registers and reporting data via JSON bulk MQTT messages. MQTT write commands are forwarded back to MODBUS slaves (FC06/FC16). A captive-portal web config page served in AP mode allows full configuration without recompilation.

Hardware target: **ESP32S3 with 16MB Flash + PSRAM (OCT mode @ 80MHz)**.

## Build Commands

```bash
# Set up ESP-IDF environment first (Windows)
%IDF_PATH%\export.ps1

# Build
idf.py set-target esp32s3
idf.py build

# Flash + monitor
idf.py -p COMx flash monitor

# Clean build
idf.py fullclean && idf.py build

# Size analysis
idf.py size
idf.py size-components    # per-component breakdown
idf.py size-files         # per-file breakdown

# Menuconfig
idf.py menuconfig
```

No tests exist yet — all verification is done on hardware.

## Architecture

### Dual-core task layout

| Task | Core | Priority | Role |
|------|------|----------|------|
| WiFi Manager | 0 | 5 | AP/STA state machine (runs from `app_main`) |
| Web Server | 0 | 3 | HTTP + DNS (created/destroyed with AP mode) |
| MQTT Publisher | 0 | 4 | Drains `report_queue` → builds JSON → publishes |
| MODBUS Poll | 1 | 5 | Polls registers, drains `cmd_queue` for writes |
| OTA Handler | 0 | 2 | Blocks on queue → downloads firmware → reboots |
| Factory Reset | 0 | 1 | Polls button GPIO, triggers NVS erase on 5s hold |
| LED Indicator | 0 | 1 | WS2812 RMT driver, 25Hz pattern engine (GPIO 38) |
| (MQTT internal) | 0 | 5 | `esp-mqtt` library task |

### Data flow

```
MODBUS Slave ←RS485→ UART1 ←→ modbus_master ←→ modbus_poll_task(Core1)
                                                        ↓
                                                  report_queue (FreeRTOS Queue)
                                                        ↓
                                                  mqtt_pub_task(Core0)
                                                        ↓
                                                MQTT → Cloud Broker
                                                        ↑
                                                  cmd_queue (FreeRTOS Queue)
                                                        ↑
                                              MQTT write handler ← Cloud
                                                        ↑
                                              MQTT OTA handler  ← Cloud
                                                        ↓
                                                  ota_queue
                                                        ↓
                                                  ota_task(Core0)
                                                        ↓
                                               esp_https_ota → reboot
```

**Read path**: `modbus_poll_task` iterates `register_entry_t[]` (parsed from NVS `reg_list`), calls FC03/FC04, pushes each register reading into `report_queue`. The `mqtt_pub_task` batches them with cJSON and publishes to `{dev}/data`.

**Write path**: MQTT event handler receives `{dev}/write`, calls `pipeline_parse_write_json()` which posts `write_cmd_t` items to `cmd_queue`. Between poll cycles, `modbus_poll_task` drains the queue non-blockingly and executes FC06/FC16.

**OTA path**: MQTT event handler receives `{dev}/ota` with `{"url":"https://..."}`, parses JSON, posts to `ota_queue`. The `ota_task` blocks on the queue, then calls `esp_https_ota()` to download + flash the firmware. On success, `esp_restart()` boots the new image. On failure, logs error and waits for the next request.

**Config path**: `web_server.c` serves the embedded `web_config.html` in AP mode. POST `/save` → `nvs_config_save()` → `wifi_manager_on_config_saved()` → AP→STA transition.

### Key modules

- **`nvs_config.c`** — Single `config_t` RAM cache backed by NVS. All other modules read config through `nvs_config_get()`. The web POST handler uses `nvs_config_set_*()` individual setters, then commits with `nvs_config_save()`.
- **`wifi_manager.c`** — State machine: `AP_MODE → STA_START → STA_CONNECTING → STA_READY → STA_LOST`. On STA failure (>5 retries), falls back to AP mode if `ap_fallback==1`. Exposes an EventGroup for other tasks to synchronize.
- **`modbus_master.c`** — Low-level MODBUS RTU protocol: CRC16 lookup table (polynomial 0xA001), frame assembly, RS485 DE/RE toggling, response parsing with timeout (200ms default). Supports FC03, FC04, FC06, FC16. UART1 is used (UART0 reserved for logging).
- **`data_pipeline.c`** — Two FreeRTOS queues (`report_queue`[20] and `cmd_queue`[10]) plus cJSON formatting/parsing. This is the sole integration point between MODBUS and MQTT.
- **`ota_handler.c`** — OTA firmware update module. `ota_request(url)` posts a URL to an internal queue (depth 1). The OTA task downloads via `esp_https_ota()`, writes to the inactive OTA partition, and reboots. Status messages are forwarded to MQTT via a callback.
- **`web_config.html`** — Self-contained (<8KB) config page with inline CSS/JS. Loads current config via `GET /api/config`, saves via `POST /save`. Dynamically builds register list rows. Uses `{dev}` topic template placeholder.
- **`modbus_params.c`** — Parses register list string format `"slave,fc,start,count;..."` into `register_entry_t[]` array (max 32 entries).
- **`led_indicator.c`** — WS2812 RGB LED on GPIO 38 driven by RMT peripheral. Background task at 25Hz evaluates current state→color pattern mapping. Other modules call `led_indicator_set()` to signal state changes.

### LED indicator states (WS2812 on GPIO 38)

| State | Color | Pattern | Trigger |
|-------|-------|---------|---------|
| `LED_STATE_OFF` | ⚫ Off | — | Initial / idle |
| `LED_STATE_AP_MODE` | 🟡 Yellow | 1s on/off | AP started, awaiting config |
| `LED_STATE_STA_CONNECTING` | 🔵 Blue | 200ms blink | STA connecting to WiFi |
| `LED_STATE_STA_READY` | 🔵 Cyan | Breathe | WiFi connected, MQTT pending |
| `LED_STATE_MQTT_CONNECTED` | 🟢 Green | Solid | MQTT broker connected |
| `LED_STATE_MODBUS_ERR` | 🔴 Red | 120ms single flash | MODBUS poll error |
| `LED_STATE_OTA_PROGRESS` | 🟣 Purple | 1.5s pulse | OTA download in progress |
| `LED_STATE_OTA_SUCCESS` | 🟢 Green | 3× blink | OTA done, rebooting |
| `LED_STATE_FACTORY_RESET` | 🔴 Red | 100ms fast blink | Factory reset triggered |

### NVS config schema

All config is in namespace `"config"`. Keys: `configd`, `dev_name`, `wifi_ssid`, `wifi_pass`, `ap_fallback`, `mqtt_uri`, `mqtt_port`, `mqtt_user`, `mqtt_pass`, `mqtt_data_t`, `mqtt_write_t`, `mqtt_stat_t`, `baudrate`, `data_bits`, `stop_bits`, `parity`, `reg_list`, `tx_pin`, `rx_pin`, `de_pin`, `poll_intv`, `custom1-3`.

### MQTT protocol

| Topic | Direction | Format |
|-------|-----------|--------|
| `{dev_name}/data` | Device→Cloud | `{"ts":<epoch_ms>,"regs":{"s1:30001":1234,...}}` |
| `{dev_name}/write` | Cloud→Device | `{"s1:40001":9999,...}` |
| `{dev_name}/status` | Device→Cloud | `"online"` / `"offline"` (LWT) |
| `{dev_name}/ota` | Cloud→Device | `{"url":"https://ota.example.com/fw.bin"}` |

Register key format: `s{slave_addr}:{register_addr}` (e.g., `s1:0`, `s2:100`).

### OTA firmware update flow

1. Cloud publishes `{"url": "https://ota-server.com/gw_v1.1.bin"}` to `{dev}/ota`
2. `mqtt_event_handler` → `on_ota_received()` → `cJSON_ParseWithLength()` → `ota_request(url)`
3. `ota_task` picks up URL from queue → `esp_https_ota()` downloads + writes to inactive partition
4. On success: reboots → bootloader reads `otadata` partition → boots new image
5. On failure: logs error + publishes "OTA failed" to status topic → waits for next request
6. **Rollback**: if new firmware crashes during boot, bootloader auto-rolls back to previous working partition

### Factory reset

- Hardwired to **GPIO 9** (active LOW, internal pull-up enabled)
- `factory_reset_task` polls GPIO 9 every 100ms
- 3 seconds continuous hold triggers `nvs_config_reset()` → `nvs_flash_erase_partition("nvs")` → `esp_restart()`
- After reboot, `configd==0` → device enters AP mode as factory-fresh

### Partition layout

16MB flash (0x1000000 bytes):

| Name | Type | SubType | Offset | Size | Notes |
|------|------|---------|--------|------|-------|
| nvs | data | nvs | 0x9000 | 0x6000 (24KB) | |
| phy_init | data | phy | 0xF000 | 0x1000 (4KB) | |
| otadata | data | ota | 0x10000 | 0x2000 (8KB) | OTA boot selection |
| factory | app | factory | 0x12000 | 0x3E0000 (~4MB) | Factory image, never overwritten |
| ota_0 | app | ota_0 | 0x3F2000 | 0x3E0000 (~4MB) | OTA slot A |
| ota_1 | app | ota_1 | 0x7D2000 | 0x3E0000 (~4MB) | OTA slot B |

Remaining ~4.3MB (0xBB2000–0x1000000) available for future SPIFFS or data storage.

## Reliability & Safety Design

### Buffer overflow protection
- `modbus_recv_frame()` clamps `expected_len` to `MODBUS_FRAME_MAX` before reading — RS485 line noise can corrupt the byte-count field, which would otherwise cause a stack buffer overflow.

### Task stack sizing
| Task | Stack | Largest local | Margin |
|------|-------|---------------|--------|
| modbus | 5120 | entries[192] + results[250] = ~442 B | ~85% free |
| mqtt_pub | 5120 | json_buf[2048] + cJSON recursion | ~55% free |
| ota | 8192 | url[256] + esp_https_ota internals | generous |
| rst_btn | 2048 | ~4 locals | plenty |

### Runtime input validation
- `poll_intv` is clamped to `≥ 200` ms at point of use — a malicious HTTP POST could set `poll_intv=0`, causing 0-tick delay loops that flood the RS485 bus.
- Invalid `de_pin` values (> GPIO_NUM_MAX) are rejected in all UART operations.

### Config hot-reload
- When WiFi drops (STA_READY → STA_LOST), `modbus_poll_task` calls `modbus_uart_deinit()` and resets `modbus_init_done = false`.
- On reconnection, the task fully re-initializes MODBUS and **re-parses the register list** from NVS — config changes made via AP web no longer require a reboot.

### Resource cleanup
- `pipeline_init()`: if cmd_queue creation fails, the already-created report_queue is freed before returning error.
- `nvs_config_reset()`: checks the return value of `nvs_flash_erase_partition()` before rebooting. Adds a 500ms settling delay to ensure NVS flash write completes.

## Important Conventions

- All strings in `config_t` are fixed-length `char[]` arrays — **no dynamic string allocation** for config values, keeping the struct entirely on the stack/global.
- UART operations are NOT thread-safe between reads and writes — the `modbus_poll_task` is the sole caller of all `modbus_*` functions, ensuring sequential access.
- The MQTT event handler runs in the `esp-mqtt` library's own task context. It must NOT block. `pipeline_parse_write_json()` only does JSON parse + queue post. Same for `on_ota_received()` — it only does JSON parse + `ota_request()` (non-blocking queue send).
- Pins are all configurable via NVS so the same firmware binary works across different hardware revisions.
- `configd == 0` means factory-fresh boot → AP mode. Set to 1 on first `nvs_config_save()`.
- WiFi AP fallback is controlled by `ap_fallback` config. If disabled, STA retries indefinitely with exponential backoff.
- SPIRAM is used transparently via `CONFIG_SPIRAM_USE_MALLOC=y` — all `malloc` calls automatically allocate from PSRAM when internal RAM is low.
- The `ota_task` runs at priority 2 (lowest of all tasks) so firmware download never starves MODBUS polling or MQTT publishing.
