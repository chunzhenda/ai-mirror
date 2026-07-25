---
kind: logging_system
name: ESP-IDF 日志系统（esp_log）
category: logging_system
scope:
    - '**'
source_files:
    - Firmware/esp32_ai_mirror/main/ai_mirror_main.c
    - Firmware/esp32_ai_mirror/main/board_wifi_prov.c
    - Firmware/esp32_ai_mirror/components/espressif__esp-sr/src/esp_mn_speech_commands.c
    - Firmware/esp32_ai_mirror/managed_components/espressif__cmake_utilities/test_apps/main/test_cmake_utilities.c
---

本项目基于 ESP-IDF 框架，使用其内置的 `esp_log.h` 作为统一的日志输出机制，所有模块通过 `ESP_LOG*` 宏进行结构化日志记录。

**使用的框架与工具**
- 核心库：ESP-IDF 的 `esp_log.h`，提供 `ESP_LOGI`、`ESP_LOGW`、`ESP_LOGE`、`ESP_LOGD`、`ESP_LOGV` 等宏。
- 日志标签：每个源文件定义一个静态 `TAG` 常量（如 `"ai_mirror"`、`"wifi_prov"`），用于区分不同模块的日志来源。
- 运行时控制：可通过 `esp_log_level_set("tag", level)` 动态调整特定 TAG 的日志级别（在测试组件中有示例调用）。
- 辅助输出：启动阶段仍保留少量 `printf` 用于早期引导输出（如 `app_main` 开头的打印）。

**关键文件与位置**
- `main/ai_mirror_main.c`：主程序入口，定义 `TAG = "ai_mirror"`，大量使用 `ESP_LOGI/W/E` 记录初始化、网络、音频流程状态。
- `main/board_wifi_prov.c`：配网模块，定义 `TAG = "wifi_prov"`，记录 Wi-Fi/BLE 配网事件、错误和连接状态。
- `components/espressif__esp-sr/src/esp_mn_speech_commands.c`：语音识别组件，使用 `ESP_LOGI/E` 输出命令列表和配置警告。
- `managed_components/espressif__cmake_utilities/test_apps/main/test_cmake_utilities.c`：演示如何通过 `esp_log_level_set("*", ESP_LOG_INFO)` 全局设置日志级别。

**架构与约定**
- 每个 C 文件独立定义 `static const char *TAG = "xxx";`，并通过 `ESP_LOGX(TAG, ...)` 输出，便于按模块过滤。
- 日志级别使用规范：`ESP_LOGI` 用于正常流程信息，`ESP_LOGW` 用于可恢复异常或警告，`ESP_LOGE` 用于致命错误或失败路径。
- 结构化字段：日志消息中直接嵌入关键变量（如 URL、错误码、样本数、增益值等），未使用 JSON 等专用序列化格式。
- 无集中式日志路由或异步写入，所有日志直接输出到 ESP-IDF 默认串口控制台。

**约束与限制**
- 日志级别由 ESP-IDF 构建配置控制（`CONFIG_LOG_*`），但当前 `sdkconfig.defaults` 中未显式设置，依赖 IDF 默认值。
- 没有自定义日志后端或文件/网络输出，仅依赖 IDF 内置的 UART 输出。
- LVGL 组件自带独立的 `lv_log` 子系统，与 `esp_log` 并行存在，互不干扰。
- 性能敏感路径（如 AEC 音频循环）仅在周期性采样点输出日志，避免阻塞实时处理。