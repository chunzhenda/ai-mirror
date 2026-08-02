"""Conversation persistence and asynchronous ASR/LLM processing for the gateway."""

from __future__ import annotations

import base64
from concurrent.futures import Future, ThreadPoolExecutor
import json
import os
import queue
import re
import sqlite3
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
import uuid
import wave
from collections.abc import Callable
from datetime import datetime, timezone
from pathlib import Path
from typing import Any
from urllib.parse import urlencode, urlparse


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds")


_TRANSCRIPT_FILLERS = {
    "嗯",
    "嗯嗯",
    "啊",
    "哦",
    "噢",
    "呃",
    "额",
    "唔",
}

_CONVERSATION_END_COMMANDS = {
    f"{verb}{subject}{mode}"
    for verb in ("退出", "结束", "停止")
    for subject in ("聊天", "长聊天", "对话", "会话")
    for mode in ("", "模式")
}
_VOICE_COMMAND_PREFIXES = ("麻烦你", "请你", "麻烦", "请")
_VOICE_COMMAND_SUFFIXES = ("谢谢你", "谢谢", "一下", "吧")

_TTS_SILENT_FORMAT_MARKS = str.maketrans("", "", "《》〈〉*＊")


def _is_meaningful_transcript(transcript: str) -> bool:
    """Reject punctuation-only and common one-syllable VAD false positives."""
    compact = re.sub(r"\s+", "", transcript)
    content = re.sub(r"[^0-9A-Za-z\u3400-\u9fff]", "", compact)
    return bool(content) and content not in _TRANSCRIPT_FILLERS


def _is_end_conversation_command(transcript: str) -> bool:
    """Match explicit voice commands while rejecting conversational mentions."""
    command = re.sub(r"[^0-9A-Za-z\u3400-\u9fff]", "", transcript).lower()
    for prefix in _VOICE_COMMAND_PREFIXES:
        if command.startswith(prefix):
            command = command[len(prefix) :]
            break
    while command:
        suffix = next(
            (item for item in _VOICE_COMMAND_SUFFIXES if command.endswith(item)), None
        )
        if suffix is None:
            break
        command = command[: -len(suffix)]
    return command in _CONVERSATION_END_COMMANDS


def _prepare_tts_text(text: str) -> str:
    """Remove visual-only formatting marks before sending LLM text to TTS."""
    cleaned = text.translate(_TTS_SILENT_FORMAT_MARKS)
    cleaned = re.sub(r"[ \t]+", " ", cleaned)
    cleaned = re.sub(r" *\n+ *", "\n", cleaned)
    return cleaned.strip()


def split_tts_sentences(text: str, max_chars: int = 80) -> list[str]:
    """Split a cleaned answer into short, speech-friendly TTS chunks."""
    cleaned = _prepare_tts_text(text)
    if not cleaned:
        return []
    max_chars = max(16, min(int(max_chars), 240))
    rough = [part.strip() for part in re.split(r"(?<=[。！？!?；;])|[\r\n]+", cleaned)]
    chunks: list[str] = []
    pending = ""
    for part in rough:
        if not part:
            continue
        if (
            pending
            and len(pending) + len(part) <= max_chars
            and not re.search(r"[。！？!?；;]$", pending)
        ):
            pending += part
        elif pending:
            chunks.append(pending)
            pending = part
        else:
            pending = part
        while len(pending) > max_chars:
            chunks.append(pending[:max_chars])
            pending = pending[max_chars:]
    if pending:
        chunks.append(pending)
    return chunks


def limit_voice_reply(text: str, max_sentences: int = 2, max_chars: int = 160) -> str:
    """Keep spoken output short while leaving the full text visible in the Web UI."""
    chunks = split_tts_sentences(text, max_chars=max_chars)
    if not chunks:
        return ""
    return " ".join(chunks[: max(1, min(int(max_sentences), 2))])


def extract_stream_sentences(
    buffer: str, max_chars: int = 80, final: bool = False
) -> tuple[list[str], str]:
    """Return complete speech chunks and retain the unfinished LLM tail."""
    cleaned = _prepare_tts_text(buffer)
    if not cleaned:
        return [], ""
    if final:
        return split_tts_sentences(cleaned, max_chars=max_chars), ""
    boundary = [
        match.end()
        for match in re.finditer(r"(?<=[。！？!?；;])|[\r\n]+", cleaned)
    ]
    if not boundary:
        if len(cleaned) <= max_chars:
            return [], cleaned
        return split_tts_sentences(cleaned[:max_chars], max_chars=max_chars), cleaned[max_chars:]
    cut = max(boundary)
    ready = cleaned[:cut]
    tail = cleaned[cut:]
    return split_tts_sentences(ready, max_chars=max_chars), tail


def safe_console_print(message: str) -> None:
    """Write logs even when a Windows launcher exposes a legacy console encoding."""
    encoding = getattr(sys.stdout, "encoding", None) or "utf-8"
    safe_message = message.encode(encoding, errors="backslashreplace").decode(encoding)
    print(safe_message, flush=True)


DEFAULT_CONFIG: dict[str, Any] = {
    "auto_process": True,
    "asr_enabled": True,
    "asr_provider": "unisound",
    "asr_model": "iic/SenseVoiceSmall",
    "asr_language": "zh",
    "asr_device": "cpu",
    "asr_use_itn": True,
    "asr_api_url": "",
    "asr_api_key": "",
    "asr_api_model": "",
    "asr_api_timeout_seconds": 60,
    "streaming_asr_enabled": True,
    "stream_partial_interval_ms": 900,
    "unisound_base_url": "https://maas-api.unisound.com/v1",
    "unisound_api_key": "",
    "unisound_asr_model": "u2-asr",
    "unisound_llm_model": "u2",
    "unisound_tts_model": "u2-tts",
    "unisound_tts_voice": "cn_male_chenyu",
    "unisound_timeout_seconds": 120,
    "unisound_poll_interval_ms": 500,
    "llm_enabled": True,
    "llm_provider": "unisound",
    "llm_base_url": "https://api.deepseek.com",
    "llm_model": "deepseek-v4-flash",
    "llm_api_key": "",
    "llm_thinking": False,
    "llm_streaming_enabled": True,
    "llm_temperature": 0.6,
    "llm_max_tokens": 512,
    "llm_timeout_seconds": 60,
    "history_turns": 6,
    "tts_enabled": True,
    "tts_provider": "unisound",
    "tts_voice": "zh-CN-XiaoxiaoNeural",
    "tts_rate": 0,
    "tts_volume": 0,
    "tts_api_url": "",
    "tts_api_key": "",
    "tts_api_model": "",
    "tts_api_timeout_seconds": 60,
    "tts_prompt": "请使用自然、亲切、适合日常对话的语气朗读。",
    "tts_sentence_streaming_enabled": True,
    "tts_sentence_max_chars": 80,
    "tts_streaming_audio_enabled": True,
    "voice_reply_max_sentences": 2,
    "voice_reply_max_chars": 160,
    "command_websocket_enabled": True,
    "system_prompt": (
        "你是 AI Mirror 的语音助手。请使用简洁、自然的中文回答，"
        "优先给出直接结论，适合在语音交互界面中阅读。"
    ),
}

SECRET_CONFIG_KEYS = (
    "asr_api_key",
    "llm_api_key",
    "tts_api_key",
    "unisound_api_key",
)
PUBLIC_CONFIG_KEYS = tuple(key for key in DEFAULT_CONFIG if key not in SECRET_CONFIG_KEYS)
TURN_UPDATE_FIELDS = {
    "status",
    "transcript",
    "assistant_response",
    "asr_provider",
    "asr_model",
    "llm_provider",
    "llm_model",
    "asr_latency_ms",
    "llm_latency_ms",
    "tts_provider",
    "tts_voice",
    "tts_latency_ms",
    "audio_duration_ms",
    "processing_started_at",
    "processing_latency_ms",
    "playback_command_id",
    "playback_queued_at",
    "playback_started_at",
    "playback_completed_at",
    "device_wait_latency_ms",
    "playback_latency_ms",
    "end_to_end_latency_ms",
    "input_tokens",
    "output_tokens",
    "error_message",
}


class GatewayDatabase:
    def __init__(self, data_dir: Path) -> None:
        self.data_dir = data_dir.resolve()
        self.data_dir.mkdir(parents=True, exist_ok=True)
        self.path = self.data_dir / "gateway.sqlite3"
        self._lock = threading.RLock()
        self._connection = sqlite3.connect(self.path, check_same_thread=False)
        self._connection.row_factory = sqlite3.Row
        self._connection.execute("PRAGMA journal_mode=WAL")
        self._connection.execute("PRAGMA foreign_keys=ON")
        self._create_schema()
        self._seed_config()

    def _create_schema(self) -> None:
        with self._lock, self._connection:
            self._connection.executescript(
                """
                CREATE TABLE IF NOT EXISTS settings (
                    key TEXT PRIMARY KEY,
                    value_json TEXT NOT NULL,
                    updated_at TEXT NOT NULL
                );

                CREATE TABLE IF NOT EXISTS conversation_turns (
                    id TEXT PRIMARY KEY,
                    conversation_id TEXT NOT NULL,
                    device_id TEXT NOT NULL,
                    recording_id TEXT NOT NULL UNIQUE,
                    status TEXT NOT NULL,
                    transcript TEXT NOT NULL DEFAULT '',
                    assistant_response TEXT NOT NULL DEFAULT '',
                    asr_provider TEXT NOT NULL DEFAULT '',
                    asr_model TEXT NOT NULL DEFAULT '',
                    llm_provider TEXT NOT NULL DEFAULT '',
                    llm_model TEXT NOT NULL DEFAULT '',
                    asr_latency_ms INTEGER,
                    llm_latency_ms INTEGER,
                    tts_provider TEXT NOT NULL DEFAULT '',
                    tts_voice TEXT NOT NULL DEFAULT '',
                    tts_latency_ms INTEGER,
                    audio_duration_ms INTEGER,
                    processing_started_at TEXT,
                    processing_latency_ms INTEGER,
                    playback_command_id TEXT NOT NULL DEFAULT '',
                    playback_queued_at TEXT,
                    playback_started_at TEXT,
                    playback_completed_at TEXT,
                    device_wait_latency_ms INTEGER,
                    playback_latency_ms INTEGER,
                    end_to_end_latency_ms INTEGER,
                    input_tokens INTEGER NOT NULL DEFAULT 0,
                    output_tokens INTEGER NOT NULL DEFAULT 0,
                    error_message TEXT NOT NULL DEFAULT '',
                    created_at TEXT NOT NULL,
                    updated_at TEXT NOT NULL
                );

                CREATE INDEX IF NOT EXISTS idx_turns_device_created
                ON conversation_turns(device_id, created_at DESC);

                CREATE TABLE IF NOT EXISTS gateway_logs (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    created_at TEXT NOT NULL,
                    level TEXT NOT NULL,
                    component TEXT NOT NULL,
                    message TEXT NOT NULL,
                    details_json TEXT NOT NULL DEFAULT '{}',
                    turn_id TEXT
                );
                """
            )
            existing = {
                row["name"]
                for row in self._connection.execute(
                    "PRAGMA table_info(conversation_turns)"
                ).fetchall()
            }
            migrations = {
                "tts_provider": "TEXT NOT NULL DEFAULT ''",
                "tts_voice": "TEXT NOT NULL DEFAULT ''",
                "tts_latency_ms": "INTEGER",
                "audio_duration_ms": "INTEGER",
                "processing_started_at": "TEXT",
                "processing_latency_ms": "INTEGER",
                "playback_command_id": "TEXT NOT NULL DEFAULT ''",
                "playback_queued_at": "TEXT",
                "playback_started_at": "TEXT",
                "playback_completed_at": "TEXT",
                "device_wait_latency_ms": "INTEGER",
                "playback_latency_ms": "INTEGER",
                "end_to_end_latency_ms": "INTEGER",
            }
            for column, declaration in migrations.items():
                if column not in existing:
                    self._connection.execute(
                        f"ALTER TABLE conversation_turns ADD COLUMN {column} {declaration}"
                    )

    def _seed_config(self) -> None:
        seeded = dict(DEFAULT_CONFIG)
        environment_keys = {
            "llm_api_key": os.environ.get("DEEPSEEK_API_KEY", "").strip(),
            "unisound_api_key": os.environ.get("UNISOUND_API_KEY", "").strip(),
        }
        for key, value in environment_keys.items():
            if value:
                seeded[key] = value
        now = utc_now()
        with self._lock, self._connection:
            for key, value in seeded.items():
                self._connection.execute(
                    "INSERT OR IGNORE INTO settings(key, value_json, updated_at) VALUES(?, ?, ?)",
                    (key, json.dumps(value, ensure_ascii=False), now),
                )

    def _preload_sensevoice(self) -> None:
        """Load local ASR during gateway startup instead of the first user turn."""
        config = self.database.config(include_secret=True)
        if self._transcriber is not None or config.get("asr_provider") != "sensevoice_local":
            return
        try:
            self._ensure_sensevoice_model(config)
            self._asr_preload_error = None
            self.log("INFO", "asr", "SenseVoice 已在网关启动阶段预加载")
        except Exception as error:
            self._asr_preload_error = str(error)[:1000]
            self.log(
                "WARN",
                "asr",
                "SenseVoice 启动预加载失败，将在首次识别时重试",
                {"error": self._asr_preload_error},
            )

    def close(self) -> None:
        with self._lock:
            self._connection.close()

    def config(self, include_secret: bool = False) -> dict[str, Any]:
        with self._lock:
            rows = self._connection.execute(
                "SELECT key, value_json FROM settings"
            ).fetchall()
        config = dict(DEFAULT_CONFIG)
        for row in rows:
            try:
                config[row["key"]] = json.loads(row["value_json"])
            except json.JSONDecodeError:
                continue
        if include_secret:
            return config
        public = {key: config[key] for key in PUBLIC_CONFIG_KEYS}
        for key in SECRET_CONFIG_KEYS:
            public[f"{key}_configured"] = bool(config.get(key))
            public[f"{key}_masked"] = "••••••••" if config.get(key) else ""
        return public

    @staticmethod
    def _validate_config(payload: dict[str, Any], current: dict[str, Any]) -> dict[str, Any]:
        result: dict[str, Any] = {}

        for key in (
            "auto_process",
            "asr_enabled",
            "asr_use_itn",
            "llm_enabled",
            "llm_thinking",
            "llm_streaming_enabled",
            "tts_enabled",
            "streaming_asr_enabled",
            "tts_sentence_streaming_enabled",
            "tts_streaming_audio_enabled",
            "command_websocket_enabled",
        ):
            if key in payload:
                if not isinstance(payload[key], bool):
                    raise ValueError(f"{key} must be a boolean")
                result[key] = payload[key]

        for key, maximum in (
            ("asr_model", 200),
            ("llm_model", 200),
            ("tts_voice", 200),
            ("unisound_asr_model", 200),
            ("unisound_llm_model", 200),
            ("unisound_tts_model", 200),
            ("unisound_tts_voice", 200),
            ("system_prompt", 4000),
        ):
            if key in payload:
                value = payload[key]
                if not isinstance(value, str) or not value.strip() or len(value) > maximum:
                    raise ValueError(f"invalid {key}")
                result[key] = value.strip()

        for key, maximum in (
            ("asr_api_model", 200),
            ("tts_api_model", 200),
            ("tts_prompt", 4000),
        ):
            if key in payload:
                value = payload[key]
                if not isinstance(value, str) or len(value) > maximum:
                    raise ValueError(f"invalid {key}")
                result[key] = value.strip()

        if "asr_provider" in payload:
            if payload["asr_provider"] not in {
                "unisound",
                "sensevoice_local",
                "custom_http",
            }:
                raise ValueError("unsupported asr_provider")
            result["asr_provider"] = payload["asr_provider"]
        if "llm_provider" in payload:
            if payload["llm_provider"] not in {"unisound", "deepseek"}:
                raise ValueError("unsupported llm_provider")
            result["llm_provider"] = payload["llm_provider"]
        if "tts_provider" in payload:
            if payload["tts_provider"] not in {
                "unisound",
                "edge_tts",
                "custom_http",
            }:
                raise ValueError("unsupported tts_provider")
            result["tts_provider"] = payload["tts_provider"]
        if "asr_language" in payload:
            if payload["asr_language"] not in {"auto", "zh", "en", "yue", "ja", "ko"}:
                raise ValueError("invalid asr_language")
            result["asr_language"] = payload["asr_language"]
        if "asr_device" in payload:
            if payload["asr_device"] not in {"cpu", "cuda"}:
                raise ValueError("invalid asr_device")
            result["asr_device"] = payload["asr_device"]

        for key in (
            "llm_base_url",
            "asr_api_url",
            "tts_api_url",
            "unisound_base_url",
        ):
            if key not in payload:
                continue
            value = payload[key]
            if not isinstance(value, str) or len(value) > 500:
                raise ValueError(f"invalid {key}")
            value = value.strip().rstrip("/")
            if not value and key != "llm_base_url":
                result[key] = ""
                continue
            parsed = urlparse(value)
            if parsed.scheme not in {"http", "https"} or not parsed.netloc:
                raise ValueError(f"{key} must be an HTTP(S) URL")
            result[key] = value

        numeric_rules = {
            "llm_temperature": (0.0, 2.0, float),
            "llm_max_tokens": (16, 4096, int),
            "llm_timeout_seconds": (5, 180, int),
            "asr_api_timeout_seconds": (5, 300, int),
            "tts_api_timeout_seconds": (5, 300, int),
            "unisound_timeout_seconds": (5, 300, int),
            "unisound_poll_interval_ms": (100, 5000, int),
            "stream_partial_interval_ms": (300, 5000, int),
            "tts_sentence_max_chars": (16, 240, int),
            "voice_reply_max_sentences": (1, 2, int),
            "voice_reply_max_chars": (32, 320, int),
            "history_turns": (0, 20, int),
            "tts_rate": (-100, 100, int),
            "tts_volume": (-100, 100, int),
        }
        for key, (minimum, maximum, number_type) in numeric_rules.items():
            if key not in payload:
                continue
            value = payload[key]
            if isinstance(value, bool):
                raise ValueError(f"invalid {key}")
            try:
                value = number_type(value)
            except (TypeError, ValueError):
                raise ValueError(f"invalid {key}") from None
            if value < minimum or value > maximum:
                raise ValueError(f"invalid {key}")
            result[key] = value

        for key in SECRET_CONFIG_KEYS:
            if payload.get(f"clear_{key}") is True:
                result[key] = ""
            elif key in payload:
                value = payload[key]
                if not isinstance(value, str) or len(value) > 500:
                    raise ValueError(f"invalid {key}")
                if value.strip():
                    result[key] = value.strip()

        effective = {**current, **result}
        if effective["asr_provider"] == "custom_http" and not effective["asr_api_url"]:
            raise ValueError("custom_http ASR requires asr_api_url")
        if effective["tts_provider"] == "custom_http" and not effective["tts_api_url"]:
            raise ValueError("custom_http TTS requires tts_api_url")

        if not result:
            raise ValueError("no supported configuration fields")
        return result

    def update_config(self, payload: dict[str, Any]) -> dict[str, Any]:
        current = self.config(include_secret=True)
        updates = self._validate_config(payload, current)
        now = utc_now()
        with self._lock, self._connection:
            for key, value in updates.items():
                self._connection.execute(
                    """
                    INSERT INTO settings(key, value_json, updated_at) VALUES(?, ?, ?)
                    ON CONFLICT(key) DO UPDATE SET value_json=excluded.value_json,
                    updated_at=excluded.updated_at
                    """,
                    (key, json.dumps(value, ensure_ascii=False), now),
                )
        return self.config(include_secret=False)

    def ensure_turn(self, recording: dict[str, Any]) -> dict[str, Any]:
        now = utc_now()
        turn_id = uuid.uuid4().hex
        with self._lock, self._connection:
            self._connection.execute(
                """
                INSERT OR IGNORE INTO conversation_turns(
                    id, conversation_id, device_id, recording_id, status, created_at, updated_at
                ) VALUES(?, ?, ?, ?, 'pending', ?, ?)
                """,
                (
                    turn_id,
                    recording.get("session_id") or recording["device_id"],
                    recording["device_id"],
                    recording["id"],
                    recording.get("created_at", now),
                    now,
                ),
            )
        turn = self.turn_by_recording(recording["id"])
        if turn is None:
            raise RuntimeError("failed to create conversation turn")
        return turn

    @staticmethod
    def _row_dict(row: sqlite3.Row | None) -> dict[str, Any] | None:
        return None if row is None else dict(row)

    def turn(self, turn_id: str) -> dict[str, Any] | None:
        with self._lock:
            row = self._connection.execute(
                "SELECT * FROM conversation_turns WHERE id=?", (turn_id,)
            ).fetchone()
        return self._row_dict(row)

    def turn_by_recording(self, recording_id: str) -> dict[str, Any] | None:
        with self._lock:
            row = self._connection.execute(
                "SELECT * FROM conversation_turns WHERE recording_id=?", (recording_id,)
            ).fetchone()
        return self._row_dict(row)

    def turn_by_playback_command(self, command_id: str) -> dict[str, Any] | None:
        with self._lock:
            row = self._connection.execute(
                "SELECT * FROM conversation_turns WHERE playback_command_id=?",
                (command_id,),
            ).fetchone()
        return self._row_dict(row)

    def turns(self, limit: int = 100) -> list[dict[str, Any]]:
        with self._lock:
            rows = self._connection.execute(
                "SELECT * FROM conversation_turns ORDER BY created_at DESC LIMIT ?",
                (max(1, min(limit, 500)),),
            ).fetchall()
        return [dict(row) for row in rows]

    def waiting_turns(self) -> list[dict[str, Any]]:
        with self._lock:
            rows = self._connection.execute(
                """
                SELECT * FROM conversation_turns
                WHERE status IN (
                    'pending', 'waiting_for_llm_config'
                )
                ORDER BY created_at ASC
                """
            ).fetchall()
        return [dict(row) for row in rows]

    def update_turn(self, turn_id: str, **values: Any) -> dict[str, Any]:
        invalid = set(values) - TURN_UPDATE_FIELDS
        if invalid:
            raise ValueError(f"unsupported turn fields: {sorted(invalid)}")
        if not values:
            turn = self.turn(turn_id)
            if turn is None:
                raise KeyError(turn_id)
            return turn
        values["updated_at"] = utc_now()
        assignments = ", ".join(f"{key}=?" for key in values)
        params = [*values.values(), turn_id]
        with self._lock, self._connection:
            self._connection.execute(
                f"UPDATE conversation_turns SET {assignments} WHERE id=?", params
            )
        turn = self.turn(turn_id)
        if turn is None:
            raise KeyError(turn_id)
        return turn

    def reset_turn(self, turn_id: str) -> dict[str, Any]:
        return self.update_turn(
            turn_id,
            status="pending",
            error_message="",
            assistant_response="",
            asr_latency_ms=None,
            llm_latency_ms=None,
            tts_latency_ms=None,
            audio_duration_ms=None,
            processing_started_at=None,
            processing_latency_ms=None,
            playback_command_id="",
            playback_queued_at=None,
            playback_started_at=None,
            playback_completed_at=None,
            device_wait_latency_ms=None,
            playback_latency_ms=None,
            end_to_end_latency_ms=None,
            input_tokens=0,
            output_tokens=0,
        )

    def conversation_history(
        self, conversation_id: str, exclude_turn_id: str, limit: int
    ) -> list[dict[str, str]]:
        if limit <= 0:
            return []
        with self._lock:
            rows = self._connection.execute(
                """
                SELECT transcript, assistant_response FROM conversation_turns
                WHERE conversation_id=? AND id<>? AND assistant_response<>''
                ORDER BY created_at DESC LIMIT ?
                """,
                (conversation_id, exclude_turn_id, limit),
            ).fetchall()
        messages: list[dict[str, str]] = []
        for row in reversed(rows):
            messages.append({"role": "user", "content": row["transcript"]})
            messages.append({"role": "assistant", "content": row["assistant_response"]})
        return messages

    def add_log(
        self,
        level: str,
        component: str,
        message: str,
        details: dict[str, Any] | None = None,
        turn_id: str | None = None,
    ) -> dict[str, Any]:
        created_at = utc_now()
        details_json = json.dumps(details or {}, ensure_ascii=False, separators=(",", ":"))
        with self._lock, self._connection:
            cursor = self._connection.execute(
                """
                INSERT INTO gateway_logs(created_at, level, component, message, details_json, turn_id)
                VALUES(?, ?, ?, ?, ?, ?)
                """,
                (created_at, level.upper()[:16], component[:64], message[:2000], details_json, turn_id),
            )
            log_id = cursor.lastrowid
            self._connection.execute(
                "DELETE FROM gateway_logs WHERE id <= (SELECT COALESCE(MAX(id), 0) - 2000 FROM gateway_logs)"
            )
        return {
            "id": log_id,
            "created_at": created_at,
            "level": level.upper()[:16],
            "component": component[:64],
            "message": message[:2000],
            "details": details or {},
            "turn_id": turn_id,
        }

    def logs(self, limit: int = 200) -> list[dict[str, Any]]:
        with self._lock:
            rows = self._connection.execute(
                "SELECT * FROM gateway_logs ORDER BY id DESC LIMIT ?",
                (max(1, min(limit, 1000)),),
            ).fetchall()
        result = []
        for row in rows:
            item = dict(row)
            try:
                item["details"] = json.loads(item.pop("details_json"))
            except json.JSONDecodeError:
                item["details"] = {}
                item.pop("details_json", None)
            result.append(item)
        return result


Transcriber = Callable[[Path, dict[str, Any]], str]
LLMCaller = Callable[[list[dict[str, str]], dict[str, Any]], tuple[str, dict[str, int]]]
TTSCaller = Callable[[str, Path, dict[str, Any]], dict[str, int]]
PlaybackSender = Callable[[str, dict[str, Any]], dict[str, Any]]
LLMDeltaCallback = Callable[[str], None]


class _TTSStreamingCoordinator:
    """Synthesize sentences concurrently while dispatching playback in order."""

    def __init__(
        self,
        service: "ConversationService",
        turn: dict[str, Any],
        turn_id: str,
        config: dict[str, Any],
        pipeline_started: float,
    ) -> None:
        self.service = service
        self.turn = turn
        self.turn_id = turn_id
        self.config = config
        self.pipeline_started = pipeline_started
        self.started = time.perf_counter()
        self._condition = threading.Condition()
        self._futures: list[tuple[int, str, Path, Future[dict[str, int]]]] = []
        self._next_index = 0
        self._accepting = True
        self._error: Exception | None = None
        self._first_command: dict[str, Any] | None = None
        self._final_command: dict[str, Any] | None = None
        self._total_duration_ms = 0
        self._total_samples = 0
        self._thread = threading.Thread(
            target=self._dispatch_loop,
            name=f"gateway-tts-dispatch-{turn_id[:8]}",
            daemon=True,
        )
        selected_voice = (
            config["unisound_tts_voice"]
            if config["tts_provider"] == "unisound"
            else config["tts_voice"]
        )
        updated = service.database.update_turn(
            turn_id,
            status="tts_synthesizing",
            tts_provider=config["tts_provider"],
            tts_voice=selected_voice,
            error_message="",
        )
        service.publish("turn_updated", updated)
        service.log(
            "INFO",
            "tts",
            "开始流式分句合成 LLM 语音回复",
            {"max_sentences": config.get("voice_reply_max_sentences", 2)},
            turn_id,
        )
        self._thread.start()

    @property
    def has_commands(self) -> bool:
        return self._first_command is not None

    def submit(self, text: str) -> bool:
        cleaned = _prepare_tts_text(text)
        if not cleaned:
            return False
        with self._condition:
            if not self._accepting:
                return False
            max_sentences = int(self.config.get("voice_reply_max_sentences", 2))
            if len(self._futures) >= max(1, min(max_sentences, 2)):
                return False
            index = len(self._futures)
            # Streaming playback always addresses an indexed chunk.  Keep the
            # filename consistent even when the operator limits voice output
            # to one sentence; otherwise /chunk/0/pcm would resolve to a
            # non-existent file while the legacy single-file name is used.
            filename = f"{self.turn_id}-chunk-{index}.pcm"
            pcm_path = self.service.tts_dir / filename
            future = self.service._tts_executor.submit(
                self.service._run_tts, cleaned, pcm_path, self.config
            )
            self._futures.append((index, cleaned, pcm_path, future))
            self._condition.notify_all()
            return True

    def finish(self) -> None:
        with self._condition:
            self._accepting = False
            self._condition.notify_all()
        self._thread.join(timeout=max(30, int(self.config.get("tts_api_timeout_seconds", 60))))
        if self._thread.is_alive():
            raise RuntimeError("TTS 分句调度线程未在超时时间内结束")
        if self._error is not None:
            raise self._error
        tts_elapsed = round((time.perf_counter() - self.started) * 1000)
        processing_ms = round((time.perf_counter() - self.pipeline_started) * 1000)
        if self._first_command is None:
            completed = self.service.database.update_turn(
                self.turn_id,
                status="completed",
                tts_latency_ms=tts_elapsed,
                audio_duration_ms=self._total_duration_ms,
                processing_latency_ms=processing_ms,
                error_message="未生成可播放的 TTS 音频",
            )
            self.service.publish("turn_updated", completed)
            return
        if self._final_command is not None:
            with self.service._playback_turns_lock:
                self.service._final_playback_commands[self.turn_id] = self._final_command["id"]
                info = self.service._playback_turns.get(self._final_command["id"])
                if info is not None:
                    info["is_final_chunk"] = True
        queued = self.service.database.update_turn(
            self.turn_id,
            status="playback_queued",
            tts_latency_ms=tts_elapsed,
            audio_duration_ms=self._total_duration_ms,
            processing_latency_ms=processing_ms,
            error_message="",
        )
        self.service.log(
            "INFO",
            "tts",
            "流式分句 TTS 队列已建立",
            {
                "chunk_count": len(self._futures),
                "tts_latency_ms": tts_elapsed,
                "audio_duration_ms": self._total_duration_ms,
                "processing_latency_ms": processing_ms,
            },
            self.turn_id,
        )
        self.service.publish("turn_updated", queued)
        if self._final_command is not None:
            self.service._replay_pending_playback(self._final_command["id"])

    def abort(self) -> None:
        with self._condition:
            self._accepting = False
            self._condition.notify_all()

    def _dispatch_loop(self) -> None:
        while True:
            with self._condition:
                while self._next_index >= len(self._futures) and self._accepting:
                    self._condition.wait(timeout=0.2)
                if self._next_index >= len(self._futures) and not self._accepting:
                    return
                index, text, pcm_path, future = self._futures[self._next_index]
            try:
                audio = future.result()
                command_id, completion = self._queue_audio(index, text, pcm_path, audio)
                if completion is not None:
                    timeout = max(
                        30,
                        int(self.config.get("tts_api_timeout_seconds", 60)),
                    )
                    if not completion["event"].wait(timeout):
                        raise RuntimeError(
                            f"等待 ESP32 播放完成超时: command_id={command_id}"
                        )
                    if completion.get("error"):
                        raise RuntimeError(str(completion["error"]))
            except Exception as error:
                self._error = error
                with self._condition:
                    self._accepting = False
                    self._condition.notify_all()
                return
            self._next_index += 1

    def _queue_audio(
        self,
        index: int,
        text: str,
        pcm_path: Path,
        audio: dict[str, int],
    ) -> tuple[str, dict[str, Any] | None]:
        self._total_duration_ms += int(audio["duration_ms"])
        self._total_samples += int(audio["sample_count"])
        if self.service._playback_sender is None:
            return "", None
        # A streaming response does not know its final sentence count when the
        # first command is queued, so every chunk uses an indexed, immutable file.
        audio_path = f"/api/tts/{self.turn_id}/chunk/{index}/pcm"
        command = self.service._playback_sender(
            self.turn["device_id"],
            {
                "type": "play_audio",
                "turn_id": self.turn_id,
                "audio_path": audio_path,
                "sample_rate": audio["sample_rate"],
                "sample_count": audio["sample_count"],
                "chunk_index": index,
                "chunk_count": 0,
                "is_final_chunk": False,
                "stream_audio": bool(self.config.get("tts_streaming_audio_enabled", True)),
            },
        )
        completion: dict[str, Any] | None = None
        if self.config.get("tts_streaming_audio_enabled", True):
            completion = {"event": threading.Event(), "error": None}
        with self.service._playback_turns_lock:
            self.service._playback_turns[command["id"]] = {
                "turn_id": self.turn_id,
                "chunk_index": index,
                "chunk_count": 0,
                "is_final_chunk": False,
                "completion": completion,
            }
        if self._first_command is None:
            self._first_command = command
            queued = self.service.database.update_turn(
                self.turn_id,
                status="playback_queued",
                audio_duration_ms=self._total_duration_ms,
                playback_command_id=command["id"],
                playback_queued_at=utc_now(),
                error_message="",
            )
            self.service.log(
                "INFO",
                "tts",
                "首段语音已下发，后续分句在后台合成",
                {"chunk_index": index, "first_audio_latency_ms": round((time.perf_counter() - self.started) * 1000)},
                self.turn_id,
            )
            self.service.publish("turn_updated", queued)
        self._final_command = command
        return command["id"], completion


class ConversationService:
    def __init__(
        self,
        data_dir: Path,
        transcriber: Transcriber | None = None,
        llm_caller: LLMCaller | None = None,
        tts_caller: TTSCaller | None = None,
        playback_sender: PlaybackSender | None = None,
    ) -> None:
        self.database = GatewayDatabase(data_dir)
        self.recordings_dir = data_dir.resolve() / "recordings"
        self.tts_dir = data_dir.resolve() / "tts"
        self.stream_dir = data_dir.resolve() / "streaming"
        self.tts_dir.mkdir(parents=True, exist_ok=True)
        self.stream_dir.mkdir(parents=True, exist_ok=True)
        self._transcriber = transcriber
        self._llm_caller = llm_caller
        self._tts_caller = tts_caller
        self._playback_sender = playback_sender
        self._model: Any = None
        self._model_key: tuple[str, str] | None = None
        self._asr_lock = threading.RLock()
        self._jobs: queue.Queue[str | None] = queue.Queue()
        self._queued: set[str] = set()
        self._queued_lock = threading.Lock()
        self._subscribers: set[queue.Queue[dict[str, Any]]] = set()
        self._subscribers_lock = threading.Lock()
        self._worker: threading.Thread | None = None
        self._stream_executor = ThreadPoolExecutor(
            max_workers=2, thread_name_prefix="gateway-stream-asr"
        )
        self._tts_executor = ThreadPoolExecutor(
            max_workers=4, thread_name_prefix="gateway-tts"
        )
        self._stream_partial_lock = threading.RLock()
        self._stream_partial_jobs: dict[str, tuple[Future[str], Path]] = {}
        self._playback_turns: dict[str, dict[str, Any]] = {}
        self._final_playback_commands: dict[str, str] = {}
        self._pending_playback_completed: dict[str, dict[str, Any]] = {}
        self._playback_turns_lock = threading.RLock()
        self._closed = False
        self._asr_preload_error: str | None = None

    def set_playback_sender(self, sender: PlaybackSender) -> None:
        self._playback_sender = sender

    def start(self) -> None:
        if self._worker and self._worker.is_alive():
            return
        self._preload_sensevoice()
        self._worker = threading.Thread(
            target=self._worker_loop, name="gateway-ai-worker", daemon=True
        )
        self._worker.start()
        self.log("INFO", "pipeline", "AI 后台处理线程已启动")

    def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        if self._worker and self._worker.is_alive():
            self._jobs.put(None)
            self._worker.join(timeout=5)
        self._stream_executor.shutdown(wait=False, cancel_futures=True)
        self._tts_executor.shutdown(wait=False, cancel_futures=True)
        self.database.close()

    @property
    def worker_running(self) -> bool:
        return bool(self._worker and self._worker.is_alive())

    def public_config(self) -> dict[str, Any]:
        config = self.database.config(include_secret=False)
        config["worker_running"] = self.worker_running
        config["sensevoice_loaded"] = self._model is not None
        config["sensevoice_preload_error"] = self._asr_preload_error or ""
        return config

    def streaming_asr_supported(self) -> bool:
        config = self.database.config(include_secret=True)
        return bool(
            config.get("streaming_asr_enabled")
            and config.get("asr_provider") in {"sensevoice_local", "custom_http"}
        )

    def submit_stream_partial(
        self, stream_id: str, pcm: bytes, sample_rate: int
    ) -> bool:
        """Schedule a best-effort partial ASR job without blocking PCM ingestion."""
        if not self.streaming_asr_supported() or not pcm:
            return False
        with self._stream_partial_lock:
            existing = self._stream_partial_jobs.get(stream_id)
            if existing and not existing[0].done():
                return False
            snapshot = self.stream_dir / f".{stream_id}.{uuid.uuid4().hex}.wav"
            with wave.open(str(snapshot), "wb") as output:
                output.setnchannels(1)
                output.setsampwidth(2)
                output.setframerate(sample_rate)
                output.writeframes(pcm)
            config = self.database.config(include_secret=True)
            future = self._stream_executor.submit(
                self._run_stream_partial, snapshot, config
            )
            self._stream_partial_jobs[stream_id] = (future, snapshot)
            return True

    def poll_stream_partial(self, stream_id: str) -> dict[str, Any] | None:
        with self._stream_partial_lock:
            job = self._stream_partial_jobs.get(stream_id)
            if not job or not job[0].done():
                return None
            self._stream_partial_jobs.pop(stream_id, None)
        future, snapshot = job
        try:
            transcript = future.result().strip()
        except Exception as error:
            self.log(
                "WARN",
                "asr",
                "流式 ASR 局部识别失败，继续接收音频",
                {"stream_id": stream_id, "error": str(error)},
            )
            transcript = ""
        finally:
            snapshot.unlink(missing_ok=True)
        if not transcript:
            return None
        result = {"stream_id": stream_id, "transcript": transcript}
        self.publish("stream_partial", result)
        return result

    def _run_stream_partial(self, path: Path, config: dict[str, Any]) -> str:
        if config.get("asr_provider") == "sensevoice_local":
            return self._run_transcriber(path, config)
        return self._run_transcriber(path, config)

    def _ensure_sensevoice_model(self, config: dict[str, Any]) -> None:
        """Load SenseVoice once so the first user turn does not pay import/model cost."""
        try:
            from funasr import AutoModel
        except ImportError as error:
            raise RuntimeError(
                "SenseVoice 运行环境未安装，请使用 gateway/.venv 启动或安装 funasr"
            ) from error

        model_key = (str(config["asr_model"]), str(config["asr_device"]))
        with self._asr_lock:
            if self._model is not None and self._model_key == model_key:
                return
            self.log(
                "INFO",
                "asr",
                "正在加载 SenseVoice 模型，首次运行可能需要下载模型",
                {"model": model_key[0], "device": model_key[1]},
            )
            self._model = AutoModel(
                model=model_key[0],
                trust_remote_code=True,
                device=model_key[1],
            )
            self._model_key = model_key
            self.log("INFO", "asr", "SenseVoice 模型加载完成")

    def _preload_sensevoice(self) -> None:
        config = self.database.config(include_secret=True)
        if self._transcriber is not None or config.get("asr_provider") != "sensevoice_local":
            return
        try:
            started = time.perf_counter()
            self._ensure_sensevoice_model(config)
            self._asr_preload_error = None
            self.log(
                "INFO",
                "asr",
                "网关启动时已预加载 SenseVoice",
                {"latency_ms": round((time.perf_counter() - started) * 1000)},
            )
        except Exception as error:
            self._asr_preload_error = str(error)[:1000]
            self.log(
                "WARN",
                "asr",
                "SenseVoice 启动预加载失败，将在首个请求时重试",
                {"error": self._asr_preload_error},
            )

    def update_config(self, payload: dict[str, Any]) -> dict[str, Any]:
        config = self.database.update_config(payload)
        safe_changes = sorted(key for key in payload if "api_key" not in key)
        self.log("INFO", "config", "模型配置已通过 Web 更新", {"fields": safe_changes})
        self.publish("config_updated", config)
        if config.get("auto_process"):
            self.resume_waiting()
        return {**config, "worker_running": self.worker_running, "sensevoice_loaded": self._model is not None}

    def log(
        self,
        level: str,
        component: str,
        message: str,
        details: dict[str, Any] | None = None,
        turn_id: str | None = None,
    ) -> dict[str, Any]:
        entry = self.database.add_log(level, component, message, details, turn_id)
        safe_console_print(
            f"[{entry['created_at']}] {entry['level']} {component}: {message}"
        )
        self.publish("log", entry)
        return entry

    def publish(self, event_type: str, data: Any) -> None:
        event = {"type": event_type, "data": data, "time": utc_now()}
        with self._subscribers_lock:
            subscribers = list(self._subscribers)
        for subscriber in subscribers:
            try:
                subscriber.put_nowait(event)
            except queue.Full:
                try:
                    subscriber.get_nowait()
                    subscriber.put_nowait(event)
                except (queue.Empty, queue.Full):
                    pass

    def subscribe(self) -> queue.Queue[dict[str, Any]]:
        subscriber: queue.Queue[dict[str, Any]] = queue.Queue(maxsize=100)
        with self._subscribers_lock:
            self._subscribers.add(subscriber)
        return subscriber

    def unsubscribe(self, subscriber: queue.Queue[dict[str, Any]]) -> None:
        with self._subscribers_lock:
            self._subscribers.discard(subscriber)

    def recording_saved(self, recording: dict[str, Any]) -> dict[str, Any]:
        turn = self.database.ensure_turn(recording)
        self.log(
            "INFO",
            "recording",
            "录音已进入 AI 处理队列",
            {"recording_id": recording["id"], "duration_ms": recording["duration_ms"]},
            turn["id"],
        )
        self.publish("turn_updated", turn)
        if self.database.config(include_secret=True).get("auto_process"):
            self.enqueue(turn["id"])
        return turn

    def register_existing_recording(
        self, recording: dict[str, Any], reset: bool = True
    ) -> dict[str, Any]:
        turn = self.database.ensure_turn(recording)
        if reset:
            turn = self.database.reset_turn(turn["id"])
        self.enqueue(turn["id"])
        self.publish("turn_updated", turn)
        return turn

    def retry_turn(self, turn_id: str) -> dict[str, Any]:
        turn = self.database.reset_turn(turn_id)
        self.log("INFO", "pipeline", "用户请求重新处理", turn_id=turn_id)
        self.enqueue(turn_id)
        self.publish("turn_updated", turn)
        return turn

    def resume_waiting(self) -> None:
        for turn in self.database.waiting_turns():
            self.enqueue(turn["id"])

    def enqueue(self, turn_id: str) -> None:
        with self._queued_lock:
            if turn_id in self._queued:
                return
            self._queued.add(turn_id)
        self._jobs.put(turn_id)

    def _worker_loop(self) -> None:
        while True:
            turn_id = self._jobs.get()
            if turn_id is None:
                return
            try:
                self._process_turn(turn_id)
            except Exception as error:  # Worker must survive provider/runtime failures.
                self.log(
                    "ERROR",
                    "pipeline",
                    "处理任务发生未捕获错误",
                    {"error": str(error)},
                    turn_id,
                )
                try:
                    turn = self.database.update_turn(
                        turn_id, status="failed", error_message=str(error)[:1000]
                    )
                    self.publish("turn_updated", turn)
                except Exception:
                    pass
            finally:
                with self._queued_lock:
                    self._queued.discard(turn_id)
                self._jobs.task_done()

    def _recording_path(self, recording_id: str) -> Path:
        path = (self.recordings_dir / f"{recording_id}.wav").resolve()
        if path.parent != self.recordings_dir.resolve() or not path.is_file():
            raise FileNotFoundError(f"recording not found: {recording_id}")
        return path

    @staticmethod
    def _elapsed_ms(started_at: str, finished_at: str | None = None) -> int:
        started = datetime.fromisoformat(started_at)
        finished = (
            datetime.fromisoformat(finished_at)
            if finished_at
            else datetime.now(timezone.utc)
        )
        return max(0, round((finished - started).total_seconds() * 1000))

    def tts_path(self, turn_id: str, chunk_index: int | None = None) -> Path | None:
        if not uuid.UUID(turn_id).hex == turn_id:
            return None
        if chunk_index is not None and not 0 <= chunk_index <= 999:
            return None
        filename = (
            f"{turn_id}.pcm"
            if chunk_index is None
            else f"{turn_id}-chunk-{chunk_index}.pcm"
        )
        path = (self.tts_dir / filename).resolve()
        return path if path.parent == self.tts_dir and path.is_file() else None

    def _replay_pending_playback(self, command_id: str) -> None:
        """Finalize a chunk that completed before the LLM revealed the final chunk."""
        with self._playback_turns_lock:
            pending = self._pending_playback_completed.pop(command_id, None)
        if pending is not None:
            self.device_event(pending)

    def device_event(self, event: dict[str, Any]) -> None:
        command_id = event.get("command_id", "")
        if not command_id:
            return
        turn = self.database.turn_by_playback_command(command_id)
        if turn is None:
            with self._playback_turns_lock:
                playback = self._playback_turns.get(command_id)
            if playback:
                turn = self.database.turn(playback["turn_id"])
        if turn is None:
            return
        event_type = event.get("type")
        event_time = event.get("created_at", utc_now())
        details = event.get("details") if isinstance(event.get("details"), dict) else {}
        if event_type == "playback_started":
            with self._playback_turns_lock:
                playback = self._playback_turns.get(command_id)
            if playback and command_id != turn.get("playback_command_id"):
                self.log(
                    "INFO",
                    "playback",
                    "ESP32 开始播放后续分句语音",
                    {
                        "chunk_index": playback.get("chunk_index"),
                        "chunk_count": playback.get("chunk_count"),
                    },
                    turn["id"],
                )
                return
            updated = self.database.update_turn(
                turn["id"],
                status="playback_started",
                playback_started_at=event_time,
                device_wait_latency_ms=self._elapsed_ms(
                    turn["playback_queued_at"], event_time
                ),
                error_message="",
            )
            self.log(
                "INFO",
                "playback",
                "ESP32 开始播放 LLM 回复",
                {"device_wait_latency_ms": updated["device_wait_latency_ms"]},
                turn["id"],
            )
        elif event_type == "playback_completed":
            with self._playback_turns_lock:
                playback = self._playback_turns.get(command_id)
                final_command_id = self._final_playback_commands.get(turn["id"])
            if playback:
                completion = playback.get("completion")
                if isinstance(completion, dict):
                    completion["event"].set()
            if playback and playback.get("is_final_chunk") is False:
                with self._playback_turns_lock:
                    # The first short sentence can finish while the LLM is
                    # still streaming.  Defer finalization until finish()
                    # marks the last submitted chunk as final.
                    self._pending_playback_completed[command_id] = dict(event)
                self.log(
                    "INFO",
                    "playback",
                    "ESP32 完成一段流式分句语音",
                    {
                        "chunk_index": playback.get("chunk_index"),
                        "chunk_count": playback.get("chunk_count"),
                        "playback_ms": details.get("playback_ms", 0),
                    },
                    turn["id"],
                )
                return
            if playback and final_command_id and command_id != final_command_id:
                self.log(
                    "INFO",
                    "playback",
                    "ESP32 完成一段分句语音",
                    {
                        "chunk_index": playback.get("chunk_index"),
                        "chunk_count": playback.get("chunk_count"),
                        "playback_ms": details.get("playback_ms", 0),
                    },
                    turn["id"],
                )
                return
            playback_ms = int(details.get("playback_ms", 0) or 0)
            if playback_ms <= 0 and turn.get("playback_started_at"):
                playback_ms = self._elapsed_ms(turn["playback_started_at"], event_time)
            updated = self.database.update_turn(
                turn["id"],
                status="playback_completed",
                playback_completed_at=event_time,
                playback_latency_ms=playback_ms,
                end_to_end_latency_ms=self._elapsed_ms(
                    turn.get("processing_started_at") or turn["created_at"], event_time
                ),
                error_message="",
            )
            self.log(
                "INFO",
                "timing",
                "本轮语音聊天完成",
                self._timing_details(updated),
                turn["id"],
            )
            with self._playback_turns_lock:
                stale = [
                    key
                    for key, value in self._playback_turns.items()
                    if value.get("turn_id") == turn["id"]
                ]
                for key in stale:
                    self._playback_turns.pop(key, None)
                    self._pending_playback_completed.pop(key, None)
                self._final_playback_commands.pop(turn["id"], None)
        elif event_type in {"playback_failed", "playback_aborted"}:
            with self._playback_turns_lock:
                playback = self._playback_turns.get(command_id)
            if playback:
                completion = playback.get("completion")
                if isinstance(completion, dict):
                    completion["error"] = str(details.get("reason", event_type))
                    completion["event"].set()
            updated = self.database.update_turn(
                turn["id"],
                status=event_type,
                playback_completed_at=event_time,
                end_to_end_latency_ms=self._elapsed_ms(
                    turn.get("processing_started_at") or turn["created_at"], event_time
                ),
                error_message=str(details.get("reason", event_type))[:1000],
            )
            self.log("WARN", "playback", "ESP32 播放未完成", details, turn["id"])
            with self._playback_turns_lock:
                stale = [
                    key
                    for key, value in self._playback_turns.items()
                    if value.get("turn_id") == turn["id"]
                ]
                for key in stale:
                    self._playback_turns.pop(key, None)
                    self._pending_playback_completed.pop(key, None)
                self._final_playback_commands.pop(turn["id"], None)
        else:
            return
        self.publish("turn_updated", updated)

    @staticmethod
    def _timing_details(turn: dict[str, Any]) -> dict[str, Any]:
        return {
            "asr_ms": turn.get("asr_latency_ms"),
            "llm_ms": turn.get("llm_latency_ms"),
            "tts_ms": turn.get("tts_latency_ms"),
            "processing_ms": turn.get("processing_latency_ms"),
            "device_wait_ms": turn.get("device_wait_latency_ms"),
            "playback_ms": turn.get("playback_latency_ms"),
            "end_to_end_ms": turn.get("end_to_end_latency_ms"),
        }

    def _queue_continue_listening(
        self, turn: dict[str, Any], turn_id: str
    ) -> dict[str, Any]:
        if self._playback_sender is None:
            return turn
        try:
            command = self._playback_sender(
                turn["device_id"],
                {"type": "continue_listening", "turn_id": turn_id},
            )
            return self.database.update_turn(
                turn_id,
                playback_command_id=command["id"],
                playback_queued_at=utc_now(),
            )
        except Exception as error:
            self.log(
                "ERROR",
                "session",
                "继续监听命令下发失败",
                {"error": str(error)},
                turn_id,
            )
            return turn

    def _queue_end_conversation(
        self, turn: dict[str, Any], turn_id: str
    ) -> dict[str, Any]:
        if self._playback_sender is None:
            return self.database.update_turn(
                turn_id,
                status="session_end_failed",
                error_message="未配置 ESP32 会话控制命令通道",
            )
        try:
            command = self._playback_sender(
                turn["device_id"],
                {"type": "end_conversation", "turn_id": turn_id},
            )
            return self.database.update_turn(
                turn_id,
                status="session_end_queued",
                playback_command_id=command["id"],
                playback_queued_at=utc_now(),
                error_message="",
            )
        except Exception as error:
            self.log(
                "ERROR",
                "session",
                "退出长聊天命令下发失败",
                {"error": str(error)},
                turn_id,
            )
            return self.database.update_turn(
                turn_id,
                status="session_end_failed",
                error_message=str(error)[:1000],
            )

    def _process_turn(self, turn_id: str) -> None:
        turn = self.database.turn(turn_id)
        if turn is None:
            return
        pipeline_started = time.perf_counter()
        processing_started_at = utc_now()
        turn = self.database.update_turn(
            turn_id, processing_started_at=processing_started_at
        )
        config = self.database.config(include_secret=True)
        transcript = turn["transcript"]

        if not transcript:
            if not config["asr_enabled"]:
                updated = self.database.update_turn(
                    turn_id,
                    status="asr_disabled",
                    error_message="本地语音识别已禁用",
                )
                updated = self._queue_continue_listening(updated, turn_id)
                self.publish("turn_updated", updated)
                return
            selected_asr_model = (
                config["unisound_asr_model"]
                if config["asr_provider"] == "unisound"
                else (
                    config["asr_api_model"] or config["asr_model"]
                    if config["asr_provider"] == "custom_http"
                    else config["asr_model"]
                )
            )
            updated = self.database.update_turn(
                turn_id,
                status="transcribing",
                asr_provider=config["asr_provider"],
                asr_model=selected_asr_model,
                error_message="",
            )
            self.publish("turn_updated", updated)
            self.log(
                "INFO",
                "asr",
                "开始语音识别",
                {"provider": config["asr_provider"], "model": selected_asr_model},
                turn_id,
            )
            started = time.perf_counter()
            try:
                transcript = self._run_transcriber(
                    self._recording_path(turn["recording_id"]), config
                ).strip()
                if not transcript:
                    raise RuntimeError("未识别到有效语音文字")
            except Exception as error:
                elapsed = round((time.perf_counter() - started) * 1000)
                failed = self.database.update_turn(
                    turn_id,
                    status="asr_failed",
                    asr_latency_ms=elapsed,
                    error_message=str(error)[:1000],
                )
                self.log("ERROR", "asr", "语音识别失败", {"error": str(error)}, turn_id)
                failed = self._queue_continue_listening(failed, turn_id)
                self.publish("turn_updated", failed)
                return
            elapsed = round((time.perf_counter() - started) * 1000)
            turn = self.database.update_turn(
                turn_id,
                status="transcript_ready",
                transcript=transcript,
                asr_latency_ms=elapsed,
                error_message="",
            )
            self.log(
                "INFO",
                "asr",
                "语音识别完成",
                {"latency_ms": elapsed, "characters": len(transcript)},
                turn_id,
            )
            self.publish("turn_updated", turn)

        if not _is_meaningful_transcript(transcript):
            processing_ms = round((time.perf_counter() - pipeline_started) * 1000)
            ignored = self.database.update_turn(
                turn_id,
                status="ignored",
                transcript=transcript,
                processing_latency_ms=processing_ms,
                error_message="未检测到有效问题，ESP32 将继续监听",
            )
            self.log(
                "INFO",
                "asr",
                "忽略无意义的短语音并继续监听",
                {"transcript": transcript, "processing_latency_ms": processing_ms},
                turn_id,
            )
            ignored = self._queue_continue_listening(ignored, turn_id)
            self.publish("turn_updated", ignored)
            return

        if _is_end_conversation_command(transcript):
            processing_ms = round((time.perf_counter() - pipeline_started) * 1000)
            ending = self.database.update_turn(
                turn_id,
                status="session_end_requested",
                transcript=transcript,
                assistant_response="（已执行：退出长聊天模式）",
                processing_latency_ms=processing_ms,
                error_message="",
            )
            ending = self._queue_end_conversation(ending, turn_id)
            self.log(
                "INFO",
                "session",
                "识别到语音退出命令，已跳过 LLM 和 TTS",
                {"transcript": transcript, "processing_latency_ms": processing_ms},
                turn_id,
            )
            self.publish("turn_updated", ending)
            return

        if not config["llm_enabled"]:
            self._queue_continue_listening(turn, turn_id)
            return
        llm_provider = config["llm_provider"]
        llm_model = (
            config["unisound_llm_model"]
            if llm_provider == "unisound"
            else config["llm_model"]
        )
        llm_key_configured = bool(
            config.get("unisound_api_key")
            if llm_provider == "unisound"
            else config.get("llm_api_key")
        )
        llm_label = "云知声 U2" if llm_provider == "unisound" else "DeepSeek"
        if not llm_key_configured:
            waiting = self.database.update_turn(
                turn_id,
                status="waiting_for_llm_config",
                error_message=f"请在 Web 模型配置中填写{llm_label} API Key",
            )
            self.log("WARN", "llm", f"等待配置{llm_label} API Key", turn_id=turn_id)
            waiting = self._queue_continue_listening(waiting, turn_id)
            self.publish("turn_updated", waiting)
            return

        generating = self.database.update_turn(
            turn_id,
            status="llm_generating",
            llm_provider=llm_provider,
            llm_model=llm_model,
            error_message="",
        )
        self.publish("turn_updated", generating)
        self.log(
            "INFO",
            "llm",
            f"开始调用{llm_label}",
            {"provider": llm_provider, "model": llm_model},
            turn_id,
        )
        messages = [{"role": "system", "content": config["system_prompt"]}]
        messages.extend(
            self.database.conversation_history(
                turn["conversation_id"], turn_id, config["history_turns"]
            )
        )
        messages.append({"role": "user", "content": transcript})
        started = time.perf_counter()
        tts_coordinator: _TTSStreamingCoordinator | None = None
        streamed_voice_chunks: list[str] = []
        if config["tts_enabled"] and config.get("tts_sentence_streaming_enabled", True):
            tts_coordinator = _TTSStreamingCoordinator(
                self, turn, turn_id, config, pipeline_started
            )

        def on_llm_delta(piece: str) -> None:
            if tts_coordinator is None:
                return
            buffered = getattr(on_llm_delta, "buffer", "") + piece
            chunks, remainder = extract_stream_sentences(
                buffered, config.get("tts_sentence_max_chars", 80)
            )
            setattr(on_llm_delta, "buffer", remainder)
            max_sentences = max(
                1, min(int(config.get("voice_reply_max_sentences", 2)), 2)
            )
            for chunk in chunks:
                if len(streamed_voice_chunks) >= max_sentences:
                    break
                if tts_coordinator.submit(chunk):
                    streamed_voice_chunks.append(chunk)
        try:
            response, usage = self._run_llm(
                messages,
                config,
                on_delta=on_llm_delta if tts_coordinator is not None else None,
            )
            if not response.strip():
                raise RuntimeError(f"{llm_label}返回了空回复")
        except Exception as error:
            if tts_coordinator is not None:
                tts_coordinator.abort()
            elapsed = round((time.perf_counter() - started) * 1000)
            failed = self.database.update_turn(
                turn_id,
                status="llm_failed",
                llm_latency_ms=elapsed,
                error_message=str(error)[:1000],
            )
            self.log(
                "ERROR",
                "llm",
                f"{llm_label}调用失败",
                {"error": str(error)},
                turn_id,
            )
            failed = self._queue_continue_listening(failed, turn_id)
            self.publish("turn_updated", failed)
            return
        elapsed = round((time.perf_counter() - started) * 1000)
        llm_update: dict[str, Any] = {
            "assistant_response": response.strip(),
            "llm_latency_ms": elapsed,
            "input_tokens": int(usage.get("prompt_tokens", 0)),
            "output_tokens": int(usage.get("completion_tokens", 0)),
            "error_message": "",
        }
        if tts_coordinator is None:
            llm_update["status"] = "llm_ready"
        llm_ready = self.database.update_turn(
            turn_id, **llm_update
        )
        self.log(
            "INFO",
            "llm",
            f"{llm_label}回复完成",
            {
                "latency_ms": elapsed,
                "input_tokens": llm_ready["input_tokens"],
                "output_tokens": llm_ready["output_tokens"],
            },
            turn_id,
        )
        self.publish("turn_updated", llm_ready)

        if not config["tts_enabled"]:
            completed = self.database.update_turn(
                turn_id,
                status="completed",
                processing_latency_ms=round(
                    (time.perf_counter() - pipeline_started) * 1000
                ),
            )
            completed = self._queue_continue_listening(completed, turn_id)
            self.publish("turn_updated", completed)
            return

        if tts_coordinator is not None:
            voice_text = limit_voice_reply(
                response.strip(),
                config.get("voice_reply_max_sentences", 2),
                config.get("voice_reply_max_chars", 160),
            )
            voice_chunks = split_tts_sentences(
                voice_text, config.get("tts_sentence_max_chars", 80)
            )
            for chunk in voice_chunks[len(streamed_voice_chunks) :]:
                if not tts_coordinator.submit(chunk):
                    break
                streamed_voice_chunks.append(chunk)
            try:
                tts_coordinator.finish()
            except Exception as error:
                failed = self.database.update_turn(
                    turn_id,
                    status="tts_failed",
                    tts_latency_ms=round((time.perf_counter() - started) * 1000),
                    processing_latency_ms=round(
                        (time.perf_counter() - pipeline_started) * 1000
                    ),
                    error_message=str(error)[:1000],
                )
                self.log("ERROR", "tts", "流式分句语音合成失败", {"error": str(error)}, turn_id)
                failed = self._queue_continue_listening(failed, turn_id)
                self.publish("turn_updated", failed)
            return

        selected_tts_voice = (
            config["unisound_tts_voice"]
            if config["tts_provider"] == "unisound"
            else config["tts_voice"]
        )
        synthesizing = self.database.update_turn(
            turn_id,
            status="tts_synthesizing",
            tts_provider=config["tts_provider"],
            tts_voice=selected_tts_voice,
            error_message="",
        )
        self.publish("turn_updated", synthesizing)
        self.log("INFO", "tts", "开始合成 LLM 语音回复", turn_id=turn_id)
        started = time.perf_counter()
        pcm_path = self.tts_dir / f"{turn_id}.pcm"
        voice_text = limit_voice_reply(
            response.strip(),
            config.get("voice_reply_max_sentences", 2),
            config.get("voice_reply_max_chars", 160),
        )
        try:
            audio = self._run_tts(voice_text, pcm_path, config)
        except Exception as error:
            tts_elapsed = round((time.perf_counter() - started) * 1000)
            failed = self.database.update_turn(
                turn_id,
                status="tts_failed",
                tts_latency_ms=tts_elapsed,
                processing_latency_ms=round(
                    (time.perf_counter() - pipeline_started) * 1000
                ),
                error_message=str(error)[:1000],
            )
            self.log("ERROR", "tts", "语音合成失败", {"error": str(error)}, turn_id)
            failed = self._queue_continue_listening(failed, turn_id)
            self.publish("turn_updated", failed)
            return
        tts_elapsed = round((time.perf_counter() - started) * 1000)
        processing_ms = round((time.perf_counter() - pipeline_started) * 1000)
        if self._playback_sender is None:
            completed = self.database.update_turn(
                turn_id,
                status="completed",
                tts_latency_ms=tts_elapsed,
                audio_duration_ms=audio["duration_ms"],
                processing_latency_ms=processing_ms,
                error_message="未配置 ESP32 播放命令通道",
            )
            self.publish("turn_updated", completed)
            return
        queued_at = utc_now()
        try:
            command = self._playback_sender(
                turn["device_id"],
                {
                    "type": "play_audio",
                    "turn_id": turn_id,
                    "audio_path": f"/api/tts/{turn_id}/pcm",
                    "sample_rate": audio["sample_rate"],
                    "sample_count": audio["sample_count"],
                },
            )
        except Exception as error:
            failed = self.database.update_turn(
                turn_id,
                status="playback_failed",
                tts_latency_ms=tts_elapsed,
                audio_duration_ms=audio["duration_ms"],
                processing_latency_ms=processing_ms,
                error_message=str(error)[:1000],
            )
            self.log("ERROR", "playback", "播放命令下发失败", {"error": str(error)}, turn_id)
            self.publish("turn_updated", failed)
            return
        queued = self.database.update_turn(
            turn_id,
            status="playback_queued",
            tts_latency_ms=tts_elapsed,
            audio_duration_ms=audio["duration_ms"],
            processing_latency_ms=processing_ms,
            playback_command_id=command["id"],
            playback_queued_at=queued_at,
            error_message="",
        )
        self.log(
            "INFO",
            "tts",
            "语音合成完成并已下发 ESP32",
            {
                "tts_latency_ms": tts_elapsed,
                "audio_duration_ms": audio["duration_ms"],
                "processing_latency_ms": processing_ms,
            },
            turn_id,
        )
        self.publish("turn_updated", queued)

    def _process_tts_sentences(
        self,
        turn: dict[str, Any],
        turn_id: str,
        response: str,
        config: dict[str, Any],
        pipeline_started: float,
    ) -> None:
        """Synthesize and queue the first sentence as soon as it is ready."""
        chunks = split_tts_sentences(
            response,
            config.get("tts_sentence_max_chars", 80),
        )
        if not chunks:
            chunks = [_prepare_tts_text(response)]
        chunk_count = len(chunks)
        selected_tts_voice = (
            config["unisound_tts_voice"]
            if config["tts_provider"] == "unisound"
            else config["tts_voice"]
        )
        synthesizing = self.database.update_turn(
            turn_id,
            status="tts_synthesizing",
            tts_provider=config["tts_provider"],
            tts_voice=selected_tts_voice,
            error_message="",
        )
        self.publish("turn_updated", synthesizing)
        self.log(
            "INFO",
            "tts",
            "开始分句合成 LLM 语音回复",
            {"chunk_count": chunk_count},
            turn_id,
        )
        started = time.perf_counter()
        first_command: dict[str, Any] | None = None
        final_command: dict[str, Any] | None = None
        total_duration_ms = 0
        total_samples = 0
        for index, chunk in enumerate(chunks):
            pcm_path = self.tts_dir / (
                f"{turn_id}.pcm" if chunk_count == 1 else f"{turn_id}-chunk-{index}.pcm"
            )
            try:
                audio = self._run_tts(chunk, pcm_path, config)
            except Exception as error:
                tts_elapsed = round((time.perf_counter() - started) * 1000)
                failed = self.database.update_turn(
                    turn_id,
                    status="tts_failed",
                    tts_latency_ms=tts_elapsed,
                    processing_latency_ms=round(
                        (time.perf_counter() - pipeline_started) * 1000
                    ),
                    error_message=str(error)[:1000],
                )
                self.log(
                    "ERROR",
                    "tts",
                    "分句语音合成失败",
                    {"chunk_index": index, "error": str(error)},
                    turn_id,
                )
                failed = self._queue_continue_listening(failed, turn_id)
                self.publish("turn_updated", failed)
                return

            total_duration_ms += int(audio["duration_ms"])
            total_samples += int(audio["sample_count"])
            if self._playback_sender is None:
                continue
            audio_path = (
                f"/api/tts/{turn_id}/pcm"
                if chunk_count == 1
                else f"/api/tts/{turn_id}/chunk/{index}/pcm"
            )
            try:
                command = self._playback_sender(
                    turn["device_id"],
                    {
                        "type": "play_audio",
                        "turn_id": turn_id,
                        "audio_path": audio_path,
                        "sample_rate": audio["sample_rate"],
                        "sample_count": audio["sample_count"],
                        "chunk_index": index,
                        "chunk_count": chunk_count,
                        "is_final_chunk": index == chunk_count - 1,
                    },
                )
            except Exception as error:
                failed = self.database.update_turn(
                    turn_id,
                    status="playback_failed",
                    tts_latency_ms=round((time.perf_counter() - started) * 1000),
                    audio_duration_ms=total_duration_ms,
                    processing_latency_ms=round(
                        (time.perf_counter() - pipeline_started) * 1000
                    ),
                    error_message=str(error)[:1000],
                )
                self.log(
                    "ERROR",
                    "playback",
                    "分句播放命令下发失败",
                    {"chunk_index": index, "error": str(error)},
                    turn_id,
                )
                self.publish("turn_updated", failed)
                return
            playback_info = {
                "turn_id": turn_id,
                "chunk_index": index,
                "chunk_count": chunk_count,
            }
            with self._playback_turns_lock:
                self._playback_turns[command["id"]] = playback_info
            if first_command is None:
                first_command = command
            final_command = command
            if first_command["id"] == command["id"]:
                queued = self.database.update_turn(
                    turn_id,
                    status="playback_queued",
                    audio_duration_ms=total_duration_ms,
                    playback_command_id=command["id"],
                    playback_queued_at=utc_now(),
                    error_message="",
                )
                self.publish("turn_updated", queued)

        tts_elapsed = round((time.perf_counter() - started) * 1000)
        processing_ms = round((time.perf_counter() - pipeline_started) * 1000)
        if self._playback_sender is None:
            completed = self.database.update_turn(
                turn_id,
                status="completed",
                tts_latency_ms=tts_elapsed,
                audio_duration_ms=total_duration_ms,
                processing_latency_ms=processing_ms,
                error_message="未配置 ESP32 播放命令通道",
            )
            self.publish("turn_updated", completed)
            return
        if first_command is None or final_command is None:
            failed = self.database.update_turn(
                turn_id,
                status="playback_failed",
                tts_latency_ms=tts_elapsed,
                audio_duration_ms=total_duration_ms,
                processing_latency_ms=processing_ms,
                error_message="未生成可播放的 TTS 分片",
            )
            self.publish("turn_updated", failed)
            return
        with self._playback_turns_lock:
            self._final_playback_commands[turn_id] = final_command["id"]
        queued = self.database.update_turn(
            turn_id,
            status="playback_queued",
            tts_latency_ms=tts_elapsed,
            audio_duration_ms=total_duration_ms,
            processing_latency_ms=processing_ms,
            error_message="",
        )
        self.log(
            "INFO",
            "tts",
            "首段语音已提前下发，分句队列已建立",
            {
                "chunk_count": chunk_count,
                "tts_latency_ms": tts_elapsed,
                "audio_duration_ms": total_duration_ms,
                "processing_latency_ms": processing_ms,
            },
            turn_id,
        )
        self.publish("turn_updated", queued)

    def _run_transcriber(self, path: Path, config: dict[str, Any]) -> str:
        if self._transcriber is not None:
            return self._transcriber(path, config)
        if config["asr_provider"] == "unisound":
            return self._run_unisound_asr(path, config)
        if config["asr_provider"] == "custom_http":
            return self._run_custom_asr(path, config)
        try:
            from funasr.utils.postprocess_utils import rich_transcription_postprocess
        except ImportError as error:
            raise RuntimeError(
                "SenseVoice 运行环境未安装，请使用 gateway/.venv 启动或安装 funasr"
            ) from error
        self._ensure_sensevoice_model(config)
        with self._asr_lock:
            if self._model is None:
                raise RuntimeError("SenseVoice 模型未加载")
            result = self._model.generate(
                input=str(path),
                cache={},
                language=config["asr_language"],
                use_itn=config["asr_use_itn"],
                batch_size=1,
            )
        if not result or not isinstance(result[0], dict):
            raise RuntimeError("SenseVoice 返回格式无效")
        return rich_transcription_postprocess(str(result[0].get("text", "")))

    @staticmethod
    def _run_custom_asr(path: Path, config: dict[str, Any]) -> str:
        boundary = f"----ai-mirror-{uuid.uuid4().hex}"
        chunks: list[bytes] = []

        def add_field(name: str, value: str) -> None:
            chunks.extend(
                [
                    f"--{boundary}\r\n".encode("ascii"),
                    f'Content-Disposition: form-data; name="{name}"\r\n\r\n'.encode(
                        "ascii"
                    ),
                    value.encode("utf-8"),
                    b"\r\n",
                ]
            )

        add_field("model", config["asr_api_model"] or config["asr_model"])
        add_field("language", config["asr_language"])
        add_field("response_format", "json")
        chunks.extend(
            [
                f"--{boundary}\r\n".encode("ascii"),
                b'Content-Disposition: form-data; name="file"; filename="audio.wav"\r\n',
                b"Content-Type: audio/wav\r\n\r\n",
                path.read_bytes(),
                b"\r\n",
                f"--{boundary}--\r\n".encode("ascii"),
            ]
        )
        headers = {
            "Content-Type": f"multipart/form-data; boundary={boundary}",
            "User-Agent": "ai-mirror-gateway/1.0",
        }
        if config.get("asr_api_key"):
            headers["Authorization"] = f"Bearer {config['asr_api_key']}"
        request = urllib.request.Request(
            config["asr_api_url"], data=b"".join(chunks), headers=headers, method="POST"
        )
        try:
            with urllib.request.urlopen(
                request, timeout=config["asr_api_timeout_seconds"]
            ) as response:
                raw = response.read(1024 * 1024 + 1)
        except urllib.error.HTTPError as error:
            detail = error.read(2048).decode("utf-8", errors="replace")
            raise RuntimeError(f"Custom ASR HTTP {error.code}: {detail}") from None
        except urllib.error.URLError as error:
            raise RuntimeError(f"Custom ASR network error: {error.reason}") from None
        if len(raw) > 1024 * 1024:
            raise RuntimeError("Custom ASR response is too large")
        try:
            result = json.loads(raw)
            text = result.get("text")
            if text is None and isinstance(result.get("result"), dict):
                text = result["result"].get("text")
        except (UnicodeDecodeError, json.JSONDecodeError, AttributeError):
            raise RuntimeError("Custom ASR must return JSON containing text") from None
        if not isinstance(text, str):
            raise RuntimeError("Custom ASR response does not contain text")
        return text

    @staticmethod
    def _unisound_headers(
        config: dict[str, Any], content_type: str | None = "application/json"
    ) -> dict[str, str]:
        api_key = str(config.get("unisound_api_key", "")).strip()
        if not api_key:
            raise RuntimeError("请先在 Web 模型配置中填写云知声 Token Plan API Key")
        headers = {
            "Authorization": f"Bearer {api_key}",
            "User-Agent": "ai-mirror-gateway/1.0",
        }
        if content_type:
            headers["Content-Type"] = content_type
        return headers

    @classmethod
    def _unisound_json_request(
        cls,
        config: dict[str, Any],
        method: str,
        path: str,
        payload: dict[str, Any] | None = None,
        *,
        content_type: str | None = "application/json",
        raw_body: bytes | None = None,
    ) -> dict[str, Any]:
        if payload is not None and raw_body is not None:
            raise ValueError("payload and raw_body are mutually exclusive")
        data = raw_body
        if payload is not None:
            data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        request = urllib.request.Request(
            f"{config['unisound_base_url'].rstrip('/')}{path}",
            data=data,
            headers=cls._unisound_headers(config, content_type),
            method=method,
        )
        try:
            with urllib.request.urlopen(
                request, timeout=config["unisound_timeout_seconds"]
            ) as response:
                raw = response.read(2 * 1024 * 1024 + 1)
        except urllib.error.HTTPError as error:
            detail = error.read(4096).decode("utf-8", errors="replace")
            raise RuntimeError(f"Unisound HTTP {error.code}: {detail}") from None
        except urllib.error.URLError as error:
            raise RuntimeError(f"Unisound 网络错误: {error.reason}") from None
        if len(raw) > 2 * 1024 * 1024:
            raise RuntimeError("Unisound JSON response is too large")
        try:
            result = json.loads(raw)
        except (UnicodeDecodeError, json.JSONDecodeError):
            raise RuntimeError("Unisound 返回了无效 JSON") from None
        if not isinstance(result, dict):
            raise RuntimeError("Unisound JSON response must be an object")
        base_resp = result.get("base_resp")
        if isinstance(base_resp, dict) and int(base_resp.get("status_code", 0)) != 0:
            raise RuntimeError(
                "Unisound 业务错误 "
                f"{base_resp.get('status_code')}: {base_resp.get('status_msg', '')}"
            )
        return result

    @classmethod
    def _unisound_upload_asr_file(
        cls, path: Path, config: dict[str, Any]
    ) -> int:
        boundary = f"----ai-mirror-unisound-{uuid.uuid4().hex}"
        body = b"".join(
            [
                f"--{boundary}\r\n".encode("ascii"),
                b'Content-Disposition: form-data; name="purpose"\r\n\r\n',
                b"a2t_async_input\r\n",
                f"--{boundary}\r\n".encode("ascii"),
                b'Content-Disposition: form-data; name="file"; filename="recording.wav"\r\n',
                b"Content-Type: audio/wav\r\n\r\n",
                path.read_bytes(),
                b"\r\n",
                f"--{boundary}--\r\n".encode("ascii"),
            ]
        )
        result = cls._unisound_json_request(
            config,
            "POST",
            "/files/upload",
            content_type=f"multipart/form-data; boundary={boundary}",
            raw_body=body,
        )
        try:
            return int(result["file"]["file_id"])
        except (KeyError, TypeError, ValueError):
            raise RuntimeError("Unisound 上传响应缺少 file_id") from None

    @classmethod
    def _unisound_delete_asr_file(
        cls, file_id: int, config: dict[str, Any]
    ) -> None:
        cls._unisound_json_request(
            config,
            "POST",
            "/files/delete",
            {"file_id": file_id, "purpose": "a2t_async_input"},
        )

    @classmethod
    def _unisound_poll_task(
        cls,
        config: dict[str, Any],
        path: str,
        task_name: str,
    ) -> dict[str, Any]:
        deadline = time.monotonic() + config["unisound_timeout_seconds"]
        while True:
            result = cls._unisound_json_request(config, "GET", path, content_type=None)
            status = str(result.get("status", ""))
            if status == "Success":
                return result
            if status == "Failed":
                base_resp = result.get("base_resp")
                detail = (
                    base_resp.get("status_msg", "")
                    if isinstance(base_resp, dict)
                    else ""
                )
                raise RuntimeError(f"Unisound {task_name}任务失败: {detail}")
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise RuntimeError(f"Unisound {task_name}任务超时")
            time.sleep(
                min(config["unisound_poll_interval_ms"] / 1000, remaining)
            )

    @classmethod
    def _run_unisound_asr(cls, path: Path, config: dict[str, Any]) -> str:
        file_id = cls._unisound_upload_asr_file(path, config)
        try:
            language_map = {
                "zh": "zh-CN",
                "yue": "zh-CN",
                "en": "en-US",
                "ja": "ja-JP",
                "ko": "ko-KR",
            }
            payload: dict[str, Any] = {
                "file_id": file_id,
                "model": config["unisound_asr_model"],
                "format": "wav",
                "sample_rate": 16000,
                "enable_auto_lang": config["asr_language"] == "auto",
                "enable_itn": config["asr_use_itn"],
                "channel": 1,
                "enable_speaker": False,
                "word_info": False,
                "hotwords": ["云知声"],
            }
            language = language_map.get(config["asr_language"])
            if language:
                payload["language"] = language
            created = cls._unisound_json_request(
                config, "POST", "/audio/asr/tasks", payload
            )
            task_id = str(created.get("task_id", "")).strip()
            if not task_id:
                raise RuntimeError("Unisound ASR 响应缺少 task_id")
            result = cls._unisound_poll_task(
                config, f"/audio/asr/tasks/{task_id}", "ASR"
            )
            segments = result.get("results")
            if not isinstance(segments, list):
                raise RuntimeError("Unisound ASR 响应缺少 results")
            return "".join(
                str(segment.get("text", ""))
                for segment in segments
                if isinstance(segment, dict)
            ).strip()
        finally:
            try:
                cls._unisound_delete_asr_file(file_id, config)
            except Exception:
                pass

    def _run_llm(
        self,
        messages: list[dict[str, str]],
        config: dict[str, Any],
        on_delta: LLMDeltaCallback | None = None,
    ) -> tuple[str, dict[str, int]]:
        if self._llm_caller is not None:
            content, usage = self._llm_caller(messages, config)
            if on_delta and content:
                on_delta(content)
            return content, usage

        provider = config["llm_provider"]
        is_unisound = provider == "unisound"
        provider_label = "Unisound U2" if is_unisound else "DeepSeek"
        model = config["unisound_llm_model"] if is_unisound else config["llm_model"]
        base_url = (
            config["unisound_base_url"] if is_unisound else config["llm_base_url"]
        )
        api_key = (
            config["unisound_api_key"] if is_unisound else config["llm_api_key"]
        )
        payload: dict[str, Any] = {
            "model": model,
            "messages": messages,
            "max_tokens": config["llm_max_tokens"],
            "stream": bool(on_delta and config.get("llm_streaming_enabled", True)),
        }
        if is_unisound:
            payload["temperature"] = config["llm_temperature"]
        else:
            payload["thinking"] = {
                "type": "enabled" if config["llm_thinking"] else "disabled"
            }
            if not config["llm_thinking"]:
                payload["temperature"] = config["llm_temperature"]
        request = urllib.request.Request(
            f"{base_url.rstrip('/')}/chat/completions",
            data=json.dumps(payload, ensure_ascii=False).encode("utf-8"),
            headers={
                "Authorization": f"Bearer {api_key}",
                "Content-Type": "application/json",
                "Accept": "text/event-stream" if payload["stream"] else "application/json",
                "User-Agent": "ai-mirror-gateway/1.0",
            },
            method="POST",
        )
        text_parts: list[str] = []
        usage: dict[str, int] = {}

        def consume_sse_line(line: str) -> None:
            line = line.strip()
            if not line or not line.startswith("data:"):
                return
            data = line[5:].strip()
            if data == "[DONE]":
                return
            try:
                item = json.loads(data)
            except json.JSONDecodeError:
                return
            if not isinstance(item, dict):
                return
            item_usage = item.get("usage")
            if isinstance(item_usage, dict):
                usage.update(
                    {
                        str(key): int(value)
                        for key, value in item_usage.items()
                        if isinstance(value, (int, float))
                    }
                )
            choices = item.get("choices")
            if not isinstance(choices, list) or not choices:
                return
            choice = choices[0] if isinstance(choices[0], dict) else {}
            delta = choice.get("delta") if isinstance(choice, dict) else {}
            piece = delta.get("content") if isinstance(delta, dict) else None
            if not isinstance(piece, str) or not piece:
                message = choice.get("message") if isinstance(choice, dict) else {}
                piece = message.get("content") if isinstance(message, dict) else None
            if isinstance(piece, str) and piece:
                text_parts.append(piece)
                if on_delta:
                    on_delta(piece)

        try:
            with urllib.request.urlopen(
                request, timeout=config["llm_timeout_seconds"]
            ) as response:
                if payload["stream"]:
                    raw_lines: list[bytes] = []
                    for raw_line in response:
                        raw_lines.append(raw_line)
                        consume_sse_line(raw_line.decode("utf-8", errors="replace"))
                    raw = b"".join(raw_lines)
                else:
                    raw = response.read()
        except urllib.error.HTTPError as error:
            detail = error.read(2048).decode("utf-8", errors="replace")
            raise RuntimeError(
                f"{provider_label} HTTP {error.code}: {detail}"
            ) from None
        except urllib.error.URLError as error:
            raise RuntimeError(
                f"{provider_label} 网络错误: {error.reason}"
            ) from None
        if not payload["stream"]:
            try:
                result = json.loads(raw)
            except (UnicodeDecodeError, json.JSONDecodeError):
                raise RuntimeError(f"{provider_label} 返回了无效 JSON") from None
            try:
                choice = result["choices"][0]
                content = choice["message"]["content"]
            except (KeyError, IndexError, TypeError):
                raise RuntimeError(f"{provider_label} 返回格式无效") from None
            if not isinstance(content, str):
                finish_reason = choice.get("finish_reason") if isinstance(choice, dict) else ""
                raise RuntimeError(
                    f"{provider_label} 未返回正文"
                    f" (finish_reason={finish_reason or 'unknown'})，请增大最大输出"
                )
            usage = result.get("usage") if isinstance(result.get("usage"), dict) else {}
            return content, usage

        if not text_parts:
            # A few providers ignore stream=true and return one ordinary JSON body.
            try:
                result = json.loads(raw)
                choice = result["choices"][0]
                content = choice.get("message", {}).get("content", "")
                if isinstance(result.get("usage"), dict):
                    usage.update(
                        {
                            str(key): int(value)
                            for key, value in result["usage"].items()
                            if isinstance(value, (int, float))
                        }
                    )
                if isinstance(content, str) and content:
                    text_parts.append(content)
                    if on_delta:
                        on_delta(content)
            except (KeyError, IndexError, TypeError, UnicodeDecodeError, json.JSONDecodeError):
                pass
        content = "".join(text_parts)
        if not content:
            raise RuntimeError(f"{provider_label} 未返回正文，请检查流式接口兼容性")
        return content, usage

    @staticmethod
    def _convert_audio_to_pcm(source_path: Path, pcm_path: Path, timeout: int) -> None:
        conversion = subprocess.run(
            [
                "ffmpeg",
                "-y",
                "-loglevel",
                "error",
                "-i",
                str(source_path),
                "-f",
                "s16le",
                "-acodec",
                "pcm_s16le",
                "-ar",
                "16000",
                "-ac",
                "1",
                str(pcm_path),
            ],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=timeout,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        )
        if conversion.returncode != 0:
            raise RuntimeError(f"ffmpeg PCM conversion failed: {conversion.stderr.strip()}")

    def _run_custom_tts(
        self, text: str, pcm_path: Path, config: dict[str, Any]
    ) -> dict[str, int]:
        payload = {
            "model": config["tts_api_model"],
            "voice": config["tts_voice"],
            "input": text,
            "prompt": config["tts_prompt"],
            "response_format": "wav",
            "sample_rate": 16000,
        }
        headers = {
            "Content-Type": "application/json",
            "Accept": "audio/wav, audio/mpeg, audio/pcm, application/json",
            "User-Agent": "ai-mirror-gateway/1.0",
        }
        if config.get("tts_api_key"):
            headers["Authorization"] = f"Bearer {config['tts_api_key']}"
        request = urllib.request.Request(
            config["tts_api_url"],
            data=json.dumps(payload, ensure_ascii=False).encode("utf-8"),
            headers=headers,
            method="POST",
        )
        try:
            with urllib.request.urlopen(
                request, timeout=config["tts_api_timeout_seconds"]
            ) as response:
                content_type = response.headers.get_content_type().lower()
                audio_format = response.headers.get("X-Audio-Format", "").lower()
                raw = response.read(16 * 1024 * 1024 + 1)
        except urllib.error.HTTPError as error:
            detail = error.read(2048).decode("utf-8", errors="replace")
            raise RuntimeError(f"Custom TTS HTTP {error.code}: {detail}") from None
        except urllib.error.URLError as error:
            raise RuntimeError(f"Custom TTS network error: {error.reason}") from None
        if not raw:
            raise RuntimeError("Custom TTS returned empty audio")
        if len(raw) > 16 * 1024 * 1024:
            raise RuntimeError("Custom TTS response is too large")

        if content_type == "application/json":
            try:
                result = json.loads(raw)
                raw = base64.b64decode(result["audio_base64"], validate=True)
                audio_format = str(result.get("format", "wav")).lower()
            except (KeyError, TypeError, ValueError, json.JSONDecodeError):
                raise RuntimeError(
                    "Custom TTS JSON must contain valid audio_base64"
                ) from None

        raw_pcm = audio_format in {"pcm", "pcm_s16le", "s16le"} or content_type in {
            "audio/l16",
            "audio/pcm",
        }
        temp_path = pcm_path.with_name(f".{pcm_path.stem}.custom-audio.tmp")
        try:
            if raw_pcm:
                pcm_path.write_bytes(raw)
            else:
                temp_path.write_bytes(raw)
                self._convert_audio_to_pcm(
                    temp_path, pcm_path, config["tts_api_timeout_seconds"]
                )
        finally:
            temp_path.unlink(missing_ok=True)

        size_bytes = pcm_path.stat().st_size
        if size_bytes <= 0 or size_bytes % 2:
            raise RuntimeError("Custom TTS produced invalid signed-16 PCM")
        sample_count = size_bytes // 2
        return {
            "sample_rate": 16000,
            "sample_count": sample_count,
            "duration_ms": round(sample_count * 1000 / 16000),
        }

    def _run_unisound_tts(
        self, text: str, pcm_path: Path, config: dict[str, Any]
    ) -> dict[str, int]:
        speed = max(0, min(100, round(50 + config["tts_rate"] / 2)))
        volume = max(0, min(100, round(50 + config["tts_volume"] / 2)))
        payload = {
            "model": config["unisound_tts_model"],
            "text": text,
            "voice_setting": {
                "voice_id": config["unisound_tts_voice"],
                "speed": speed,
                "volume": volume,
                "pitch": 50,
                "bright": 50,
                "emotion": "neutral",
                "language": "zh",
            },
            "audio_setting": {
                "audio_sample_rate": 16000,
                "format": "mp3",
                "channel": 1,
            },
        }
        created = self._unisound_json_request(
            config, "POST", "/audio/speech/tasks", payload
        )
        task_id = str(created.get("task_id", "")).strip()
        if not task_id:
            raise RuntimeError("Unisound TTS 响应缺少 task_id")
        query = urlencode({"task_id": task_id})
        result = self._unisound_poll_task(
            config, f"/audio/speech/tasks?{query}", "TTS"
        )
        try:
            file_id = int(result["file_id"])
        except (KeyError, TypeError, ValueError):
            raise RuntimeError("Unisound TTS 响应缺少 file_id") from None

        request = urllib.request.Request(
            f"{config['unisound_base_url'].rstrip('/')}/files/retrieve_content?"
            f"{urlencode({'file_id': file_id})}",
            headers=self._unisound_headers(config, None),
            method="GET",
        )
        try:
            with urllib.request.urlopen(
                request, timeout=config["unisound_timeout_seconds"]
            ) as response:
                raw = response.read(16 * 1024 * 1024 + 1)
        except urllib.error.HTTPError as error:
            detail = error.read(4096).decode("utf-8", errors="replace")
            raise RuntimeError(f"Unisound TTS 下载 HTTP {error.code}: {detail}") from None
        except urllib.error.URLError as error:
            raise RuntimeError(f"Unisound TTS 下载网络错误: {error.reason}") from None
        if not raw:
            raise RuntimeError("Unisound TTS 返回了空音频")
        if len(raw) > 16 * 1024 * 1024:
            raise RuntimeError("Unisound TTS 音频超过 16 MiB")

        mp3_path = pcm_path.with_name(f".{pcm_path.stem}.unisound.mp3")
        try:
            mp3_path.write_bytes(raw)
            self._convert_audio_to_pcm(
                mp3_path, pcm_path, config["unisound_timeout_seconds"]
            )
        finally:
            mp3_path.unlink(missing_ok=True)
        size_bytes = pcm_path.stat().st_size
        if size_bytes <= 0 or size_bytes % 2:
            raise RuntimeError("Unisound TTS 生成的 PCM 数据无效")
        sample_count = size_bytes // 2
        return {
            "sample_rate": 16000,
            "sample_count": sample_count,
            "duration_ms": round(sample_count * 1000 / 16000),
        }

    def _run_tts(
        self, text: str, pcm_path: Path, config: dict[str, Any]
    ) -> dict[str, int]:
        text = _prepare_tts_text(text)
        if not text:
            raise RuntimeError("LLM 回复在清理格式符后没有可朗读内容")
        if self._tts_caller is not None:
            return self._tts_caller(text, pcm_path, config)
        if config["tts_provider"] == "unisound":
            return self._run_unisound_tts(text, pcm_path, config)
        if config["tts_provider"] == "custom_http":
            return self._run_custom_tts(text, pcm_path, config)
        try:
            import edge_tts
        except ImportError as error:
            raise RuntimeError("缺少 edge-tts，请重新安装 gateway/requirements.txt") from error
        mp3_path = pcm_path.with_name(f".{pcm_path.stem}.tmp.mp3")
        try:
            last_error: Exception | None = None
            for attempt in range(2):
                try:
                    edge_tts.Communicate(
                        text,
                        config["tts_voice"],
                        rate=f"{config['tts_rate']:+d}%",
                        volume=f"{config['tts_volume']:+d}%",
                    ).save_sync(str(mp3_path))
                    last_error = None
                    break
                except Exception as error:
                    last_error = error
                    mp3_path.unlink(missing_ok=True)
                    if attempt == 0:
                        time.sleep(0.4)
            if last_error is not None:
                raise RuntimeError(f"Edge TTS 网络合成失败：{last_error}")
            conversion = subprocess.run(
                [
                    "ffmpeg",
                    "-y",
                    "-loglevel",
                    "error",
                    "-i",
                    str(mp3_path),
                    "-f",
                    "s16le",
                    "-acodec",
                    "pcm_s16le",
                    "-ar",
                    "16000",
                    "-ac",
                    "1",
                    str(pcm_path),
                ],
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                timeout=60,
                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
            )
            if conversion.returncode != 0:
                raise RuntimeError(f"ffmpeg PCM 转换失败：{conversion.stderr.strip()}")
        finally:
            mp3_path.unlink(missing_ok=True)
        size_bytes = pcm_path.stat().st_size
        if size_bytes <= 0 or size_bytes % 2:
            raise RuntimeError("TTS 生成的 PCM 数据无效")
        sample_count = size_bytes // 2
        return {
            "sample_rate": 16000,
            "sample_count": sample_count,
            "duration_ms": round(sample_count * 1000 / 16000),
        }
