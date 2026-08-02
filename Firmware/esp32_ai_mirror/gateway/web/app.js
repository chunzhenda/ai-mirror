const $ = (selector) => document.querySelector(selector);

const state = {
  busy: false,
  refreshTimer: null,
  refreshDebounce: null,
  eventSource: null,
  configLoaded: false,
  turnsByRecording: new Map(),
  logs: [],
};

const MAX_VISIBLE_CONVERSATIONS = 20;

const STATUS_LABELS = {
  pending: "等待处理",
  transcribing: "语音识别中",
  transcript_ready: "文字已识别",
  waiting_for_llm_config: "等待 LLM 配置",
  llm_generating: "AI 回复中",
  llm_ready: "AI 回复完成",
  tts_synthesizing: "语音合成中",
  playback_queued: "等待设备播放",
  playback_started: "ESP32 播放中",
  playback_completed: "播放完成",
  playback_aborted: "播放已打断",
  completed: "已完成",
  asr_disabled: "ASR 已禁用",
  asr_failed: "识别失败",
  llm_failed: "LLM 失败",
  tts_failed: "TTS 失败",
  playback_failed: "播放失败",
  failed: "处理失败",
  session_end_requested: "正在退出长聊天",
  session_end_queued: "退出命令已下发",
  session_end_failed: "退出长聊天失败",
};

function formatTime(value) {
  if (!value) return "未知";
  return new Intl.DateTimeFormat("zh-CN", {
    month: "2-digit",
    day: "2-digit",
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit",
  }).format(new Date(value));
}

function formatDuration(ms) {
  const seconds = Math.max(0, Number(ms || 0) / 1000);
  return `${seconds.toFixed(1)} 秒`;
}

function formatLatency(ms) {
  return ms === null || ms === undefined ? "—" : `${Number(ms)} ms`;
}

function node(tag, className, text) {
  const element = document.createElement(tag);
  if (className) element.className = className;
  if (text !== undefined) element.textContent = text;
  return element;
}

async function api(path, options = {}) {
  const response = await fetch(path, {
    cache: "no-store",
    ...options,
    headers: { "Content-Type": "application/json", ...(options.headers || {}) },
  });
  const payload = response.status === 204 ? null : await response.json();
  if (!response.ok) {
    throw new Error(payload?.error || `HTTP ${response.status}`);
  }
  return payload;
}

function renderDevices(devices) {
  const container = $("#devices");
  const list = $("#device-list");
  $("#device-count").textContent = String(devices.length);
  container.replaceChildren();
  list.replaceChildren();

  devices.forEach((device) => {
    const option = document.createElement("option");
    option.value = device.id;
    list.append(option);

    const lastSeen = new Date(device.last_seen).getTime();
    const online = Date.now() - lastSeen < 10_000;
    const item = node("div", "list-item");
    const title = node("div");
    title.append(node("strong", "", device.id));
    title.append(node("small", "", `最近通信 ${formatTime(device.last_seen)}`));
    item.append(
      title,
      node("span", `device-state ${online ? "online" : "offline"}`, online ? "在线" : "离线"),
    );
    container.append(item);
  });

  container.className = devices.length ? "list" : "list empty";
  if (!devices.length) container.textContent = "还没有设备连接";
}

function renderEvents(events) {
  const container = $("#events");
  $("#event-count").textContent = String(events.length);
  container.replaceChildren();
  events.slice(0, 12).forEach((event) => {
    const item = node("div", "event-item");
    const heading = node("div", "event-heading");
    heading.append(node("strong", "", event.type), node("time", "", formatTime(event.created_at)));
    item.append(heading, node("small", "", event.device_id));
    if (event.details && Object.keys(event.details).length) {
      item.append(node("code", "", JSON.stringify(event.details)));
    }
    container.append(item);
  });
  container.className = events.length ? "list" : "list empty";
  if (!events.length) container.textContent = "暂无设备事件";
}

function createRecordingCard(recording) {
  const card = node("article", "recording-card");
  card.dataset.recordingId = recording.id;
  const meta = node("div", "recording-meta");
  meta.append(node("strong", "recording-device"), node("span", "recording-summary"));
  const media = node("div", "recording-media");
  const audio = document.createElement("audio");
  audio.controls = true;
  audio.preload = "metadata";
  audio.src = recording.audio_url;
  const processButton = node("button", "compact process-recording", "识别并对话");
  processButton.type = "button";
  processButton.addEventListener("click", () => processRecording(recording.id));
  media.append(audio, processButton);
  card.append(meta, media);
  return card;
}

function updateRecordingCard(card, recording) {
  const turn = state.turnsByRecording.get(recording.id);
  card.querySelector(".recording-device").textContent = recording.device_id;
  card.querySelector(".recording-summary").textContent =
    `${formatDuration(recording.duration_ms)} · ${recording.sample_rate / 1000} kHz · ${formatTime(recording.created_at)}`;
  const button = card.querySelector(".process-recording");
  button.textContent = turn ? `状态：${STATUS_LABELS[turn.status] || turn.status}` : "识别并对话";
  button.disabled = Boolean(turn && ["transcribing", "llm_generating", "pending"].includes(turn.status));
}

function renderRecordings(recordings) {
  const container = $("#recordings");
  $("#recording-count").textContent = String(recordings.length);
  if (!recordings.length) {
    container.replaceChildren();
    container.className = "recordings empty";
    container.textContent = "暂无录音。设备录制结束后会自动上传。";
    return;
  }

  container.className = "recordings";
  const existingCards = new Map(
    [...container.querySelectorAll(".recording-card[data-recording-id]")].map((card) => [
      card.dataset.recordingId,
      card,
    ]),
  );
  if (!existingCards.size) container.replaceChildren();

  const activeIds = new Set(recordings.map((recording) => recording.id));
  recordings.forEach((recording, index) => {
    const card = existingCards.get(recording.id) || createRecordingCard(recording);
    updateRecordingCard(card, recording);
    const cardAtIndex = container.children[index] || null;
    if (cardAtIndex !== card) container.insertBefore(card, cardAtIndex);
  });
  existingCards.forEach((card, recordingId) => {
    if (!activeIds.has(recordingId)) card.remove();
  });
}

function renderConversations(turns) {
  const container = $("#conversations");
  const visibleTurns = turns.slice(0, MAX_VISIBLE_CONVERSATIONS);
  $("#conversation-count").textContent = String(visibleTurns.length);
  state.turnsByRecording = new Map(visibleTurns.map((turn) => [turn.recording_id, turn]));
  container.replaceChildren();

  visibleTurns.forEach((turn) => {
    const processingLatency = turn.processing_started_at ? turn.processing_latency_ms : null;
    const endToEndLatency = turn.processing_started_at ? turn.end_to_end_latency_ms : null;
    const card = node("article", "conversation-card");
    card.dataset.turnId = turn.id;
    const header = node("div", "conversation-header");
    const title = node("div");
    title.append(
      node("strong", "", turn.device_id),
      node("small", "", `${formatTime(turn.created_at)} · 处理 ${formatLatency(processingLatency)} · 端到端 ${formatLatency(endToEndLatency)}`),
    );
    const statusClass = ["asr_failed", "llm_failed", "tts_failed", "playback_failed", "session_end_failed", "failed"].includes(turn.status) ? "error" : turn.status;
    header.append(title, node("span", `turn-status ${statusClass}`, STATUS_LABELS[turn.status] || turn.status));

    const messages = node("div", "message-stack");
    const user = node("div", "message user-message");
    user.append(node("span", "message-role", "用户"), node("p", "", turn.transcript || "正在等待语音识别…"));
    const assistant = node("div", "message assistant-message");
    assistant.append(
      node("span", "message-role", "AI Mirror"),
      node("p", "", turn.assistant_response || (turn.status === "completed" ? "（空回复）" : "等待生成回复…")),
    );
    messages.append(user, assistant);

    const timings = node("div", "timing-grid");
    [
      ["ASR", turn.asr_latency_ms],
      ["LLM", turn.llm_latency_ms],
      ["TTS", turn.tts_latency_ms],
      ["后台处理", processingLatency],
      ["设备等待", turn.device_wait_latency_ms],
      ["播放", turn.playback_latency_ms ?? turn.audio_duration_ms],
      ["总耗时", endToEndLatency],
    ].forEach(([label, value]) => {
      const item = node("span", "timing-item");
      item.append(node("small", "", label), node("strong", "", formatLatency(value)));
      timings.append(item);
    });

    const footer = node("div", "conversation-footer");
    const usage = node(
      "small",
      "",
      `会话 ${turn.conversation_id.slice(0, 8)} · 录音 ${turn.recording_id.slice(0, 8)} · Token ${turn.input_tokens || 0}/${turn.output_tokens || 0}`,
    );
    const retry = node("button", "compact", "重新处理");
    retry.type = "button";
    retry.addEventListener("click", () => retryTurn(turn.id));
    footer.append(usage, retry);
    if (turn.error_message) footer.prepend(node("span", "turn-error", turn.error_message));
    card.append(header, messages, timings, footer);
    container.append(card);
  });

  container.className = visibleTurns.length ? "conversations" : "conversations empty";
  if (!visibleTurns.length) container.textContent = "暂无对话。完成一次录音后会自动开始处理。";
}

function renderLogs() {
  const level = $("#log-level").value;
  const logs = level === "ALL" ? state.logs : state.logs.filter((entry) => entry.level === level);
  const container = $("#logs");
  $("#log-count").textContent = String(logs.length);
  container.replaceChildren();
  logs.slice(0, 200).forEach((entry) => {
    const item = node("div", `log-entry level-${entry.level.toLowerCase()}`);
    const heading = node("div", "log-heading");
    heading.append(
      node("span", "log-level", entry.level),
      node("strong", "", entry.component),
      node("time", "", formatTime(entry.created_at)),
    );
    item.append(heading, node("p", "", entry.message));
    if (entry.details && Object.keys(entry.details).length) {
      item.append(node("code", "", JSON.stringify(entry.details, null, 2)));
    }
    container.append(item);
  });
  container.className = logs.length ? "logs" : "logs empty";
  if (!logs.length) container.textContent = "当前筛选条件下没有日志";
}

function updateProviderSections() {
  const asrCustom = $("#asr-provider").value === "custom_http";
  const asrLocal = $("#asr-provider").value === "sensevoice_local";
  const llmDeepseek = $("#llm-provider").value === "deepseek";
  const ttsCustom = $("#tts-provider").value === "custom_http";
  const ttsEdge = $("#tts-provider").value === "edge_tts";
  $("[data-provider-section='asr']").hidden = !asrCustom;
  $("[data-provider-section='sensevoice']").hidden = !asrLocal;
  $("[data-provider-section='deepseek']").hidden = !llmDeepseek;
  $("[data-provider-section='tts']").hidden = !ttsCustom;
  $("[data-provider-section='edge-tts']").hidden = !ttsEdge;
}

function asrProviderLabel(provider, sensevoiceLoaded = false) {
  if (provider === "unisound") return "云知声 U2-ASR";
  if (provider === "custom_http") return "自定义 ASR";
  return sensevoiceLoaded ? "SenseVoice 已加载" : "SenseVoice 等待首次使用";
}

function llmProviderLabel(provider) {
  return provider === "unisound" ? "云知声 U2" : "DeepSeek";
}

function populateConfig(config) {
  $("#asr-provider").value = config.asr_provider;
  $("#asr-model").value = config.asr_model;
  $("#asr-language").value = config.asr_language;
  $("#asr-device").value = config.asr_device;
  $("#asr-enabled").checked = config.asr_enabled;
  $("#asr-itn").checked = config.asr_use_itn;
  $("#streaming-asr-enabled").checked = config.streaming_asr_enabled;
  $("#stream-partial-interval").value = config.stream_partial_interval_ms;
  $("#asr-api-url").value = config.asr_api_url;
  $("#asr-api-model").value = config.asr_api_model;
  $("#asr-api-timeout").value = config.asr_api_timeout_seconds;
  $("#asr-api-key").value = "";
  $("#asr-api-key").placeholder = config.asr_api_key_configured
    ? "已配置；留空不会修改"
    : "输入自定义 ASR API Key（可选）";
  $("#unisound-base-url").value = config.unisound_base_url;
  $("#unisound-asr-model").value = config.unisound_asr_model;
  $("#unisound-llm-model").value = config.unisound_llm_model;
  $("#unisound-tts-model").value = config.unisound_tts_model;
  $("#unisound-tts-voice").value = config.unisound_tts_voice;
  $("#unisound-timeout").value = config.unisound_timeout_seconds;
  $("#unisound-poll-interval").value = config.unisound_poll_interval_ms;
  $("#unisound-api-key").value = "";
  $("#unisound-api-key").placeholder = config.unisound_api_key_configured
    ? "已配置；留空不会修改"
    : "输入 Token Plan API Key";
  $("#llm-provider").value = config.llm_provider;
  $("#llm-base-url").value = config.llm_base_url;
  $("#llm-model").value = config.llm_model;
  $("#llm-temperature").value = config.llm_temperature;
  $("#llm-max-tokens").value = config.llm_max_tokens;
  $("#llm-timeout").value = config.llm_timeout_seconds;
  $("#history-turns").value = config.history_turns;
  $("#system-prompt").value = config.system_prompt;
  $("#llm-enabled").checked = config.llm_enabled;
  $("#llm-thinking").checked = config.llm_thinking;
  $("#tts-enabled").checked = config.tts_enabled;
  $("#tts-sentence-streaming-enabled").checked = config.tts_sentence_streaming_enabled;
  $("#tts-sentence-max-chars").value = config.tts_sentence_max_chars;
  $("#tts-provider").value = config.tts_provider;
  $("#tts-voice").value = config.tts_voice;
  $("#tts-rate").value = config.tts_rate;
  $("#tts-volume").value = config.tts_volume;
  $("#tts-api-url").value = config.tts_api_url;
  $("#tts-api-model").value = config.tts_api_model;
  $("#tts-api-timeout").value = config.tts_api_timeout_seconds;
  $("#tts-prompt").value = config.tts_prompt;
  $("#tts-api-key").value = "";
  $("#tts-api-key").placeholder = config.tts_api_key_configured
    ? "已配置；留空不会修改"
    : "输入自定义 TTS API Key（可选）";
  $("#auto-process").checked = config.auto_process;
  $("#llm-api-key").value = "";
  $("#llm-api-key").placeholder = config.llm_api_key_configured
    ? "已配置；留空不会修改"
    : "输入 DeepSeek API Key";
  const worker = config.worker_running ? "后台线程运行中" : "后台线程未启动";
  const model = asrProviderLabel(config.asr_provider, config.sensevoice_loaded);
  const llm = llmProviderLabel(config.llm_provider);
  const unisound = config.unisound_api_key_configured ? "云知声 Key 已配置" : "云知声 Key 未配置";
  const key = config.llm_provider === "deepseek"
    ? `${unisound} · ${config.llm_api_key_configured ? "DeepSeek Key 已配置" : "DeepSeek Key 未配置"}`
    : unisound;
  $("#ai-status").textContent = `${worker} · ${model} · ${llm} · ${key}`;
  updateProviderSections();
  state.configLoaded = true;
}

async function loadConfig(force = false) {
  if (state.configLoaded && !force) return;
  populateConfig(await api("/api/config"));
}

async function refresh() {
  if (state.busy) return;
  state.busy = true;
  try {
    const [health, devices, recordings, events, conversations, logs] = await Promise.all([
      api("/health"),
      api("/api/devices"),
      api("/api/recordings"),
      api("/api/events"),
      api(`/api/conversations?limit=${MAX_VISIBLE_CONVERSATIONS}`),
      api("/api/logs?limit=200"),
    ]);
    $("#gateway-status").className = "status-pill online";
    $("#gateway-status").textContent = `网关在线 · ${formatTime(health.time)} · SSE`;
    const worker = health.ai_worker_running ? "后台线程运行中" : "后台线程未启动";
    const model = asrProviderLabel(health.asr_provider, health.sensevoice_loaded);
    const llm = llmProviderLabel(health.llm_provider);
    const unisound = health.unisound_api_key_configured ? "云知声 Key 已配置" : "云知声 Key 未配置";
    const key = health.llm_provider === "deepseek"
      ? `${unisound} · ${health.llm_api_key_configured ? "DeepSeek Key 已配置" : "DeepSeek Key 未配置"}`
      : unisound;
    $("#ai-status").textContent = `${worker} · ${model} · ${llm} · ${key}`;
    renderDevices(devices.devices);
    renderConversations(conversations.turns);
    renderRecordings(recordings.recordings);
    renderEvents(events.events);
    state.logs = logs.logs;
    renderLogs();
    await loadConfig();
  } catch (error) {
    $("#gateway-status").className = "status-pill offline";
    $("#gateway-status").textContent = `连接失败：${error.message}`;
  } finally {
    state.busy = false;
  }
}

function scheduleRefresh() {
  clearTimeout(state.refreshDebounce);
  state.refreshDebounce = setTimeout(refresh, 120);
}

function connectSSE() {
  if (state.eventSource) state.eventSource.close();
  const stream = new EventSource("/api/stream");
  state.eventSource = stream;
  stream.addEventListener("ready", () => {
    $("#gateway-status").className = "status-pill online";
    scheduleRefresh();
  });
  stream.addEventListener("update", (event) => {
    try {
      const payload = JSON.parse(event.data);
      if (payload.type === "config_updated") loadConfig(true);
    } catch (_) {
      // A malformed live event must not disable the polling fallback.
    }
    scheduleRefresh();
  });
  stream.onerror = () => {
    $("#gateway-status").textContent = "SSE 重连中，轮询仍在工作";
  };
}

async function sendCommand(commandType) {
  const deviceId = $("#device-id").value.trim();
  const result = $("#command-result");
  if (!/^[A-Za-z0-9._-]{1,64}$/.test(deviceId)) {
    result.textContent = "设备 ID 只能包含字母、数字、点、下划线和连字符";
    result.className = "command-result error";
    return;
  }
  result.textContent = `正在发送 ${commandType}…`;
  result.className = "command-result";
  try {
    const command = await api(`/api/devices/${encodeURIComponent(deviceId)}/commands`, {
      method: "POST",
      body: JSON.stringify({ type: commandType }),
    });
    result.textContent = `已排队：${command.type}（${command.id.slice(0, 8)}）`;
    result.className = "command-result success";
    scheduleRefresh();
  } catch (error) {
    result.textContent = `发送失败：${error.message}`;
    result.className = "command-result error";
  }
}

async function processRecording(recordingId) {
  try {
    await api(`/api/recordings/${recordingId}/process`, { method: "POST" });
    scheduleRefresh();
  } catch (error) {
    $("#command-result").textContent = `处理录音失败：${error.message}`;
    $("#command-result").className = "command-result error";
  }
}

async function retryTurn(turnId) {
  try {
    await api(`/api/conversations/${turnId}/retry`, { method: "POST" });
    scheduleRefresh();
  } catch (error) {
    $("#command-result").textContent = `重新处理失败：${error.message}`;
    $("#command-result").className = "command-result error";
  }
}

async function saveConfig(event) {
  event.preventDefault();
  const result = $("#config-result");
  const payload = {
    auto_process: $("#auto-process").checked,
    asr_enabled: $("#asr-enabled").checked,
    asr_provider: $("#asr-provider").value,
    asr_model: $("#asr-model").value.trim(),
    asr_language: $("#asr-language").value,
    asr_device: $("#asr-device").value,
    asr_use_itn: $("#asr-itn").checked,
    streaming_asr_enabled: $("#streaming-asr-enabled").checked,
    stream_partial_interval_ms: Number($("#stream-partial-interval").value),
    asr_api_url: $("#asr-api-url").value.trim(),
    asr_api_model: $("#asr-api-model").value.trim(),
    asr_api_timeout_seconds: Number($("#asr-api-timeout").value),
    unisound_base_url: $("#unisound-base-url").value.trim(),
    unisound_asr_model: $("#unisound-asr-model").value.trim(),
    unisound_llm_model: $("#unisound-llm-model").value.trim(),
    unisound_tts_model: $("#unisound-tts-model").value.trim(),
    unisound_tts_voice: $("#unisound-tts-voice").value.trim(),
    unisound_timeout_seconds: Number($("#unisound-timeout").value),
    unisound_poll_interval_ms: Number($("#unisound-poll-interval").value),
    llm_enabled: $("#llm-enabled").checked,
    llm_provider: $("#llm-provider").value,
    llm_base_url: $("#llm-base-url").value.trim(),
    llm_model: $("#llm-model").value.trim(),
    llm_thinking: $("#llm-thinking").checked,
    llm_temperature: Number($("#llm-temperature").value),
    llm_max_tokens: Number($("#llm-max-tokens").value),
    llm_timeout_seconds: Number($("#llm-timeout").value),
    history_turns: Number($("#history-turns").value),
    system_prompt: $("#system-prompt").value.trim(),
    tts_enabled: $("#tts-enabled").checked,
    tts_sentence_streaming_enabled: $("#tts-sentence-streaming-enabled").checked,
    tts_sentence_max_chars: Number($("#tts-sentence-max-chars").value),
    tts_provider: $("#tts-provider").value,
    tts_voice: $("#tts-voice").value.trim(),
    tts_rate: Number($("#tts-rate").value),
    tts_volume: Number($("#tts-volume").value),
    tts_api_url: $("#tts-api-url").value.trim(),
    tts_api_model: $("#tts-api-model").value.trim(),
    tts_api_timeout_seconds: Number($("#tts-api-timeout").value),
    tts_prompt: $("#tts-prompt").value.trim(),
  };
  const asrApiKey = $("#asr-api-key").value.trim();
  if (asrApiKey) payload.asr_api_key = asrApiKey;
  const unisoundApiKey = $("#unisound-api-key").value.trim();
  if (unisoundApiKey) payload.unisound_api_key = unisoundApiKey;
  const apiKey = $("#llm-api-key").value.trim();
  if (apiKey) payload.llm_api_key = apiKey;
  const ttsApiKey = $("#tts-api-key").value.trim();
  if (ttsApiKey) payload.tts_api_key = ttsApiKey;
  result.textContent = "正在保存模型配置…";
  result.className = "command-result";
  try {
    const config = await api("/api/config", { method: "POST", body: JSON.stringify(payload) });
    populateConfig(config);
    result.textContent = "模型配置已保存；等待中的任务会自动继续";
    result.className = "command-result success";
    scheduleRefresh();
  } catch (error) {
    result.textContent = `保存失败：${error.message}`;
    result.className = "command-result error";
  }
}

async function clearCustomApiKey(kind) {
  const label = kind.toUpperCase();
  if (!window.confirm(`确认清除 ${label} 自定义 API Key？`)) return;
  try {
    const config = await api("/api/config", {
      method: "POST",
      body: JSON.stringify({ [`clear_${kind}_api_key`]: true }),
    });
    populateConfig(config);
    $("#config-result").textContent = `${label} 自定义 API Key 已清除`;
    $("#config-result").className = "command-result success";
  } catch (error) {
    $("#config-result").textContent = `清除失败：${error.message}`;
    $("#config-result").className = "command-result error";
  }
}

async function clearApiKey() {
  if (!window.confirm("确认清除本机保存的 DeepSeek API Key？")) return;
  try {
    const config = await api("/api/config", {
      method: "POST",
      body: JSON.stringify({ clear_llm_api_key: true }),
    });
    populateConfig(config);
    $("#config-result").textContent = "DeepSeek API Key 已清除";
    $("#config-result").className = "command-result success";
  } catch (error) {
    $("#config-result").textContent = `清除失败：${error.message}`;
    $("#config-result").className = "command-result error";
  }
}

async function clearUnisoundApiKey() {
  if (!window.confirm("确认清除本机保存的云知声 API Key？")) return;
  try {
    const config = await api("/api/config", {
      method: "POST",
      body: JSON.stringify({ clear_unisound_api_key: true }),
    });
    populateConfig(config);
    $("#config-result").textContent = "云知声 API Key 已清除";
    $("#config-result").className = "command-result success";
  } catch (error) {
    $("#config-result").textContent = `清除失败：${error.message}`;
    $("#config-result").className = "command-result error";
  }
}

document.querySelectorAll("[data-command]").forEach((button) => {
  button.addEventListener("click", () => sendCommand(button.dataset.command));
});
$("#model-config").addEventListener("submit", saveConfig);
$("#asr-provider").addEventListener("change", updateProviderSections);
$("#llm-provider").addEventListener("change", updateProviderSections);
$("#tts-provider").addEventListener("change", updateProviderSections);
$("#clear-asr-api-key").addEventListener("click", () => clearCustomApiKey("asr"));
$("#clear-tts-api-key").addEventListener("click", () => clearCustomApiKey("tts"));
$("#clear-api-key").addEventListener("click", clearApiKey);
$("#clear-unisound-api-key").addEventListener("click", clearUnisoundApiKey);
$("#log-level").addEventListener("change", renderLogs);

refresh();
connectSSE();
state.refreshTimer = setInterval(refresh, 10_000);
