# LCD 中文显示（CJK 字库）实现方案

## 现状（2026-05）

- LCD 文本渲染走 `esp_painter`，字库为 `components/common/esp_painter/font/basic_font_*.c`，是单字节 ASCII 点阵（仅 0x20–0x7E）。
- `esp_painter_draw_text()` 直接以 `*text` 单字节为索引取 glyph，遇到 UTF-8 多字节字符（中文 3 字节）会按 3 个无效"字符"分别绘制，输出像素垃圾。
- **当前缓解**（已合入）：`app_status_screen.c::ass_draw_text()` 在调用 `esp_painter_draw_string` 前对非 ASCII 字节做折叠——每个 UTF-8 多字节字符替换为 `?`，连续替换合并为一个 `?`，避免乱码堆积。中文消息因此显示为类似 `Hello ? ?` 的占位符。

## 目标

在状态屏（消息列表 / 标题 / 副标题）能正确显示中英文混排，并尽量低成本扩展到 LVGL 之外的现有路径。

## 设计方案

### 字库选择

| 方案 | Glyph 数 | 大小 (16×16) | 备注 |
|------|---------|--------------|------|
| GB2312 一级字库（3755 字）+ 二级（3008 字） | 6763 | ~217 KB | 覆盖日常中文 99%+；推荐 |
| Unicode CJK 基本区 (4E00–9FFF) | 20992 | ~672 KB | 体积大，需 SD 存储 |
| 自定义子集（按词频取 1500 字） | 1500 | ~50 KB | 体积友好但有缺字 |

推荐起步用 **GB2312 一级 (3755 字)**，单 size 约 121 KB，可存 flash 也可存 SD。多 size（16/20/24）共占 ~440 KB。

### 存储位置

- 16 MB Flash 板：放在 `storage` FATFS 分区，路径 `/fatfs/fonts/wqy_16.bin` 等。允许 Web 后台更换字库。
- SD 卡板：优先 `/sdcard/fonts/`，缺失时回落到 `/fatfs/fonts/`。

### 字库二进制格式（自定义，简单首选）

```
struct font_header {
    char     magic[4];      // "CJKF"
    uint32_t glyph_count;
    uint16_t glyph_w;
    uint16_t glyph_h;
    uint32_t index_offset;  // sorted uint32_t codepoint table
    uint32_t bitmap_offset; // glyph_count × ceil(w*h/8) bytes
};
```

查表：UTF-8 → codepoint → 在 `index[]` 上二分查找 → 命中后取 `bitmap_offset + idx * stride`。

### `esp_painter` 改造点

1. 抽象 `esp_painter_font_provider_t`：保留现有 ASCII basic_font 作为内置 provider；新增 `cjk_file_provider`，懒加载文件、维护 LRU glyph cache（建议 256 entry × 64 byte = 16 KB SPRAM）。
2. `esp_painter_draw_text()` 改为按 UTF-8 解码逐 codepoint 绘制；ASCII 走原 basic_font，非 ASCII 走 cjk provider。
3. `esp_painter_measure_text()` 同步按 codepoint 计算宽度（CJK 字符宽度通常等于 ASCII 宽度的 2 倍）。
4. 字符未命中时仍走 `?` 占位，避免 fopen 失败放大成渲染失败。

### Wi-Fi/HTTP 后台支持

- 新增 `/api/font` 端点：列出 `/fatfs/fonts/`、上传/删除字库文件、设为默认。
- `app_status_screen` 启动时通过 `app_config` 读取 `font_cjk_path`，传给 `esp_painter`。

## 工作分解

| # | 任务 | 估时 | 输出 |
|---|------|------|------|
| 1 | 准备 GB2312 字库脚本：python 用 PIL 渲染 → 自定义二进制 | 0.5d | `tools/font_builder/build_cjk_font.py` |
| 2 | `esp_painter` 加 UTF-8 解码 + provider 抽象 | 1d | `esp_painter_font_provider.h/c`，向后兼容 |
| 3 | `cjk_file_provider`（mmap-style 读取 + LRU glyph cache） | 1d | `esp_painter/font/cjk_file_provider.c` |
| 4 | `app_status_screen` 接入 + 移除 ASCII fold 临时缓解 | 0.5d | 状态屏中文显示 |
| 5 | HTTP `/api/font` 端点 + Web UI 上传 | 0.5d | 后台可在线更换字库 |
| 6 | docs/ 双语说明 + 默认字库随固件烧录到 storage 分区 | 0.5d | 用户文档 + flash 流程 |

合计：约 4 人日。

## 风险

- **Flash 占用**：GB2312 16/20/24 三 size 共 ~440 KB，需校核 storage 分区剩余空间；如不够，先只内置 16 size。
- **SD 性能**：每次 cache miss 需读 SD，单次 ~64 byte 读取在 4 MHz SPI 下 < 1 ms，但要走 SPI 仲裁锁。已通过 `spi_bus_arbiter` 解决。
- **缺字**：GB2312 不含 emoji 与生僻字；继续走 `?` 占位。
