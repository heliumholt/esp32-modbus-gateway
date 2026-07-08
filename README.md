# ESP32S3 MODBUS-MQTT Gateway

ESP32S3 固件：RS485 MODBUS-RTU 主机 + MQTT 数据网关。

## 功能

- **MODBUS RTU 主机**：通过 RS485 轮询从机寄存器（FC03/FC04），支持反写（FC06/FC16）
- **MQTT 上报**：JSON 批量格式上报寄存器数据
- **MQTT 反写**：通过 MQTT 接收命令写入从机寄存器
- **OTA 固件升级**：通过 MQTT `{dev}/ota` 主题下发固件 URL，自动下载 + 重启
- **Web 配网页**：AP/STA 双模式可访问，Captive Portal + 实时 Payload 预览
- **WS2812 LED 指示灯**：GPIO 38 多色状态显示（WiFi/MQTT/MODBUS/OTA）
- **工厂复位**：长按 GPIO 9 5 秒自动清除配置，无需重新烧录
- **通用硬件**：串口 TX/RX/DE 引脚可配置，同一固件适配不同 PCB

## 构建

### 前置条件

- ESP-IDF v6.0.2
- Python 3.8+

### 设置 ESP-IDF 环境

```bash
# Windows (PowerShell)
%IDF_PATH%\export.ps1

# Linux/macOS
. $IDF_PATH/export.sh
```

### 构建 & 烧录

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```

## 使用方式

### 首次配置

1. 上电后 ESP32S3 进入 AP 模式
2. 手机/电脑连接 WiFi：`BOX_XXXX`（XX 为 MAC 地址后两字节，无密码）
3. 浏览器自动弹出配置页面（如未弹出，手动访问 `http://192.168.4.1`）
4. 填写 WiFi、MQTT Broker、MODBUS 参数，保存
5. 设备自动切换 STA 模式连接路由器
6. STA 模式下仍可通过设备 IP 访问配置页

### Web 配置页

| 区块 | 说明 |
|------|------|
| 设备 | 设备名称（用于 MQTT 主题 `{dev}` 占位符） |
| WiFi | SSID / 密码 / STA 失败回退 AP |
| MQTT Broker | URI、端口、用户名、密码、客户端 ID、主题模板 |
| MODBUS | 波特率、数据位、停止位、校验、轮询间隔 |
| 寄存器列表 | slave,fc,start,count 动态行编辑 |
| 串口引脚 | TX / RX / DE GPIO |
| 自定义参数 | custom1-3 自由字段 |
| **Payload 预览** | 实时展示解析后的主题 + JSON 结构 |

### MQTT 协议

| 主题 | 方向 | 格式 |
|------|------|------|
| `{dev_name}/data` | 设备→云端 | `{"ts":1712345678000,"regs":{"s1:0":1234,...}}` |
| `{dev_name}/write` | 云端→设备 | `{"s1:40001":9999}` |
| `{dev_name}/status` | 设备→云端 | `"online"` / `"offline"` (LWT) |
| `{dev_name}/ota` | 云端→设备 | `{"url":"https://ota.example.com/fw.bin"}` |

### 寄存器列表格式

```
从机地址,功能码,起始寄存器,数量;...
```

示例：`1,3,0,10;1,4,0,5;2,3,100,4`
- 从机1，保持寄存器(FC03)，地址0起，读10个
- 从机1，输入寄存器(FC04)，地址0起，读5个
- 从机2，保持寄存器(FC03)，地址100起，读4个

### 工厂复位

**方法一**：长按连接在 **GPIO 9** 的按钮 5 秒，LED 红灯快闪后自动清除 NVS 并重启

**方法二**：串口擦除
```bash
idf.py erase-flash
```

## LED 指示灯 (GPIO 38)

| 颜色 | 图案 | 含义 |
|------|------|------|
| 🟡 黄色 | 1s 慢闪 | AP 模式，等待配置 |
| 🔵 蓝色 | 200ms 快闪 | STA 连接中 |
| 🔵 青色 | 呼吸渐变 | WiFi 已连接，等待 MQTT |
| 🟢 绿色 | 常亮 | 完全运行（WiFi + MQTT） |
| 🔴 红色 | 单次 120ms 闪 | MODBUS 轮询错误 |
| 🟣 紫色 | 1.5s 脉冲 | OTA 固件下载中 |
| 🟢 绿色 | 3 连闪 | OTA 成功，即将重启 |
| 🔴 红色 | 100ms 快闪 | 工厂复位中 |

## 硬件连接

```
ESP32S3          MAX485 (RS485 Transceiver)
--------         --------
GPIO 16 (TX) →   DI
GPIO 17 (RX) ←   RO
GPIO 8  (DE) →   DE + RE (短接)
GND         →    GND
3.3V        →    VCC

MAX485          RS485 Bus
--------        ---------
A           →   A (Data+)
B           →   B (Data-)
```

默认引脚（可在配网页修改）：**TX=16, RX=17, DE=8**

```
ESP32S3          WS2812 LED
--------         ----------
GPIO 38     →    DIN
3.3V/5V     →    VCC
GND         →    GND
```

## 目录结构

```
cc/
├── CMakeLists.txt
├── partitions.csv
├── sdkconfig.defaults
├── README.md
└── main/
    ├── CMakeLists.txt
    ├── Kconfig.projbuild
    ├── app_main.c              # 入口 + 任务创建
    ├── wifi_manager.c/.h       # WiFi AP/STA 状态机
    ├── nvs_config.c/.h         # NVS 配置持久化
    ├── web_server.c/.h         # HTTP 服务器 + 配网页 API
    ├── dns_responder.c/.h      # Captive Portal DNS
    ├── web_config.html         # 配网页 (内嵌)
    ├── uart_config.c/.h        # UART 驱动
    ├── modbus_master.c/.h      # MODBUS RTU 主机
    ├── modbus_params.c/.h      # 寄存器列表解析
    ├── mqtt_client.c/.h        # MQTT 客户端
    ├── data_pipeline.c/.h      # 数据管道 (队列 + JSON)
    ├── ota_handler.c/.h        # OTA 固件升级
    └── led_indicator.c/.h      # WS2812 LED 状态灯
```
