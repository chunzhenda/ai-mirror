---
kind: configuration_system
name: ESP32 AI魔镜固件配置系统
category: configuration_system
scope:
    - '**'
source_files:
    - Firmware/esp32_ai_mirror/sdkconfig
    - Firmware/esp32_ai_mirror/sdkconfig.defaults
    - Firmware/esp32_ai_mirror/main/ai_mirror_config.h
    - Firmware/esp32_ai_mirror/partitions_n16r8.csv
    - Firmware/esp32_ai_mirror/CMakeLists.txt
    - Firmware/esp32_ai_mirror/main/Kconfig.projbuild
---

该仓库为ESP32-S3智能魔镜硬件原型项目，采用ESP-IDF框架进行开发。配置系统主要基于ESP-IDF的标准机制：

**核心配置文件：**
- `sdkconfig` - ESP-IDF主配置文件，包含所有编译时选项
- `sdkconfig.defaults` - 默认配置值
- `sdkconfig.ci` / `sdkconfig.ci.bsp` - CI环境专用配置
- `partitions_n16r8.csv` - Flash分区表定义
- `CMakeLists.txt` - 构建系统配置

**应用层配置：**
- `main/ai_mirror_config.h` - 应用特定配置头文件
- `main/Kconfig.projbuild` - 项目自定义Kconfig选项
- `main/idf_component.yml` - ESP-IDF组件依赖声明

**配置加载机制：**
- 使用ESP-IDF的menuconfig系统进行交互式配置
- 通过Kconfig和sdkconfig实现编译时配置
- 运行时配置可能通过NVS（非易失性存储）管理WiFi配网等动态参数
- WiFi配网功能由`board_wifi_prov.c`模块处理

**特点：**
- 典型的嵌入式ESP-IDF项目配置结构
- 分层配置：SDK默认配置 + 项目覆盖 + 目标板配置
- 支持多目标构建（不同硬件变体）
- 使用ESP-IDF组件管理系统管理第三方库配置