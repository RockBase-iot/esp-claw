# WhatsApp 集成方案

## 背景

设备已支持 Telegram / Feishu / QQ / WeChat 四个 IM 通道，全部走 `cap_im_*` 能力组对接 `claw_event_router` → LLM。本方案补齐 WhatsApp。

## 通道选型

WhatsApp 没有像 Telegram 那样的开放 Bot API，可选三条路：

| 方案 | 协议 | 接入门槛 | 设备侧成本 | 备注 |
|------|------|----------|------------|------|
| **A. WhatsApp Cloud API（Meta 官方）** | HTTPS + Webhook | 需 Meta Business、商业号、官方审核 | 中 | 出站 HTTPS POST + 入站需公网回调 |
| B. Twilio / 360dialog 等 BSP 中转 | HTTPS + Webhook | BSP 账号 + 月费 | 中 | 同 A，但回调由 BSP 转发 |
| C. WPPConnect / Baileys 网关（个人号） | WebSocket | 自建网关 + 扫码 | 低 | 违反 WhatsApp ToS，不推荐生产用 |

推荐 **方案 A**：与现有 Telegram 实现最相近，且为长期可维护方案。

## 入站消息（Webhook）

WhatsApp Cloud 要求开发者提供公网 HTTPS Webhook URL，设备直连无法直接做（NAT/无证书）。两种工程化解：

1. **本地反向代理**：在用户 PC 或路由器跑 `cloudflared` / `ngrok`/ `frp` 暴露设备 80 端口（已存在 `http_server`）。需要文档指引用户配置。
2. **云端中继 (推荐)**：在 esp-claw 提供一个 Cloudflare Worker / 阿里云函数计算端点，作为 Meta → 设备的中转：
   - Worker 持久化最近 N 条消息到 KV / Durable Object。
   - 设备每 5 s 用 HTTPS 长轮询 `https://wa-relay.esp-claw.com/poll?token=...`。
   - 与现有 `cap_im_feishu` 的 WS-fallback 模型一致。

## 设计

### `cap_im_whatsapp` 组件

参考 `cap_im_tg` 结构：

```
cap_im_whatsapp/
├── CMakeLists.txt
├── idf_component.yml
├── include/cap_im_whatsapp.h
├── src/cap_im_whatsapp.c       # registration + send
├── src/cap_im_whatsapp_poll.c  # long-poll task (relay mode)
├── src/cmd_cap_im_whatsapp.c   # CLI: send / status
└── skills/                     # LLM-callable tool descriptors
```

### 配置项（`app_config` + NVS namespace `app`）

| Key | 含义 | 默认 |
|-----|------|------|
| `wa_enabled` | 总开关 | `0` |
| `wa_phone_number_id` | Meta 商业号 ID | `""` |
| `wa_access_token` | Meta Bearer token | `""` |
| `wa_relay_url` | 中继轮询 URL | `https://wa-relay.esp-claw.com/poll` |
| `wa_relay_token` | 中继鉴权 token | `""` |
| `wa_default_to` | 默认收件人手机号 | `""` |

接入第 3 项 SD 备份后这些字段也会自动备份。

### 出站 API

`POST https://graph.facebook.com/v20.0/{phone_number_id}/messages`，请求体：

```json
{ "messaging_product": "whatsapp",
  "to": "<E.164>",
  "type": "text",
  "text": {"body": "<msg>"} }
```

复用 `cap_im_tg` 中 `_http_event_handler` 的接收缓冲套路即可。

### 入站轮询任务

```c
static void wa_poll_task(void *arg) {
    while (running) {
        http_get(relay_url, headers={Authorization: Bearer relay_token});
        for each message in resp.messages:
            claw_event_router_post_message(channel="whatsapp", sender, text);
        wait_ms(5000);
    }
}
```

### LLM Skill descriptor

复用 Telegram 的 `send_message` schema，仅平台字段改为 `"whatsapp"`，并在工具说明里注明对方需要为 24h 内有过会话的用户（WhatsApp 业务对话窗口规则）。

## 工作分解

| # | 任务 | 估时 |
|---|------|------|
| 1 | Meta Business 沙盒注册 + 拿到测试 phone_number_id / token | 0.5d（外部依赖） |
| 2 | 中继 Worker：Webhook 接收 + KV 缓冲 + 轮询接口 | 0.5d |
| 3 | `cap_im_whatsapp` 组件骨架，复用 `cap_im_tg` 模板 | 1d |
| 4 | 出站 send / 入站 poll 任务 | 0.5d |
| 5 | `app_config` 配置项 + Web 配置页（已有 IM 配置区追加一段） | 0.5d |
| 6 | Skill descriptor + skills_list.json 注册 | 0.25d |
| 7 | docs/ 双语用户指南（含 Meta 申请步骤截图） | 1d |

合计：约 4 人日（不含 Meta 审核等待）。

## 风险

- Meta 商业号审核周期 1–2 周，需提前发起。
- 业务对话窗口规则（24h 内对方先发起或使用模板消息）会限制设备主动通知场景；模板消息需 Meta 审批。
- 中继 Worker 是单点，需考虑 Cloudflare Worker 免费额度（10 万次/天）。每 5 s 轮询单设备约 17 280 次/天 → 单 Worker 上限约 5 设备，须按 token 分账户或要求用户自部署。
