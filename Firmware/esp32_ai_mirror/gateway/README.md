# AI Mirror 本地语音网关

本服务负责完整语音聊天链路：ESP32 上传 16 kHz 单声道 PCM，网关默认
使用云知声 `u2-asr` 识别文字，调用云知声 `u2` 生成回答，再通过云知声
`u2-tts` 合成中文语音，最终把 16-bit PCM 下发给 ESP32 播放。Web 中仍可
切换 DeepSeek、本地 SenseVoiceSmall、Edge TTS 或自定义 HTTP API。

## 启动

需要 Python 3.10+、`ffmpeg`，以及与运行环境匹配的 PyTorch。Windows 示例：

```powershell
E:\python\amaconda3\python.exe -m venv gateway\.venv --system-site-packages
gateway\.venv\Scripts\python.exe -m pip install -r gateway\requirements.txt
gateway\.venv\Scripts\python.exe -m gateway.src.app --host 0.0.0.0 --port 8000 --token ai-mirror-dev
```

可选使用环境变量注入密钥：

```powershell
$env:UNISOUND_API_KEY = '<Token Plan API Key>'
$env:DEEPSEEK_API_KEY = '<DeepSeek API Key>'
```

也可在 Web 配置页填写。密钥只保存在网关 SQLite，配置 API 只返回
`已配置` 状态，不会返回明文或下发给 ESP32。

访问 `http://192.168.10.100:8000`。云知声 API Key、ASR/LLM/TTS 模型、TTS 音色、
语速、音量和 DeepSeek 备用配置都可以在 Web 页面修改。切换到本地 SenseVoice 时，首次
识别会下载并加载 `iic/SenseVoiceSmall`，速度比后续调用慢。

## 目录结构

```text
gateway/
├─ src/              # 网关服务端源码
│  ├─ app.py         # HTTP/WebSocket 服务与 API
│  └─ ai_services.py # ASR、LLM、TTS、SQLite 和任务队列
├─ tests/            # 网关自动化测试
│  └─ test_gateway.py
├─ web/              # Web 管理界面
├─ data/             # 录音、TTS、SQLite 数据
├─ docs/             # 网关相关资料
├─ requirements.txt
└─ .venv/
```

从项目根目录使用模块方式启动网关，确保源码包能正确加载：

```powershell
gateway\.venv\Scripts\python.exe -m gateway.src.app --host 0.0.0.0 --port 8000 --token ai-mirror-dev
```

数据保存在：

- 录音：`gateway/data/recordings/`
- TTS PCM：`gateway/data/tts/`
- 配置、对话和日志：`gateway/data/gateway.sqlite3`

## ESP32 配置

在 `idf.py menuconfig` 的 `AI Mirror Configuration` 中设置：

- `Enable local gateway communication`：启用
- `Local gateway base URL`：`http://192.168.10.100:8000`
- `Gateway device ID`：`ai-mirror-001`
- `Gateway device token`：与网关的 `--token` 一致

录音结束后固件通过 WebSocket 上传 PCM。网关处理完 LLM 回答后下发
`play_audio` 命令；固件把 PCM 下载到 PSRAM，通过 ES8311/I2S 播放，并回传
`playback_started`、`playback_completed` 或失败事件。

当前固件优先使用同一 WebSocket 的流式协议：开始录音发送
`stream_session_start`，随后发送多个 PCM 二进制 chunk，结束时发送
`stream_session_end`。网关会在本地 SenseVoice 或自定义 ASR 模式下按间隔提交局部
ASR；云知声异步 ASR 仍在结束后做最终识别。连接或发送失败时自动回退到原来的整段
录音上传。LLM 回复默认按标点分句，首句 TTS 完成后立即下发，后续句子通过带有
`/api/tts/{turn_id}/chunk/{index}/pcm` 的 `play_audio` 命令排队播放。

固件使用连续会话模式：第一次通过唤醒词或 BTN1 开始，AI 播放结束后进入
60 秒 VAD 监听窗口；检测到连续人声会带 400 ms 预录音频自动开始下一轮，
不再要求重复唤醒。等待网关回答最多 120 秒，超时后回到唤醒词模式。
同一轮会话上传相同 `session_id` 和递增的 `turn_index`，网关只在该会话内
组装 LLM 历史上下文。

跟进监听使用“连续 VAD + 回放保护窗 + 环境噪声能量门”抑制误触发。ASR
如果只得到纯标点或“嗯/啊”等无意义短词，网关不会调用 LLM，而会下发
`continue_listening` 让 ESP32 保持当前会话并继续监听；ASR/LLM/TTS 失败时
也使用相同恢复路径，避免设备一直卡在等待回复状态。

长聊天中可以直接说“退出聊天模式”“结束长聊天”“停止对话”或“退出会话”。
ASR 识别到这些明确命令后，网关会跳过 LLM/TTS 并下发 `end_conversation`；
ESP32 播放结束提示音后立即回到唤醒词模式。命令允许“请/请你/麻烦你”等
前缀以及“吧/一下/谢谢”等后缀，但不会把“再见”“不聊了”视为退出命令，
以减少普通对话中的误触发。`/api/capabilities` 通过
`voice_end_conversation=true` 声明该能力。

## 自定义 ASR/TTS HTTP API

Web 页面可以分别选择本地/默认提供方或 `自定义 HTTP API`，并配置 URL、
API Key、模型和超时。API Key 只保存在 SQLite，不通过 Web API 明文返回。

自定义 ASR 契约：网关向配置的 URL 发送 `multipart/form-data`：

- `file`：WAV 音频
- `model`、`language`、`response_format=json`
- 可选 `Authorization: Bearer <ASR API Key>`
- 响应：`{"text":"识别文字"}` 或 `{"result":{"text":"识别文字"}}`

自定义 TTS 契约：网关发送 JSON：

```json
{
  "model": "模型名称",
  "voice": "声音名称",
  "input": "LLM 回复",
  "prompt": "Web 中配置的 TTS 提示词",
  "response_format": "wav",
  "sample_rate": 16000
}
```

TTS 可以直接返回 WAV/MP3 等音频二进制；也可以返回
`{"audio_base64":"...","format":"wav"}`。若返回原始 PCM，格式应为
`pcm_s16le`，或使用 `audio/pcm`/`audio/L16` Content-Type。网关最终统一转成
16 kHz mono signed-16 PCM。

## 云知声 MaaS 接入

默认 provider 为 `unisound`，公共 Base URL 为
`https://maas-api.unisound.com/v1`。当前默认模型依次是 `u2-asr`、`u2` 和
`u2-tts`，默认音色是 `cn_male_chenyu`。

ASR 流程：

1. 以 `purpose=a2t_async_input` 上传网关保存的 WAV。
2. 创建 `/audio/asr/tasks` 任务并轮询结果。
3. 拼接转写分段，并尽力删除云端输入文件。

LLM 流程：使用 OpenAI 兼容的 `/chat/completions` 接口调用 `u2`。Web 可切换
到 DeepSeek；两套模型、密钥和 Base URL 配置相互独立并同时保留。

TTS 流程：

1. 创建 `/audio/speech/tasks` 异步合成任务。
2. 轮询成功后下载 16 kHz 单声道 MP3。
3. 通过 `ffmpeg` 转换为 ESP32 需要的 16 kHz mono signed-16 PCM。

TTS 生成文件的 `file_id` 不能直接用于 ASR；如需做 TTS→ASR 闭环，
必须先下载，再以 `a2t_async_input` 目的重新上传。

方案 C 的连续流接口已经启用：固件通过 `stream_begin/write/end` 在同一录音
WebSocket 上上传多个 PCM chunk。网关 `/api/capabilities` 会报告
`continuous_streaming=true`；SenseVoice 或自定义 ASR 可返回 `stream_partial`，
云知声异步 ASR 则在流结束后执行最终识别。流式失败时固件回退到整段上传。

## 耗时口径

Web 对每轮聊天展示：

- `ASR`：当前选中的云知声、SenseVoice 或自定义 ASR 处理时间
- `LLM`：当前选中的云知声 U2 或 DeepSeek 返回完整回答的时间
- `TTS`：语音合成和转码时间
- `处理耗时`：后台开始处理至播放命令入队
- `设备等待`：播放命令入队至 ESP32 开始播放（包含轮询和 PCM 下载）
- `播放`：ESP32 实际播放时间
- `总耗时`：后台开始处理至 ESP32 播放完成

这些时间保存到 SQLite，并同步显示在 Web 对话卡片和日志中。Web 对话区域只
展示最新 20 条，较早记录仍保留在 SQLite 和录音目录中。

## 测试

```powershell
gateway\.venv\Scripts\python.exe -m unittest discover -s gateway\tests -p "test_*.py"
idf.py build
idf.py -p COM10 flash monitor
```

## 主要 API

| 方法 | 路径 | 用途 |
| --- | --- | --- |
| `GET` | `/health` | 网关健康检查 |
| `GET` | `/api/capabilities` | 录音协议与连续流能力 |
| `GET` | `/api/recordings` | 录音列表 |
| `GET` | `/api/recordings/{id}/audio` | 播放 WAV |
| `POST` | `/api/recordings/{id}/process` | 处理或重新处理录音 |
| `GET/POST` | `/api/config` | 模型和 TTS 配置 |
| `GET` | `/api/conversations` | 对话和耗时 |
| `GET` | `/api/logs` | 运行日志 |
| `GET` | `/api/stream` | SSE 实时更新 |
| `GET` | `/api/tts/{turn_id}/pcm` | ESP32 下载 LLM 回复音频 |
| `GET` | `/api/devices/{id}/commands` | ESP32 长轮询命令 |
| `POST` | `/api/devices/{id}/events` | ESP32 回传事件和播放耗时 |

ESP32 API 使用 `X-Device-Token`。当前 Web 管理接口面向可信局域网，若暴露到
更大网络，应补充登录鉴权、HTTPS 和访问控制。
