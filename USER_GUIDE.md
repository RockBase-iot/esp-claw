# ESP-Claw 用户入门指南

> 本文档面向首次使用 ESP-Claw 的用户，涵盖从环境搭建到设备交互的完整流程。

---

## 目录

- [一、项目简介](#一项目简介)
- [二、两种部署方式](#两种部署方式)
- [三、方式一：在线烧录（推荐新手）](#三方式一在线烧录推荐新手)
- [四、方式二：源码编译（推荐开发者）](#四方式二源码编译推荐开发者)
  - [4.1 安装 ESP-IDF](#41-安装-esp-idf)
  - [4.2 ESP32-C5 芯片支持说明](#42-esp32-c5-芯片支持说明)
  - [4.3 环境准备](#43-环境准备)
  - [4.4 下载源码](#44-下载源码)
  - [4.5 配置开发板](#45-配置开发板)
  - [4.6 配置关键参数](#46-配置关键参数)
  - [4.7 编译与烧录](#47-编译与烧录)
  - [4.8 首次启动验证](#48-首次启动验证)
- [五、通过 IM 与设备交互](#五通过-im-与设备交互)
- [六、本地文件系统（FATFS）](#六本地文件系统fatfs)
- [七、进阶：自定义与扩展](#七进阶自定义与扩展)
- [八、常见问题](#八常见问题)
- [九、相关资源](#九相关资源)

---

## 一、项目简介

ESP-Claw 是面向物联网设备的 **Chat Coding（聊天造物）** 式 AI 智能体框架。它在乐鑫 ESP32 系列芯片上本地完成感知、推理、决策与执行的完整闭环，核心特点包括：

- **聊天即编程**：通过 IM 发送自然语言，设备自动生成并执行 Lua 脚本控制硬件
- **事件驱动**：传感器、定时器、消息等任意事件均可触发 Agent 处理
- **本地记忆**：设备端结构化长期记忆（JSONL + 标签），隐私不上云
- **MCP 双身份**：既是 MCP Server（暴露硬件能力），也是 MCP Client（调用网络服务）
- **丰富外设**：支持摄像头、麦克风、LED 灯带、显示屏、GPIO 等

---

## 二、两种部署方式

| 方式 | 适用人群 | 特点 |
|------|----------|------|
| **在线烧录**（推荐新手） | 不想搭建编译环境的用户 | 浏览器直接配置并烧录，无需安装任何软件 |
| **源码编译** | 开发者、需要二次开发 | 完整控制功能裁剪、自定义硬件、调试 |

---

## 三、方式一：在线烧录（推荐新手）

如果你只是想快速体验，**无需下载本仓库**，直接访问：

**[https://esp-claw.com/zh-cn/flash](https://esp-claw.com/zh-cn/flash)**

### 步骤：

1. 使用 **Chrome / Edge** 浏览器（需支持 Web Serial API）
2. 通过 USB 连接你的 ESP32 开发板
3. 在网页上选择开发板型号（如 ESP32-S3-DevKitC-1）
4. 填写 Wi-Fi 名称、密码、LLM API Key、IM Bot Token 等参数
5. 点击**一键烧录**，固件将自动下载并写入设备
6. 烧录完成后，打开串口监视器即可看到启动日志

---

## 四、方式二：源码编译（推荐开发者）

### 4.1 安装 ESP-IDF

ESP-Claw 基于 [ESP-IDF](https://github.com/espressif/esp-idf)（Espressif IoT Development Framework）构建，**这是编译本项目的前置必要条件**。

#### 版本要求

| 项目 | 要求 |
|------|------|
| **ESP-IDF 版本** | **v5.5 或更高版本**，推荐 **v5.5.4** |
| **目标芯片** | ESP32、ESP32-S3 等（当前主要验证 ESP32-S3） |
| **操作系统** | Windows 10/11、Linux（Ubuntu 20.04+ 推荐）、macOS 11+ |
| **Python** | 3.9 或更高版本 |

#### 安装方式一：官方安装器（推荐 Windows 用户）

1. 访问 [ESP-IDF 发布页面](https://github.com/espressif/esp-idf/releases) 或 [乐鑫官网下载页](https://dl.espressif.com/dl/esp-idf/)
2. 下载对应系统的安装器：
   - Windows: `esp-idf-tools-setup-online-x.x.exe`
   - macOS: `esp-idf-tools-setup-macos-x.x.dmg`
3. 运行安装器，按向导选择：
   - ESP-IDF 版本：**v5.5.4**（或最新的 v5.5.x）
   - 安装路径：建议保持默认或选择短路径（如 `C:\esp\v5.5.4`）
   - 组件：至少选择目标芯片（如 ESP32-S3）和基础工具链
4. 安装完成后，安装器会自动创建桌面快捷方式或终端入口

#### 安装方式二：命令行安装（推荐 Linux / macOS 用户）

```bash
# 1. 克隆 ESP-IDF 仓库（指定 v5.5.4 版本）
git clone -b v5.5.4 --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf-v5.5.4

# 2. 进入目录并运行安装脚本
cd ~/esp/esp-idf-v5.5.4
./install.sh esp32s3

# 3. 导出环境变量（每次新终端都需要执行）
. ./export.sh
```

> 如果你希望同时支持多个芯片目标，可将 `esp32s3` 替换为 `all`：
> ```bash
> ./install.sh all
> ```

#### 安装方式三：VS Code 插件（推荐习惯 IDE 的用户）

1. 安装 [Visual Studio Code](https://code.visualstudio.com/)
2. 在插件市场搜索并安装 **ESP-IDF Extension**
3. 按 `Ctrl+Shift+P`（macOS: `Cmd+Shift+P`），输入 `ESP-IDF: Configure ESP-IDF Extension`
4. 在向导中选择：
   - **ESP-IDF 版本**：v5.5.4
   - **工具链**：让插件自动下载
   - **Python 环境**：使用系统 Python 或插件自带的
5. 配置完成后，VS Code 的底部状态栏会显示当前 ESP-IDF 版本和目标芯片

#### 验证 ESP-IDF 安装

无论使用哪种方式，安装成功后请在终端执行：

```bash
idf.py --version
```

应输出类似：

```
ESP-IDF v5.5.4
```

同时验证工具链：

```bash
xtensa-esp32s3-elf-gcc --version
```

应输出编译器版本信息。

#### 环境变量导出（重要）

**ESP-IDF 的编译命令需要在已导出环境的终端中执行**。不同系统的导出方式：

| 系统 | 导出命令 |
|------|----------|
| Linux / macOS (bash/zsh) | `. <IDF_PATH>/export.sh` |
| Windows (CMD) | `<IDF_PATH>\export.bat` |
| Windows (PowerShell) | `<IDF_PATH>\export.ps1` |

> 建议将导出命令加入你的 shell 配置文件（如 `~/.bashrc`、`~/.zshrc`），或使用 ESP-IDF 安装器创建的快捷方式启动终端。

---

### 4.2 ESP32-C5 芯片支持说明

ESP-Claw 正在积极支持乐鑫最新发布的 **ESP32-C5** 芯片。如果你使用的是基于 ESP32-C5 的开发板（如 `nm_cyd_c5` 或计划支持的 `esp32-c5-devkitc-1`），请仔细阅读本节的特殊要求和注意事项。

#### ESP32-C5 芯片特性

| 特性 | 规格 |
|------|------|
| **CPU 架构** | 单核 32 位 RISC-V（最高 240MHz）+ 低功耗协处理器（40MHz） |
| **无线连接** | **双频 Wi-Fi 6**（2.4GHz + 5GHz，802.11ax）、Bluetooth 5.2 (LE)、Zigbee、Thread (802.15.4) |
| **内存** | 384KB SRAM + 320KB ROM |
| **PSRAM 支持** | Quad SPI 模式（ESP32-C5 无 Octal PSRAM 模式） |

> **关键区别**：ESP32-C5 是乐鑫首款支持 **5GHz Wi-Fi 6** 的芯片。相比 ESP32-S3 仅支持 2.4GHz，C5 在拥挤的无线环境中具有更强的抗干扰能力和更高的吞吐量。

#### ESP-IDF 版本要求

ESP32-C5 需要 **ESP-IDF v5.5.4 或更高版本** 才能获得稳定的支持。低于此版本的 ESP-IDF 可能缺少 ESP32-C5 的完整工具链和驱动。

安装时需显式指定 ESP32-C5 目标：

```bash
# 命令行安装时
cd ~/esp/esp-idf-v5.5.4
./install.sh esp32c5

# 或使用 VS Code 插件时，在配置向导中选择 esp32c5 目标
```

#### 已支持与计划支持的开发板

| 开发板名称 | 状态 | 说明 |
|-----------|------|------|
| **`nm_cyd_c5`** | ✅ 已支持 | [NM-CYD-C5](https://github.com/RockBase-iot/NM-CYD-C5) "Cheap Yellow Display" 兼容板，基于 ESP32-C5-WROOM-1 模块，板载 16MB Flash + 8MB PSRAM、2.8" 320×240 ST7789 LCD + XPT2046 电阻触摸屏 |
| **`esp32-c5-devkitc-1`** | 🚧 计划支持 | 乐鑫官方 ESP32-C5-DevKitC-1 开发板，基于 ESP32-C5-WROOM-1 模块，预计配置 8MB Flash + 4MB PSRAM |

#### NM-CYD-C5 外设配置

`nm_cyd_c5` 开发板已预配置以下外设：

| 外设 | 接口/引脚 | 说明 |
|------|----------|------|
| **LCD 显示屏** | SPI2 (CS=23, DC=24, RST=-1, BL=25) | 2.8" 320×240 ST7789 面板，横屏显示 |
| **触摸屏** | 共享 SPI2 (TOUCH_CS=1) | XPT2046 电阻触摸屏（需用户代码额外初始化） |
| **SPI 总线** | MOSI=7, MISO=2, SCLK=6 | LCD、触摸、SD 卡槽共享 |
| **I2C 扩展** | SDA=9, SCL=8 | CN1 扩展排针，3.3V 供电 |
| **背光控制** | GPIO25 | LEDC PWM 背光，高电平有效 |

> **注意**：XPT2046 触摸屏和 SD 卡槽共享 `spi_display` 总线，但当前框架的 Lua 触摸模块主要面向 I2C 触摸路径，如需使用 XPT2046 请在用户代码中自行挂载。

#### ESP32-C5 编译注意事项

1. **必须先设置目标芯片**：
   ```bash
   cd application/basic_demo
   idf.py set-target esp32c5
   ```

2. **然后生成板级配置**（以 `nm_cyd_c5` 为例）：
   ```bash
   idf.py gen-bmgr-config -c ./boards -b nm_cyd_c5
   ```

3. **单核架构差异**：
   - ESP32-C5 是**单核** RISC-V 芯片，ESP32-S3 使用的双核相关配置（如 `CONFIG_FREERTOS_UNICORE` 等）不适用于 C5
   - PSRAM 仅支持 **Quad SPI 模式**，不支持 ESP32-S3 的 Octal PSRAM（`CONFIG_SPIRAM_MODE_OCT`）

4. **Wi-Fi 频段**：
   - ESP32-C5 支持 2.4GHz 和 5GHz 双频
   - 在 `menuconfig` 的 Wi-Fi 配置中，5GHz 频段可能需要额外配置
   - 注意：5GHz 频段穿墙能力弱于 2.4GHz，但干扰更少、速度更快

5. **完整的编译烧录命令示例**：
   ```bash
   cd application/basic_demo
   idf.py set-target esp32c5
   idf.py gen-bmgr-config -c ./boards -b nm_cyd_c5
   idf.py menuconfig        # 配置 Wi-Fi / LLM / IM
   idf.py build
   idf.py -p <PORT> flash monitor
   ```

#### ESP32-C5 与 ESP32-S3 主要差异对比

| 维度 | ESP32-S3 | ESP32-C5 |
|------|----------|----------|
| CPU 架构 | 双核 Xtensa LX7 | 单核 RISC-V |
| Wi-Fi | 2.4GHz Wi-Fi 4/5 | **2.4GHz + 5GHz Wi-Fi 6** |
| PSRAM 模式 | Octal (最高 120MB/s) | Quad (最高 80MB/s) |
| 低功耗协处理器 | 有 | 有（40MHz） |
| 多协议 | Wi-Fi + BLE | Wi-Fi 6 + BLE + Zigbee + Thread |

---

### 4.3 环境准备

#### 必备工具清单

| 工具 | 用途 | 安装命令 |
|------|------|----------|
| **ESP-IDF v5.5+** | 乐鑫官方开发框架 | 见 4.1 节 |
| **Python 3.9+** | 构建脚本运行环境 | 系统自带或官网下载 |
| **esp-bmgr-assist** | 开发板管理器辅助包 | `pip install esp-bmgr-assist` |
| **Git** | 克隆源码仓库 | 系统自带或官网下载 |

#### 安装 esp-bmgr-assist

```bash
pip install esp-bmgr-assist
```

> 此包仅在生成开发板配置时需要，安装一次即可。

#### 支持的开发板

本项目预置了以下开发板配置：

| 开发板名称 | 说明 |
|-----------|------|
| `esp32_S3_DevKitC_1` | ESP32-S3-DevKitC-1 官方开发板 |
| `esp32_S3_DevKitC_1_breadboard` | 带面包板外设的扩展配置 |
| `m5stack_cores3` | M5Stack CoreS3 |
| `nm_cyd_c5` | ESP32-C5 "Cheap Yellow Display" 兼容板（见 4.2 节） |

> 如果你使用的是其他 ESP32-S3 开发板，通常选择 `esp32_S3_DevKitC_1` 即可兼容运行。

---

### 4.4 下载源码

```bash
git clone https://github.com/espressif/esp-claw.git
cd esp-claw
```

仓库结构说明：

```text
esp-claw/
├── application/basic_demo/   # 主应用示例（ESP-IDF 工程）
│   ├── main/                 # 启动入口、Wi-Fi、HTTP 配置服务
│   ├── boards/               # 开发板定义
│   ├── fatfs_image/          # 默认 FATFS 内容
│   └── tools/cmake/          # CMake 补丁和同步辅助
├── components/
│   ├── claw_modules/         # 运行时核心（事件路由、记忆、技能管理）
│   ├── claw_capabilities/    # 能力插件（IM、MCP、Lua、文件等）
│   └── lua_modules/          # Lua 硬件绑定（GPIO、显示屏、摄像头等）
└── docs/                     # 文档网站源码（Astro/Starlight）
```

---

### 4.5 配置开发板

**此步骤必须在首次编译前执行**，用于生成开发板相关的支持文件。

```bash
cd application/basic_demo

# 以 ESP32-S3-DevKitC-1 为例
idf.py gen-bmgr-config -c ./boards -b esp32_S3_DevKitC_1
```

> 如果需要查看所有可用开发板，直接查看 `boards/` 目录下的文件夹名称即可。

---

### 4.6 配置关键参数

执行 menuconfig 配置 Wi-Fi、LLM、IM 等核心参数：

```bash
idf.py menuconfig
```

在 menuconfig 中，你需要重点关注并配置以下选项（通常在 `ESP-Claw Demo Configuration` 或对应子菜单下）：

| 配置项 | 说明 | 获取方式 |
|--------|------|----------|
| **Wi-Fi SSID** | 设备连接的 Wi-Fi 名称 | 你的路由器 |
| **Wi-Fi Password** | Wi-Fi 密码 | 你的路由器 |
| **LLM Provider** | 大模型服务商（OpenAI / Anthropic / 阿里云百炼等） | - |
| **LLM API Key** | 大模型 API 密钥 | [Anthropic](https://console.anthropic.com) / [OpenAI](https://platform.openai.com) / [阿里云百炼](https://bailian.console.aliyun.com) |
| **LLM Model** | 模型名称（如 `gpt-5.4`、`qwen3.5-plus`） | 服务商文档 |
| **Telegram Bot Token** | Telegram 机器人令牌 | [@BotFather](https://t.me/BotFather) |
| **QQ App ID / App Secret** | QQ 机器人凭证 | [QQ 开放平台](https://q.qq.com) |
| **Search API Key** | 网络搜索密钥（Brave / Tavily） | 对应搜索服务官网 |
| **Timezone** | 时区设置 | 如 `CST-8` 表示中国标准时间 |

> **安全提示**：API Key 和 Token 最终存储在设备的 NVS（非易失性存储）中。编译时设置的默认值仅用于开发测试，**请勿将带有真实密钥的固件提交到公共仓库**。

#### 使用自定义模型供应商（如 NVIDIA、本地部署等）

ESP-Claw 支持通过 **Custom** 选项接入任意兼容 OpenAI API 格式的第三方模型供应商。例如：

| 供应商 | Base URL 示例 | 认证方式 |
|--------|--------------|----------|
| **NVIDIA NIM** | `https://integrate.api.nvidia.com/v1` | `bearer` |
| **OpenRouter** | `https://openrouter.ai/api/v1` | `bearer` |
| **本地 Ollama** | `http://192.168.x.x:11434/v1` | `none` |
| **Azure OpenAI** | `https://<your-resource>.openai.azure.com/openai/deployments/<deployment-id>/chat/completions?api-version=2024-02-01` | `api-key` |
| **智谱 AI (GLM)** | `https://open.bigmodel.cn/api/paas/v4` | `bearer` |
| **DeepSeek** | `https://api.deepseek.com/v1` | `bearer` |

**通过 `menuconfig` 配置自定义供应商的步骤：**

1. 进入 `Default LLM Settings` 菜单
2. 将 **LLM provider preset** 选择为 **`Custom`**
3. 填写以下字段：
   - **Default LLM backend type**：`openai_compatible`（大多数供应商）或 `custom`
   - **Default LLM profile**：`custom_openai_compatible`（推荐）
   - **Default LLM model**：模型名称，如 `nvidia/llama-3.1-nemotron-70b-instruct`
   - **Default LLM base URL**：供应商 API 地址，如 `https://integrate.api.nvidia.com/v1`
   - **Default LLM auth type**：`bearer`（Authorization: Bearer 方式）或 `api-key`（x-api-key 方式）
   - **Default LLM API key**：你的 API 密钥

> **运行时修改**：你也可以在设备启动后，通过浏览器访问设备的 HTTP 配置页面（设备 IP:80）在线修改 LLM 参数，无需重新编译固件。

**验证请求格式**：ESP-Claw 的 LLM 后端通过 `base_url` 拼接完整的请求路径。例如设置 base URL 为 `https://integrate.api.nvidia.com/v1` 时，实际发出的聊天补全请求地址为：
```
POST https://integrate.api.nvidia.com/v1/chat/completions
```
请确保你填写的 base URL 末尾已包含 `/v1` 路径（如供应商文档要求）。

---

##### NVIDIA NIM 配置示例（详细）

以下以调用 **NVIDIA NIM** 上托管的 `deepseek-ai/deepseek-v4-pro` 模型为例，给出与 Python OpenAI SDK 的完整参数映射。

**Python SDK 原始代码：**

```python
from openai import OpenAI

client = OpenAI(
  base_url = "https://integrate.api.nvidia.com/v1",
  api_key = "$NVIDIA_API_KEY"
)

completion = client.chat.completions.create(
  model="deepseek-ai/deepseek-v4-pro",
  messages=[{"role":"user","content":""}],
  temperature=1,
  top_p=0.95,
  max_tokens=16384,
  extra_body={"chat_template_kwargs":{"thinking":False}},
  stream=True
)
```

**ESP-Claw 中的对应配置：**

| Python SDK 参数 | ESP-Claw 配置项 | 推荐值 | 说明 |
|----------------|-----------------|--------|------|
| `base_url` | **Default LLM base URL** | `https://integrate.api.nvidia.com/v1` | 必须手动填写完整地址 |
| `api_key` | **Default LLM API key** | `nvapi-xxxxxxxx...` | NVIDIA 生成的 API 密钥 |
| `model` | **Default LLM model** | `deepseek-ai/deepseek-v4-pro` | 模型完整名称 |
| — | **Default LLM backend type** | `openai_compatible` | NVIDIA NIM 为 OpenAI 兼容接口 |
| — | **Default LLM profile** | `custom_openai_compatible` | 使用 OpenAI 兼容协议解析响应 |
| — | **Default LLM auth type** | `bearer` | 通过 `Authorization: Bearer` 头部传递 API Key |

**menuconfig 中的完整填写示例（`Default LLM Settings` 菜单）：**

```text
( ) OpenAI
( ) Qwen Compatible
( ) Anthropic
(X) Custom

( ) Default LLM API key         [nvapi-AbCdEfGhIjKlMnOpQrStUvWxYz...]
( ) Default LLM backend type    [openai_compatible]
( ) Default LLM profile         [custom_openai_compatible]
( ) Default LLM model           [deepseek-ai/deepseek-v4-pro]
( ) Default LLM base URL        [https://integrate.api.nvidia.com/v1]
( ) Default LLM auth type       [bearer]
( ) Default LLM timeout ms      [120000]
```

**当前固件限制说明：**

以下参数在 Python SDK 示例中存在，但 **当前 ESP-Claw 固件中已硬编码或不支持自定义**，你在配置时无需（也无法）填写：

| 参数 | 当前固件行为 | 说明 |
|------|-------------|------|
| `temperature` | ❌ 不可配置 | 固件请求体中未包含此字段，模型使用服务商默认值 |
| `top_p` | ❌ 不可配置 | 同上 |
| `max_tokens` | ⚠️ 固定为 **8192** | 固件硬编码，与 Python 示例中的 16384 不同。如模型上下文较长，8192 通常已足够 |
| `extra_body` / `thinking` | ❌ 不支持 | 固件无法传递额外请求体字段。若模型默认输出 `<think>...</think>` 思考过程，ESP-Claw 会将其作为普通文本处理；目前不影响功能执行，但可能在对话中显示额外内容 |
| `stream` | ⚙️ 内部控制 | 固件后端使用流式解析（SSE），无需手动开启 |

> **关于 `thinking=False` 的注意事项**：NVIDIA 上部分 DeepSeek 模型默认会在回复前输出思考链（`<think>...</think>`）。由于当前固件无法通过 `extra_body` 关闭此行为，若你希望过滤掉思考内容，可以在 IM 中明确告诉 Agent："回复时只输出最终结论，不要包含思考过程"。

**运行时修改（推荐）**：

如果你在编译时还不确定使用哪个模型，可以先按任意预设编译，设备启动后通过浏览器访问 `http://<设备IP>` 打开配置页面，在 **LLM** 标签页中动态修改 Base URL、Model 和 API Key，保存后设备会自动重启并应用新配置，无需重新烧录固件。

---

### 4.7 编译与烧录

```bash
# 编译固件
idf.py build

# 烧录到设备（自动检测串口）
idf.py flash

# 打开串口监视器查看启动日志
idf.py monitor
```

也可以将烧录和监视合并执行：

```bash
idf.py flash monitor
```

**常用快捷键**（在 monitor 中）：

| 快捷键 | 功能 |
|--------|------|
| `Ctrl+]` | 退出监视器 |
| `Ctrl+T` `Ctrl+R` | 复位设备 |
| `Ctrl+T` `Ctrl+F` | 烧录固件 |
| `Ctrl+T` `Ctrl+H` | 显示所有快捷键帮助 |

---

### 4.8 首次启动验证

设备成功启动后，串口日志应显示类似以下内容：

```text
I (1234) claw_core: Agent runtime initialized
I (1234) basic_demo_wifi: Connected to AP, IP: 192.168.x.x
I (1234) config_http: HTTP server started on port 80
```

此时设备已：

1. ✅ 连接到 Wi-Fi
2. ✅ 启动 HTTP 配置服务器（AP 模式下为 captive portal）
3. ✅ 初始化 Agent 运行时和事件路由
4. ✅ 加载本地技能与 Lua 模块

如果配置了 Telegram Bot，你会收到一条上线通知消息。

---

## 五、通过 IM 与设备交互

设备运行后，你可以通过以下方式与它对话：

### 5.1 Telegram（最简单）

1. 在 Telegram 中找到你创建的 Bot
2. 直接发送消息，例如：
   - `"让板载 LED 闪烁 3 次"`
   - `"读取当前 GPIO 34 的电压"`
   - `"写一段 Lua 脚本，实现呼吸灯效果"`

### 5.2 QQ / 飞书 / 微信

配置对应的 App ID / Token 后，在对应的群聊或私聊中 @ 机器人即可。

### 5.3 常用交互示例

| 你说 | 设备会做 |
|------|----------|
| `"把灯带改成彩虹色"` | 生成 Lua 脚本 → 驱动 LED 灯带 → 执行动画 |
| `"记住我喜欢冷色调"` | 提取偏好 → 存入本地 `/fatfs/memory/MEMORY.md` |
| `"如果温度超过 30 度就打开风扇"` | 生成事件规则 → 持久化为本地自动化规则 |
| `"生成一个像素风小游戏"` | 编写完整 Lua 游戏脚本并在屏幕上运行 |

---

## 六、本地文件系统（FATFS）

设备启动后会挂载 FATFS 分区到 `/fatfs`，关键目录如下：

| 路径 | 用途 |
|------|------|
| `/fatfs/sessions/` | 对话会话历史 |
| `/fatfs/memory/MEMORY.md` | 长期结构化记忆 |
| `/fatfs/skills/` | 技能文档与清单 |
| `/fatfs/scripts/` | Lua 脚本存储 |
| `/fatfs/router_rules/router_rules.json` | 自动化规则配置 |
| `/fatfs/inbox/` | 消息附件缓存 |

你可以通过设备暴露的 MCP Tool 或文件管理接口读写这些文件。

---

## 七、进阶：自定义与扩展

### 7.1 添加新的 Lua 模块

如果你希望将新的硬件能力暴露给 Agent：

1. 在 `components/lua_modules/` 下创建新组件
2. 实现 `luaopen_<name>()` 和 `lua_module_<name>_register()`
3. 在 `application/basic_demo/main/basic_demo_lua_modules.c` 中注册该模块

### 7.2 添加新的 Capability

1. 在 `components/claw_capabilities/` 下创建组件
2. 定义 `claw_cap_descriptor_t` 描述你的工具能力
3. 实现 `cap_<name>_register_group()` 注册函数
4. 在 `app_claw.c` 中调用注册函数

### 7.3 裁剪功能以节省空间

如果 Flash 或内存紧张，可以在 `idf.py menuconfig` 中关闭不需要的组件（如禁用某些 IM 渠道、Web 搜索等）。

---

## 八、常见问题

### Q1: 编译报错 `Board manager config not found`

**原因**：未执行开发板配置生成步骤。  
**解决**：

```bash
cd application/basic_demo
idf.py gen-bmgr-config -c ./boards -b esp32_S3_DevKitC_1
```

### Q2: 设备无法连接 Wi-Fi

- 确认 `menuconfig` 中的 SSID/密码正确
- ESP32-S3 仅支持 **2.4GHz Wi-Fi**，不支持 5GHz
- 检查路由器是否开启了 WPA3（某些 ESP-IDF 版本兼容性不佳）

### Q3: 在线烧录时浏览器找不到设备

- 确保使用 **Chrome 89+** 或 **Edge 89+**
- 确保设备进入下载模式（通常按住 BOOT 键，点按 RESET，松开 BOOT）
- 检查 USB 数据线是否支持数据传输（而非仅充电线）

### Q4: LLM 响应很慢或报错

- 检查 API Key 是否有效、余额是否充足
- 确认网络连接稳定
- 自编程等复杂任务建议使用 **GPT-5.4**、**Qwen3.5-plus** 或同等推理能力的模型

### Q5: Windows 下 `idf.py` 命令无法识别

**原因**：未在 ESP-IDF 环境中执行命令。  
**解决**：

- 方式一：使用 ESP-IDF 安装器创建的 "ESP-IDF x.x CMD" 或 "ESP-IDF x.x PowerShell" 快捷方式启动终端
- 方式二：手动执行 `<IDF_PATH>\export.bat`（CMD）或 `<IDF_PATH>\export.ps1`（PowerShell）

### Q6: 烧录时出现 `A fatal error occurred: Failed to connect to ESP32-S3`

**原因**：设备未进入下载模式，或串口被占用。  
**解决**：

1. 按住开发板上的 **BOOT** 按钮
2. 按一下 **RESET** 按钮
3. 松开 **BOOT** 按钮
4. 重新执行 `idf.py flash`

---

## 九、相关资源

| 资源 | 链接 |
|------|------|
| 项目主页 | https://esp-claw.com/zh-cn/ |
| 在线文档 | https://esp-claw.com/zh-cn/tutorial/ |
| 在线烧录 | https://esp-claw.com/zh-cn/flash |
| 源码仓库 | https://github.com/espressif/esp-claw |
| ESP-IDF 官方文档 | https://docs.espressif.com/projects/esp-idf/zh_CN/v5.5/esp32s3/get-started/index.html |
| 问题反馈 | GitHub Issues |

---

## 快速开始检查清单

如果你已经有一块 ESP32-S3 开发板，按以下顺序操作：

- [ ] 安装 ESP-IDF v5.5.4（见 4.1 节）
- [ ] 安装 `esp-bmgr-assist`：`pip install esp-bmgr-assist`
- [ ] 克隆仓库：`git clone https://github.com/espressif/esp-claw.git`
- [ ] 进入目录：`cd esp-claw/application/basic_demo`
- [ ] 生成板级配置：`idf.py gen-bmgr-config -c ./boards -b esp32_S3_DevKitC_1`
- [ ] 配置参数：`idf.py menuconfig`（Wi-Fi / LLM / IM）
- [ ] 编译烧录：`idf.py build flash monitor`
- [ ] 收到设备上线通知，开始聊天交互！

祝你玩得开心！如果有具体报错或需要针对某款开发板的详细接线图，欢迎提交 Issue 或继续提问。
