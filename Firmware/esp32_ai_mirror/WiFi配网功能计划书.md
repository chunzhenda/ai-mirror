# WiFi 配网功能计划书 (SoftAP + Captive Portal)

> 生成日期: 2026-07-13
> 目标硬件: ESP32-S3 (项目 `ai_mirror`, ESP-IDF 5.1.4)
> 配套文档: `小智项目分析.md`
> 状态: 计划阶段（未动代码）
> 性质: 本文档仅为规划，**不修改任何现有代码**。

---

## 一、功能目标

设备首次或异常上电时进入 **SoftAP 配网模式**：开放热点 + 迷你 DNS 重定向 + HTTP 配置页，用户用手机连上热点即**自动弹出网页**，填入家里的 WiFi 名称和密码提交；设备保存凭据并切换到 STA 模式联网，联网成功后**自动进入项目主程序**（音频 + LVGL 眼图等现有功能继续运行）。

**核心体验**：手机连热点 → 自动弹网页 → 填 WiFi 密码 → 设备自动联网跑起来。无需安装 APP、无需扫码（当前屏幕只有 240×240 眼图动画，画 QR 码成本高）。

---

## 二、技术选型

| 项 | 选择 | 理由 |
|---|---|---|
| 配网方式 | **自建 SoftAP + Captive Portal** | 无新组件依赖；契合项目裸 IDF 风格；连热点即弹网页，UX 最佳 |
| 触发方式 | **自动触发**（NVS 无凭据 / STA 连接超时） | 当前未接按键，无需新增硬件 |
| Web 服务 | `esp_http_server`（IDF 内置） | 轻量、官方维护 |
| DNS 重定向 | 自写 ~120 行迷你 DNS server | 触发手机/电脑自动弹出 captive portal 浏览器 |
| 凭据存储 | `nvs_flash`（IDF 内置） | 持久化、擦写安全 |
| 网页资源 | `EMBED_FILES` 嵌入 HTML | 复用项目现有 `canon.pcm` 嵌入手法，无需文件系统 |
| 凭据提交格式 | JSON（`cJSON` 内置） | 便于扩展（后续可加 token、设备名等字段） |

**不选 `wifi_provisioning` 组件的原因**：需引入 managed 依赖、要在 240×240 屏画 QR 码、且要求用户装 ESP Provisioning APP，与"连热点即用"目标不符。后续若要做 BLE 配网或量产 QR 码方案再迁移。

---

## 三、整体启动流程（状态机）

```
设备上电 (app_main)
   │
   ▼
[1] NVS 初始化  (nvs_flash_init，处理版本/满页→erase重试)
   │
   ▼
[2] 默认事件循环 + netif + wifi 驱动初始化 (esp_wifi_init)
   │
   ▼
[3] 从 NVS 读取 WiFi 凭据 (ssid / password)
   │
   ├───── 无凭据 ─────────────────────┐
   │                                   ▼
   │                          [A] 进入配网模式
   │
   └───── 有凭据 ──────┐
                        ▼
                [B] STA 模式连接，等待 IP_EVENT_STA_GOT_IP
                        │  超时 15s
          ┌─── Got IP──┤──────────────┐
          │             │ 超时/失败     │
          ▼             ▼              │
     [C] 联网成功   [A] 进入配网模式      │
          │         (保留旧凭据，         │
          ▼          供用户重新选网)       │
     [D] 运行主程序                      │
     (audio + LCD + LVGL + ...) ◄──────┘
```

**配网模式 [A] 内部**：
```
开 SoftAP "AI-Mirror-XXXX" (XXXX=MAC 末 2 字节, 192.168.4.1)
   │
   ├─ 启动 DNS 重定向任务 (UDP:53, 所有 A 查询 → 192.168.4.1)
   ├─ 启动 httpd, 注册 URI:
   │     GET  /                 → 重定向到 /config
   │     GET  /config           → 返回 config.html
   │     GET  /scan             → 返回周边 AP 的 JSON 列表
   │     POST /save             → 解析 {ssid,password} → 存 NVS → 返回成功页 → 延时 1s esp_restart()
   │     GET  /generate_204, /hotspot-detect.html, /ncsi.txt,
   │          /connecttest.txt, /fwlink … → 重定向到 /config
   │        (各平台 captive 探测 URL 都导向配网页)
   └─ LCD 显示配网提示 (复用 LVGL: 显示 "请连接热点 AI-Mirror-XXXX 配网")
```

---

## 四、模块与文件清单

复用 `小智项目分析.md` 规划的 `app_wifi.*`，并新增配网相关文件。**本计划书阶段只规划，不改代码。**

| 文件 | 职责 | 状态 |
|---|---|---|
| `main/app_wifi.h/c` | WiFi 公共接口：`app_wifi_init()`、`app_wifi_connect_blocking(timeout)`、`app_wifi_start_ap_config()`、状态查询 | 规划 |
| `main/app_wifi_prov.h/c` | 配网门户：DNS server、httpd URI handler、扫描、保存、重启编排 | 规划 |
| `main/wifi_creds.h/c` | NVS 凭据读写封装：`creds_load()` / `creds_save()` / `creds_erase()` | 规划 |
| `main/dns_server.c` | 迷你 DNS 重定向（UDP:53 → 192.168.4.1） | 规划 |
| `main/config.html` | 配网页面（响应式表单 + 扫码列表 JS） | 规划 |
| `main/ai_mirror_main.c` | **改动点**：在 `app_main` 开头插入网络初始化阶段；配网模式下显示提示 UI | 现有，待改 |
| `main/CMakeLists.txt` | **改动点**：`SRCS` 增加 `app_wifi.c` 等；`EMBED_FILES` 增加 `config.html`；`REQUIRES` 增加 `esp_wifi esp_http_server nvs_flash esp_event esp_netif json` | 现有，待改 |
| `main/ai_mirror_config.h` | 增加配网相关宏（AP 名前缀、STA 超时、AP 密码开关等） | 现有，待改 |
| `main/Kconfig.projbuild` | 增加菜单项：AP SSID 前缀、STA 连接超时、是否开放 AP、配网服务端口 | 现有，待改 |

---

## 五、详细设计

### 5.1 NVS 存储结构

- 命名空间: `"wifi_creds"`
- 键值:
  - `"ssid"` (string) — WiFi 名称
  - `"pass"` (string) — WiFi 密码（明文，IoT 常规做法；如需更高安全可改 NVS 加密分区，后续优化）
  - `"flag"` (u8) — 配网完成标志 `0xA5`，用于区分"已配过"与"全新设备"
- 读写封装 `wifi_creds.c` 对外只暴露 `creds_load(creds_t*)` / `creds_save(creds_t*)` / `creds_erase()`。

### 5.2 SoftAP 配置

- SSID: `AI-Mirror-XXXX`（XXXX = MAC 末 2 字节十六进制，避免多台设备重名）
- 认证: **开放（OPEN）**便于一键连接；如担心被蹭配网，可在 Kconfig 开启 WPA2-PSK 并设固定密码（默认 `12345678`）。配网页本身无敏感数据，开放 AP 风险可控。
- 信道: 默认 1
- `max_connection`: 4（允许多设备同时配置/调试）
- `esp_netif_create_default_wifi_ap()` → DHCP 网段 `192.168.4.0/24`，网关 `192.168.4.1`（IDF 默认）

### 5.3 Captive Portal（DNS 重定向 + 探测 URL）

**DNS server**（`dns_server.c`，~120 行）：
- 监听 UDP:53，收到 DNS A 查询 → 构造应答，answer 的 IP 固定填 `192.168.4.1`
- 一个 FreeRTOS 任务，8KB 栈，优先级 5
- 作用：手机连上热点后系统做连通性探测（如 `captive.apple.com`、`connectivitycheck.gstatic.com`、`nmcheck.gstatic.com`），DNS 把它们全指向设备 IP，探测 HTTP 请求落到 httpd，httpd 返回重定向到 `/config`，手机判定为 captive portal → **自动弹出配网页**。

**HTTP 探测端点**：注册一组 GET handler 把常见探测路径全部 302 重定向到 `/config`：
- Android: `/generate_204`、`/gen_204`、`/connecttest.txt`
- iOS/macOS: `/hotspot-detect.html`
- Windows: `/ncsi.txt`、`/connecttest.txt`、`/redirect`
- 通用: `/fwlink`、`/`

### 5.4 配网页面 (`config.html`)

嵌入式单页，手机响应式。要素：
- 标题 "AI Mirror 配网"
- SSID 下拉：页面加载时 `fetch('/scan')` 拉取周边 AP JSON 填充；附"手动输入"框
- 密码输入框（type=password，带显隐切换）
- "连接"按钮 → `POST /save` JSON `{ssid, password}`
- 连接中状态页：提交后显示"设备正在连接，请在 WiFi 列表确认是否回到家庭网络…约 10 秒后设备将重启"
- 视觉：暗色背景 + 圆角卡片，契合"AI 镜"调性；纯静态，无外部 CDN（离线可用）
- 通过 `EMBED_FILES "config.html"` 嵌入，`extern const char config_html_start[] asm(...)` + `_end` 访问（IDF 标准 EMBED_FILES 符号）。

### 5.5 /scan 周边热点扫描

- 收到 `/scan` → `esp_wifi_scan_start`（blocked，~2s）→ `esp_wifi_scan_get_ap_records` → 拼装 JSON `{"aps":[{"ssid":"x","rssi":-55,"auth":4},...]}` 返回
- 前端用此填充下拉框，并标注信号强度与加密类型
- 注意：AP 模式下扫描会短暂影响已连接手机的通信，需控制调用频率（前端防抖 3s）

### 5.6 /save 提交与凭据处理

- POST body 为 JSON `{ssid, password}`
- `cJSON` 解析 → 长度/字符校验（ssid 1~32 字节，password 8~63 字节，允许空密码时特殊处理）
- `creds_save()` 写 NVS → `nvs_commit`
- 返回成功 JSON `{"ok":true}` 或成功 HTML 页
- 启动一个 1s 定时器任务调用 `esp_restart()`（给 httpd 时间把响应发完）

### 5.7 STA 连接验证与重试

- `app_wifi_connect_blocking(timeout_ms=15000)`：
  - `esp_wifi_set_mode(WIFI_MODE_STA)` + `esp_wifi_set_config` + `esp_wifi_start`
  - 用 `EventBits` 等待 `IP_EVENT_STA_GOT_IP` 或 `WIFI_EVENT_STA_DISCONNECTED`
  - 超时或收到 DISCONNECTED → 返回失败
- 失败处理：进入配网模式 [A]，**保留旧凭据**（用户可在页面上看到上次填的 SSID 预填，方便改密码）。连续失败 ≥3 次可选择性 `creds_erase()`（Kconfig 开关，默认关）。
- 成功：返回主流程，主程序继续。

### 5.8 回归主程序

- `app_main` 改为：`nvs_init → wifi_init → 若需配网则阻塞在配网模式(不返回，靠 esp_restart 切换)；若 STA 成功则继续原有 audio/LCD/LVGL/任务`。
- 配网模式下 LCD 显示提示文字（LVGL label："请连接热点 AI-Mirror-XXXX 进行配网"），与现有眼图动画并存或替换。
- 联网成功后**不重启**，直接在 `app_main` 内继续现有初始化序列（I2S/ES8311/SPI/GC9A01/LVGL/音频任务原样保留），满足"配置完成后设备自动进入项目中运行"。

### 5.9 menuconfig (Kconfig.projbuild) 新增项

```
menu "WiFi Provisioning"
    config WIFI_PROV_AP_SSID_PREFIX
        string "AP SSID prefix"
        default "AI-Mirror-"
    config WIFI_PROV_STA_TIMEOUT_MS
        int "STA connect timeout (ms)"
        default 15000
    config WIFI_PROV_AP_OPEN
        bool "Use open AP (no password)"
        default y
    config WIFI_PROV_AP_PASSWORD
        string "AP password (when not open)"
        depends on !WIFI_PROV_AP_OPEN
        default "12345678"
    config WIFI_PROV_HTTP_PORT
        int "HTTP server port"
        default 80
endmenu
```

---

## 六、依赖与构建改动点（仅说明，不执行）

- `main/idf_component.yml`：无需新增 managed 组件（全部用 IDF 内置）。
- `main/CMakeLists.txt`：
  - `SRCS` 增加 `app_wifi.c app_wifi_prov.c wifi_creds.c dns_server.c`
  - `EMBED_FILES` 在 `canon.pcm` 后增加 `config.html`
  - `REQUIRES` 增加 `esp_wifi esp_http_server nvs_flash esp_event esp_netif json`（`json` 即 cJSON，IDF 内置组件名 `json`）
- `sdkconfig.defaults` 可补：`CONFIG_ESP_WIFI_ENABLED=y` 等默认（IDF 5.1 已默认开 WiFi）。
- 不需要 PSRAM；配网模式内存占用估算：httpd(~8KB) + DNS(~8KB栈) + scan buffer(~1KB) + HTML(~3KB) < 30KB，ESP32-S3 内部 RAM 充裕。

---

## 七、实施步骤（分阶段，便于后续落地）

| 阶段 | 步骤 | 任务 | 涉及文件 | 预计 |
|---|---|---|---|---|
| **P0 基础设施** | 0.1 | NVS 初始化 + 事件循环 + netif + wifi 驱动 | `app_wifi.c`, `wifi_creds.c` | 1.5h |
| | 0.2 | NVS 凭据读写封装 + 单元自测 | `wifi_creds.c` | 1h |
| **P1 STA 连接** | 1.1 | STA 模式 + 事件处理 + 阻塞等待 IP | `app_wifi.c` | 2h |
| | 1.2 | 接入 app_main：有凭据则联网，成功后跑主程序 | `ai_mirror_main.c` | 1h |
| | 1.3 | 测试：硬编码凭据验证联网+主程序正常 | — | 0.5h |
| **P2 配网门户** | 2.1 | SoftAP 启动 + netif ap + DHCP | `app_wifi_prov.c` | 1h |
| | 2.2 | 迷你 DNS 重定向 server | `dns_server.c` | 1.5h |
| | 2.3 | httpd + URI 路由（config/scan/save/探测端点） | `app_wifi_prov.c` | 2.5h |
| | 2.4 | config.html 响应式页面 + 扫描/提交 JS | `config.html` | 2h |
| **P3 闭环** | 3.1 | /save → NVS → 重启 → STA 全链路联调 | — | 1h |
| | 3.2 | 触发逻辑：无凭据/超时回退配网；LCD 配网提示 | `app_wifi.c`, `lvgl_demo_ui.c` | 1.5h |
| | 3.3 | Kconfig 菜单 + CMakeLists 依赖 | `Kconfig.projbuild`, `CMakeLists.txt` | 0.5h |
| **P4 验收** | 4.1 | 真机端到端：首次配网 / 换网 / 断网回退 / 重启记忆 | — | 1.5h |
| | 4.2 | 文档与边界用例 | — | 0.5h |
| | | **合计** | | **~18h** |

建议作为 `小智项目分析.md` **阶段1（基础通信）** 的前置子任务先行落地——配网是后续所有网络功能（云通信、ASR/LLM/TTS）的前提。

---

## 八、测试用例 / 验收标准

1. **全新设备**：擦除 NVS → 上电 → 热点 `AI-Mirror-XXXX` 出现 → 手机连接 → 自动弹出配网页 → 选家里 WiFi 填密码 → 提交 → 设备重启 → 联网成功 → 眼图动画+音频正常运行。
2. **重启记忆**：已配网设备直接上电 → 跳过配网 → 直接 STA 联网 → 主程序运行。
3. **密码错误**：提交错误密码 → STA 15s 超时 → 自动回退配网模式 → 旧 SSID 预填 → 可重新输入。
4. **换网**：配网模式下提交新 SSID → 覆盖 NVS → 重启连新网成功。
5. **断网启动**：已配网但路由器关机 → STA 超时 → 回退配网模式（保留凭据）；路由器恢复后重启设备即可恢复。
6. **多平台弹出**：iOS / 安卓 / Windows 三种手机连热点均能自动弹出配网页（验证各平台探测 URL 覆盖）。
7. **扫描列表**：`/scan` 返回周边 AP 且信号/加密类型正确，前端下拉正常。
8. **主程序不回归**：配网成功后不重启音频/LCD 状态机，眼图与 echo 正常工作。

---

## 九、风险与边界

| 风险 | 影响 | 缓解 |
|---|---|---|
| 不同手机 captive portal 弹出策略差异 | 个别机型不自动弹 | 网页显式提示"无法自动打开请访问 192.168.4.1"；覆盖主流探测 URL |
| 开放 AP 被他人连接提交错误凭据 | 设备被改连陌生网络 | 可选 WPA2 AP 密码（Kconfig）；/save 做最小校验；配网成功即关 AP |
| 扫描与 AP 并发导致已连手机短暂掉线 | 扫描瞬间通信中断 | 控制扫描频率（前端防抖 3s），扫描时长调到最短 |
| 密码明文存 NVS | 物理拿到设备可读 | 后续可启用 NVS 加密分区（flash encryption + NVS encryption） |
| DNS server 占用 UDP 53 与某些环境冲突 | 极少见 | 仅在配网模式启用，配网完成即销毁 |
| httpd 在配网模式常驻内存 | 内存 | 配网成功后 `httpd_stop` + 停 DNS 任务，释放回主程序 |
| STA 连接阻塞导致首启动变慢 | 体验 | 超时 15s 可调；首启动显示"连接中…"而非黑屏 |

---

## 十、后续项目建议

> 当前项目本质是 ESP-IDF `ai_mirror` 例程 + GC9A01/LVGL 眼图，目标是"AI 镜/小智语音助手"。配网是通往一切网络能力的第一步。以下给出后续可选方向与建议，供定方向时参考。

### 10.1 推荐主线（承接 `小智项目分析.md`）
配网落地后，按该文档的 5 阶段推进最稳妥：
- **阶段1** WiFi+HTTP（本次配网 + 云通信基础）
- **阶段2** 语音链路（VAD + 音频收发环形缓冲）
- **阶段3** 显示交互（UI 框架、表情系统、设置页）
- **阶段4** 云端对接（WebSocket/HTTP、JSON 协议、ASR/LLM/TTS 闭环、状态机）
- **阶段5** 优化（PSRAM、Opus 编码、功耗、动画）

理由：现有音频(✅)+显示(✅)已通，缺的是网络与云端，配网正好补齐网络入口。

### 10.2 若方向未定，三条候选路线
1. **小智语音助手主线**（最契合现状）：复用已有 ES8311+LCD，补 WiFi+WebSocket 接小智协议，最快形成可对话的 AI 镜。工作量见 `小智项目分析.md` MVP（~12h 必做）。
2. **智能魔镜信息终端**：偏显示——天气/时间/待办/家居状态，弱化语音，强调 LVGL UI 与 HTTP/MQTT 拉数据。适合不想做复杂音频链路时。
3. **离线本地 AI**：ESP32-S3 + 外挂模块跑本地唤醒词/命令词（如 ESP-SR），不依赖云端，隐私优先。语音识别精度有限，适合固定指令场景。

### 10.3 与本配网功能的衔接
- 无论走哪条主线，**配网模块都是复用资产**：`app_wifi.*` / `wifi_creds.*` / 配网页可直接沿用，后续云通信、OTA、MQTT 都建立在"已联网"之上。
- 建议后续为 `app_wifi` 增加 **OTA 升级** 能力（基于 `esp_https_ota`），配网+OTA 是 IoT 设备的两大基础设施，趁早打好。
- 若未来上量，可平滑迁移到 `wifi_provisioning`（BLE + QR 码），配网页/凭据存储层无需改动。

### 10.4 近期可立即并行的小改进（不阻塞配网）
- 开启 PSRAM（`sdkconfig` 当前未开），为后续音频缓冲/Opus 预留空间。
- 把 `app_main` 的音频/显示初始化抽成 `app_audio_init()` / `app_display_init()`，为多模块解耦做准备（`小智项目分析.md` 已规划该文件结构）。
- 为 `ai_mirror_config.h` 的引脚补注释与版本号，便于换板维护。

---

## 十一、验收交付物

- 本计划书 `WiFi配网功能计划书.md`
- （后续实施时）`app_wifi.*`、`app_wifi_prov.*`、`wifi_creds.*`、`dns_server.c`、`config.html`
- 改动后的 `ai_mirror_main.c`、`CMakeLists.txt`、`Kconfig.projbuild`、`ai_mirror_config.h`
- 真机端到端演示录像/截图（首次配网、换网、断网回退）
