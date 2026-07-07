# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ESP32S3 MODBUS-MQTT Gateway — firmware that bridges RS485 MODBUS-RTU slaves to MQTT over WiFi. The ESP32 acts as a MODBUS master polling slave registers and reporting data via JSON bulk MQTT messages. MQTT write commands are forwarded back to MODBUS slaves (FC06/FC16). A captive-portal web config page served in AP mode allows full configuration without recompilation.

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
```

**Read path**: `modbus_poll_task` iterates `register_entry_t[]` (parsed from NVS `reg_list`), calls FC03/FC04, pushes each register reading into `report_queue`. The `mqtt_pub_task` batches them with cJSON and publishes to `{dev}/data`.

**Write path**: MQTT event handler receives `{dev}/write`, calls `pipeline_parse_write_json()` which posts `write_cmd_t` items to `cmd_queue`. Between poll cycles, `modbus_poll_task` drains the queue non-blockingly and executes FC06/FC16.

**Config path**: `web_server.c` serves the embedded `web_config.html` in AP mode. POST `/save` → `nvs_config_save()` → `wifi_manager_on_config_saved()` → AP→STA transition.

### Key modules

- **`nvs_config.c`** — Single `config_t` RAM cache backed by NVS. All other modules read config through `nvs_config_get()`. The web POST handler uses `nvs_config_set_*()` individual setters, then commits with `nvs_config_save()`.
- **`wifi_manager.c`** — State machine: `AP_MODE → STA_START → STA_CONNECTING → STA_READY → STA_LOST`. On STA failure (>5 retries), falls back to AP mode if `ap_fallback==1`. Exposes an EventGroup for other tasks to synchronize.
- **`modbus_master.c`** — Low-level MODBUS RTU protocol: CRC16 lookup table (polynomial 0xA001), frame assembly, RS485 DE/RE toggling, response parsing with timeout (200ms default). Supports FC03, FC04, FC06, FC16. UART1 is used (UART0 reserved for logging).
- **`data_pipeline.c`** — Two FreeRTOS queues (`report_queue`[20] and `cmd_queue`[10]) plus cJSON formatting/parsing. This is the sole integration point between MODBUS and MQTT.
- **`web_config.html`** — Self-contained (<8KB) config page with inline CSS/JS. Loads current config via `GET /api/config`, saves via `POST /save`. Dynamically builds register list rows. Uses `{dev}` topic template placeholder.
- **`modbus_params.c`** — Parses register list string format `"slave,fc,start,count;..."` into `register_entry_t[]` array (max 32 entries).

### NVS config schema

All config is in namespace `"config"`. Keys: `configd`, `dev_name`, `wifi_ssid`, `wifi_pass`, `ap_fallback`, `mqtt_uri`, `mqtt_port`, `mqtt_user`, `mqtt_pass`, `mqtt_data_t`, `mqtt_write_t`, `mqtt_stat_t`, `baudrate`, `data_bits`, `stop_bits`, `parity`, `reg_list`, `tx_pin`, `rx_pin`, `de_pin`, `poll_intv`, `custom1-3`.

### MQTT protocol

| Topic | Direction | Format |
|-------|-----------|--------|
| `{dev_name}/data` | Device→Cloud | `{"ts":<epoch_ms>,"regs":{"s1:30001":1234,...}}` |
| `{dev_name}/write` | Cloud→Device | `{"s1:40001":9999,...}` |
| `{dev_name}/status` | Device→Cloud | `"online"` / `"offline"` (LWT) |

Register key format: `s{slave_addr}:{register_addr}` (e.g., `s1:0`, `s2:100`).

### Partition layout

8MB flash: NVS 24KB (`0x9000-0xF000`), factory app ~4MB (`0x10000+`).

## Important Conventions

- All strings in `config_t` are fixed-length `char[]` arrays — **no dynamic string allocation** for config values, keeping the struct entirely on the stack/global.
- UART operations are NOT thread-safe between reads and writes — the `modbus_poll_task` is the sole caller of all `modbus_*` functions, ensuring sequential access.
- The MQTT event handler runs in the `esp-mqtt` library's own task context. It must NOT block. `pipeline_parse_write_json()` only does JSON parse + queue post.
- Pins are all configurable via NVS so the same firmware binary works across different hardware revisions.
- `configd == 0` means factory-fresh boot → AP mode. Set to 1 on first `nvs_config_save()`.
- WiFi AP fallback is controlled by `ap_fallback` config. If disabled, STA retries indefinitely with exponential backoff.