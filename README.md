# AI Mirror

一个基于 **ESP32-S3** 的开源智能镜像硬件原型。项目将圆形显示屏上的眼睛动画与 ES8311 音频编解码器结合，提供本地音乐播放或麦克风回声两种演示模式；同时开源了固件与 PCB 制造资料。

> 当前仓库实现的是显示与音频基础原型，尚未包含联网、云端 AI 对话或语音识别功能。

## 功能

- 240 × 240 GC9A01 圆形 LCD，基于 LVGL 播放循环眼睛动画
- ES8311 音频编解码器，16 kHz I2S 音频链路
- 可在 ESP-IDF `menuconfig` 中选择：
  - **音乐模式**：循环播放固件内置的 `canon.pcm`
  - **回声模式**：将麦克风采集的声音实时输出到扬声器/耳机
- 提供 PCB Gerber 与 BOM，便于打样和复现

## 仓库结构

```
.
├── Firmware/
│   └── esp32_ai_mirror/       # ESP-IDF 固件工程
│       ├── main/              # 应用、眼睛动画资源与音频示例
│       └── sdkconfig          # 当前 ESP32-S3 配置
└── PCB/
    ├── Gerber_PCB1_*.zip      # PCB 制造文件
    └── BOM_Board1_PCB1_*.xlsx # 物料清单
```

## 硬件概览

默认固件配置面向 ESP32-S3，并使用以下外设：

| 模块 | 接口 / 配置 |
| --- | --- |
| 主控 | ESP32-S3 |
| 显示屏 | GC9A01，SPI，240 × 240 |
| 显示引脚 | SCLK GPIO18、MOSI GPIO19、MISO GPIO21、DC GPIO5、RST GPIO3、CS GPIO4、背光 GPIO2 |
| 音频编解码器 | ES8311，I2C + I2S |
| ES8311 I2C | SCL GPIO9、SDA GPIO10 |
| ES8311 I2S | MCLK GPIO6、BCLK GPIO7、WS GPIO8、DOUT GPIO11、DIN GPIO12 |

引脚定义位于 [`Firmware/esp32_ai_mirror/main/example_config.h`](Firmware/esp32_ai_mirror/main/example_config.h) 与 [`Firmware/esp32_ai_mirror/main/i2s_es8311_example.c`](Firmware/esp32_ai_mirror/main/i2s_es8311_example.c)。若使用不同硬件，请先核对原理图和引脚分配再烧录。

## 快速开始

### 1. 准备环境

安装与项目兼容的 ESP-IDF（组件清单要求 IDF 5.x），并在终端加载 ESP-IDF 环境。

### 2. 构建

```bash
cd Firmware/esp32_ai_mirror
idf.py set-target esp32s3
idf.py build
```

首次构建时，ESP-IDF Component Manager 会根据 `main/idf_component.yml` 获取所需组件。

### 3. 配置模式（可选）

```bash
idf.py menuconfig
```

在 **Example Configuration** 中选择音乐或回声模式，并按需调整音量和回声模式的麦克风增益。

### 4. 烧录与查看日志

将 `PORT` 替换为开发板串口，例如 Windows 下的 `COM3`：

```bash
idf.py -p PORT flash monitor
```

按 `Ctrl-]` 退出串口监视器。

## 自定义

### 替换眼睛动画

动画界面位于 [`Firmware/esp32_ai_mirror/main/lvgl_demo_ui.c`](Firmware/esp32_ai_mirror/main/lvgl_demo_ui.c)，当前由 `image.c` 中的四帧图片资源驱动。替换资源后，更新该文件中的帧数组即可。

### 替换音乐

音乐模式将 `main/canon.pcm` 嵌入固件。请准备 16 kHz、16-bit PCM 音频，替换文件后保持 `main/CMakeLists.txt` 中的 `EMBED_FILES` 与源文件内的二进制符号名称一致。

## PCB 文件

`PCB/` 目录包含 Gerber 压缩包及 BOM。制造前请自行审核版本、层叠、阻抗和元件封装；这些文件按原样提供。

## 开发说明

- 主程序入口：[`Firmware/esp32_ai_mirror/main/i2s_es8311_example.c`](Firmware/esp32_ai_mirror/main/i2s_es8311_example.c)
- 界面实现：[`Firmware/esp32_ai_mirror/main/lvgl_demo_ui.c`](Firmware/esp32_ai_mirror/main/lvgl_demo_ui.c)
- 菜单配置：[`Firmware/esp32_ai_mirror/main/Kconfig.projbuild`](Firmware/esp32_ai_mirror/main/Kconfig.projbuild)

欢迎提交 Issue 或 Pull Request 来改进硬件、界面和固件。

## 许可证

本仓库暂未提供许可证文件。在增加明确许可证前，请先联系项目维护者确认使用、修改与分发权限。
