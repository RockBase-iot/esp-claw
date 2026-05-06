# Token / Context 用量统计 Dashboard 方案

## 现状

- LLM 客户端 `components/claw_modules/claw_core/src/llm/claw_llm_runtime.c` 当前 **不解析** OpenAI 兼容响应里的 `usage.{prompt_tokens, completion_tokens, total_tokens}` 字段，也未估算上下文长度。
- `cap_llm_inspect` 仅做单次图像 inspect，不归集统计。
- 既有 `app_status_screen` 状态屏可显示文字行；`http_server` 已经在 80 端口暴露 REST API；Web UI 已有 `/api/inbox` 等端点的 JS 渲染。
- 配置项现已可备份到 SD 卡（见同目录下另一份 plan）。

## 目标

让用户/开发者能直观看到：

1. 单次对话 / 工具调用 链上的 prompt / completion / total token 数。
2. 当前会话累计；近 24 h 与近 7 d 的滑窗汇总；按模型 profile / 按 IM 通道 / 按工具名分组。
3. 与所选模型 `context_window` 的占比，便于及时触发 Session 压缩。
4. 简单成本估算（按用户配置的 USD/1K token 单价）。
5. 可在 Web 后台、设备 LCD、串口 CLI 三处呈现。

## 设计

### 数据采集

在 `claw_llm_runtime` 接到 LLM 响应 JSON 后，新增辅助函数：

```c
typedef struct {
    char     model[48];
    char     profile[32];     // openai / qwen_compatible / ...
    char     channel[16];     // im_tg / im_feishu / cli / ...
    uint32_t prompt_tokens;
    uint32_t completion_tokens;
    uint32_t total_tokens;
    int64_t  ts_ms;
    uint32_t latency_ms;
    int32_t  http_status;
    bool     ok;
} claw_llm_usage_event_t;

void claw_llm_usage_emit(const claw_llm_usage_event_t *e);
```

调用点：在 `_perform_request()` 完成 + 解析响应后，从响应 JSON `usage` 节点取值，计算耗时，发出事件。

对于不返回 `usage` 的 provider（如部分自建），fallback 估算：`prompt_tokens ≈ utf8_byte_count / 4`（保留 `estimated:true` 标记）。

### 存储层 `cap_llm_usage`

新增能力组件 `components/claw_capabilities/cap_llm_usage/`：

```
cap_llm_usage/
├── include/cap_llm_usage.h
├── src/cap_llm_usage.c          // 注册 + 内存 ring
├── src/cap_llm_usage_persist.c  // append-only JSONL 到 SD/FATFS
├── src/cap_llm_usage_query.c    // 聚合查询 (1h / 24h / 7d / total)
└── skills/
```

- **内存层**：保留最近 256 条事件（约 30 KB）；
- **持久化**：每事件一行 JSONL 写到 `<storage>/llm_usage/usage-YYYYMMDD.jsonl`，按天滚动，保留 7 天；
- **聚合**：启动时扫描当天文件填充滑窗。每条事件到来时增量更新 in-memory 计数器（按 profile / channel / 工具）；
- **HTTP/CLI 接口**：
  - `GET /api/llm_usage/summary?window=1h|24h|7d|all` → `{prompt, completion, total, count, by_profile, by_channel, est_cost_usd}`
  - `GET /api/llm_usage/recent?n=20` → 最近 N 条事件
  - CLI: `llm_usage` 打印同上汇总。

### 上下文占比

`claw_llm_runtime` 已知 `model` 与 `max_completion_tokens`；扩展配置：

| key | 含义 |
|-----|------|
| `llm_context_window` | 当前模型上下文上限（如 128000） |
| `llm_cost_in_usd_per_1k` | 输入单价 |
| `llm_cost_out_usd_per_1k` | 输出单价 |

Summary 输出 `context_used_pct = max(prompt_tokens) / llm_context_window`。

### 前端 Dashboard

在 Web UI 加 `dashboard.html`：

- 顶部 4 张卡片：今日 token、累计 token、估算成本、context 峰值占比。
- 折线图（最近 24 h 每小时分桶）：prompt vs completion。
- 饼图：按 channel / 按 profile。
- 表格：最近 20 条调用。
- 用 Chart.js（单文件 ~100 KB）即可；文件放 FATFS `static/`。

### LCD 状态屏

新增一页 `APP_STATUS_PAGE_USAGE`（按现有 `messages` 页样式）：
```
LLM 用量
今日:  12,345 tk  ($0.024)
累计:  98,765 tk
ctx :  18% (max 23%)
```

### CLI

```text
> llm_usage
window  prompt   compl   total    count  est_cost
1h        450    230     680        3    $0.0014
24h     8,210  3,440  11,650       42    $0.0235
7d     53,120 21,300  74,420      301    $0.1490
total 102,400 41,800 144,200      612    $0.2880
top channel: im_tg (38%), im_feishu (29%), cli (33%)
top profile: openai (61%), qwen_compatible (39%)
```

## 工作分解

| # | 任务 | 估时 |
|---|------|------|
| 1 | `claw_llm_runtime` 解析 `usage`，新增 `claw_llm_usage_emit()` 钩子 | 0.5d |
| 2 | `cap_llm_usage` 组件骨架 + ring buffer + JSONL 持久化（含 SPI 总线锁） | 1d |
| 3 | 聚合查询（窗口分桶 + by_xxx 分组） | 0.5d |
| 4 | HTTP `/api/llm_usage/*` 端点 | 0.5d |
| 5 | Web Dashboard 页面（Chart.js 卡片 + 折线 + 饼图 + 表格） | 1.5d |
| 6 | LCD `APP_STATUS_PAGE_USAGE` 页 | 0.5d |
| 7 | CLI `llm_usage` 命令 | 0.25d |
| 8 | docs/ 双语用户指南 | 0.5d |

合计：约 5 人日。

## 风险

- 部分 provider（自建 vLLM / Ollama 等）不在响应里给 `usage` → 统计偏差；用 byte/4 估算 + UI 标注 `estimated`。
- JSONL 持久化每对话写一次 SD（含 SPI 锁开销），单写约 1–3 ms，可接受。如担心 SD 寿命，可加批量缓冲（默认 30 s flush）。
- 估价依赖用户填入的单价；模型升级或服务商调价时需要手动更新；接受不做线上拉取价格表。
- Web 端图表加大固件镜像（Chart.js ~100 KB），需校核 FATFS 余量；可改用纯 SVG 自绘以省体积。
