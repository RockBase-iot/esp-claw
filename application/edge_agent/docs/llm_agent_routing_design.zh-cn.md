# edge_agent 上的 IM → LLM Agent 路由设计

> 文档版本：v1（2026-05-01）
> 适用：`application/edge_agent`，目标平台 ESP32-C5（240 MHz RISC-V，4 MB PSRAM，16 MB flash）

## 1. 结论先行

**用户最初担心 "edge_agent 没有把非硬件 IM 任务转给 LLM 处理"，这一点在阅读源码后被推翻：edge_agent 已经具备 openclaw 风格的 LLM 路由能力，且复用的是 esp-claw 的标准框架。** 关键证据：

- [application/edge_agent/main/main.c](application/edge_agent/main/main.c#L516-L517) 在 `app_main` 末尾调用 `app_claw_start(s_claw_config, s_claw_paths)`。
- 该函数实现位于共享组件 [components/common/app_claw/app_claw.c](components/common/app_claw/app_claw.c#L199)，与 `basic_demo` 使用同一套实现。
- 该实现把 `claw_event_router_config_t.default_route_messages_to_agent` 设为 `llm_enabled`（[app_claw.c#L222](components/common/app_claw/app_claw.c#L222)），并在 [claw_event_router.c#L2008-L2024](components/claw_modules/claw_event_router/src/claw_event_router.c#L2008-L2024) 实现"未命中规则的 message 类事件 → `claw_event_router_run_default_agent` → `claw_core` → LLM"的兜底路径。
- 所有硬件能力以 `cap_lua` + `lua_modules/*`（gpio/display/audio/button/led_strip/...）的形式注册到 `claw_cap` 总线，LLM 通过 tool-calling 主动调用；纯对话任务则不会触发任何 tool，只产生文本回复。

也就是说：**"硬件相关 / 非硬件相关" 在框架层并没有提前分流，两者走同一条管线**。LLM 自己根据 tool/skill 目录做出决定——能用工具就用，用不上就直接回。

---

## 2. 当前 IM 消息处理流程（edge_agent 现状）

```text
┌──────────────────────────────────────────────────────────────────────┐
│                         IM 适配层（按通道）                            │
│  cap_im_wechat / cap_im_local(Web) / cap_im_qq / cap_im_tg / feishu  │
└──────────────────────────────┬───────────────────────────────────────┘
                               │ inbound 推送
                               ▼
                ┌────────────────────────────┐
                │  claw_event_router         │
                │  (规则引擎 + 默认路由)       │
                └──────────────┬─────────────┘
        ┌──────────────────────┼──────────────────────┐
        ▼ 命中规则             ▼ 未命中且为 message    ▼ 系统/触发事件
   action 列表                run_default_agent          publish_trigger
   (run_lua / call_cap        (走 LLM Agent 默认路径)
    / send_message / ...)
                              │
                              ▼
                ┌────────────────────────────┐
                │  claw_core (Agent Loop)    │
                │  - 拼装 system prompt       │
                │  - 注入 context providers   │
                │  - 调 LLM /chat/completions │
                │  - 解析 tool_calls 循环     │
                └──────────────┬─────────────┘
                               │
                ┌──────────────┴────────────────────────────┐
                ▼                                            ▼
   claw_cap 工具总线                              outbound 绑定 + observer
   - cap_lua → lua_modules                       - "wechat" → wechat_send_message
     (硬件: gpio/display/audio/...)              - "web" → local_send_message
   - cap_files / cap_web_search / cap_time       - "qq/tg/feishu" → 同上
   - cap_mcp_client / cap_mcp_server             - app_message_inbox 同步落屏
   - cap_skill_mgr (activate_skill)
   - claw_memory (FULL 模式开启)
```

### 2.1 当前默认路由判定（一句话总结）

> [`claw_event_router_process_event`](components/claw_modules/claw_event_router/src/claw_event_router.c#L2008-L2024) 中：**当事件未命中任何用户规则、且 `event_type == "message"`、且文本非空、且 LLM 已配置时**，调用 `claw_event_router_run_default_agent` 把消息塞给 `claw_core`（LLM Agent）。

### 2.2 硬件 vs 非硬件并不"分流"

`lua_modules/lua_module_*` 通过 `cap_lua_register_module` 注册到 `cap_lua` 能力组，再通过 `claw_cap` 暴露为 LLM tool。Agent 侧的视角是统一的"工具集合"。LLM 在生成回复时：

- 需要点灯、读传感器、调显示 → 调用对应 lua tool（间接触达硬件）。
- 想自己回 "今天天气怎么样" → 调用 `cap_web_search`，或直接生成文本。
- 纯闲聊 → 不发起 tool_call，仅返回 message。

所以"非硬件任务怎么处理" = "LLM 自己决定不调用任何硬件类 tool，最后产生文本回复并通过 outbound binding 发回 IM 频道"。这就是 openclaw 的设计哲学，esp-claw 已经完整复刻。

### 2.3 edge_agent 与 basic_demo 的差异

| 维度 | basic_demo | edge_agent |
|---|---|---|
| 启动入口 | 私有 `main/app_claw.c`（早期实现） | 复用 `components/common/app_claw/`（统一实现） |
| 能力裁剪 | 硬编码全开 | 通过 `CONFIG_APP_CLAW_CAP_*` Kconfig 选择性开关 |
| Memory mode | `FULL` / `LIGHTWEIGHT` 二选一 | 同上（当前 sdkconfig 默认 `FULL`） |
| 屏幕表达 | emote | emote + 状态屏 + 触摸手势 + 收件箱 UI |
| Web IM | 可选 | 通过 `cap_im_local` + `http_server_webim_bind_im` 内置 |
| 路由是否走 LLM | 是（同一 `app_claw_start`） | **是（同一 `app_claw_start`）** |

---

## 3. 可行性结论

> **可行，且当前代码即支持。** `edge_agent` 在 LLM 配置完整（`llm_api_key` + `llm_profile` + `llm_model` 三者非空）后，IM 消息会自动经过 LLM Agent 处理。

需要确认的"用户认知偏差点"：

1. 如果用户觉得"消息没有进 LLM"，第一排查点是 `app_config` 中 LLM 三件套是否齐全，否则 `llm_enabled = false`，`default_route_messages_to_agent` 也会被强制为 `false`，框架会直接落入"没有规则就丢弃"的状态。
2. 如果用户在 `router_rules.json` 里写了 `consume_on_match = true` 的规则吃掉了消息，也不会走默认 LLM 路径。

---

## 4. ESP32-C5 (240 MHz + 4 MB PSRAM) 上的关键限制

除"算力 / RAM"以外，逐项列出实际工程瓶颈：

| 类别 | 限制 | 影响 / 量级 | 缓解 |
|---|---|---|---|
| **TLS 内存** | mbedTLS 单连接握手峰值 30–50 KB，证书包另算 | 同时只跑得动 1–2 路 HTTPS（LLM + IM），第三路就开始 OOM | `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y` 已开（见 `sdkconfig.defaults`）；继续按需启用证书 bundle 子集 |
| **JSON 解析** | LLM 响应 cJSON 解析常驻 4–10 KB，长 tool_call 参数会翻倍 | 对话越长，解析峰值越大 | 限制 `max_message_chars`、限制 `max_tool_iterations`、流式解析尚未实现 |
| **Flash 分区** | 8 MB 默认分区下 app slot 仅 3 MB；FATFS 1.5 MB | skill / lua 脚本 / 历史消息共享 1.5 MB，长程对话历史会被滚动覆盖 | 升级 16 MB（项目已支持 `partitions_16MB.csv`），再切到 OTA 双槽 |
| **网络带宽 / 延迟** | Wi-Fi RTT 50–200 ms，Qwen / DeepSeek 首 token ~ 500–2000 ms | 一次 tool-call 循环（5 轮）端到端 8–25 s | system prompt 精简、`max_tool_iterations` 调小（当前 32，可降到 8–12）、关闭 stream 之外的额外往返 |
| **mbedTLS RX 缓冲** | 默认 `MBEDTLS_SSL_IN_CONTENT_LEN` 16384，每路占 16 KB internal | 多路并发立即吃光 internal heap | 短时关闭 IM 长连接，或把 IM 推送切到 webhook 模式 |
| **LLM 上下文成本** | 一次注入：profile 摘要 + memory 索引 + skill 列表 + 工具 schema + 历史 ≈ 2–6 KB | 4096 字符 message cap 不是真正瓶颈，**工具 schema 才是**；当前注册 ~15 个 group 时 schema 已 ~3–4 KB | `claw_cap_set_llm_visible_groups` 严格挑选；非 IM 任务期间动态隐藏 IM 工具 |
| **任务栈** | claw_core 16 KB，event_router 8 KB，scheduler 6 KB，cap_lua 自带 | C5 单核内 ~140 KB internal SRAM 用于栈池本身 | 已用 PSRAM 做 task stack（`FREERTOS_TASK_CREATE_ALLOW_EXT_MEM=y`）|
| **存储 IO 抖动** | SD 卡共享 SPI2 + LCD/触摸；FATFS 写入消息 JSONL 时容易抢锁 | 屏幕渲染掉帧；最坏情况读消息超时 | 已通过 `display_arbiter` 仲裁；持久化做了滚动文件（256 KB cap）|
| **本地推理** | 4 MB PSRAM 跑不动任何 ≥ 1B 参数模型；甚至 KWS 之外的 INT8 量化 NN 也只剩百 KB 头寸 | 不可能本地跑 LLM；"边缘 = 接入端 + tool runtime"，模型必须远端 | 维持云端 LLM 路线；本地仅做规则匹配 + KWS / VAD |
| **多模态** | 4 MB PSRAM 上 base64 编码 1 张 320×240 JPEG ≈ 30–60 KB，OK；但解码 + 重采样 + 上传同时发生时压力大 | 单图 OK，连续多图会触发 OOM | 上传前限大小，关闭同时进行的其他 HTTPS |
| **并发会话** | `claw_core` `request_queue_len = 4` / `response_queue_len = 4` | 同时 ≥ 5 路 IM 消息会丢或阻塞 | 单设备单用户场景下不是问题 |
| **OTA / 升级** | 8 MB flash 没有第二个 ota slot；16 MB 下 app=4 MB ota_0 + 4 MB ota_1，FATFS 4 MB | 8 MB 板子升级必须靠 serial / web flasher | 推荐 16 MB 板子做生产部署 |

---

## 5. 在 240 MHz + 4 MB PSRAM 约束下能做到的能力上限

按"能做到 / 勉强能做到 / 做不到"三档归类：

### 5.1 能做到（已实现，工程上稳定）

- **多 IM 通道接入**：QQ / Telegram / 飞书 / 微信 / 本地 Web IM。
- **LLM 工具调用 Agent 循环**：单轮端到端 5–15 s（视模型与网络），最多 8–12 轮 tool 迭代是稳定区间。
- **结构化记忆**（FULL 模式）：profile / preference / fact / event / rule 五类，自动从对话中抽取，支持 `memory_recall`。
- **本地技能（skill）+ Lua 脚本**：FATFS 上 50–80 个小脚本（每个 < 10 KB），按需 `activate_skill` 注入到上下文。
- **MCP 双角色**：作为 server 暴露硬件 tool；作为 client 调远端 MCP service。
- **事件路由规则**：`router_rules.json` 提前过滤 / 转发 / 直回，不必都进 LLM（省 token）。
- **定时任务 + 事件触发** Agent：温度阈值、时刻、IM 消息均可触发。
- **图像理解**：单张小图（< 100 KB base64）传给视觉模型 OK。

### 5.2 勉强能做到（要做工程裁剪）

- **同时 2 路并发 LLM 会话**：建议关闭其他 HTTPS（OTA、文件上传），并将 mbedTLS RX 缓冲降到 8 KB。
- **本地 KWS / VAD 唤醒**：可接 esp-sr，但要和 emote 屏幕渲染争 PSRAM；建议挂在 FULL memory 关闭时跑。
- **向量检索 RAG**：只能存 ~200–500 条短句的 384 维 INT8 向量；嵌入模型必须云端；适合做 FAQ 而不是文档级 RAG。
- **超长对话**：靠 `claw_memory` 摘要后再注入（"summary 标签而非 body"），对应 `MEMORY_MODE_FULL` 的 stage_note 异步抽取流程已实现。
- **音频实时对话**：可以跑 16 kHz PCM 上传 + TTS 拉流播放，但要单独串行 IM 链路。

### 5.3 做不到（4 MB PSRAM + 单核 RISC-V 的硬限）

- **本地任何 ≥ 1B 参数 LLM 推理**：直接放弃。
- **本地多模态视觉模型**：连 INT8 MobileNet-class 都很挤，更别说 VLM。
- **同时 4 路 HTTPS（LLM + IM + OTA + 推流）**：必然 OOM。
- **流式 ASR + 流式 LLM + 流式 TTS 全链路**：当前 esp-tls 还做不到三路并行 stream。

### 5.4 一句话能力定位

> **edge_agent on ESP32-C5 = "云端 LLM 大脑 + 本地 tool/lua/MCP 手脚 + 本地结构化记忆 + 多 IM 入口 + 触摸 / emote UI"，是一个面向单用户的、具备完整 tool-calling Agent 循环的边缘代理设备**。它不是"本地推理"，而是"本地决策接入 + 工具沙箱 + 个性化数据闭环"。

---

## 6. 实施路径（已实现 vs 待优化）

注意：**P0 阶段并不需要"新增功能"，只需"打开配置 + 调通"**；真正的工程性工作集中在 P2–P4 的优化与可观测性。

### P0：开箱可用（已完成，仅需正确配置）

- [x] 在 `idf.py menuconfig` 中填入 LLM 三件套（`api_key` / `profile` / `model`）。
- [x] 配置 IM 通道凭证（任选一种：微信 token+base_url / TG bot token / 飞书 app_id+secret / 本地 Web IM）。
- [x] 烧录后 IM 发消息 → `claw_event_router` 默认路由 → `claw_core` LLM 处理 → outbound 回 IM。
- [x] 验证：未命中规则的纯文本会经 LLM；硬件请求（"开灯"等）会触发 `cap_lua` 调 `lua_module_gpio` / `led_strip`。

**难度：低**。**周期：半天**（含烧录、调通 LLM 凭证、验证 IM 回环）。

### P1：能力可控（已实现，开发者按场景裁剪）

- [x] `claw_cap_set_llm_visible_groups` 严选 LLM 可见 group，省 token。
- [x] `router_rules.json` 写硬规则吃掉高频指令（如"状态"、"重启"），不进 LLM。
- [x] `MEMORY_MODE_LIGHTWEIGHT` 切换：当 4 MB PSRAM 紧张或不需要长程记忆时关 FULL。
- [ ] 提供脚本化的 token / 上下文用量统计 dashboard（当前只有 `cap_llm_inspect` 群组，需要把数据落 FATFS）。

**难度：低-中**。**周期：1–2 天**（主要是 dashboard）。

### P2：稳定性与观测（部分已实现，建议补齐）

- [x] mbedTLS 外部内存分配已开（`MBEDTLS_EXTERNAL_MEM_ALLOC=y`）。
- [x] FreeRTOS 任务可挂 PSRAM 栈。
- [ ] **TLS 缓冲池化**：把 `MBEDTLS_SSL_IN/OUT_CONTENT_LEN` 从 16 KB 降到 8 KB，验证 LLM 长响应是否被截断。
- [ ] **request 限流**：`request_queue_len` 4 已经够小，但加一个"忙时直接回 'busy, please retry'"的 fallback rule。
- [ ] **OOM watchdog**：内存 monitor 任务（`APP_ENABLE_MEM_LOG`）从条件编译改 Kconfig，并把 PSRAM/internal min-free 上报到 IM。
- [ ] **claw_core 阶段日志**：`CLAW_CORE_STAGE_VERBOSITY_VERBOSE` 已默认开，但需要把 stage 时间序列化到 FATFS 以便排查 latency。

**难度：中**。**周期：3–5 天**。

### P3：能力增强（未实现，按价值排序）

| 增强项 | 价值 | 难度 | 周期 |
|---|---|---|---|
| **本地 KWS 唤醒触发 Agent**（接 esp-sr，触发时把 IM message=语音转写文本投入 router）| 高 | 中-高 | 1–2 周 |
| **流式 LLM 响应**（`claw_core` 增量解析 + 增量 IM 回包）| 中 | 高（要改 cJSON 流式状态机）| 2–3 周 |
| **本地嵌入式 RAG**（最近 200 条 FAQ + INT8 384 维向量）| 中 | 中 | 1 周 |
| **多设备协同**（MCP server 互联，事件总线跨设备）| 中 | 高（需要 mDNS + 鉴权 + 心跳）| 2–3 周 |
| **离线降级**（断网时本地规则 + cache 应答）| 中 | 低 | 3–5 天 |
| **图像采集 → 视觉 LLM 推理**（按键拍照 → cap_im 上传带图）| 中 | 低-中（已有 attachment 通路）| 3–5 天 |
| **持久 vector store on SD**（替换 FATFS 容量瓶颈）| 低-中 | 中（依赖 SD 兼容性，目前还在调）| 1 周 |

### P4：生态与扩展（长期方向）

- 把 `cap_mcp_server` 的 schema 自动从 `claw_cap` 反射，零样板暴露能力。
- skill marketplace（本地导入 / OTA 拉取 skill 包）。
- 远端 admin panel：通过 web IM 频道远程调 router_rules、scheduler、memory。

---

## 7. 难度与周期总览

| 阶段 | 工作内容 | 工程难度 | 估计周期 |
|---|---|---|---|
| P0 | 验证现有 LLM 路由跑通 | ★ | 0.5 天 |
| P1 | Token / 上下文裁剪、规则吃指令、dashboard | ★★ | 1–2 天 |
| P2 | TLS / 内存 / 限流 / 阶段日志稳定性 | ★★★ | 3–5 天 |
| P3-A | KWS 唤醒 + 流式响应（最有价值组合） | ★★★★ | 3–5 周 |
| P3-B | RAG / 多设备 / 离线降级（按需） | ★★★ | 各 1–2 周 |
| P4 | MCP schema 自动化、skill marketplace | ★★★★ | 持续投入 |

**最小可用 → 量产可用 关键路径**：P0（半天） + P2（1 周）= **约 1 周** 即可让 ESP32-C5 板子稳定地把 IM 消息交给云端 LLM 处理，硬件类工具 LLM 自调，非硬件类纯文本回复直回 IM。

---

## 8. 关键文件索引

| 路径 | 角色 |
|---|---|
| [application/edge_agent/main/main.c](application/edge_agent/main/main.c) | 启动顺序：UI / 收件箱 / WiFi / HTTP / `app_claw_start` |
| [components/common/app_claw/app_claw.c](components/common/app_claw/app_claw.c) | 共享 Agent 启动器（router + memory + skill + cap + core） |
| [components/common/app_claw/app_capabilities.c](components/common/app_claw/app_capabilities.c) | 能力组按 Kconfig 注册到 `claw_cap` |
| [components/common/app_claw/Kconfig](components/common/app_claw/Kconfig) | `APP_CLAW_CAP_*` / `APP_CLAW_MEMORY_MODE_*` 开关 |
| [components/claw_modules/claw_event_router/src/claw_event_router.c](components/claw_modules/claw_event_router/src/claw_event_router.c) | 规则匹配 + `default_route_messages_to_agent` 兜底 |
| [components/claw_modules/claw_core](components/claw_modules/claw_core) | LLM Agent 循环、context provider 注册 |
| [components/claw_modules/claw_memory](components/claw_modules/claw_memory) | 五类结构化记忆 + 异步抽取 |
| [components/claw_modules/claw_skill](components/claw_modules/claw_skill) | skill 加载 + activate / deactivate |
| [components/claw_capabilities/cap_lua](components/claw_capabilities/cap_lua) | LLM ↔ Lua 桥；硬件能力的统一入口 |
| [components/lua_modules/](components/lua_modules) | gpio / display / audio / button / led_strip / camera / ... |
| [application/edge_agent/main/app_message_inbox.c](application/edge_agent/main/app_message_inbox.c) | 屏幕侧消息环形缓冲 + JSONL 持久化（与 LLM 路径并行的 observer） |
| [application/edge_agent/sdkconfig.defaults](application/edge_agent/sdkconfig.defaults) | PSRAM / mbedTLS / FATFS / FreeRTOS 关键配置 |

---

## 9. 一句话总结给 PM 的版本

> **不需要新做"IM → LLM"功能**。esp-claw 框架已经把 openclaw 的 Agent Loop / Memory / Skill / MCP / 多 IM 通道全做完，edge_agent 通过共享 `app_claw` 组件直接复用。在 ESP32-C5（240 MHz + 4 MB PSRAM + 16 MB flash）上能稳定跑"云端 LLM 大脑 + 本地 tool/lua/MCP 手脚"的单用户边缘 Agent；本地推理不可能，多路并发 HTTPS 受限，其他都在工程优化范围内。落地最小 1 周，做透 4–6 周。
