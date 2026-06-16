# Meshtastic 接口使用文档

本文档面向 `cap_meshtastic` 能力组件，覆盖 ESP-Claw 与外接 Meshtastic LoRa 电台
（如 Heltec LoRa32 V3）桥接后对外暴露的全部接口：

1. [HTTP REST API](#http-rest-api)（`/api/mesh/*`，配合 Web 控制台与第三方集成）—— 本分支新增
2. [LLM 可调用工具](#llm-可调用工具)（Agent 通过自然语言触发）
3. [串口控制台命令](#串口控制台命令)（`mesh ...`）
4. [C 编程接口](#c-编程接口)（`cap_meshtastic.h`）
5. [编译期配置与接线](#编译期配置与接线)

> 桥接的工作原理、电台 PROTO 模式设置与接线，请先阅读上层教程
> `docs/.../tutorial/meshtastic-bridge.mdx`。本文聚焦“接口”本身。

---

## HTTP REST API

所有接口挂载在设备 HTTP 服务器下，路径前缀 `/api/mesh`。

- 仅当固件编译开启 `CONFIG_APP_CLAW_CAP_MESHTASTIC` 且服务回调已注册时可用；
  否则统一返回 **`503 Service Unavailable`**，响应体为 `Meshtastic not configured`。
- 请求/响应均为 `application/json`（错误占位响应除外）。
- 接口**无独立鉴权**，依赖局域网信任模型（与设备其余 HTTP API 一致）。
  注意：`DELETE` 与 `POST` 为状态变更接口，请勿暴露到不可信网络。
- 发送 mesh 文本**未**通过 HTTP 暴露，仅可经 LLM 工具 / 控制台 / C API 发送。

基地址示例：`http://<设备IP>`（AP 模式下通常为 `http://192.168.4.1`）。

### GET /api/mesh/status

返回桥接链路状态与持久化消息存储概况。

**响应 200**

```json
{
  "connected": true,
  "message_count": 12,
  "file_size_bytes": 3456,
  "store_path": "/sdcard/meshtastic/messages.jsonl"
}
```

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `connected` | bool | 是否已收到电台 `MyNodeInfo`（链路建立） |
| `message_count` | number | 已持久化的消息条数 |
| `file_size_bytes` | number | 存储文件大小（字节） |
| `store_path` | string | 存储文件绝对路径；存储未初始化时该字段缺省 |

> 存储优先使用 SD 卡（无大小上限），无 SD 卡时回退内部 FATFS（上限 256 KB，
> 超限自动滚动保留较新约一半内容）。

```bash
curl http://192.168.4.1/api/mesh/status
```

### GET /api/mesh/messages

读取已接收的 mesh 文本消息，**最新在前**。

**查询参数**

| 参数 | 类型 | 默认 | 说明 |
| --- | --- | --- | --- |
| `limit` | int | 200 | 返回的最大条数，有效范围 `1 ~ 9999`，越界则回退默认值 |

**响应 200**

```json
{
  "messages": [
    {
      "from": "!aabbccdd",
      "from_num": 2864434397,
      "channel": 0,
      "packet_id": 123456789,
      "text": "hello mesh",
      "ts": 1718500000000
    }
  ],
  "count": 1
}
```

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `from` | string | 发送节点的 `!xxxxxxxx` 形式节点 ID |
| `from_num` | number | 发送节点号（十进制） |
| `channel` | number | 信道索引（0 为主信道） |
| `packet_id` | number | Meshtastic 数据包 ID |
| `text` | string | 消息正文 |
| `ts` | number | 接收时间，epoch 毫秒（系统时钟未同步时回退为开机相对毫秒） |
| `count` | number | 本次返回的消息条数 |

```bash
curl "http://192.168.4.1/api/mesh/messages?limit=50"
```

### DELETE /api/mesh/messages

清空持久化的全部消息。

**响应 200**

```json
{ "ok": true, "message": "messages cleared" }
```

```bash
curl -X DELETE http://192.168.4.1/api/mesh/messages
```

### GET /api/mesh/im

列出“收到 mesh 消息后可主动推送到的 IM 渠道”及其状态。当前固定四个渠道：
`feishu` / `qq` / `telegram` / `wechat`。

**响应 200**

```json
{
  "channels": [
    { "channel": "feishu",   "enabled": true,  "has_target": true },
    { "channel": "qq",       "enabled": false, "has_target": false },
    { "channel": "telegram", "enabled": false, "has_target": false },
    { "channel": "wechat",   "enabled": false, "has_target": true }
  ]
}
```

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `channel` | string | 渠道标识 |
| `enabled` | bool | 用户是否开启“把 mesh 入站消息推送到该渠道” |
| `has_target` | bool | 是否已学习到一个具体会话作为推送目标 |

> `has_target` 由“最近与设备对话的会话”自动学习并持久化到 NVS；
> 只有 `enabled && has_target` 同时为真时，入站 mesh 消息才会推送到该渠道。

```bash
curl http://192.168.4.1/api/mesh/im
```

### POST /api/mesh/im

开启或关闭某个 IM 渠道的 mesh 消息推送。偏好持久化到 NVS。

**请求体**

```json
{ "channel": "feishu", "enabled": true }
```

| 字段 | 类型 | 必填 | 说明 |
| --- | --- | --- | --- |
| `channel` | string | 是 | 必须为 `feishu`/`qq`/`telegram`/`wechat` 之一 |
| `enabled` | bool | 是 | `true` 开启推送，`false` 关闭 |

**响应 200**

```json
{ "ok": true, "channel": "feishu", "enabled": true }
```

**错误**

| 状态码 | 场景 |
| --- | --- |
| `400 Bad Request` | 请求体非法 JSON / 缺少 `channel` 或 `enabled` / 渠道未知或不可用 |

```bash
curl -X POST http://192.168.4.1/api/mesh/im \
  -H 'Content-Type: application/json' \
  -d '{"channel":"feishu","enabled":true}'
```

### HTTP 错误码一览

| 状态码 | 含义 |
| --- | --- |
| `200 OK` | 成功 |
| `400 Bad Request` | 请求参数 / JSON 体非法（仅 `POST /api/mesh/im`） |
| `500 Internal Server Error` | 内存不足或读取/操作失败 |
| `503 Service Unavailable` | Meshtastic 能力未编译或未配置 |

---

## LLM 可调用工具

Agent 可通过自然语言触发以下工具（family = `meshtastic`）。

| 工具 | 作用 | 入参 |
| --- | --- | --- |
| `meshtastic_send_text` | 发送文本到 mesh（广播或指定节点） | 见下 |
| `meshtastic_list_nodes` | 列出已知节点（名称、SNR、位置、遥测） | 无 |
| `meshtastic_get_status` | 链路状态、本机节点 ID、固件版本、节点数、诊断 | 无 |
| `meshtastic_get_messages` | 返回最近接收的文本消息 | 无 |
| `meshtastic_request_config` | 请求电台重新流式下发节点库与配置 | 无 |

`meshtastic_send_text` 入参 schema：

```json
{
  "text": "hello mesh",       // 必填，消息正文（≤237 字节）
  "dest": "broadcast",        // 可选，"broadcast"/"all" 或 "!aabbccdd"/十进制节点号，默认广播
  "channel": 0,               // 可选，信道索引，默认 0
  "want_ack": false           // 可选，是否请求送达确认
}
```

`meshtastic_get_status` 在未连接时会返回 `hint` 字段，提示排障方向
（无字节 / 非 PROTO 模式 / 等待握手）。

---

## 串口控制台命令

在设备串口控制台输入 `mesh`：

```text
mesh status                                  # 链路状态 + 诊断（rx_bytes / frames_decoded）
mesh nodes                                   # 列出已知节点
mesh messages                                # 列出已接收消息
mesh send "hello mesh"                       # 广播一条文本
mesh send "hi node" --dest !aabbccdd --ch 0 --ack   # 定向发送并请求 ACK
mesh config                                  # 请求电台重新下发节点库 / 配置
```

| 参数 | 说明 |
| --- | --- |
| `<action>` | `status`(默认) / `nodes` / `messages` / `send` / `config` |
| `<text>` | `send` 的消息正文 |
| `--dest <id>` | 目的节点 ID，默认广播 |
| `--ch <n>` | 信道索引，默认 0 |
| `--ack` | 请求送达确认 |

---

## C 编程接口

头文件：[`include/cap_meshtastic.h`](../include/cap_meshtastic.h)。

### 生命周期与配置

```c
/* 在能力组启动前覆盖 UART 引脚 / 端口 / 波特率（可选）。 */
esp_err_t cap_meshtastic_set_uart_config(const cap_meshtastic_uart_config_t *config);

/* 注册并启动 Meshtastic 能力组（创建 UART 与后台 RX 任务）。 */
esp_err_t cap_meshtastic_register_group(void);

/* 链路是否已建立（已收到 MyNodeInfo）。 */
bool cap_meshtastic_is_connected(void);
```

### 收发

```c
/* 发送文本：dest=0xFFFFFFFF 为广播，channel=0 为主信道。 */
esp_err_t cap_meshtastic_send_text(uint32_t dest, uint32_t channel,
                                   bool want_ack, const char *text);

/* 请求电台重新流式下发节点库与配置。 */
esp_err_t cap_meshtastic_request_config(void);
```

### IM 推送目标

```c
/* 显式设置/清除入站 mesh 消息的 IM 推送会话（sticky，不会被自动学习覆盖）。 */
esp_err_t cap_meshtastic_set_notify_target(const char *channel, const char *chat_id);

/* 记录“最近对话会话”，供未显式配置时的自动推送使用（可在 IM 消息观察者中调用）。 */
void cap_meshtastic_note_im_target(const char *channel, const char *chat_id);

/* 查询 / 设置每个 IM 渠道的推送开关（持久化到 NVS）。 */
size_t    cap_meshtastic_get_im_push(cap_meshtastic_im_push_t *out, size_t max);
esp_err_t cap_meshtastic_set_im_push_enabled(const char *channel, bool enabled);
```

### 持久化消息存储

```c
/* 配置存储路径与上限（max_bytes=0 表示不限，适合 SD 卡）。 */
esp_err_t cap_meshtastic_set_store_path(const char *base_path, size_t max_bytes);

/* 读取已存消息到 cJSON 数组（最新在前）。 */
esp_err_t cap_meshtastic_read_stored_messages(cJSON *array, size_t max_count);

esp_err_t   cap_meshtastic_clear_stored_messages(void);
size_t      cap_meshtastic_stored_count(void);
size_t      cap_meshtastic_store_file_size(void);
const char *cap_meshtastic_store_path(void);
```

> 当存储后端（如 SD 卡）与 LCD 共用 SPI 总线时，需通过
> `meshtastic_store_set_bus_lock()`（见 `meshtastic_store.h`）注入总线锁回调，
> 避免 SDSPI 轮询与 LCD DMA 事务竞争。

---

## 编译期配置与接线

`Component config -> Claw Meshtastic Capability`（`CONFIG_CAP_MESHTASTIC_*`）：

| 配置项 | 默认 | 说明 |
| --- | --- | --- |
| `CAP_MESHTASTIC_UART_PORT` | 1 | UART 外设号（勿用作控制台的 UART0） |
| `CAP_MESHTASTIC_TX_GPIO` | 5 | 主板 TX（→ 电台 RX）；NM-Display-28inch 用 43 |
| `CAP_MESHTASTIC_RX_GPIO` | 4 | 主板 RX（← 电台 TX）；NM-Display-28inch 用 44 |
| `CAP_MESHTASTIC_BAUD` | 115200 | 须与电台 `serial.baud` 一致 |

UART 必须**交叉连接**（主板 TX → 电台 RX，主板 RX → 电台 TX）：

| 主板 | 默认 TX | 默认 RX |
| --- | --- | --- |
| NM-CYD-C5 | GPIO5 | GPIO4 |
| NM-Display-28inch | GPIO43 | GPIO44（板级默认已设置） |

电台侧需将 Serial 模块设为 **PROTO** 模式（非 TEXTMSG）：

```bash
meshtastic --set serial.enabled true
meshtastic --set serial.mode PROTO
meshtastic --set serial.baud BAUD_115200
meshtastic --set serial.rxd 19
meshtastic --set serial.txd 20
meshtastic --reboot
```

---

## 排障速查

通过 `GET /api/mesh/status` 或 `mesh status` 的诊断字段定位问题：

| 现象 | 可能原因 |
| --- | --- |
| `connected=false` 且 `rx_bytes=0` | 未收到任何字节：检查供电、TX↔RX 交叉接线、波特率是否与 `serial.baud` 一致 |
| `rx_bytes>0` 但 `frames_decoded=0` | 收到字节但无 protobuf 帧：电台未处于 **PROTO** 模式或波特率不匹配 |
| 已解出帧但仍 `connected=false` | 等待电台完成 config 握手，稍候数秒 |
| `connected=true` 但消息列表为空 | 链路与节点库正常，但尚无实时 mesh 报文：从其他节点发一条文本即可填充 |
