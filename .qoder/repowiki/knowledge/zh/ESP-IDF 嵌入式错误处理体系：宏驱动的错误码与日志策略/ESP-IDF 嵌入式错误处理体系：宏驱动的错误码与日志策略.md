---
kind: error_handling
name: ESP-IDF 嵌入式错误处理体系：宏驱动的错误码与日志策略
category: error_handling
scope:
    - '**'
source_files:
    - Firmware/esp32_ai_mirror/main/ai_mirror_main.c
    - Firmware/esp32_ai_mirror/main/board_wifi_prov.c
    - Firmware/esp32_ai_mirror/main/board_buttons.c
    - Firmware/esp32_ai_mirror/components/espressif__esp-sr/src/esp_mn_speech_commands.c
---

该仓库为 ESP32-S3 智能魔镜固件项目，基于 ESP-IDF 框架开发。错误处理完全遵循 ESP-IDF 的标准模式，采用统一的 `esp_err_t` 错误码、宏辅助函数和结构化日志系统。

## 1. 核心系统与工具
- **错误类型**：使用 ESP-IDF 的 `esp_err_t` 作为统一错误返回类型（定义在 `esp_err.h`）
- **错误检查宏**：`ESP_ERROR_CHECK()`、`ESP_RETURN_ON_ERROR()`、`ESP_RETURN_ON_FALSE()`、`ESP_GOTO_ON_ERROR()`
- **日志系统**：`ESP_LOGE()`、`ESP_LOGW()`、`ESP_LOGI()`、`ESP_LOGD()` 配合 `TAG` 标签
- **错误名称转换**：`esp_err_to_name()` 将错误码转换为可读字符串

## 2. 关键文件与位置
- **主应用入口**：`main/ai_mirror_main.c` - 包含大部分初始化错误处理
- **WiFi配网模块**：`main/board_wifi_prov.c` - 网络相关错误处理
- **按钮输入模块**：`main/board_buttons.c` - 硬件输入错误处理
- **ESP-SR组件**：`components/espressif__esp-sr/src/` - 语音识别库的错误处理

## 3. 架构与约定
- **启动阶段严格检查**：使用 `ESP_ERROR_CHECK()` 确保关键初始化失败时立即中止
- **运行时错误传播**：使用 `ESP_RETURN_ON_ERROR()` 向上层返回具体错误码
- **条件性错误处理**：使用 `ESP_RETURN_ON_FALSE()` 处理指针有效性等条件检查
- **分级日志记录**：ERROR级别用于致命错误，WARNING级别用于可恢复问题，INFO级别用于正常流程
- **资源清理模式**：错误路径中调用相应的deinit函数释放资源

## 4. 具体实现模式
- **音频初始化**：I2S和ES8311编解码器初始化失败时记录错误并跳过音频功能
- **WiFi连接**：连接超时或认证失败时提供用户交互重试机制
- **内存分配**：PSRAM分配失败时优雅降级或终止任务
- **硬件访问**：GPIO、SPI、I2C等硬件操作失败时返回具体错误码
- **网络请求**：HTTP客户端请求失败时记录详细错误信息

## 5. 约束与规范
- 所有可能失败的API调用必须检查返回值或使用相应宏
- 错误日志必须包含具体的错误原因和上下文信息
- 资源分配失败时必须进行适当的清理和回滚
- 用户可见的错误应通过UI或LED提供反馈
- 避免在错误路径中使用动态内存分配