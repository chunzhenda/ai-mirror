# AI Mirror / AI 智能魔镜

> An open-source **ESP32-S3** smart mirror hardware prototype that pairs a round LCD face animation with an ES8311 audio codec, WS2812 RGB LED, on-device Wi-Fi provisioning, and an onboard acoustic-echo-cancellation (AEC) demo. Firmware and PCB manufacturing files are both open-sourced.
>
> 一个基于 **ESP32-S3** 的开源智能魔镜硬件原型。圆形 LCD 上运行多场景表情动画，搭配 ES8311 音频编解码器、WS2812 RGB 灯效、设备端 Wi-Fi 配网（SoftAP / BLE 双模式 + 屏幕二维码），以及内置声学回声消除（AEC）演示。固件与 PCB 制造资料全部开源。

---

## Features / 功能特性

### Display & Face Animation / 显示与表情动画
- **240×240 GC9A01 round LCD** driven over SPI, rendered through LVGL + `esp_lvgl_port`, double-buffered. / 通过 SPI 驱动的 240×240 GC9A01 圆形 LCD，基于 LVGL + `esp_lvgl_port` 渲染，双缓冲。
- **Multi-scene animated cute face** with 6 cycling scenes: `IDLE`, `HAPPY`, `SAD`, `ANGRY`, `LISTENING`, `BLINK`, switching every 2.6s. / **多场景萌系表情动画**，6 个场景循环切换：待机、开心、难过、生气、倾听、眨眼，每 2.6s 切换。
- **Procedural motion** — smoothstep easing, eye-tracking drift, idle/fast blink, floating sparkles, cheek blush, tear drops, eyebrows, and listening sound-wave bars, all computed with integer-only math on the LVGL refresh path. / **程序化动画** — smoothstep 缓动、视线漂移、待机/快速眨眼、浮动光点、腮红、泪滴、眉毛、倾听声波条，全部使用整数运算在 LVGL 刷新路径中计算。

### Audio System / 音频系统
- **ES8311 codec** + **I²S** full-duplex link at 16 kHz / 16-bit, configurable mic gain (0–42 dB) and output volume (0–100). / **ES8311 编解码器** + **I²S** 全双工链路，16 kHz / 16-bit，可配置麦克风增益（0–42 dB）与输出音量（0–100）。
- Two selectable audio modes (via `menuconfig`): / 两种可选音频模式（通过 `menuconfig` 切换）：
  - **Music mode** — loops the embedded `canon.pcm` track. / **音乐模式** — 循环播放固件内置 `canon.pcm`。
  - **AEC demo mode** (default) — an Acoustic Echo Cancellation demo built on **esp-sr** `aec_pro`: plays a 330 Hz reference tone, captures the mic, runs AEC to remove the echo, and records the cleaned audio while holding **BTN1**; ~2 s after release it plays back a peak-normalized version so you can hear your voice with the tone suppressed. / **AEC 演示模式**（默认） — 基于 **esp-sr** `aec_pro` 的声学回声消除演示：播放 330 Hz 参考音，采集麦克风，运行 AEC 去除回声，按住 **BTN1** 录制清洗后的音频，松开约 2s 后播放峰值归一化的录音。
- Graceful degradation: if audio init fails, the rest of the device keeps running. / 优雅降级：音频初始化失败时设备其余部分照常运行。

### Wi-Fi Provisioning / Wi-Fi 配网
- **Dual transport** selectable on-device: **SoftAP** or **BLE**, using the ESP-IDF `wifi_provisioning` manager with Security-1 (PoP) handshake. / **双传输模式**，可在设备上选择 **SoftAP** 或 **BLE**，基于 ESP-IDF `wifi_provisioning` 管理器，采用 Security-1（PoP）握手。
- **QR code on the round display** — scan with the *ESP BLE Provisioning* / *Espressif Provisioning* phone app to configure Wi-Fi, no PC needed. / **屏幕二维码配网** — 用 *ESP BLE Provisioning* / *Espressif Provisioning* 手机 App 扫码配置 Wi-Fi，无需电脑。
- **Boot decision state machine**: / **启动决策状态机**：
  - No saved credentials → enters provisioning automatically. / 无已存凭据 → 自动进入配网。
  - Has saved credentials → auto-connects; **hold BTN1 for 2s** during the boot window to force re-provisioning. / 有已存凭据 → 自动连接；启动窗口内 **长按 BTN1 2s** 可强制重新配网。
- **Internet connectivity check** (HTTP probe to `connectivitycheck.gstatic.com` / `espressif.com`) with on-screen retry/continue prompt. / **联网检测**（HTTP 探测 `connectivitycheck.gstatic.com` / `espressif.com`），并提供屏幕重试/继续选择。
- Saved Wi-Fi credentials persist in NVS across reboots. / 已存 Wi-Fi 凭据持久化于 NVS，重启后保留。

### On-screen UI Screens / 屏幕 UI 界面
All built with LVGL on the round display, sharing a consistent visual language (accent dot, halo, centered labels): / 全部基于 LVGL 在圆屏上绘制，共享统一视觉语言（强调点、光晕、居中文字）：
- Boot / status messages / 启动与状态提示
- Provisioning transport selection (AP / BLE toggle) / 配网模式选择（AP / BLE 切换）
- QR code provisioning screen with service name & transport / 二维码配网页（含服务名与传输模式）
- Internet check success / warning (retry vs continue) / 联网检测成功 / 警告（重试 vs 继续）
- Setup-failure screen with button navigation / 配网失败页（按键导航）

### Buttons & RGB LED / 按键与 RGB 灯
- **2 GPIO buttons** (BTN1 = GPIO13, BTN2 = GPIO14, active-high) with a 10 ms scan / 30 ms debounce state machine, supporting press/release callbacks and long-press detection. / **2 个 GPIO 按键**（BTN1 = GPIO13，BTN2 = GPIO14，高电平有效），10 ms 扫描 / 30 ms 去抖状态机，支持按下/释放回调与长按检测。
- **WS2812 RGB LED** (GPIO48) driven via RMT with a custom WS2812 encoder, HSV→RGB conversion, and a boot-time rainbow demo animation. / **WS2812 RGB LED**（GPIO48），通过 RMT 驱动，含自定义 WS2812 编码器、HSV→RGB 转换及开机彩虹演示动画。

### Hardware & Manufacturing Files / 硬件与制造文件
- **PCB Gerber** archives + **BOM** provided for fabrication and reproduction. / 提供 **PCB Gerber** 压缩包与 **BOM** 物料清单，便于打样复现。
- **AXP2101** power-management-IC datasheet included. / 内附 **AXP2101** 电源管理芯片数据手册。
- Optional **BSP** (Board Support Package) support for the ESP-BOX board. / 可选 **BSP**（板级支持包）支持 ESP-BOX 开发板。

---

## Repository Structure / 仓库结构

```
.
├── Firmware/esp32_ai_mirror/        # ESP-IDF firmware project / ESP-IDF 固件工程
│   ├── main/
│   │   ├── ai_mirror_main.c         # Entry point: boot flow, audio/display/network orchestration / 入口：启动流程与音频/显示/网络编排
│   │   ├── ai_mirror_ui.c           # LVGL face animation + provisioning UI screens / LVGL 表情动画与配网 UI 界面
│   │   ├── ai_mirror_config.h       # Audio & I²C/I²S pin definitions / 音频与 I²C/I²S 引脚定义
│   │   ├── board_buttons.c/h        # Debounced 2-button input driver / 去抖双按键输入驱动
│   │   ├── board_rgb.c/h            # WS2812 RGB LED driver + rainbow demo / WS2812 RGB LED 驱动与彩虹演示
│   │   ├── board_wifi_prov.c/h      # Wi-Fi provisioning (SoftAP/BLE) + saved-credential connect / Wi-Fi 配网（SoftAP/BLE）与凭据连接
│   │   ├── canon.pcm                # Embedded 16 kHz/16-bit music clip / 内置 16 kHz/16-bit 音乐片段
│   │   ├── Kconfig.projbuild        # Audio mode / mic gain / volume / BSP menuconfig / 音频模式/增益/音量/BSP 菜单配置
│   │   └── idf_component.yml        # Component Manager dependencies / 组件管理器依赖
│   ├── components/                  # Local components: esp_lcd_gc9a01, esp-sr / 本地组件：GC9A01 驱动、esp-sr
│   ├── managed_components/          # Auto-fetched IDF components / 自动获取的 IDF 组件
│   └── sdkconfig*                   # ESP32-S3 build configuration / ESP32-S3 构建配置
├── PCB/
│   ├── Gerber_PCB1_*.zip            # PCB manufacturing files / PCB 制造文件
│   └── BOM_Board1_PCB1_*.xlsx       # Bill of materials / 物料清单
└── datasheet/
    └── AXP2101.pdf                  # PMIC datasheet / 电源管理芯片数据手册
```

---

## Hardware Overview / 硬件概览

Default firmware targets **ESP32-S3** with the following peripherals: / 默认固件面向 **ESP32-S3**，使用以下外设：

| Module / 模块 | Interface / Pins / 接口与引脚 |
| --- | --- |
| MCU / 主控 | ESP32-S3 |
| Display / 显示屏 | GC9A01, SPI, 240×240 |
| Display pins / 显示引脚 | SCLK GPIO18, MOSI GPIO19, MISO GPIO21, DC GPIO5, RST GPIO3, CS GPIO4, Backlight GPIO2 |
| Audio codec / 音频编解码器 | ES8311, I²C + I²S |
| ES8311 I²C | SCL GPIO9, SDA GPIO10 |
| ES8311 I²S | MCLK GPIO6, BCLK GPIO7, WS GPIO8, DOUT GPIO11, DIN GPIO12 |
| Buttons / 按键 | BTN1 GPIO13, BTN2 GPIO14 (active-high) / 高电平有效 |
| RGB LED | WS2812, GPIO48 (RMT-driven) / RMT 驱动 |

Pin definitions live in [`Firmware/esp32_ai_mirror/main/ai_mirror_config.h`](Firmware/esp32_ai_mirror/main/ai_mirror_config.h), [`board_buttons.h`](Firmware/esp32_ai_mirror/main/board_buttons.h), and [`board_rgb.h`](Firmware/esp32_ai_mirror/main/board_rgb.h). Verify against your schematic before flashing a different board. / 引脚定义位于 [`ai_mirror_config.h`](Firmware/esp32_ai_mirror/main/ai_mirror_config.h)、[`board_buttons.h`](Firmware/esp32_ai_mirror/main/board_buttons.h) 与 [`board_rgb.h`](Firmware/esp32_ai_mirror/main/board_rgb.h)。使用不同硬件前请核对原理图与引脚分配。

---

## Getting Started / 快速开始

### 1. Prepare the environment / 准备环境
Install ESP-IDF 5.x and load its environment in your terminal. / 安装 ESP-IDF 5.x，并在终端加载 ESP-IDF 环境。

### 2. Build / 构建
```bash
cd Firmware/esp32_ai_mirror
idf.py set-target esp32s3
idf.py build
```
The ESP-IDF Component Manager auto-fetches dependencies listed in `main/idf_component.yml` on first build. / 首次构建时，ESP-IDF Component Manager 会根据 `main/idf_component.yml` 自动获取依赖组件。

### 3. Configure (optional) / 配置（可选）
```bash
idf.py menuconfig
```
Under **AI Mirror Configuration** choose: / 在 **AI Mirror Configuration** 中选择：
- Audio mode: **music playback** or **microphone echo test (AEC demo)** / 音频模式：**音乐播放** 或 **麦克风回声测试（AEC 演示）**
- MIC gain (0–42 dB, echo mode only) / 麦克风增益（0–42 dB，仅回声模式）
- Voice volume (0–100) / 输出音量（0–100）
- Enable Board Support Package (BSP) / 启用板级支持包（BSP）

### 4. Flash & monitor / 烧录与查看日志
Replace `PORT` with your board's serial port (e.g. `COM3` on Windows): / 将 `PORT` 替换为开发板串口（如 Windows 下的 `COM3`）：
```bash
idf.py -p PORT flash monitor
```
Exit the monitor with `Ctrl-]`. / 按 `Ctrl-]` 退出串口监视器。

---

## First-boot Wi-Fi Setup / 首次配网

1. On first boot (no saved Wi-Fi), the mirror enters provisioning and shows a **transport selector** — use **BTN2** to toggle SoftAP/BLE, **BTN1** to confirm. / 首次上电（无已存 Wi-Fi）时进入配网，显示**模式选择** — 用 **BTN2** 切换 SoftAP/BLE，**BTN1** 确认。
2. A **QR code** appears on the round display. Scan it with the *ESP BLE Provisioning* app (Android/iOS) and follow the app to pick your Wi-Fi and enter the password. / 圆屏显示**二维码**，用 *ESP BLE Provisioning* App（Android/iOS）扫码，按 App 提示选择 Wi-Fi 并输入密码。
   - **PoP (Proof of Possession)**: `abcd1234` / **配网口令**：`abcd1234`
3. The device saves credentials to NVS, connects, and runs an internet check. On success the face animation starts. / 设备将凭据存入 NVS、连接并执行联网检测，成功后启动表情动画。
4. On later boots the device auto-connects. **Hold BTN1 for 2s** during boot to re-enter provisioning and change networks. / 后续启动自动连接。启动时 **长按 BTN1 2s** 可重新进入配网更换网络。

---

## Customization / 自定义

### Replace the face animation / 替换表情动画
The face is fully procedural in [`ai_mirror_ui.c`](Firmware/esp32_ai_mirror/main/ai_mirror_ui.c) — scenes, easing, and elements are defined in code (no image assets). Edit the `face_scene_t` enum and `draw_scene()` to change expressions. / 表情完全由程序化代码生成（位于 [`ai_mirror_ui.c`](Firmware/esp32_ai_mirror/main/ai_mirror_ui.c)），场景、缓动与元素均写在代码中（无图片资源）。修改 `face_scene_t` 枚举与 `draw_scene()` 即可改变表情。

### Replace the music / 替换音乐
Music mode embeds `main/canon.pcm`. Prepare 16 kHz / 16-bit PCM, replace the file, and keep the `EMBED_FILES` entry in `main/CMakeLists.txt` and the `_binary_*_start/end` symbols in `ai_mirror_main.c` consistent. / 音乐模式嵌入 `main/canon.pcm`。请准备 16 kHz / 16-bit PCM，替换文件后保持 `main/CMakeLists.txt` 中 `EMBED_FILES` 与 `ai_mirror_main.c` 中的 `_binary_*_start/end` 符号名一致。

### Convert any audio to PCM / 任意音频转 PCM
```bash
ffmpeg -i input.mp3 -ss 00:00:00 -t 00:00:20 -f s16le -ar 16000 -ac 1 -acodec pcm_s16le canon.pcm
```

---

## PCB Files / PCB 文件

The `PCB/` directory contains Gerber archives and the BOM. Review the version, stack-up, impedance, and footprints before manufacturing — these files are provided as-is. / `PCB/` 目录包含 Gerber 压缩包与 BOM。制造前请自行审核版本、层叠、阻抗与元件封装 — 文件按原样提供。

---

## Development Notes / 开发说明

- Entry point: [`ai_mirror_main.c`](Firmware/esp32_ai_mirror/main/ai_mirror_main.c) — `app_main()` boot flow. / 入口：[`ai_mirror_main.c`](Firmware/esp32_ai_mirror/main/ai_mirror_main.c) 的 `app_main()` 启动流程。
- Face + UI: [`ai_mirror_ui.c`](Firmware/esp32_ai_mirror/main/ai_mirror_ui.c). / 表情与界面：[`ai_mirror_ui.c`](Firmware/esp32_ai_mirror/main/ai_mirror_ui.c)。
- Wi-Fi provisioning: [`board_wifi_prov.c`](Firmware/esp32_ai_mirror/main/board_wifi_prov.c). / Wi-Fi 配网：[`board_wifi_prov.c`](Firmware/esp32_ai_mirror/main/board_wifi_prov.c)。
- Buttons: [`board_buttons.c`](Firmware/esp32_ai_mirror/main/board_buttons.c). / 按键：[`board_buttons.c`](Firmware/esp32_ai_mirror/main/board_buttons.c)。
- RGB LED: [`board_rgb.c`](Firmware/esp32_ai_mirror/main/board_rgb.c). / RGB 灯：[`board_rgb.c`](Firmware/esp32_ai_mirror/main/board_rgb.c)。
- Menuconfig: [`Kconfig.projbuild`](Firmware/esp32_ai_mirror/main/Kconfig.projbuild). / 菜单配置：[`Kconfig.projbuild`](Firmware/esp32_ai_mirror/main/Kconfig.projbuild)。

Issues and Pull Requests to improve hardware, UI, and firmware are welcome. / 欢迎提交 Issue 或 Pull Request 改进硬件、界面与固件。

---

## Roadmap (not yet implemented) / 路线图（尚未实现）

Cloud AI dialogue (ASR / LLM / TTS), WebSocket communication, VAD (voice activity detection), and OTA updates are planned but not yet implemented — see the internal analysis docs `小智项目分析.md` and `WiFi配网功能计划书.md`. / 云端 AI 对话（ASR / LLM / TTS）、WebSocket 通信、VAD 语音活动检测与 OTA 升级已规划但尚未实现 — 详见内部分析文档 `小智项目分析.md` 与 `WiFi配网功能计划书.md`。

---

## License / 许可证

No license file is included yet. Contact the maintainer before using, modifying, or distributing this project. / 本仓库暂未提供许可证文件，使用、修改或分发前请联系项目维护者确认权限。
