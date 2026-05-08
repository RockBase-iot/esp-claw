# Changelog

## [2026-05-07]

合并上游 `espressif/master`（`582022f`），适配官方重大架构重构，更新 NM-CYD-C5 技能与构建配置以支持最新框架。

> 上游涉及 IM 平台合并、Skill/Lua 构建系统标准化、Session 索引加固、HTTP 复用层引入等 12 项核心变更，详见下方分类记录。

---

### Added

- **IM 统一平台 `cap_im_platform`**
  - 将原先分散的 5 个独立 IM 组件（`cap_im_attachment`、`cap_im_feishu`、`cap_im_qq`、`cap_im_tg`、`cap_im_wechat`）合并为单一组件。
  - 通过 `CMakeLists.txt` 中的 `if(CONFIG_APP_CLAW_CAP_IM_xxx)` 条件编译控制各子模块启停。
  - 新增统一入口 `cap_im_platform.c`，公共头文件全部收敛到 `cap_im_platform/include/`。
  - 下游影响：`idf_component.yml` 中所有旧 IM 依赖路径需替换为 `cap_im_platform`。
  - 关联提交：`714747e`

- **Skill / Lua 标准化构建系统**
  - 新增 `skill_builder` + `lua_module_builder` 组件。
  - `skill_builder`：提供 `skill_sync.cmake` + `sync_component_skills.py`，构建时自动扫描各组件 `skills/` 目录并同步到 FATFS。
  - `lua_module_builder`：提供 `lua_sync.cmake` + Python 工具链，自动同步 Lua 模块的 `test/` 与 `lib/` 目录。
  - 关联提交：`fa2db28`

- **HTTP 客户端复用层 `http_reuse`**
  - 新增 `components/common/http_reuse/` 组件。
  - 封装 `esp_http_client_*` API，提供连接池复用机制，减少重复 TCP/TLS 握手开销。
  - 配套 `Kconfig.projbuild` 支持配置连接池大小与超时。
  - 关联提交：`8afce25`

- **Session 索引与持久化加固**
  - 新增 base64 编码的 session history header，支持快速定位会话边界。
  - session history 文件损坏或格式无效时，自动重建索引而非直接失败。
  - 关联提交：`429712a`、`e799d29`、`eaf9eff`

- **大文件截断读取保护（`cap_files`）**
  - 新增 `CAP_FILES_TRUNCATION_SUFFIX_RESERVE (96 bytes)`。
  - 当文件超过 `CAP_FILES_MAX_FILE_SIZE`（32 KiB）时，不再直接拒绝，而是截断读取并保留尾部提示空间。
  - 所有 I/O 错误路径（`fopen`/`opendir`/`mkdir`/`stat`/`fread`/`fwrite`）补充 `errno` 日志，便于调试 SD 卡或 FATFS 异常。

- **Skill 示例 `light_switch`**
  - 位于 `edge_agent/main/skills/light_switch/`，包含 GPIO 开关与灯带控制示例脚本。

- **UART / JTAG 控制台输出配置**
  - 新增 `sdkconfig.console.jtag` 模板文件。
  - CI 支持根据构建参数选择 UART 或 JTAG 控制台输出。
  - 关联提交：`53f2585`

---

### Changed

- **Skill / Lua 目录结构重构**
  - 旧格式（`skills/cap_xxx.md` + `skills_list.json`、`lua_scripts/basic_xxx.lua`）已废弃。
  - 新格式：
    ```text
    skills/
    └── skill_name/
        ├── SKILL.md          # 统一入口文档
        └── scripts/
            └── script.lua    # 附属脚本
    ```
  - `edge_agent` 具体迁移：
    - `edge_agent/main/lua_scripts/` → `edge_agent/main/skills/lua_demo/scripts/`
    - `edge_agent/main/skills/weather.md` → `edge_agent/main/skills/weather_search/SKILL.md`
  - 约 20+ 个 `skills_list.json` 被批量删除，所有 `.md` skill 文件重命名为 `SKILL.md` 并移入子目录。
  - 关联提交：`fa2db28`

- **`cap_boards`：从 YAML 元数据自动生成 Skill**
  - `generate_board_skill.py` 大幅改写（`-244` / `+165` 行）。
  - 从 `board_info.yaml` / `board_devices.yaml` 的元数据直接生成 `SKILL.md`，不再依赖手写的 `skills_list.json`。
  - 关联提交：`c64fc06`

- **Feishu：图片存储 MIME 感知**
  - 接收到的图片附件不再使用固定扩展名，而是根据 HTTP `Content-Type` 自动推导（`.jpg`、`.png`、`.gif` 等）。
  - 避免不同 MIME 类型的图片覆盖同一文件。
  - 关联提交：`27f29b9`

- **LLM System Prompt 精简**
  - `claw_core` 的 LLM 系统提示词裁剪，减少 token 消耗，提升上下文利用率。
  - 关联提交：`ef3486b`

- **USB-UAC 音频配置更新**
  - `esp32_S3_DevKitC_1_breadboard` 的 UAC 编解码器配置更新。
  - `sdkconfig.defaults.board` 新增 UAC 相关默认配置。
  - 关联提交：`430edc9`、`3df147b`

- **控制台输出适配**
  - `lilygo_t_display_s3` 等 board 的 `setup_device.c` 同步适配 UART/JTAG 配置。

---

### Removed

- **已删除的独立 IM 组件**

  | 旧组件 | 新归属 |
  | :--- | :--- |
  | `cap_im_attachment` | `cap_im_platform/src/cap_im_attachment.c` |
  | `cap_im_feishu` | `cap_im_platform/src/cap_im_feishu.c` |
  | `cap_im_qq` | `cap_im_platform/src/cap_im_qq.c` |
  | `cap_im_tg` | `cap_im_platform/src/cap_im_tg.c` |
  | `cap_im_wechat` | `cap_im_platform/src/cap_im_wechat.c` |

- **废弃的 Lua 模块 `lua_module_dht`**
  - 功能合并入 `lua_module_environmental_sensor`。
  - 删除独立的 `lua_module_dht/CMakeLists.txt`、`idf_component.yml` 和源码。
  - 关联提交：`19210ef`

- **冗余构建脚本与 CMake 模块**
  - 删除 `tools/bmgr_patch.py`、`skills_sync.cmake`、`board_manager_patch.cmake`、`sync_component_lua_scripts.py`、`sync_component_skills.py` 等 4+ 个冗余文件。
  - `edge_agent/main/CMakeLists.txt` 移除 `REQUIRES ${main_requires}` 显式列表，依赖关系完全迁移到 `idf_component.yml` 声明式管理。
  - 关联提交：`1b8b52b`

---

### Deprecated

- `cap_files` 仍然不支持多挂载点访问（如 `/sdcard` 独立挂载点）。LLM 通过 `cap_files` 能力无法直接操作 SD 卡内容，仅能通过 Web 文件管理器访问。

---

### Migration Notes

1. **依赖更新**：若自定义组件仍引用旧 IM 组件，请将 `idf_component.yml` 中的路径统一替换为 `cap_im_platform`。
2. **Skill 开发**：新 Skill 请使用 `SKILL.md` + `scripts/` 目录结构，不再手写 `skills_list.json`。
3. **Lua 脚本**：Lua 模块的示例脚本请放入组件的 `test/` 目录，公共库放入 `lib/` 目录，由 `lua_module_builder` 自动同步。

### Check List

 - [x] skills/skill_creator
 - [x] skills/nm_cyd_c5_*
 - [x] skills/nmminer
 - [x] app_sd_mirror.c/h, SD card backup/restore function.
 - [ ] running statble test.