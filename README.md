# Air724UG + ESP32-C3 短信转发网关

本项目使用 Air724UG 模块负责接收短信和来电事件，使用 ESP32-C3 通过串口接收这些事件，并在局域网内提供 Web 监控页面。推荐方案是让 ESP32-C3 连接 WiFi 后转发 Bark 推送；Air724UG 侧只使用蜂窝短信/语音能力，不使用 SIM 卡流量。

## 工作原理

```text
SIM 卡短信/来电
    ↓
Air724UG Luat 脚本监听短信和来电
    ↓ UART3 / 115200 / JSON Lines
ESP32-C3 Web Monitor
    ↓ WiFi
Bark 推送 / 局域网页面查看
```

Air724UG 收到短信或来电后，会把通知内容写入 UART3。ESP32-C3 从串口读取 JSON Lines，提供设备状态、原始日志、Bark 配置和测试等网页功能。Bark 请求由 ESP32-C3 通过 WiFi 发送，不经过 Air724UG 的蜂窝数据网络。

## 不使用 SIM 卡流量

Air724UG 默认配置为本地串口转发模式：

```lua
NOTIFY_TYPE = { "usb_uart" }
USB_NOTIFY_ENABLE = true
USB_NOTIFY_EOL = "\r\n"
QUERY_TRAFFIC_INTERVAL = 0
RNDIS_ENABLE = false
NTP_AUTO_SYNC = false
```

这些配置的含义是：

- `NOTIFY_TYPE = { "usb_uart" }`：通知只输出到 UART，不直接访问 Telegram、Bark、飞书、钉钉等网络渠道。
- `RNDIS_ENABLE = false`：关闭 Air724UG 的 RNDIS 网卡。
- `NTP_AUTO_SYNC = false`：Air724UG 不通过蜂窝数据做 NTP 校时；时间由 ESP32-C3 下发同步。
- `QUERY_TRAFFIC_INTERVAL = 0`：关闭定时短信查询流量，避免误发运营商查询短信。

如需保持“不使用流量”，不要在 Air724UG 的 `script/config.lua` 中启用网络通知渠道、RNDIS、录音上传或 NTP 自动校时。

## 硬件连接

| ESP32-C3 | Air724UG | 说明 |
| --- | --- | --- |
| GPIO20 / RX | UART3 TX | ESP32-C3 接收 Air724UG 上报 |
| GPIO21 / TX | UART3 RX | ESP32-C3 下发状态、校时、重启命令 |
| GND | GND | 必须共地 |

串口参数：

```text
115200 baud
8 data bits
No parity
1 stop bit
Line ending: \r\n
```

注意 TX/RX 需要交叉连接，并确认两侧 IO 电平兼容。

## 固件组成

- `smsnotify.ino`：ESP32-C3 Arduino 固件，负责 WiFi 配网、Web 页面、Bark 推送和 UART 通信。
- `script/`：Air724UG Luat 脚本，负责短信/来电监听、状态查询、UART3 JSON Lines 上报。
- `script/config.lua`：Air724UG 运行配置，默认已设置为只走 `usb_uart`。

## 部署步骤

### 1. 烧录 ESP32-C3

使用 Arduino IDE 或兼容工具打开 `smsnotify.ino`，选择 ESP32-C3 开发板并烧录。

ESP32-C3 固件使用以下默认设置：

```cpp
#define RX_PIN 20
#define TX_PIN 21
#define UART_BAUD 115200
```

依赖库包括：

- `WiFi`
- `WebServer`
- `WebSocketsServer`
- `HTTPClient`
- `Preferences`

### 2. 下载 Air724UG 脚本

将 `script/` 目录中的 Luat 脚本下载到 Air724UG 模块。保持 `script/config.lua` 中的默认离线串口配置：

```lua
NOTIFY_TYPE = { "usb_uart" }
RNDIS_ENABLE = false
NTP_AUTO_SYNC = false
QUERY_TRAFFIC_INTERVAL = 0
```

### 3. 连接串口

按“硬件连接”表连接 ESP32-C3 与 Air724UG，并给两个模块稳定供电。首次联调时建议同时查看 ESP32-C3 串口日志，确认能看到 `UART_RX` 或状态请求日志。

### 4. ESP32-C3 配网

ESP32-C3 启动后会优先连接已保存的 WiFi。如果连接失败，会进入 SmartConfig 配网模式。

使用 ESPTouch / SmartConfig 类 App 将当前 WiFi 信息发送给 ESP32-C3。配网成功后，ESP32-C3 会在串口输出当前局域网 IP。

### 5. 访问 Web Monitor

在同一局域网浏览器访问：

```text
http://<ESP32-C3-IP>/
```

默认网页密码：

```text
123456
```

登录后建议立即在网页中修改密码。

### 6. 配置 Bark

在 Web Monitor 的 “Bark 配置” 中填写：

```text
BARK_API = https://api.day.app
BARK_KEY = <你的 Bark key>
```

保存后点击 “测试 Bark”。短信和来电通知中包含 `#SMS` 或 `#CALL` 时，ESP32-C3 会通过 WiFi 发送 Bark 推送。

## Web Monitor 功能

- 设备状态：显示号码或 ICCID、身份类型、运营商、信号、温度、启动原因、更新时间。
- 原始日志：显示 UART 收发、状态消息和 Bark 请求结果。
- 刷新状态：向 Air724UG 发送带 `id` 的 `get_status` 命令，并显示请求/回复诊断信息。
- 同步时间：ESP32-C3 启动 NTP 校时，并定期把当前时间下发给 Air724UG。
- 重启下位机：向 Air724UG 发送 `reboot` 命令。
- 原始命令：手动发送 JSON 行，便于联调。
- Bark 配置：保存 Bark API、Key，测试推送；Bark 请求会先进入队列，再由 ESP32-C3 后台发送。
- 网页密码：修改 Web Monitor 登录密码。

## 串口协议摘要

Air724UG 与 ESP32-C3 使用 JSON Lines 通信，每行以 `\r\n` 结尾。

### Air724UG 上报通知

```json
{
  "channel": "usb_uart",
  "message": "短信内容\n\n发件号码: 10086\n发件时间: 2026-05-12 12:00:00\n#SMS",
  "timestamp": "2026-05-12 12:00:00",
  "device_info_appended": true
}
```

ESP32-C3 只会对包含 `#SMS` 或 `#CALL` 的通知触发 Bark 转发。

### ESP32-C3 查询状态

```json
{"cmd":"get_status","id":"status-1-1a2b"}
```

Air724UG 回复：

```json
{
  "channel": "usb_uart",
  "event": "get_status",
  "id": "status-1-1a2b",
  "ok": true,
  "timestamp": "2026-05-12 12:00:00",
  "phone_number": "13800138000",
  "identity_type": "phone_number",
  "operator": "中国移动",
  "signal_strength": "-86dBm",
  "temperature": "32℃",
  "poweron_reason": "0",
  "poweron_reason_zh": "电源键或上电开机"
}
```

### ESP32-C3 同步时间

```json
{
  "cmd": "sync_time",
  "id": "time-2-1a90",
  "source": "esp32_web",
  "clock": {
    "year": 2026,
    "month": 5,
    "day": 12,
    "hour": 12,
    "min": 0,
    "sec": 0
  },
  "host_timestamp": "2026-05-12 12:00:00"
}
```

### ESP32-C3 重启 Air724UG

```json
{"cmd":"reboot","id":"reboot-3-1af0"}
```

## 常见问题

### 收不到短信通知

1. 确认 SIM 卡能正常接收短信。
2. 确认 Air724UG 脚本已运行，并且 `NOTIFY_TYPE = { "usb_uart" }`。
3. 确认 ESP32-C3 与 Air724UG 的 TX/RX 已交叉连接且共地。
4. 在 Web Monitor 查看原始日志，确认是否出现 `UART_RX`。

### Web 页面显示下位机离线

ESP32-C3 默认每 30 秒尝试发送一次 `get_status`，且同一时间只保留一个等待回复的状态请求。请求和回复会通过 `id` 对应，页面会显示最后接收、最后请求、最后回复、请求状态和 Bark 队列。如果 90 秒未收到 Air724UG 任何串口消息，页面会显示下位机离线。请检查 UART3 连接、Air724UG 脚本运行状态和供电稳定性。

### Bark 不推送

1. 确认 ESP32-C3 已连接 WiFi，并且浏览器能访问 Web Monitor。
2. 在 Web Monitor 中保存 Bark API 与 Key。
3. 点击 “测试 Bark” 查看原始日志中的 `[BARK]` 结果。
4. 确认真实短信通知包含 `#SMS`，来电通知包含 `#CALL`。

### Air724UG 时间为空

默认 `NTP_AUTO_SYNC = false`，Air724UG 不使用蜂窝数据自动校时。ESP32-C3 会启动 WiFi/NTP 校时，并定期下发 `sync_time` 给 Air724UG。

### 如何避免消耗 SIM 卡流量

保持 Air724UG 侧只启用 `usb_uart`，不要启用以下配置：

- `RNDIS_ENABLE = true`
- `NTP_AUTO_SYNC = true`
- `UPLOAD_URL = "..."`
- `NOTIFY_TYPE` 中除 `usb_uart` 以外的网络通知渠道

本项目推荐把所有互联网访问都放在 ESP32-C3 的 WiFi 侧完成。
