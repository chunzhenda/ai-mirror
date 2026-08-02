#!/usr/bin/env python3
"""AI Mirror local gateway: ESP32 transport, WAV storage, and web console."""

from __future__ import annotations

import argparse
import base64
import hashlib
import hmac
import json
import mimetypes
import queue
import re
import shutil
import socket
import struct
import threading
import time
import uuid
import wave
from collections import defaultdict, deque
from dataclasses import dataclass
from datetime import datetime, timezone
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, Callable
from urllib.parse import unquote, urlparse, parse_qs

from .ai_services import ConversationService, LLMCaller, Transcriber, TTSCaller
from .maintenance import cleanup_tree


GATEWAY_ROOT = Path(__file__).resolve().parents[1]


MAX_RECORDING_BYTES = 8 * 1024 * 1024
MAX_JSON_BYTES = 64 * 1024
MAX_EVENTS = 200
MAX_COMMANDS_PER_DEVICE = 20
MAX_IDEMPOTENCY_KEY_LENGTH = 160
DEVICE_ID_RE = re.compile(r"^[A-Za-z0-9._-]{1,64}$")
COMMAND_TYPES = {
    "ping",
    "get_status",
    "start_recording",
    "play_audio",
    "continue_listening",
    "end_conversation",
}
WEBSOCKET_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
STREAM_PCM_CHUNK_BYTES = 2048
STREAM_PCM_PREFILL_BYTES = 32 * 1024
STREAM_PCM_PACE_FACTOR = 1.15
STREAM_PCM_END_GUARD_SECONDS = 0.4


@dataclass
class _CommandWebSocketSession:
    token: str
    stop: threading.Event
    connection: Any


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds")


def stream_pcm_file(
    path: Path,
    write_frame: Callable[[int, bytes], None],
    stop: threading.Event,
    *,
    sample_rate: int,
    chunk_bytes: int = STREAM_PCM_CHUNK_BYTES,
    prefill_bytes: int = STREAM_PCM_PREFILL_BYTES,
    pace_factor: float = STREAM_PCM_PACE_FACTOR,
    sleep_fn: Callable[[float], None] = time.sleep,
    monotonic_fn: Callable[[], float] = time.monotonic,
) -> dict[str, Any]:
    """Send PCM at a device-consumable rate after a short startup prefill.

    The ESP32 keeps only a small bounded queue for streaming playback. Sending
    an entire long file as quickly as TCP accepts it fills that queue and
    causes the device command socket to be reset before playback completes.
    A short prefill absorbs normal Wi-Fi jitter; afterward the sender follows
    the PCM playback clock with a small safety margin.
    """
    if sample_rate <= 0:
        raise ValueError("sample_rate must be positive")
    if chunk_bytes <= 0:
        raise ValueError("chunk_bytes must be positive")
    if prefill_bytes < 0:
        raise ValueError("prefill_bytes cannot be negative")
    if pace_factor <= 0:
        raise ValueError("pace_factor must be positive")

    started = monotonic_fn()
    next_deadline: float | None = None
    bytes_sent = 0
    chunks_sent = 0
    completed = False
    with path.open("rb") as audio_file:
        while not stop.is_set():
            chunk = audio_file.read(chunk_bytes)
            if not chunk:
                completed = True
                break
            write_frame(0x2, chunk)
            bytes_sent += len(chunk)
            chunks_sent += 1

            if bytes_sent <= prefill_bytes:
                continue
            if next_deadline is None:
                next_deadline = monotonic_fn()
            next_deadline += (len(chunk) / (sample_rate * 2)) * pace_factor
            delay = next_deadline - monotonic_fn()
            if delay > 0:
                sleep_fn(delay)

    return {
        "bytes_sent": bytes_sent,
        "chunks_sent": chunks_sent,
        "completed": completed,
        "elapsed_ms": round((monotonic_fn() - started) * 1000),
        "prefill_bytes": prefill_bytes,
        "pace_factor": pace_factor,
    }


class GatewayStore:
    def __init__(self, data_dir: Path) -> None:
        self.data_dir = data_dir.resolve()
        self.recordings_dir = self.data_dir / "recordings"
        self.recordings_dir.mkdir(parents=True, exist_ok=True)
        self._condition = threading.Condition(threading.RLock())
        self._devices: dict[str, dict[str, Any]] = {}
        self._recordings: list[dict[str, Any]] = []
        self._events: deque[dict[str, Any]] = deque(maxlen=MAX_EVENTS)
        self._commands: dict[str, deque[dict[str, Any]]] = defaultdict(
            lambda: deque(maxlen=MAX_COMMANDS_PER_DEVICE)
        )
        self._command_websocket_sessions: dict[str, _CommandWebSocketSession] = {}
        self._recording_websocket_devices: set[str] = set()
        self._recording_callback = None
        self._load_recordings()

    def set_recording_callback(self, callback) -> None:
        self._recording_callback = callback

    def _load_recordings(self) -> None:
        loaded: list[dict[str, Any]] = []
        for metadata_path in self.recordings_dir.glob("*.json"):
            try:
                metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
                wav_path = self.recordings_dir / metadata["filename"]
                if wav_path.is_file() and DEVICE_ID_RE.fullmatch(metadata["device_id"]):
                    loaded.append(metadata)
            except (OSError, KeyError, TypeError, ValueError, json.JSONDecodeError):
                continue
        loaded.sort(key=lambda item: item.get("created_at", ""), reverse=True)
        self._recordings = loaded

    def _touch_device_locked(self, device_id: str) -> None:
        device = self._devices.setdefault(
            device_id,
            {
                "id": device_id,
                "command_websocket_connected": False,
                "recording_websocket_connected": False,
                "reconnect_count": 0,
            },
        )
        device["last_seen"] = utc_now()

    def touch_device(self, device_id: str) -> None:
        with self._condition:
            self._touch_device_locked(device_id)

    def devices(self) -> list[dict[str, Any]]:
        with self._condition:
            devices = [dict(device) for device in self._devices.values()]
        return sorted(devices, key=lambda item: item["last_seen"], reverse=True)

    def device_health(self, stale_after_seconds: int = 30) -> list[dict[str, Any]]:
        now = time.time()
        result = []
        for device in self.devices():
            try:
                age = max(
                    0.0,
                    now - datetime.fromisoformat(str(device["last_seen"])).timestamp(),
                )
            except (KeyError, TypeError, ValueError, OSError):
                age = float("inf")
            item = dict(device)
            item["last_seen_age_seconds"] = round(age, 3)
            item["stale"] = age > max(1, stale_after_seconds)
            item["status"] = "stale" if item["stale"] else "online"
            result.append(item)
        return result

    def mark_recording_websocket(self, device_id: str, connected: bool) -> None:
        with self._condition:
            self._touch_device_locked(device_id)
            if connected:
                self._recording_websocket_devices.add(device_id)
            else:
                self._recording_websocket_devices.discard(device_id)
            device = self._devices[device_id]
            device["recording_websocket_connected"] = connected
            device[
                "last_recording_websocket_" + ("connected_at" if connected else "disconnected_at")
            ] = utc_now()

    def _mark_command_websocket_locked(self, device_id: str, connected: bool) -> None:
        self._touch_device_locked(device_id)
        device = self._devices[device_id]
        if connected:
            device["reconnect_count"] = int(device.get("reconnect_count", 0)) + 1
        device["command_websocket_connected"] = connected
        device[
            "last_command_websocket_" + ("connected_at" if connected else "disconnected_at")
        ] = utc_now()

    def save_recording(
        self,
        device_id: str,
        pcm: bytes,
        sample_rate: int,
        channels: int,
        bits: int,
        session_id: str = "",
        turn_index: int = 0,
        transport: str = "turn",
        protocol_version: int = 1,
    ) -> dict[str, Any]:
        recording_id = uuid.uuid4().hex
        filename = f"{recording_id}.wav"
        wav_path = self.recordings_dir / filename
        temp_path = self.recordings_dir / f".{recording_id}.tmp"

        with wave.open(str(temp_path), "wb") as output:
            output.setnchannels(channels)
            output.setsampwidth(bits // 8)
            output.setframerate(sample_rate)
            output.writeframes(pcm)
        temp_path.replace(wav_path)

        sample_count = len(pcm) // (channels * (bits // 8))
        metadata = {
            "id": recording_id,
            "device_id": device_id,
            "session_id": session_id,
            "turn_index": turn_index,
            "transport": transport,
            "protocol_version": protocol_version,
            "created_at": utc_now(),
            "sample_rate": sample_rate,
            "channels": channels,
            "bits_per_sample": bits,
            "sample_count": sample_count,
            "duration_ms": round(sample_count * 1000 / sample_rate),
            "size_bytes": wav_path.stat().st_size,
            "filename": filename,
            "audio_url": f"/api/recordings/{recording_id}/audio",
        }
        metadata_path = self.recordings_dir / f"{recording_id}.json"
        metadata_path.write_text(
            json.dumps(metadata, ensure_ascii=False, indent=2), encoding="utf-8"
        )

        with self._condition:
            self._touch_device_locked(device_id)
            self._recordings.insert(0, metadata)
            self._events.appendleft(
                {
                    "id": uuid.uuid4().hex,
                    "device_id": device_id,
                    "type": "recording_uploaded",
                    "created_at": metadata["created_at"],
                    "details": {
                        "recording_id": recording_id,
                        "duration_ms": metadata["duration_ms"],
                    },
                }
            )
        if self._recording_callback:
            try:
                self._recording_callback(dict(metadata))
            except Exception as error:
                # Recording persistence and the ESP32 ACK must not depend on AI services.
                print(f"AI queue callback failed: {error}")
        return dict(metadata)

    def cleanup_recordings(
        self,
        *,
        max_age_seconds: float,
        protected_ids: set[str] | None = None,
        max_total_bytes: int = 0,
    ) -> dict[str, int]:
        """Remove expired recording WAV/metadata pairs under the store lock."""

        protected = protected_ids or set()
        now = time.time()
        with self._condition:
            records = list(self._recordings)
        records.sort(key=lambda item: str(item.get("created_at", "")))
        sizes = sum(int(item.get("size_bytes", 0)) for item in records)
        deleted = 0
        deleted_bytes = 0
        candidates: list[tuple[dict[str, Any], bool]] = []
        for metadata in records:
            try:
                created = datetime.fromisoformat(str(metadata["created_at"])).timestamp()
            except (KeyError, TypeError, ValueError, OSError):
                created = now
            if metadata.get("id") in protected:
                continue
            expired = now - created >= max_age_seconds
            if expired or (max_total_bytes > 0 and sizes > max_total_bytes):
                candidates.append((metadata, expired))
        for metadata, expired in candidates:
            if not expired and max_total_bytes > 0 and sizes <= max_total_bytes:
                break
            recording_id = str(metadata.get("id", ""))
            filename = str(metadata.get("filename", ""))
            wav_path = (self.recordings_dir / filename).resolve()
            metadata_path = (self.recordings_dir / f"{recording_id}.json").resolve()
            if wav_path.parent != self.recordings_dir or metadata_path.parent != self.recordings_dir:
                continue
            size = int(metadata.get("size_bytes", 0))
            try:
                wav_path.unlink(missing_ok=True)
                metadata_path.unlink(missing_ok=True)
            except OSError:
                continue
            sizes = max(0, sizes - size)
            deleted += 1
            deleted_bytes += size
            with self._condition:
                self._recordings = [
                    item for item in self._recordings if item.get("id") != recording_id
                ]
        return {"deleted_files": deleted * 2, "deleted_bytes": deleted_bytes}

    def recordings(self) -> list[dict[str, Any]]:
        with self._condition:
            return [dict(item) for item in self._recordings]

    def recording(self, recording_id: str) -> dict[str, Any] | None:
        with self._condition:
            metadata = next(
                (item for item in self._recordings if item["id"] == recording_id), None
            )
        return None if metadata is None else dict(metadata)

    def recording_path(self, recording_id: str) -> Path | None:
        with self._condition:
            metadata = next(
                (item for item in self._recordings if item["id"] == recording_id), None
            )
        if not metadata:
            return None
        path = (self.recordings_dir / metadata["filename"]).resolve()
        if path.parent != self.recordings_dir or not path.is_file():
            return None
        return path

    def enqueue_command(
        self,
        device_id: str,
        command_type: str,
        payload: dict[str, Any] | None = None,
        *,
        request_id: str | None = None,
    ) -> dict[str, Any]:
        idempotency_key = ""
        if payload:
            idempotency_key = str(payload.get("idempotency_key", "")).strip()
            if not idempotency_key and command_type == "play_audio":
                turn_id = str(payload.get("turn_id", ""))
                chunk_index = str(payload.get("chunk_index", "0"))
                if turn_id:
                    idempotency_key = f"play_audio:{turn_id}:{chunk_index}"
        idempotency_key = idempotency_key[:MAX_IDEMPOTENCY_KEY_LENGTH]
        command = {
            "id": uuid.uuid4().hex,
            "device_id": device_id,
            "type": command_type,
            "created_at": utc_now(),
        }
        if request_id:
            command["request_id"] = request_id
        if payload:
            command.update(
                {
                    key: value
                    for key, value in payload.items()
                    if key not in {"id", "device_id", "created_at"}
                }
            )
        with self._condition:
            if idempotency_key:
                for queued in self._commands[device_id]:
                    if queued.get("idempotency_key") == idempotency_key:
                        return dict(queued)
                command["idempotency_key"] = idempotency_key
            if len(self._commands[device_id]) >= MAX_COMMANDS_PER_DEVICE:
                raise OverflowError(f"device command queue is full: {device_id}")
            self._commands[device_id].append(command)
            self._condition.notify_all()
        return dict(command)

    def take_command(self, device_id: str, wait_ms: int) -> dict[str, Any] | None:
        deadline = time.monotonic() + wait_ms / 1000
        with self._condition:
            self._touch_device_locked(device_id)
            while not self._commands[device_id]:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    return None
                self._condition.wait(remaining)
            return dict(self._commands[device_id].popleft())

    def register_command_websocket(
        self,
        device_id: str,
        stop: threading.Event | None = None,
        connection: Any | None = None,
    ) -> str:
        stop = stop or threading.Event()
        token = uuid.uuid4().hex
        previous: _CommandWebSocketSession | None = None
        with self._condition:
            previous = self._command_websocket_sessions.get(device_id)
            self._command_websocket_sessions[device_id] = _CommandWebSocketSession(
                token=token,
                stop=stop,
                connection=connection,
            )
            self._mark_command_websocket_locked(device_id, True)
            self._condition.notify_all()
        if previous is not None:
            # A reconnect can arrive before the old request handler has unwound.
            # Stop and close the previous socket so only the newest sender can
            # consume commands for this device.
            previous.stop.set()
            if previous.connection is not None:
                try:
                    previous.connection.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
        return token

    def unregister_command_websocket(self, device_id: str, token: str | None = None) -> None:
        with self._condition:
            session = self._command_websocket_sessions.get(device_id)
            if session is not None and (token is None or session.token == token):
                self._command_websocket_sessions.pop(device_id, None)
                self._mark_command_websocket_locked(device_id, False)
            self._condition.notify_all()

    def command_websocket_connected(self, device_id: str) -> bool:
        with self._condition:
            return device_id in self._command_websocket_sessions

    def command_websocket_devices(self) -> list[str]:
        with self._condition:
            return sorted(self._command_websocket_sessions)

    def add_event(
        self,
        device_id: str,
        payload: dict[str, Any],
        *,
        request_id: str | None = None,
    ) -> dict[str, Any]:
        event = {
            "id": uuid.uuid4().hex,
            "device_id": device_id,
            "command_id": str(payload.get("command_id", "")),
            "type": str(payload.get("type", "event"))[:64],
            "created_at": utc_now(),
            "details": payload.get("details", {}),
        }
        if request_id:
            event["request_id"] = request_id
        with self._condition:
            self._touch_device_locked(device_id)
            self._events.appendleft(event)
        return dict(event)

    def events(self) -> list[dict[str, Any]]:
        with self._condition:
            return [dict(item) for item in self._events]


class GatewayHTTPServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(
        self,
        address: tuple[str, int],
        store: GatewayStore,
        device_token: str,
        web_root: Path,
        ai_service: ConversationService,
    ) -> None:
        super().__init__(address, GatewayHandler)
        self.store = store
        self.device_token = device_token
        self.web_root = web_root.resolve()
        self.ai_service = ai_service
        self._maintenance_stop = threading.Event()
        self._maintenance_thread = threading.Thread(
            target=self._maintenance_loop,
            name="gateway-maintenance",
            daemon=True,
        )
        self._maintenance_thread.start()
        self._closed = False

    def _maintenance_loop(self) -> None:
        # Run once shortly after startup, then keep the cleanup cadence bounded
        # so a large artifact directory cannot block request handling.
        self._maintenance_stop.wait(1)
        while not self._maintenance_stop.is_set():
            try:
                config = self.ai_service.database.config(include_secret=True)
                recovered_turns = self.ai_service.recover_stale_turns()
                protected = self.ai_service.active_recording_ids()
                recording_result = self.store.cleanup_recordings(
                    max_age_seconds=max(
                        3600,
                        int(config.get("recording_retention_hours", 168)) * 3600,
                    ),
                    protected_ids=protected,
                    max_total_bytes=max(0, int(config.get("storage_max_bytes", 0))),
                )
                temp_result = cleanup_tree(
                    self.store.recordings_dir,
                    max_age_seconds=max(
                        300,
                        int(config.get("temporary_retention_minutes", 60)) * 60,
                    ),
                    protected_names=set(),
                    allowed_suffixes={".tmp"},
                )
                artifact_result = self.ai_service.cleanup_artifacts(
                    config=config,
                    protected_turn_ids=self.ai_service.active_turn_ids(),
                )
                deleted = (
                    recording_result["deleted_files"]
                    + temp_result["deleted_files"]
                    + artifact_result["deleted_files"]
                )
                if deleted:
                    self.ai_service.log(
                        "INFO",
                        "maintenance",
                        "artifact cleanup completed",
                        {
                            "recordings": recording_result,
                            "temporary": temp_result,
                            "tts": artifact_result,
                        },
                    )
                if recovered_turns:
                    self.ai_service.log(
                        "WARN",
                        "maintenance",
                        "stale conversation turns recovered",
                        {"count": recovered_turns},
                    )
            except Exception as error:
                self.ai_service.log(
                    "ERROR",
                    "maintenance",
                    "artifact cleanup failed",
                    {"error": str(error)[:500]},
                )
            self._maintenance_stop.wait(300)

    def health_snapshot(self) -> dict[str, Any]:
        disk = shutil.disk_usage(self.store.data_dir)
        snapshot = self.ai_service.health_snapshot()
        config = self.ai_service.database.config(include_secret=True)
        devices = self.store.device_health(
            int(config.get("health_stale_device_seconds", 30))
        )
        minimum_free_bytes = 64 * 1024 * 1024
        # ``storage_max_bytes`` is an artifact quota, not a whole-volume
        # quota.  Readiness only needs a safe free-space floor here; the
        # maintenance worker enforces the artifact quota separately.
        disk_ok = disk.free >= minimum_free_bytes
        snapshot["checks"]["disk"] = disk_ok
        snapshot["ready"] = bool(snapshot.get("ready") and disk_ok)
        snapshot["ok"] = snapshot["ready"]
        snapshot.update(
            {
                "time": utc_now(),
                "disk": {
                    "total_bytes": disk.total,
                    "used_bytes": disk.used,
                    "free_bytes": disk.free,
                    "used_percent": round((disk.used / disk.total) * 100, 2)
                    if disk.total
                    else 0,
                },
                "device_count": len(devices),
                "devices": devices,
                "command_websocket_devices": self.store.command_websocket_devices(),
            }
        )
        return snapshot

    def server_close(self) -> None:
        if self._closed:
            return
        self._closed = True
        self._maintenance_stop.set()
        self._maintenance_thread.join(timeout=2)
        self.ai_service.close()
        super().server_close()


class GatewayHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server: GatewayHTTPServer

    def setup(self) -> None:
        super().setup()
        self.request_id = uuid.uuid4().hex

    def log_message(self, fmt: str, *args: Any) -> None:
        print(f"[{self.log_date_time_string()}] {self.address_string()} {fmt % args}")

    def _send_json(self, status: int, payload: Any) -> None:
        body = json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode(
            "utf-8"
        )
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Request-ID", self.request_id)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _send_empty(self, status: int) -> None:
        self.send_response(status)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def _device_authorized(self) -> bool:
        provided = self.headers.get("X-Device-Token", "")
        if hmac.compare_digest(provided, self.server.device_token):
            return True
        self._send_json(HTTPStatus.UNAUTHORIZED, {"error": "invalid device token"})
        return False

    def _read_json(self) -> dict[str, Any] | None:
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            self._send_json(HTTPStatus.BAD_REQUEST, {"error": "invalid content length"})
            return None
        if length <= 0 or length > MAX_JSON_BYTES:
            self._send_json(HTTPStatus.BAD_REQUEST, {"error": "invalid JSON body size"})
            return None
        try:
            value = json.loads(self.rfile.read(length))
        except (UnicodeDecodeError, json.JSONDecodeError):
            self._send_json(HTTPStatus.BAD_REQUEST, {"error": "invalid JSON"})
            return None
        if not isinstance(value, dict):
            self._send_json(HTTPStatus.BAD_REQUEST, {"error": "JSON object required"})
            return None
        return value

    @staticmethod
    def _device_id(value: str) -> str | None:
        device_id = unquote(value)
        return device_id if DEVICE_ID_RE.fullmatch(device_id) else None

    def _serve_file(self, path: Path, content_type: str | None = None) -> None:
        try:
            body = path.read_bytes()
        except OSError:
            self._send_json(HTTPStatus.NOT_FOUND, {"error": "file not found"})
            return
        self.send_response(HTTPStatus.OK)
        self.send_header(
            "Content-Type",
            content_type or mimetypes.guess_type(path.name)[0] or "application/octet-stream",
        )
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Accept-Ranges", "none")
        self.end_headers()
        self.wfile.write(body)

    def _serve_sse(self) -> None:
        subscriber = self.server.ai_service.subscribe()
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", "text/event-stream; charset=utf-8")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "keep-alive")
        self.send_header("X-Accel-Buffering", "no")
        self.end_headers()

        def send_event(event_type: str, data: Any) -> None:
            body = json.dumps(data, ensure_ascii=False, separators=(",", ":"))
            self.wfile.write(f"event: {event_type}\ndata: {body}\n\n".encode("utf-8"))
            self.wfile.flush()

        try:
            send_event(
                "ready",
                {
                    "time": utc_now(),
                    "worker_running": self.server.ai_service.worker_running,
                },
            )
            while True:
                try:
                    event = subscriber.get(timeout=15)
                except queue.Empty:
                    self.wfile.write(b": keep-alive\n\n")
                    self.wfile.flush()
                    continue
                send_event("update", event)
        except (BrokenPipeError, ConnectionResetError, OSError):
            return
        finally:
            self.server.ai_service.unsubscribe(subscriber)

    def _read_exact(self, length: int) -> bytes:
        data = bytearray()
        while len(data) < length:
            chunk = self.rfile.read(length - len(data))
            if not chunk:
                raise ConnectionError("WebSocket connection closed")
            data.extend(chunk)
        return bytes(data)

    def _write_websocket_frame(self, opcode: int, payload: bytes = b"") -> None:
        header = bytearray([0x80 | opcode])
        length = len(payload)
        if length < 126:
            header.append(length)
        elif length <= 0xFFFF:
            header.append(126)
            header.extend(struct.pack("!H", length))
        else:
            header.append(127)
            header.extend(struct.pack("!Q", length))
        lock = getattr(self, "_websocket_write_lock", None)
        if lock is None:
            self.wfile.write(bytes(header) + payload)
            self.wfile.flush()
            return
        with lock:
            self.wfile.write(bytes(header) + payload)
            self.wfile.flush()

    def _read_websocket_message(self) -> tuple[int, bytes] | None:
        message_opcode = None
        message = bytearray()
        while True:
            first, second = self._read_exact(2)
            final = bool(first & 0x80)
            opcode = first & 0x0F
            masked = bool(second & 0x80)
            length = second & 0x7F
            if length == 126:
                length = struct.unpack("!H", self._read_exact(2))[0]
            elif length == 127:
                length = struct.unpack("!Q", self._read_exact(8))[0]

            if opcode >= 0x8:
                if not final or length > 125:
                    raise ValueError("invalid WebSocket control frame")
                mask = self._read_exact(4) if masked else b""
                payload = self._read_exact(length)
                if masked:
                    payload = bytes(
                        value ^ mask[index % 4]
                        for index, value in enumerate(payload)
                    )
                if opcode == 0x8:
                    self._write_websocket_frame(0x8, payload)
                    return None
                if opcode == 0x9:
                    self._write_websocket_frame(0xA, payload)
                continue

            if not masked:
                raise ValueError("client WebSocket frames must be masked")
            if opcode in {0x1, 0x2}:
                if message_opcode is not None:
                    raise ValueError("unexpected new WebSocket message")
                message_opcode = opcode
            elif opcode != 0x0 or message_opcode is None:
                raise ValueError("invalid WebSocket continuation frame")

            limit = MAX_JSON_BYTES if message_opcode == 0x1 else MAX_RECORDING_BYTES
            if len(message) + length > limit:
                raise ValueError("WebSocket message is too large")
            mask = self._read_exact(4)
            payload = self._read_exact(length)
            message.extend(
                value ^ mask[index % 4] for index, value in enumerate(payload)
            )
            if final:
                return message_opcode, bytes(message)

    def _handle_stream_session(
        self, device_id: str, metadata: dict[str, Any]
    ) -> None:
        """Receive PCM chunks while recording and finalize one normal turn."""
        try:
            sample_rate = int(metadata["sample_rate"])
            channels = int(metadata["channels"])
            bits = int(metadata["bits_per_sample"])
            session_id = str(metadata.get("session_id", ""))
            turn_index = int(metadata.get("turn_index", 0))
            stream_id = str(metadata.get("stream_id", ""))
            protocol_version = int(metadata.get("protocol_version", 2))
        except (KeyError, TypeError, ValueError):
            raise ValueError("invalid stream metadata") from None
        if protocol_version != 2:
            raise ValueError("unsupported stream protocol version")
        if metadata.get("transport") != "stream":
            raise ValueError("stream transport must be stream")
        if not stream_id or not DEVICE_ID_RE.fullmatch(stream_id):
            raise ValueError("invalid stream id")
        if session_id and not DEVICE_ID_RE.fullmatch(session_id):
            raise ValueError("invalid session id")
        if turn_index < 0 or turn_index > 1_000_000:
            raise ValueError("invalid turn index")
        if sample_rate not in {8000, 16000, 24000, 32000, 44100, 48000}:
            raise ValueError("unsupported sample rate")
        if channels != 1 or bits != 16:
            raise ValueError("only mono signed-16 PCM is supported")

        started = time.monotonic()
        last_partial_at = started
        stream_config = self.server.ai_service.database.config(include_secret=True)
        partial_interval = max(
            0.3, int(stream_config.get("stream_partial_interval_ms", 900)) / 1000
        )
        pcm = bytearray()
        start_ack = json.dumps(
            {
                "type": "stream_session_started",
                "stream_id": stream_id,
                "protocol_version": 2,
                "streaming_asr": self.server.ai_service.streaming_asr_supported(),
            },
            ensure_ascii=False,
            separators=(",", ":"),
        ).encode("utf-8")
        self._write_websocket_frame(0x1, start_ack)
        self.server.ai_service.log(
            "INFO",
            "stream",
            "流式音频会话已开始",
            {"device_id": device_id, "stream_id": stream_id},
        )

        while True:
            message = self._read_websocket_message()
            if message is None:
                return
            opcode, payload = message
            if opcode == 0x2:
                if not payload or len(pcm) + len(payload) > MAX_RECORDING_BYTES:
                    raise ValueError("stream audio is too large")
                pcm.extend(payload)
                now = time.monotonic()
                if now - last_partial_at >= partial_interval:
                    self.server.ai_service.submit_stream_partial(
                        stream_id, bytes(pcm), sample_rate
                    )
                    last_partial_at = now
                partial = self.server.ai_service.poll_stream_partial(stream_id)
                if partial:
                    self._write_websocket_frame(
                        0x1,
                        json.dumps(
                            {"type": "stream_partial", **partial},
                            ensure_ascii=False,
                            separators=(",", ":"),
                        ).encode("utf-8"),
                    )
                continue
            if opcode != 0x1:
                raise ValueError("stream control message must be text")
            try:
                control = json.loads(payload)
            except (UnicodeDecodeError, json.JSONDecodeError):
                raise ValueError("invalid stream control message") from None
            if not isinstance(control, dict):
                raise ValueError("stream control message must be an object")
            if control.get("type") != "stream_session_end":
                raise ValueError("unsupported stream control message")
            if str(control.get("stream_id", stream_id)) != stream_id:
                raise ValueError("stream id mismatch")
            if not pcm or len(pcm) % 2:
                raise ValueError("stream contains no complete PCM samples")
            partial = self.server.ai_service.poll_stream_partial(stream_id)
            if partial:
                self._write_websocket_frame(
                    0x1,
                    json.dumps(
                        {"type": "stream_partial", **partial},
                        ensure_ascii=False,
                        separators=(",", ":"),
                    ).encode("utf-8"),
                )
            recording = self.server.store.save_recording(
                device_id,
                bytes(pcm),
                sample_rate,
                channels,
                bits,
                session_id,
                turn_index,
                "stream",
                2,
            )
            ended = json.dumps(
                {
                    "type": "stream_session_ended",
                    "stream_id": stream_id,
                    "recording": recording,
                    "duration_ms": round(len(pcm) / 2 * 1000 / sample_rate),
                },
                ensure_ascii=False,
                separators=(",", ":"),
            ).encode("utf-8")
            self._write_websocket_frame(0x1, ended)
            self.server.ai_service.log(
                "INFO",
                "stream",
                "流式音频会话已结束",
                {
                    "device_id": device_id,
                    "stream_id": stream_id,
                    "duration_ms": round(len(pcm) / 2 * 1000 / sample_rate),
                    "ingest_ms": round((time.monotonic() - started) * 1000),
                },
            )
            return

    def _handle_recording_websocket(self, device_id: str) -> None:
        if self.headers.get("Upgrade", "").lower() != "websocket":
            self._send_json(HTTPStatus.BAD_REQUEST, {"error": "WebSocket upgrade required"})
            return
        connection_tokens = {
            token.strip().lower()
            for token in self.headers.get("Connection", "").split(",")
        }
        websocket_key = self.headers.get("Sec-WebSocket-Key", "")
        if "upgrade" not in connection_tokens or not websocket_key:
            self._send_json(HTTPStatus.BAD_REQUEST, {"error": "invalid WebSocket handshake"})
            return

        accept = base64.b64encode(
            hashlib.sha1((websocket_key + WEBSOCKET_GUID).encode("ascii")).digest()
        ).decode("ascii")
        self.send_response(HTTPStatus.SWITCHING_PROTOCOLS)
        self.send_header("Upgrade", "websocket")
        self.send_header("Connection", "Upgrade")
        self.send_header("Sec-WebSocket-Accept", accept)
        self.end_headers()
        self.close_connection = True
        self.server.store.mark_recording_websocket(device_id, True)
        print(f"WebSocket recording client connected: {device_id}")
        self.server.ai_service.log(
            "INFO", "websocket", "ESP32 录音 WebSocket 已连接", {"device_id": device_id}
        )

        try:
            while True:
                metadata_message = self._read_websocket_message()
                if metadata_message is None:
                    return
                opcode, payload = metadata_message
                if opcode != 0x1:
                    raise ValueError("recording metadata must be a text frame")
                try:
                    metadata = json.loads(payload)
                    if isinstance(metadata, dict) and metadata.get("type") == "stream_session_start":
                        self._handle_stream_session(device_id, metadata)
                        continue
                    sample_rate = int(metadata["sample_rate"])
                    channels = int(metadata["channels"])
                    bits = int(metadata["bits_per_sample"])
                    sample_count = int(metadata["sample_count"])
                    session_id = str(metadata.get("session_id", ""))
                    turn_index = int(metadata.get("turn_index", 0))
                    transport = str(metadata.get("transport", "turn"))
                    protocol_version = int(metadata.get("protocol_version", 1))
                except (KeyError, TypeError, ValueError, UnicodeDecodeError, json.JSONDecodeError):
                    raise ValueError("invalid recording metadata") from None
                if metadata.get("type") != "recording_pcm":
                    raise ValueError("unsupported WebSocket message type")
                if session_id and not DEVICE_ID_RE.fullmatch(session_id):
                    raise ValueError("invalid session id")
                if turn_index < 0 or turn_index > 1_000_000:
                    raise ValueError("invalid turn index")
                if protocol_version != 1:
                    raise ValueError("unsupported recording protocol version")
                if transport != "turn":
                    raise ValueError("unsupported recording transport")
                if sample_rate not in {8000, 16000, 24000, 32000, 44100, 48000}:
                    raise ValueError("unsupported sample rate")
                if channels != 1 or bits != 16 or sample_count <= 0:
                    raise ValueError("only non-empty mono signed-16 PCM is supported")
                expected_bytes = sample_count * channels * (bits // 8)
                if expected_bytes > MAX_RECORDING_BYTES:
                    raise ValueError("recording is too large")

                pcm_message = self._read_websocket_message()
                if pcm_message is None:
                    return
                opcode, pcm = pcm_message
                if opcode != 0x2 or len(pcm) != expected_bytes:
                    raise ValueError("PCM binary frame size does not match metadata")

                recording = self.server.store.save_recording(
                    device_id,
                    pcm,
                    sample_rate,
                    channels,
                    bits,
                    session_id,
                    turn_index,
                    transport,
                    protocol_version,
                )
                acknowledgement = json.dumps(
                    {"type": "recording_saved", "recording": recording},
                    ensure_ascii=False,
                    separators=(",", ":"),
                ).encode("utf-8")
                self._write_websocket_frame(0x1, acknowledgement)
                print(
                    f"WebSocket recording saved: device={device_id} "
                    f"samples={sample_count} bytes={len(pcm)}"
                )
        except (ConnectionError, OSError):
            return
        except ValueError as error:
            payload = json.dumps(
                {"type": "recording_error", "error": str(error)},
                separators=(",", ":"),
            ).encode("utf-8")
            try:
                self._write_websocket_frame(0x1, payload)
                self._write_websocket_frame(0x8, struct.pack("!H", 1008))
            except OSError:
                pass
        finally:
            self.server.store.mark_recording_websocket(device_id, False)

    def _handle_command_websocket(self, device_id: str) -> None:
        """Keep one bidirectional command channel open for low-latency control/audio."""
        if self.headers.get("Upgrade", "").lower() != "websocket":
            self._send_json(HTTPStatus.BAD_REQUEST, {"error": "WebSocket upgrade required"})
            return
        connection_tokens = {
            token.strip().lower()
            for token in self.headers.get("Connection", "").split(",")
        }
        websocket_key = self.headers.get("Sec-WebSocket-Key", "")
        if "upgrade" not in connection_tokens or not websocket_key:
            self._send_json(HTTPStatus.BAD_REQUEST, {"error": "invalid WebSocket handshake"})
            return
        accept = base64.b64encode(
            hashlib.sha1((websocket_key + WEBSOCKET_GUID).encode("ascii")).digest()
        ).decode("ascii")
        self.send_response(HTTPStatus.SWITCHING_PROTOCOLS)
        self.send_header("Upgrade", "websocket")
        self.send_header("Connection", "Upgrade")
        self.send_header("Sec-WebSocket-Accept", accept)
        self.end_headers()
        self.close_connection = True
        self._websocket_write_lock = threading.Lock()
        stop = threading.Event()
        session_token = self.server.store.register_command_websocket(
            device_id, stop=stop, connection=self.connection
        )

        def send_json(payload: dict[str, Any]) -> None:
            self._write_websocket_frame(
                0x1,
                json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8"),
            )

        def command_sender() -> None:
            try:
                while not stop.is_set():
                    command = self.server.store.take_command(device_id, 500)
                    if command is None:
                        continue
                    self.server.ai_service.log(
                        "INFO",
                        "websocket",
                        "command WebSocket send",
                        {
                            "device_id": device_id,
                            "command_id": command.get("id", ""),
                            "command_type": command.get("type", ""),
                            "transport": command.get("transport", ""),
                        },
                    )
                    send_json(command)
                    if command.get("type") != "play_audio" or command.get("transport") != "websocket_pcm":
                        continue
                    try:
                        # Let the ESP32 command task finish handling the JSON
                        # control frame before the first binary frame arrives.
                        # This keeps the control/data boundary observable on
                        # constrained WebSocket clients and still adds only a
                        # single scheduling slice to the first audio chunk.
                        time.sleep(0.02)
                        turn_id = str(command["turn_id"])
                        chunk_index = int(command["chunk_index"])
                        sample_rate = int(command.get("sample_rate", 16000))
                        audio_path = self.server.ai_service.tts_path(turn_id, chunk_index)
                        if audio_path is None:
                            raise FileNotFoundError("TTS chunk not found")
                        stream_stats = stream_pcm_file(
                            audio_path,
                            self._write_websocket_frame,
                            stop,
                            sample_rate=sample_rate,
                        )
                        self.server.ai_service.log(
                            "INFO",
                            "websocket",
                            "command WebSocket PCM stream sent",
                            {
                                "device_id": device_id,
                                "command_id": command.get("id", ""),
                                "turn_id": turn_id,
                                "sample_rate": sample_rate,
                                **stream_stats,
                            },
                        )
                        if not stop.is_set() and stream_stats["completed"]:
                            # Give the ESP32 WebSocket task one scheduling slice
                            # after the final binary frame.  On long streams the
                            # receiver may still be copying the last PCM block
                            # into its bounded playback queue; sending the text
                            # terminator immediately can otherwise starve that
                            # callback and trigger the device socket timeout.
                            time.sleep(STREAM_PCM_END_GUARD_SECONDS)
                            send_json(
                                {
                                    "type": "audio_stream_end",
                                    "command_id": command["id"],
                                    "turn_id": turn_id,
                                    "sample_count": command.get("sample_count", 0),
                                    "is_final_chunk": bool(command.get("is_final_chunk", False)),
                                }
                            )
                    except (KeyError, TypeError, ValueError, OSError) as error:
                        send_json(
                            {
                                "type": "audio_stream_end",
                                "command_id": command.get("id", ""),
                                "error": str(error)[:200],
                            }
                        )
            except (ConnectionError, BrokenPipeError, ConnectionResetError, OSError) as error:
                self.server.ai_service.log(
                    "WARNING",
                    "websocket",
                    "command WebSocket sender stopped",
                    {"device_id": device_id, "error": str(error)[:200]},
                )
                stop.set()

        sender = threading.Thread(
            target=command_sender,
            name=f"gateway-command-ws-{device_id}",
            daemon=True,
        )
        sender.start()
        self.server.ai_service.log(
            "INFO", "websocket", "ESP32 命令 WebSocket 已连接", {"device_id": device_id}
        )
        try:
            while not stop.is_set():
                message = self._read_websocket_message()
                if message is None:
                    break
                opcode, payload = message
                if opcode != 0x1:
                    raise ValueError("command WebSocket event must be a text frame")
                try:
                    event_payload = json.loads(payload)
                except (UnicodeDecodeError, json.JSONDecodeError):
                    raise ValueError("invalid command WebSocket event") from None
                if not isinstance(event_payload, dict):
                    raise ValueError("command WebSocket event must be an object")
                if event_payload.get("type") == "event":
                    event_type = str(event_payload.get("event_type", "event"))
                    normalized = {
                        "command_id": event_payload.get("command_id", ""),
                        "type": event_type,
                        "details": event_payload.get("details", {}),
                    }
                else:
                    normalized = {
                        "command_id": event_payload.get("command_id", ""),
                        "type": event_payload.get("event_type", event_payload.get("type", "event")),
                        "details": event_payload.get("details", {}),
                    }
                event = self.server.store.add_event(
                    device_id, normalized, request_id=self.request_id
                )
                self.server.ai_service.device_event(event)
        except (ConnectionError, BrokenPipeError, ConnectionResetError, OSError) as error:
            self.server.ai_service.log(
                "WARNING",
                "websocket",
                "command WebSocket reader stopped",
                {"device_id": device_id, "error": str(error)[:200]},
            )
        except ValueError as error:
            self.server.ai_service.log(
                "WARNING",
                "websocket",
                "command WebSocket protocol error",
                {"device_id": device_id, "error": str(error)[:200]},
            )
            try:
                send_json({"type": "command_error", "error": str(error)[:200]})
                self._write_websocket_frame(0x8, struct.pack("!H", 1008))
            except OSError:
                pass
        finally:
            stop.set()
            self.server.store.unregister_command_websocket(device_id, session_token)
            sender.join(timeout=1)
            self.server.ai_service.log(
                "INFO", "websocket", "ESP32 命令 WebSocket 已断开", {"device_id": device_id}
            )

    def do_GET(self) -> None:  # noqa: N802
        parsed = urlparse(self.path)
        path = parsed.path

        if path == "/health/live":
            self._send_json(HTTPStatus.OK, {"ok": True, "time": utc_now()})
            return
        if path == "/health/ready":
            snapshot = self.server.health_snapshot()
            status = HTTPStatus.OK if snapshot.get("ready") else HTTPStatus.SERVICE_UNAVAILABLE
            self._send_json(status, snapshot)
            return
        if path == "/health/devices":
            config = self.server.ai_service.database.config(include_secret=True)
            self._send_json(
                HTTPStatus.OK,
                {
                    "time": utc_now(),
                    "devices": self.server.store.device_health(
                        int(config.get("health_stale_device_seconds", 30))
                    ),
                    "command_websocket_devices": self.server.store.command_websocket_devices(),
                },
            )
            return
        if path == "/health":
            snapshot = self.server.health_snapshot()
            config = self.server.ai_service.public_config()
            legacy_payload = {**snapshot, **config}
            # Preserve the historical /health contract (process reachable),
            # while /health/ready carries the stricter dependency verdict.
            legacy_payload["ok"] = True
            self._send_json(HTTPStatus.OK, legacy_payload)
            return
        if path == "/api/capabilities":
            self._send_json(
                HTTPStatus.OK,
                {
                    "recording_protocol_version": 2,
                    "turn_upload": True,
                    "continuous_streaming": True,
                    "streaming_audio": True,
                    "command_websocket": True,
                    "streaming_pcm_websocket": True,
                    "streaming_asr": self.server.ai_service.streaming_asr_supported(),
                    "sentence_tts": True,
                    "stream_fallback": True,
                    "voice_end_conversation": True,
                    "stream_messages": [
                        "stream_session_start",
                        "stream_pcm_chunk",
                        "stream_session_end",
                    ],
                    "reserved_stream_messages": [
                        "stream_session_start",
                        "stream_pcm_chunk",
                        "stream_session_end",
                    ],
                },
            )
            return
        if path == "/api/config":
            self._send_json(HTTPStatus.OK, self.server.ai_service.public_config())
            return
        if path == "/api/conversations":
            query = parse_qs(parsed.query)
            try:
                limit = int(query.get("limit", ["100"])[0])
            except ValueError:
                self._send_json(HTTPStatus.BAD_REQUEST, {"error": "invalid limit"})
                return
            turns = self.server.ai_service.database.turns(limit)
            for turn in turns:
                turn["audio_url"] = f"/api/recordings/{turn['recording_id']}/audio"
            self._send_json(HTTPStatus.OK, {"turns": turns})
            return
        if path == "/api/logs":
            query = parse_qs(parsed.query)
            try:
                limit = int(query.get("limit", ["200"])[0])
            except ValueError:
                self._send_json(HTTPStatus.BAD_REQUEST, {"error": "invalid limit"})
                return
            self._send_json(
                HTTPStatus.OK,
                {"logs": self.server.ai_service.database.logs(limit)},
            )
            return
        if path == "/api/stream":
            self._serve_sse()
            return
        if path == "/api/devices":
            self._send_json(HTTPStatus.OK, {"devices": self.server.store.devices()})
            return
        if path == "/api/recordings":
            self._send_json(
                HTTPStatus.OK, {"recordings": self.server.store.recordings()}
            )
            return
        if path == "/api/events":
            self._send_json(HTTPStatus.OK, {"events": self.server.store.events()})
            return

        command_websocket_match = re.fullmatch(r"/ws/devices/([^/]+)/commands", path)
        if command_websocket_match:
            if not self._device_authorized():
                return
            device_id = self._device_id(command_websocket_match.group(1))
            if not device_id:
                self._send_json(HTTPStatus.BAD_REQUEST, {"error": "invalid device id"})
                return
            self._handle_command_websocket(device_id)
            return

        websocket_match = re.fullmatch(r"/ws/devices/([^/]+)/recordings", path)
        if websocket_match:
            if not self._device_authorized():
                return
            device_id = self._device_id(websocket_match.group(1))
            if not device_id:
                self._send_json(HTTPStatus.BAD_REQUEST, {"error": "invalid device id"})
                return
            self._handle_recording_websocket(device_id)
            return

        audio_match = re.fullmatch(r"/api/recordings/([0-9a-f]{32})/audio", path)
        if audio_match:
            recording_path = self.server.store.recording_path(audio_match.group(1))
            if recording_path is None:
                self._send_json(HTTPStatus.NOT_FOUND, {"error": "recording not found"})
            else:
                self._serve_file(recording_path, "audio/wav")
            return

        tts_match = re.fullmatch(
            r"/api/tts/([0-9a-f]{32})(?:/chunk/([0-9]{1,3}))?/pcm", path
        )
        if tts_match:
            if not self._device_authorized():
                return
            chunk_index = int(tts_match.group(2)) if tts_match.group(2) else None
            tts_path = self.server.ai_service.tts_path(tts_match.group(1), chunk_index)
            if tts_path is None:
                self._send_json(HTTPStatus.NOT_FOUND, {"error": "TTS audio not found"})
            else:
                self._serve_file(tts_path, "application/octet-stream")
            return

        command_match = re.fullmatch(r"/api/devices/([^/]+)/commands", path)
        if command_match:
            if not self._device_authorized():
                return
            device_id = self._device_id(command_match.group(1))
            if not device_id:
                self._send_json(HTTPStatus.BAD_REQUEST, {"error": "invalid device id"})
                return
            query = parse_qs(parsed.query)
            try:
                wait_ms = max(0, min(30000, int(query.get("wait_ms", ["1000"])[0])))
            except ValueError:
                self._send_json(HTTPStatus.BAD_REQUEST, {"error": "invalid wait_ms"})
                return
            command = self.server.store.take_command(device_id, wait_ms)
            if command is None:
                self._send_empty(HTTPStatus.NO_CONTENT)
            else:
                self._send_json(HTTPStatus.OK, command)
            return

        assets = {"/": "index.html", "/index.html": "index.html", "/app.js": "app.js", "/styles.css": "styles.css"}
        if path in assets:
            self._serve_file(self.server.web_root / assets[path])
            return

        self._send_json(HTTPStatus.NOT_FOUND, {"error": "not found"})

    def do_POST(self) -> None:  # noqa: N802
        path = urlparse(self.path).path

        if path == "/api/config":
            payload = self._read_json()
            if payload is None:
                return
            try:
                config = self.server.ai_service.update_config(payload)
            except ValueError as error:
                self._send_json(HTTPStatus.BAD_REQUEST, {"error": str(error)})
                return
            self._send_json(HTTPStatus.OK, config)
            return

        process_match = re.fullmatch(
            r"/api/recordings/([0-9a-f]{32})/process", path
        )
        if process_match:
            recording = self.server.store.recording(process_match.group(1))
            if recording is None:
                self._send_json(HTTPStatus.NOT_FOUND, {"error": "recording not found"})
                return
            turn = self.server.ai_service.register_existing_recording(recording)
            self._send_json(HTTPStatus.ACCEPTED, turn)
            return

        retry_match = re.fullmatch(
            r"/api/conversations/([0-9a-f]{32})/retry", path
        )
        if retry_match:
            if self.server.ai_service.database.turn(retry_match.group(1)) is None:
                self._send_json(HTTPStatus.NOT_FOUND, {"error": "turn not found"})
                return
            turn = self.server.ai_service.retry_turn(retry_match.group(1))
            self._send_json(HTTPStatus.ACCEPTED, turn)
            return

        recording_match = re.fullmatch(r"/api/devices/([^/]+)/recordings", path)
        if recording_match:
            if not self._device_authorized():
                return
            device_id = self._device_id(recording_match.group(1))
            if not device_id:
                self._send_json(HTTPStatus.BAD_REQUEST, {"error": "invalid device id"})
                return
            try:
                length = int(self.headers.get("Content-Length", "0"))
                sample_rate = int(self.headers.get("X-Sample-Rate", "0"))
                channels = int(self.headers.get("X-Channels", "1"))
                bits = int(self.headers.get("X-Bits-Per-Sample", "16"))
            except ValueError:
                self._send_json(HTTPStatus.BAD_REQUEST, {"error": "invalid audio headers"})
                return
            if length <= 0 or length > MAX_RECORDING_BYTES or length % 2:
                self._send_json(HTTPStatus.BAD_REQUEST, {"error": "invalid PCM body size"})
                return
            if sample_rate not in {8000, 16000, 24000, 32000, 44100, 48000}:
                self._send_json(HTTPStatus.BAD_REQUEST, {"error": "unsupported sample rate"})
                return
            if channels != 1 or bits != 16:
                self._send_json(HTTPStatus.BAD_REQUEST, {"error": "only mono signed-16 PCM is supported"})
                return
            pcm = self.rfile.read(length)
            if len(pcm) != length:
                self._send_json(HTTPStatus.BAD_REQUEST, {"error": "incomplete PCM body"})
                return
            try:
                metadata = self.server.store.save_recording(
                    device_id, pcm, sample_rate, channels, bits
                )
            except OSError as error:
                self._send_json(
                    HTTPStatus.INTERNAL_SERVER_ERROR,
                    {"error": "failed to save recording", "detail": str(error)},
                )
                return
            self._send_json(HTTPStatus.CREATED, metadata)
            return

        command_match = re.fullmatch(r"/api/devices/([^/]+)/commands", path)
        if command_match:
            device_id = self._device_id(command_match.group(1))
            if not device_id:
                self._send_json(HTTPStatus.BAD_REQUEST, {"error": "invalid device id"})
                return
            payload = self._read_json()
            if payload is None:
                return
            command_type = payload.get("type")
            if command_type not in COMMAND_TYPES:
                self._send_json(
                    HTTPStatus.BAD_REQUEST,
                    {"error": "unsupported command", "allowed": sorted(COMMAND_TYPES)},
                )
                return
            command_payload = None
            if command_type == "play_audio":
                audio_path = payload.get("audio_path")
                turn_id = payload.get("turn_id")
                sample_rate = payload.get("sample_rate")
                sample_count = payload.get("sample_count")
                if (
                    not isinstance(audio_path, str)
                    or not re.fullmatch(
                        r"/api/tts/[0-9a-f]{32}(?:/chunk/[0-9]{1,3})?/pcm",
                        audio_path,
                    )
                    or not isinstance(turn_id, str)
                    or not re.fullmatch(r"[0-9a-f]{32}", turn_id)
                    or sample_rate != 16000
                    or not isinstance(sample_count, int)
                    or not 0 < sample_count <= 1024 * 1024
                ):
                    self._send_json(
                        HTTPStatus.BAD_REQUEST, {"error": "invalid play_audio payload"}
                    )
                    return
                command_payload = payload
            elif command_type in {"continue_listening", "end_conversation"}:
                turn_id = payload.get("turn_id", "")
                if turn_id and (
                    not isinstance(turn_id, str)
                    or not re.fullmatch(r"[0-9a-f]{32}", turn_id)
                ):
                    self._send_json(
                        HTTPStatus.BAD_REQUEST,
                        {"error": f"invalid {command_type} payload"},
                    )
                    return
                command_payload = {"turn_id": turn_id}
            try:
                command = self.server.store.enqueue_command(
                    device_id,
                    command_type,
                    command_payload,
                    request_id=self.request_id,
                )
            except OverflowError as error:
                self._send_json(
                    HTTPStatus.TOO_MANY_REQUESTS,
                    {"error": str(error), "retryable": True},
                )
                return
            self._send_json(HTTPStatus.ACCEPTED, command)
            return

        event_match = re.fullmatch(r"/api/devices/([^/]+)/events", path)
        if event_match:
            if not self._device_authorized():
                return
            device_id = self._device_id(event_match.group(1))
            if not device_id:
                self._send_json(HTTPStatus.BAD_REQUEST, {"error": "invalid device id"})
                return
            payload = self._read_json()
            if payload is None:
                return
            event = self.server.store.add_event(
                device_id, payload, request_id=self.request_id
            )
            self.server.ai_service.device_event(event)
            self._send_json(HTTPStatus.CREATED, event)
            return

        self._send_json(HTTPStatus.NOT_FOUND, {"error": "not found"})


def build_server(
    host: str,
    port: int,
    data_dir: Path,
    token: str,
    enable_ai: bool = False,
    transcriber: Transcriber | None = None,
    llm_caller: LLMCaller | None = None,
    tts_caller: TTSCaller | None = None,
) -> GatewayHTTPServer:
    if not token:
        raise ValueError("device token must not be empty")
    web_root = GATEWAY_ROOT / "web"
    store = GatewayStore(data_dir)
    ai_service = ConversationService(data_dir, transcriber, llm_caller, tts_caller)

    def enqueue_device_command(device_id: str, payload: dict[str, Any]) -> dict[str, Any]:
        command_type = str(payload.get("type", "play_audio"))
        command_payload = dict(payload)
        if command_type == "play_audio":
            command_payload["transport"] = (
                "websocket_pcm"
                if command_payload.get("stream_audio")
                and store.command_websocket_connected(device_id)
                else "http_pcm"
            )
        return store.enqueue_command(device_id, command_type, command_payload)

    ai_service.set_playback_sender(
        enqueue_device_command
    )
    store.set_recording_callback(ai_service.recording_saved)
    server = GatewayHTTPServer(
        (host, port), store, token, web_root, ai_service
    )
    if enable_ai:
        ai_service.start()
    return server


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="0.0.0.0", help="listen address")
    parser.add_argument("--port", type=int, default=8000, help="listen port")
    parser.add_argument(
        "--data-dir",
        type=Path,
        default=GATEWAY_ROOT / "data",
        help="recording storage directory",
    )
    parser.add_argument(
        "--token", default="ai-mirror-dev", help="shared ESP32 device token"
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    server = build_server(
        args.host, args.port, args.data_dir, args.token, enable_ai=True
    )
    host, port = server.server_address[:2]
    print(f"AI Mirror gateway listening on http://{host}:{port}")
    print(f"Recordings: {server.store.recordings_dir}")
    print(f"Database: {server.ai_service.database.path}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopping gateway...")
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
