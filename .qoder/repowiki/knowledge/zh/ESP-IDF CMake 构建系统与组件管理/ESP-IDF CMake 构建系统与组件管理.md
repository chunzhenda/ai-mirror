---
kind: build_system
name: ESP-IDF CMake 构建系统与组件管理
category: build_system
scope:
    - '**'
source_files:
    - Firmware/esp32_ai_mirror/CMakeLists.txt
    - Firmware/esp32_ai_mirror/main/CMakeLists.txt
    - Firmware/esp32_ai_mirror/main/idf_component.yml
    - Firmware/esp32_ai_mirror/dependencies.lock
    - Firmware/esp32_ai_mirror/partitions_n16r8.csv
    - Firmware/esp32_ai_mirror/sdkconfig.defaults
    - Firmware/esp32_ai_mirror/.devcontainer/Dockerfile
    - Firmware/esp32_ai_mirror/.devcontainer/devcontainer.json
    - Firmware/esp32_ai_mirror/pytest_ai_mirror.py
---

## 1. 构建系统概览
本项目基于 **Espressif ESP-IDF v5.x** 框架，使用 **CMake** 作为核心构建系统。项目遵循 IDF 标准工程结构：根目录 `CMakeLists.txt` 通过 `include($ENV{IDF_PATH}/tools/cmake/project.cmake)` 引入 IDF 构建框架，`main/` 子目录包含应用源码与模块级 `CMakeLists.txt`。

## 2. 关键文件与位置
- **顶层构建入口**: `Firmware/esp32_ai_mirror/CMakeLists.txt` — 声明最小 CMake 版本 (3.16) 并包含 IDF project.cmake
- **主模块构建**: `Firmware/esp32_ai_mirror/main/CMakeLists.txt` — 使用 `idf_component_register` 注册源文件 (`ai_mirror_main.c`, `ai_mirror_ui.c`, `board_*.c`) 和嵌入资源 (`canon.pcm`)
- **组件依赖声明**: `Firmware/esp32_ai_mirror/main/idf_component.yml` — 声明 idf ^5.0、espressif/es8311、espressif/esp-box (条件依赖 esp32s3)
- **依赖锁定**: `Firmware/esp32_ai_mirror/dependencies.lock` — 记录所有已解析组件的精确版本与哈希，目标平台为 esp32s3
- **分区表**: `Firmware/esp32_ai_mirror/partitions_n16r8.csv` — 定义 NVS、OTA(双分区)、SPIFFS(assets/models)、coredump、FATFS 等分区布局
- **默认配置**: `Firmware/esp32_ai_mirror/sdkconfig.defaults` — 启用 BLE (NimBLE)、LVGL QRCode 等默认选项
- **容器化开发环境**: `.devcontainer/Dockerfile` 基于 `espressif/idf:latest`，`.devcontainer/devcontainer.json` 配置 VS Code + ESP-IDF 插件

## 3. 架构与约定
- **组件化构建**: 通过 `managed_components/` 目录管理第三方组件 (esp-sr, esp-dsp, lvgl, esp_codec_dev 等)，由 `idf_component_manager` 自动下载并锁定版本
- **多目标支持**: 通过 `sdkconfig.defaults` 与 `sdkconfig.*` 变体文件区分不同硬件平台 (bsp 模式、CI 模式)
- **OTA 双分区**: 分区表预留 `ota_0`/`ota_1` 两个应用分区，配合 `otadata` 实现无缝固件升级
- **资源嵌入**: 音频资源 (`canon.pcm`) 通过 `EMBED_FILES` 直接编译进固件，避免运行时加载开销
- **测试集成**: `pytest_ai_mirror.py` 使用 `pytest-embedded` 框架对多芯片平台执行冒烟测试，验证启动日志与 BSP 初始化

## 4. 构建流程与约束
- **标准命令**: `idf.py build` 编译、`idf.py -p PORT flash monitor` 烧录并查看串口输出
- **依赖管理**: `idf.py add-dependency "espressif/es8311^1.0.0"` 添加组件后自动下载到 `managed_components/`
- **配置菜单**: `idf.py menuconfig` 交互式配置 SDK 选项，变更持久化到 `sdkconfig` 文件
- **容器化**: 推荐使用 Docker 容器 `espressif/idf` 保证工具链一致性，VS Code devcontainer 提供开箱即用环境
- **平台约束**: 当前锁定目标为 `esp32s3`，BSP 选择 `espressif/esp-box` 通过 Kconfig 规则按 target 条件启用
- **版本锁定**: `dependencies.lock` 确保构建可重现，禁止未锁定的组件版本漂移