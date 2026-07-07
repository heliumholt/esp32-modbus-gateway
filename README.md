# ESP32S3 MODBUS-MQTT Gateway

ESP32S3 固件：RS485 MODBUS-RTU 主机 + MQTT 数据网关。

## 功能

- **MODBUS RTU 主机**：通过 RS485 轮询从机寄存器（FC03/FC04），支持反写（FC06/FC16）
- **MQTT 上报**：JSON 批量格式上报寄存器数据
- **MQTT 反写**：通过 MQTT 接收命令写入从机寄存器
- **WiFi 配网页**：AP 模式 + Captive Portal，手机连上即可配置所有参数
- **通用硬件**：串口 TX/RX/DE 引脚可配置，同一固件适配不同 PCB

## 构建

### 前置条件

- ESP-IDF v5.x (推荐 v5.2+)
- Python 3.8+

### 设置 ESP-IDF 环境

```bash
# Windows (PowerShell)
%IDF_PATH%\export.ps1

# Linux/macOS
. $IDF_PATH/export.sh
```

### 构建

```bash
cd C:\Users\Hao\CODE\cc
idf.py set-target esp32s3
idf.py build
```

### 烧录

```bash
idf.py -p COMx flash
```

### 监控串口输出

```bash
idf.py -p COMx monitor
```

## 使用方式

### 首次配置

1. 上电后 ESP32S3 进入 AP 模式
2. 手机/电脑连接 WiFi：`MODBUS_XXXX`（XX 为 MAC 地址后两字节）
3. 浏览器自动弹出配置页面（如未弹出，手动访问 `http://192.168.4.1`）
4. 填写 WiFi、MQTT、MODBUS 参数，保存

### MQTT 协议

**上报** (`{dev_name}/data`):
```json
{"ts": 1712345678000, "regs": {"s1:0": 1234, "s1:1": 5678}}
```

**反写** (`{dev_name}/write`):
```json
{"s1:40001": 9999}
```

**状态** (`{dev_name}/status`): LWT — `"online"` / `"offline"`

### 寄存器列表格式

配网页中的寄存器列表字段格式：
```
从机地址,功能码,起始寄存器,数量;...
```

示例：
```
1,3,0,10;1,4,0,5;2,3,100,4
```
表示：
- 从机1，保持寄存器(03)，地址0起，读10个
- 从机1，输入寄存器(04)，地址0起，读5个
- 从机2，保持寄存器(03)，地址100起，读4个

### 重置配置

若需恢复出厂设置：
```bash
idf.py erase-flash
```

或通过串口命令（后续版本支持）。

## 目录结构

```
cc/
├── CMakeLists.txt          # ESP-IDF 项目入口
├── partitions.csv          # 分区表 (8MB Flash)
├── sdkconfig.defaults      # Kconfig 默认值
├── README.md
└── main/
    ├── CMakeLists.txt      # 组件构建配置
    ├── Kconfig.projbuild   # menuconfig 自定义菜单
    ├── app_main.c          # 入口 + 任务创建
    ├── wifi_manager.c/.h   # WiFi AP/STA 状态机
    ├── nvs_config.c/.h     # NVS 配置持久化
    ├── web_server.c/.h     # HTTP 服务器 + 配网页 API
    ├── dns_responder.c/.h  # Captive Portal DNS
    ├── web_config.html     # 配网页 (内嵌)
    ├── uart_config.c/.h    # UART 驱动 (可配置引脚)
    ├── modbus_master.c/.h  # MODBUS RTU 主机
    ├── modbus_params.c/.h  # 寄存器列表解析
    ├── mqtt_client.c/.h    # MQTT 客户端
    └── data_pipeline.c/.h  # 数据管道 (队列 + JSON)
```

## 硬件连接

```
ESP32S3          MAX485 (RS485 Transceiver)
--------         --------
GPIO (TX)   →    DI
GPIO (RX)   ←    RO
GPIO (DE)   →    DE + RE (短接)
GND         →    GND
3.3V        →    VCC (如 MAX485 支持 3.3V)

MAX485          RS485 Bus
--------        ---------
A           →   A (Data+)
B           →   B (Data-)
```

默认引脚（可在配网页修改）：TX=17, RX=18, DE=21
