---
kind: dependency_management
name: ESP-IDF 组件管理器依赖管理
category: dependency_management
scope:
    - '**'
source_files:
    - Firmware/esp32_ai_mirror/main/idf_component.yml
    - Firmware/esp32_ai_mirror/dependencies.lock
    - Firmware/esp32_ai_mirror/CMakeLists.txt
    - Firmware/esp32_ai_mirror/main/CMakeLists.txt
---

本项目基于 ESP-IDF v5.0+ 构建，使用官方 **idf-component-manager**（ESP 组件管理器）进行第三方库与 BSP 的声明、版本锁定与解析。依赖管理围绕以下文件与机制展开：

- **`main/idf_component.yml`**：项目级依赖声明入口，通过 `dependencies` 字段列出所需组件及版本约束（如 `idf: "^5.0"`、`espressif/es8311: "^1.0.0"`、`espressif/esp-box: ^2.4.2`），并支持按目标芯片（`target in [esp32s3]`）的条件选择。
- **`dependencies.lock`**：由组件管理器生成的锁文件，记录每个依赖的精确版本号、component_hash 以及来源服务（`https://api.components.espressif.com/`），确保构建可重现。
- **根级 `CMakeLists.txt`**：仅包含 `include($ENV{IDF_PATH}/tools/cmake/project.cmake)` 和 `project(ai_mirror)`，将组件解析完全委托给 ESP-IDF 工具链。
- **`main/CMakeLists.txt`**：通过 `idf_component_register` 注册源码与嵌入文件，不直接引用外部库路径，依赖由组件管理器注入。

组件实际源码位于 `managed_components/` 目录（如 `espressif__esp-sr`、`lvgl__lvgl`、`espressif__esp-box` 等），由 idf-component-manager 自动拉取并管理。所有依赖均从 Espressif 官方组件服务获取，未使用私有仓库或 vendor 目录手动维护。SDK 配置通过 `sdkconfig`、`sdkconfig.defaults`、`sdkconfig.ci` 等文件管理，配合 Kconfig 系统提供编译期选项。

该方案遵循 ESP-IDF 标准实践：以 YAML 声明依赖、生成锁文件保证一致性、通过 CMake 集成组件，无需手写 include 路径或链接命令。