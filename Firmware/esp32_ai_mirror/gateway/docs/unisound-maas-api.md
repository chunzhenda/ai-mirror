# 云知声 MaaS Token Plan API 接入调研

> 调研日期：2026-08-01  
> 范围：仅查阅 `unisound.com` 官方文档与官方产品页。本文未包含或保存任何真实 API Key；鉴权和最小文本请求由主会话独立执行，测试结果已脱敏。

## 结论

云知声 MaaS 提供 OpenAI 兼容文本接口，以及 WebSocket/异步任务形态的 ASR、异步任务形态的 TTS。Token Plan 专属 Key 使用标准 Bearer 鉴权，可以先通过 `GET /v1/models` 验证 Key 和当前账户可用模型，再用 `POST /v1/chat/completions` 做最小文本调用。

实测结果：`GET /v1/models` 返回 HTTP 200，可用文本模型为 `u2`、`glm-5.2`、`kimi-k3`；`u2` 最小非流式对话返回 HTTP 200 和 `OK`，完整请求用量为 101 tokens。

**授权说明存在需要留档的冲突：**官网公开页面规定 Token Plan 额度仅可用于 AI 编程工具，禁止用于自动化脚本和自定义应用后端。但用户已向云知声负责人确认该 Token Plan 可用语音模型，而且技术实测证明当前 Key 确实具有 `u2-tts` 和 `u2-asr` 调用权限。建议保留负责人的书面确认或工单编号，作为 ESP32 网关正式上线的授权依据。

### Token Plan 语音模型实测

- `u2-tts`：创建任务 HTTP 200、业务码 0，合成 15 个汉字；任务状态 `Success`。
- 下载音频：MP3、16000 Hz、单声道、3.472 秒、27776 字节。
- TTS 输出的 `file_id` 不能直接作为 ASR 输入，否则返回 `100002 data not found`。必须下载后使用 `/v1/files/upload` 和 `purpose=a2t_async_input` 重新上传。
- `u2-asr`：重新上传后创建转写任务 HTTP 200、业务码 0，任务状态 `Success`。
- 原文：“你好，云知声语音链路测试成功。”
- ASR：“你好，云知声语音链路测试成功！”
- 结论：TTS → 文件下载 → ASR 上传 → 转写的语音模型闭环通过，文本内容一致，仅句末标点不同。

对于当前 ESP32 网关项目：

- LLM 可直接复用现有 OpenAI 兼容调用方式，只需把 Base URL 改成 `https://maas-api.unisound.com/v1`，模型从 `GET /v1/models` 的返回中选择。
- ASR 不能直接套用网关当前“multipart 文件上传后立即返回文本”的自定义 HTTP 契约。短语音推荐由网关实现云知声一句话 ASR WebSocket 适配器；已有 Opus 音频可以作为二进制帧发送。
- TTS 也不能直接套用网关当前“HTTP 请求立即返回音频”的自定义 TTS 契约。官方公开接口是创建任务、轮询、下载文件的异步流程，需要单独适配。

官方入口：[平台概览](https://maas.unisound.com/docs/guide/overview)、[API 概览](https://maas.unisound.com/docs/api/overview)、[Token Plan 快速接入](https://maas.unisound.com/docs/token-plan/quickstart)。

## 鉴权与 Base URL

- OpenAI 兼容 Base URL：`https://maas-api.unisound.com/v1`
- Anthropic 兼容 Base URL：`https://maas-api.unisound.com/anthropic`
- HTTP 和 WebSocket 均通过请求头 `Authorization: Bearer <API_KEY>` 鉴权。
- Token Plan Key 格式为 `tp-xxxxx`，与按量付费/模型资源包 Key 不可互换，仅在 Token Plan 订阅有效期内可用。
- 官方明确要求不要把 Key 明文嵌入浏览器或 ESP32 固件。正式应用的普通 MaaS API Key 应仅保存在网关服务端密钥存储或环境变量中。
- 公开条款与负责人对该账户的语音授权确认存在冲突；正式接入前应保存书面确认，同时在网关中保持 Key 不落盘、不输出日志、可随时轮换。

来源：[Token Plan 快速接入](https://maas.unisound.com/docs/token-plan/quickstart)、[文本生成 API](https://maas.unisound.com/docs/api/text/openai-compatible)、[一句话 ASR](https://maas.unisound.com/docs/guide/speech/oneshot-asr)。

## 关键端点

| 能力 | 方法与端点 | 说明 |
| --- | --- | --- |
| 枚举可用模型 | `GET https://maas-api.unisound.com/v1/models` | OpenAI 兼容；返回 `id`、`context_window`、`max_output` 等 |
| 文本对话 | `POST https://maas-api.unisound.com/v1/chat/completions` | OpenAI 兼容；支持流式、非流式、多轮消息与工具调用 |
| 一句话 ASR | `WSS wss://maas-api.unisound.com/v2/ws/asr/oneshot?model=u2-asr` | 适合约 25 秒以内的语音指令和短对话 |
| 实时 ASR | WebSocket，见官方实时转写文档 | 适合持续音频流；当前项目方案 C 可在后续接入 |
| 文件上传 | `POST https://maas-api.unisound.com/v1/files/upload` | 异步 ASR/TTS 使用 `file_id` 时的前置接口 |
| 创建异步 ASR | `POST https://maas-api.unisound.com/v1/audio/asr/tasks` | 使用 `file_id` 或可公网访问的 `file_url` |
| 查询异步 ASR | `GET https://maas-api.unisound.com/v1/audio/asr/tasks/{task_id}` | 状态为 `Waiting`、`Processing`、`Success` 或 `Failed` |
| 创建异步 TTS | `POST https://maas-api.unisound.com/v1/audio/speech/tasks` | 返回 `task_id`、`file_id`、`usage_characters` |
| 查询异步 TTS | `GET https://maas-api.unisound.com/v1/audio/speech/tasks?task_id=...` | 成功后取得音频 `file_id` |
| 下载文件 | `GET https://maas-api.unisound.com/v1/files/retrieve_content?file_id=...` | 下载异步生成的音频 |

来源：[模型列表 API](https://maas.unisound.com/docs/api/models/openai-compatible)、[文本生成 API](https://maas.unisound.com/docs/api/text/openai-compatible)、[API 概览](https://maas.unisound.com/docs/api/overview)、[创建异步 ASR](https://maas.unisound.com/docs/api/transcribe/start)、[查询异步 ASR](https://maas.unisound.com/docs/api/transcribe/result)、[创建异步 TTS](https://maas.unisound.com/docs/api/speech/t2a-async)、[查询异步 TTS](https://maas.unisound.com/docs/api/speech/t2a-async-query)。

## 模型与能力

官方 Token Plan 产品页当前列出的套餐能力包括：

- 文本：`u2`、`u2-med`、`glm-5.2`、Kimi K3；
- 语音：`u2-asr`、`u2-tts`、`u2-tts-clone`；
- 视觉：`u1-ocr`、`u1-ocr-med`。

官方页面之间存在轻微版本差异：文本生成 API 页当前列出 `u2`、`u2-med`、`glm-5.2`，速率限制页还列出 `kimi-k3`，产品页展示 Kimi K3。不要把网页静态清单硬编码为账户能力；运行时以带当前 Token Plan Key 调用 `GET /v1/models` 的结果为准。

平台概览还给出：U2 最大输出约 64K Tokens；异步 ASR 单文件最长 5 小时；异步 TTS 单请求最多 5 万字符，返回的下载 URL 有效期为 9 小时。

来源：[Token Plan 产品页](https://maas.unisound.com/token-plan)、[文本生成 API](https://maas.unisound.com/docs/api/text/openai-compatible)、[模型列表 API](https://maas.unisound.com/docs/api/models/openai-compatible)、[API 概览](https://maas.unisound.com/docs/api/overview)。

## 最小验证请求

以下示例只从 PowerShell 环境变量读取 Key，不会把 Key 写入仓库。建议先在控制台轮换已经暴露过的 Key，然后在新终端中输入新 Key。

### 1. 设置临时环境变量

```powershell
$secureKey = Read-Host 'Unisound API Key' -AsSecureString
$env:UNISOUND_API_KEY = [System.Net.NetworkCredential]::new('', $secureKey).Password
Remove-Variable secureKey
```

输入不会回显；明文只放入当前 PowerShell 进程的环境变量。测试结束后执行 `Remove-Item Env:UNISOUND_API_KEY`。也可以由系统的机密管理方式注入变量。不要把 Key 写入 `.env` 后提交。

### 2. 验证鉴权与模型权限

```powershell
curl.exe -sS `
  -H "Authorization: Bearer $env:UNISOUND_API_KEY" `
  "https://maas-api.unisound.com/v1/models"
```

通过标准：HTTP 200，返回 `object: "list"` 且 `data` 中至少有一个可用文本模型。若返回 401，应先根据业务码区分 Key 错误、过期、套餐余额或模型权限问题。

### 3. 非流式文本对话

```powershell
$body = @{
  model = 'u2'
  messages = @(
    @{ role = 'user'; content = '只回复：unisound-ok' }
  )
  stream = $false
  max_tokens = 32
} | ConvertTo-Json -Depth 5

Invoke-RestMethod `
  -Method Post `
  -Uri 'https://maas-api.unisound.com/v1/chat/completions' `
  -Headers @{ Authorization = "Bearer $env:UNISOUND_API_KEY" } `
  -ContentType 'application/json' `
  -Body $body
```

通过标准：HTTP 200，`choices[0].message.content` 非空，响应包含 `usage.prompt_tokens`、`usage.completion_tokens` 和 `usage.total_tokens`。

### 4. 流式文本对话

请求体把 `stream` 改为 `true`；如需在最后一块获得用量，增加：

```json
{
  "stream": true,
  "stream_options": {
    "include_usage": true
  }
}
```

官方定义流式对象类型为 `chat.completion.chunk`，客户端应逐块拼接 `choices[].delta.content`；启用 `include_usage` 后，用量仅出现在最后一个数据块。来源：[文本生成 API](https://maas.unisound.com/docs/api/text/openai-compatible)。

## ASR 最小接入说明

### 一句话 ASR（推荐用于当前语音聊天）

连接：

```text
wss://maas-api.unisound.com/v2/ws/asr/oneshot?model=u2-asr
Authorization: Bearer <API_KEY>
```

帧顺序：

1. 建立 WebSocket。
2. 发送文本 JSON `start` 帧，例如音频格式 `pcm` 或 `opus`、采样率 `16k`。
3. 循环发送原始二进制音频帧，不做 Base64。
4. 发送文本 JSON `{"type":"end"}`。
5. 接收 JSON，`type=variable` 为中间结果、`type=fixed` 为稳定结果，直到 `end=true`。

注意：`start` 之前发送的音频会被丢弃；布尔参数需要按官方协议传字符串 `"true"` / `"false"`；默认单会话识别上限约 25 秒。支持 `pcm`、`opus`、`speex`、`amr`、`adpcm`。来源：[一句话 ASR 使用指南](https://maas.unisound.com/docs/guide/speech/oneshot-asr)、[一句话 ASR API](https://maas.unisound.com/docs/api/transcribe/oneshot)。

### 异步 ASR（适合长录音，不适合低延迟对话）

创建任务最小请求体：

```json
{
  "file_url": "https://example.com/test.opus",
  "model": "u2-asr",
  "format": "opus",
  "sample_rate": 16000,
  "channel": 1
}
```

也可以先上传文件并用 `file_id` 替代 `file_url`。音频要求：1 秒至 5 小时、不超过 1 GB，支持 `mp3`、`opus`、`wav`、`amr`、`m4a`、`ogg`。来源：[创建异步 ASR](https://maas.unisound.com/docs/api/transcribe/start)。

## TTS 最小接入说明

创建任务最小请求：

```json
{
  "model": "u2-tts",
  "text": "你好，这是一条云知声语音合成测试。",
  "voice_setting": {
    "voice_id": "cn_male_chenyu",
    "speed": 50,
    "volume": 50,
    "pitch": 50,
    "bright": 50,
    "emotion": "happy",
    "language": "zh"
  },
  "audio_setting": {
    "audio_sample_rate": 16000,
    "format": "pcm",
    "channel": 1
  }
}
```

流程为：创建任务 → 使用 `task_id` 轮询 → `Success` 后取得 `file_id` → 下载音频。官方支持输出 `mp3` 或 `pcm`，采样率可选 8000、16000、24000、32000，单声道。当前项目播放链路是 PCM16/16 kHz，建议请求 `pcm`、16000、单声道，并在适配器中校验返回 PCM 的位宽与字节序。

来源：[创建异步 TTS](https://maas.unisound.com/docs/api/speech/t2a-async)、[查询异步 TTS](https://maas.unisound.com/docs/api/speech/t2a-async-query)、[API 概览](https://maas.unisound.com/docs/api/overview)。

## 流式与非流式能力对比

| 能力 | 非流式/异步 | 流式 |
| --- | --- | --- |
| LLM | `stream=false`，一次返回完整 `chat.completion` | `stream=true`，返回 `chat.completion.chunk` 增量 |
| 短语音 ASR | 不提供当前网关契约形态的同步 HTTP 文件接口 | 一句话 ASR 使用 WebSocket，可返回中间和最终结果 |
| 长音频 ASR | 创建任务并轮询结果 | 官方另有实时 ASR WebSocket |
| TTS | 公开文档为异步任务、轮询、下载 | 本次查阅的官方公开 API 中未发现实时流式 TTS 端点 |

不要因为产品页描述“实时”就假设所有语音接口均是 HTTP 流；应按各 API 页协议实现。

## 速率限制

官方发布的默认限制：

| 类型 | 模型/接口 | 限制 |
| --- | --- | --- |
| 文本 | `u2`、`kimi-k3`、`glm-5.2`、`u2-med` | 100 RPM / 10,000,000 TPM |
| ASR | `v1_audio_asr_tasks` / `u2-asr` | 20 RPM |
| TTS | `v1_audio_speech_tasks` / `u2-tts`、`u2-tts-clone` | 20 RPM |
| 声音克隆 | `v1_audio_voices_clone` / `u2-tts-clone` | 20 RPM |

官方未在公开速率限制页说明 Token Plan 有不同的单独限额。因此实现时应遵守上述平台限制，并为 HTTP 429 做指数退避和随机抖动。来源：[速率限制](https://maas.unisound.com/docs/api/rate-limits)。

## Token Plan 额度与限制

Token Plan 公开条款写明额度只允许在 AI 编程工具中使用，并禁止自动化脚本和自定义应用后端；超出范围可能导致暂停订阅或封禁 Key。当前账户已经获得负责人口径确认，且 ASR/TTS 技术调用实测成功；仍建议取得并保留可追溯的书面授权。公开条款来源：[Token Plan 订阅概要](https://maas.unisound.com/docs/token-plan/overview)。

官方产品页在调研时展示四档月度套餐：Lite 1.8 亿 Credits、Standard 6 亿 Credits、Pro 21 亿 Credits、Max 48 亿 Credits；页面说明全部服务统一使用 Credits 结算，并覆盖 U2、U2-Med、U2-ASR、U2-TTS、U2-TTS-Clone、U1-OCR、U1-OCR-Med、GLM-5.2、Kimi K3。促销价格与可用模型会变化，应以登录后的订阅管理页面为准。

公开文档没有给出各模型“输入 Token、输出 Token、ASR 秒数、TTS 字符数分别折算多少 Credits”的完整换算表，也没有说明未用 Credits 是否结转。测试和上线前需要在控制台套餐详情确认：

- 当前订阅有效期与自动续费状态；
- 可用 Credits 和到期时间；
- 各模型 Credits 换算倍率；
- 是否存在账户级并发或日限额；
- Credits 用尽后的行为（拒绝请求还是切换其他付费资源）。

来源：[Token Plan 产品页](https://maas.unisound.com/token-plan)、[Token Plan 快速接入](https://maas.unisound.com/docs/token-plan/quickstart)。

## 关键错误码与处理建议

| HTTP/业务码 | 含义 | 建议 |
| --- | --- | --- |
| 400 / `100001` | 参数错误 | 记录响应体并检查字段类型和必填项 |
| 401 / `100101`、`100102` | 缺少或格式错误的鉴权头 | 检查 `Authorization: Bearer ...` |
| 401 / `100103` | Key 错误或已过期 | 轮换 Key，检查 Token Plan 有效期 |
| 401 / `100104` | 账户资源不足 | 检查 Credits/充值状态 |
| 401 / `100105` | 无模型权限 | 以 `/v1/models` 验证并联系支持 |
| 401 / `100106` | Key 被禁用 | 联系平台支持 |
| 429 / `100501` | RPM 限流 | 退避后重试，限制网关并发 |
| 500 / `100997`～`100999` | 超时或服务端错误 | 有界重试；持续失败时提交 `Trace-Id` |
| `202004` | 一句话 ASR 超过默认 25 秒 | 提前结束短句或改用实时/异步 ASR |
| `202005` | ASR 音频解码失败 | 核对编码、采样率和帧内容 |
| 400 / `220001` | TTS 音色错误 | 查询/更换有效 `voice_id` |

官方建议报障时提供响应 Header 中的 `Trace-Id`。网关日志应记录 `Trace-Id`、HTTP 状态、业务码、接口耗时和重试次数，但绝不能记录完整 Authorization Header。来源：[错误码](https://maas.unisound.com/docs/api/errorcode)。

## 推荐验证顺序

1. 立即在控制台轮换任何曾经公开粘贴过的 Key，并只通过当前 PowerShell 进程环境变量注入新 Key。
2. `GET /v1/models`：验证网络、TLS、Key、套餐有效期与模型权限。
3. 非流式 `POST /v1/chat/completions`：验证 U2 最小文本调用和 `usage`。
4. 流式 Chat：验证 chunk 拼接、结束标识、最终 usage 和中途断连处理。
5. 用 1～3 秒、16 kHz、单声道的已知 PCM/Opus 录音测试一句话 ASR，核对最终文本和端到端耗时。
6. 用十几个汉字创建 TTS 任务，轮询并下载 16 kHz PCM；使用 `ffprobe` 或本地解析校验采样率、声道、位宽，再交给 ESP32 播放。
7. 接入网关后做完整 ASR → LLM → TTS → ESP32 链路测试，分别统计每阶段和总耗时。
8. 最后验证错误路径：无效 Key、无权限模型、429、ASR 解码错误、TTS 任务失败和下载 URL 过期。

验收标准：成功请求不泄露 Key；模型列表与控制台一致；文本、ASR、TTS 均能分别通过；网关在 401/429/5xx 下不会无限重试；所有请求都有可关联的本地 request ID 与官方 `Trace-Id`；完整语音对话链路的阶段耗时可在 Web 日志中查看。
