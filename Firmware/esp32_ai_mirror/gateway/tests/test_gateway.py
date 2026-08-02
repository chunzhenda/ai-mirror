import base64
import http.client
import importlib.util
import json
import os
import socket
import struct
import tempfile
import threading
import time
import unittest
import wave
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from gateway.src.app import build_server, stream_pcm_file
from gateway.src.ai_services import (
    _is_end_conversation_command,
    limit_voice_reply,
    split_tts_sentences,
)


TOKEN = "test-device-token"
DEVICE_ID = "ai-mirror-test"
BROWSER_EXECUTABLE = next(
    (
        path
        for path in (
            Path(r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"),
            Path(r"C:\Program Files\Google\Chrome\Application\chrome.exe"),
        )
        if path.is_file()
    ),
    None,
)
HAS_PLAYWRIGHT_BROWSER = (
    importlib.util.find_spec("playwright") is not None and BROWSER_EXECUTABLE is not None
)


def websocket_connect(host, port, path, token):
    connection = socket.create_connection((host, port), timeout=3)
    key = base64.b64encode(os.urandom(16)).decode("ascii")
    request = (
        f"GET {path} HTTP/1.1\r\n"
        f"Host: {host}:{port}\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        f"X-Device-Token: {token}\r\n"
        "\r\n"
    ).encode("ascii")
    connection.sendall(request)
    response = bytearray()
    while b"\r\n\r\n" not in response:
        chunk = connection.recv(4096)
        if not chunk:
            break
        response.extend(chunk)
    status_line = bytes(response).split(b"\r\n", 1)[0]
    if b" 101 " not in status_line:
        connection.close()
        raise AssertionError(f"WebSocket upgrade failed: {status_line!r}")
    return connection


def websocket_send(connection, opcode, payload):
    mask = os.urandom(4)
    length = len(payload)
    header = bytearray([0x80 | opcode])
    if length < 126:
        header.append(0x80 | length)
    elif length <= 0xFFFF:
        header.append(0x80 | 126)
        header.extend(struct.pack("!H", length))
    else:
        header.append(0x80 | 127)
        header.extend(struct.pack("!Q", length))
    masked = bytes(value ^ mask[index % 4] for index, value in enumerate(payload))
    connection.sendall(bytes(header) + mask + masked)


def recv_exact(connection, length):
    data = bytearray()
    while len(data) < length:
        chunk = connection.recv(length - len(data))
        if not chunk:
            raise ConnectionError("WebSocket connection closed")
        data.extend(chunk)
    return bytes(data)


def websocket_receive(connection):
    first, second = recv_exact(connection, 2)
    opcode = first & 0x0F
    length = second & 0x7F
    if length == 126:
        length = struct.unpack("!H", recv_exact(connection, 2))[0]
    elif length == 127:
        length = struct.unpack("!Q", recv_exact(connection, 8))[0]
    if second & 0x80:
        mask = recv_exact(connection, 4)
        payload = recv_exact(connection, length)
        payload = bytes(value ^ mask[index % 4] for index, value in enumerate(payload))
    else:
        payload = recv_exact(connection, length)
    return opcode, payload


class GatewayIntegrationTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp_dir = tempfile.TemporaryDirectory()
        self.server = build_server(
            "127.0.0.1", 0, Path(self.temp_dir.name), TOKEN
        )
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.host, self.port = self.server.server_address[:2]

    def tearDown(self) -> None:
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)
        self.temp_dir.cleanup()

    def request(self, method, path, body=None, headers=None):
        connection = http.client.HTTPConnection(self.host, self.port, timeout=3)
        connection.request(method, path, body=body, headers=headers or {})
        response = connection.getresponse()
        data = response.read()
        status = response.status
        response_headers = dict(response.getheaders())
        connection.close()
        return status, response_headers, data

    def json_request(self, method, path, payload=None, headers=None):
        body = None if payload is None else json.dumps(payload).encode("utf-8")
        request_headers = {"Content-Type": "application/json", **(headers or {})}
        status, response_headers, data = self.request(
            method, path, body, request_headers
        )
        return status, response_headers, None if not data else json.loads(data)

    def test_voice_end_command_matching_is_strict(self):
        for transcript in (
            "退出聊天模式。",
            "请你结束长聊天吧！",
            "麻烦你停止对话一下，谢谢。",
            "退出会话",
        ):
            self.assertTrue(_is_end_conversation_command(transcript), transcript)

        for transcript in (
            "请介绍如何退出聊天模式",
            "我们继续聊天吧",
            "再见",
            "不聊了",
        ):
            self.assertFalse(_is_end_conversation_command(transcript), transcript)

    def test_tts_sentence_split_keeps_speech_chunks_bounded(self):
        chunks = split_tts_sentences("第一句。第二句！第三句？", max_chars=8)
        self.assertEqual(["第一句。", "第二句！", "第三句？"], chunks)
        self.assertTrue(all(len(chunk) <= 8 for chunk in chunks))

    def test_voice_reply_is_limited_to_two_short_sentences(self):
        self.assertEqual(
            "第一句。 第二句！",
            limit_voice_reply("第一句。第二句！第三句？", 2, 160),
        )

    def test_llm_sse_stream_emits_deltas(self):
        received = {}

        class SSEHandler(BaseHTTPRequestHandler):
            def do_POST(self):  # noqa: N802
                length = int(self.headers.get("Content-Length", "0"))
                received["payload"] = json.loads(self.rfile.read(length))
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Cache-Control", "no-cache")
                self.end_headers()
                for item in (
                    {"choices": [{"delta": {"content": "第一句。"}}]},
                    {"choices": [{"delta": {"content": "第二句！"}}]},
                    {"choices": [{"delta": {}}, "usage"], "usage": {"total_tokens": 7}},
                ):
                    if isinstance(item["choices"], list) and item["choices"] and item["choices"][0] == "usage":
                        item["choices"] = [{"delta": {}}]
                    self.wfile.write(f"data: {json.dumps(item)}\n\n".encode())
                    self.wfile.flush()
                self.wfile.write(b"data: [DONE]\n\n")
                self.wfile.flush()

            def log_message(self, *_args):
                return

        provider = ThreadingHTTPServer(("127.0.0.1", 0), SSEHandler)
        thread = threading.Thread(target=provider.serve_forever, daemon=True)
        thread.start()
        try:
            config = self.server.ai_service.database.config(include_secret=True)
            config.update(
                {
                    "llm_provider": "deepseek",
                    "llm_api_key": "sse-key",
                    "llm_base_url": f"http://127.0.0.1:{provider.server_port}",
                    "llm_streaming_enabled": True,
                }
            )
            deltas = []
            response, usage = self.server.ai_service._run_llm(
                [{"role": "user", "content": "你好"}], config, deltas.append
            )
            self.assertEqual("第一句。第二句！", response)
            self.assertEqual(["第一句。", "第二句！"], deltas)
            self.assertEqual(7, usage["total_tokens"])
            self.assertTrue(received["payload"]["stream"])
        finally:
            provider.shutdown()
            provider.server_close()
            thread.join(timeout=2)

    def test_command_websocket_pushes_json_and_streaming_pcm(self):
        connection = websocket_connect(self.host, self.port, f"/ws/devices/{DEVICE_ID}/commands", TOKEN)
        try:
            command = self.server.store.enqueue_command(DEVICE_ID, "ping", {"value": 1})
            opcode, payload = websocket_receive(connection)
            self.assertEqual(1, opcode)
            self.assertEqual(command["id"], json.loads(payload)["id"])

            websocket_send(
                connection,
                1,
                json.dumps(
                    {
                        "type": "event",
                        "command_id": command["id"],
                        "event_type": "pong",
                        "details": {"ok": True},
                    }
                ).encode(),
            )
            deadline = time.monotonic() + 1
            while time.monotonic() < deadline and not self.server.store.events():
                time.sleep(0.01)
            self.assertTrue(any(item["type"] == "pong" for item in self.server.store.events()))

            turn_id = "1234567890abcdef1234567890abcdef"
            pcm = struct.pack("<4h", 1, 2, 3, 4)
            (self.server.ai_service.tts_dir / f"{turn_id}-chunk-0.pcm").write_bytes(pcm)
            stream_command = self.server.store.enqueue_command(
                DEVICE_ID,
                "play_audio",
                {
                    "turn_id": turn_id,
                    "chunk_index": 0,
                    "sample_count": 4,
                    "sample_rate": 16000,
                    "audio_path": f"/api/tts/{turn_id}/chunk/0/pcm",
                    "transport": "websocket_pcm",
                    "stream_audio": True,
                },
            )
            opcode, payload = websocket_receive(connection)
            self.assertEqual(stream_command["id"], json.loads(payload)["id"])
            opcode, payload = websocket_receive(connection)
            self.assertEqual(2, opcode)
            self.assertEqual(pcm, payload)
            opcode, payload = websocket_receive(connection)
            self.assertEqual(1, opcode)
            end = json.loads(payload)
            self.assertEqual("audio_stream_end", end["type"])
            self.assertEqual(stream_command["id"], end["command_id"])
        finally:
            connection.close()

    def test_long_pcm_stream_is_paced_after_prefill(self):
        """A long PCM stream must not burst faster than the ESP32 playback queue."""
        pcm_path = Path(self.temp_dir.name) / "long.pcm"
        pcm_path.write_bytes(b"\x00" * (2048 * 4))
        clock = {"now": 0.0, "sleeps": []}

        def monotonic():
            return clock["now"]

        def sleep(seconds):
            clock["sleeps"].append(seconds)
            clock["now"] += seconds

        sent = []
        stats = stream_pcm_file(
            pcm_path,
            lambda opcode, payload: sent.append((opcode, payload)),
            threading.Event(),
            sample_rate=16000,
            prefill_bytes=2048,
            pace_factor=1.05,
            sleep_fn=sleep,
            monotonic_fn=monotonic,
        )

        self.assertEqual(4, stats["chunks_sent"])
        self.assertEqual(8192, stats["bytes_sent"])
        self.assertEqual(4, len(sent))
        self.assertEqual(3, len(clock["sleeps"]))
        self.assertTrue(all(value > 0 for value in clock["sleeps"]))
        self.assertGreater(clock["now"], 0.18)

    def test_recording_saved_queues_fixed_end_of_speech_reply_before_pipeline(self):
        service = self.server.ai_service
        service.update_config({"end_of_speech_reply_enabled": True})
        commands = []
        pcm = struct.pack("<160h", *([120] * 160))

        def fake_tts(text, path, _config):
            self.assertEqual("好的，我听到了。", text)
            path.write_bytes(pcm)
            return {"sample_rate": 16000, "sample_count": 160, "duration_ms": 10}

        service._tts_caller = fake_tts
        service.set_playback_sender(
            lambda device_id, payload: commands.append(
                {"device_id": device_id, **payload}
            )
            or {"id": "a" * 32, "type": payload["type"]}
        )
        recording = self.server.store.save_recording(
            DEVICE_ID, pcm, 16000, 1, 16, session_id=DEVICE_ID
        )

        self.assertEqual(1, len(commands))
        self.assertEqual("play_audio", commands[0]["type"])
        self.assertEqual("end_of_speech_reply", commands[0]["purpose"])
        self.assertEqual("好的，我听到了。", service.database.config(True)["end_of_speech_reply_text"])
        self.assertEqual(160, commands[0]["sample_count"])
        self.assertTrue(
            (service.tts_dir / f"{commands[0]['turn_id']}-chunk-0.pcm").is_file()
        )

    def test_health_and_web_console(self):
        status, _, payload = self.json_request("GET", "/health")
        self.assertEqual(200, status)
        self.assertTrue(payload["ok"])

        status, _, capabilities = self.json_request("GET", "/api/capabilities")
        self.assertEqual(200, status)
        self.assertTrue(capabilities["turn_upload"])
        self.assertTrue(capabilities["continuous_streaming"])
        self.assertTrue(capabilities["streaming_audio"])
        self.assertTrue(capabilities["sentence_tts"])
        self.assertTrue(capabilities["voice_end_conversation"])

        status, headers, body = self.request("GET", "/")
        self.assertEqual(200, status)
        self.assertIn("text/html", headers["Content-Type"])
        self.assertIn("AI Mirror 控制台".encode("utf-8"), body)

        status, headers, body = self.request("GET", "/app.js")
        self.assertEqual(200, status)
        self.assertIn(b"const MAX_VISIBLE_CONVERSATIONS = 20;", body)
        self.assertIn(
            b"/api/conversations?limit=${MAX_VISIBLE_CONVERSATIONS}", body
        )

    def test_model_config_is_mutable_and_api_key_is_never_returned(self):
        status, _, initial = self.json_request("GET", "/api/config")
        self.assertEqual(200, status)
        self.assertEqual("unisound", initial["asr_provider"])
        self.assertEqual("unisound", initial["llm_provider"])
        self.assertEqual("unisound", initial["tts_provider"])
        self.assertEqual("u2-asr", initial["unisound_asr_model"])
        self.assertEqual("u2", initial["unisound_llm_model"])
        self.assertEqual("u2-tts", initial["unisound_tts_model"])
        self.assertFalse(initial["llm_api_key_configured"])
        self.assertFalse(initial["unisound_api_key_configured"])
        self.assertNotIn("llm_api_key", initial)
        self.assertNotIn("unisound_api_key", initial)

        status, _, updated = self.json_request(
            "POST",
            "/api/config",
            {
                "asr_language": "zh",
                "llm_model": "deepseek-v4-flash",
                "llm_max_tokens": 256,
                "system_prompt": "请用一句中文回答。",
                "llm_api_key": "test-secret-key",
                "asr_api_url": "http://127.0.0.1:19001/asr",
                "asr_api_key": "asr-secret-key",
                "asr_api_model": "custom-asr",
                "tts_api_url": "http://127.0.0.1:19001/tts",
                "tts_api_key": "tts-secret-key",
                "tts_api_model": "custom-tts",
                "tts_prompt": "温柔地朗读",
                "unisound_api_key": "unisound-secret-key",
                "unisound_tts_voice": "cn_male_chenyu",
            },
        )
        self.assertEqual(200, status)
        self.assertEqual(256, updated["llm_max_tokens"])
        self.assertTrue(updated["llm_api_key_configured"])
        self.assertNotIn("test-secret-key", json.dumps(updated))
        self.assertTrue(updated["asr_api_key_configured"])
        self.assertTrue(updated["tts_api_key_configured"])
        self.assertTrue(updated["unisound_api_key_configured"])
        self.assertNotIn("asr-secret-key", json.dumps(updated))
        self.assertNotIn("tts-secret-key", json.dumps(updated))
        self.assertNotIn("unisound-secret-key", json.dumps(updated))

        status, _, loaded = self.json_request("GET", "/api/config")
        self.assertEqual(200, status)
        self.assertTrue(loaded["llm_api_key_configured"])
        self.assertTrue(loaded["unisound_api_key_configured"])
        self.assertNotIn("llm_api_key", loaded)
        self.assertEqual(
            "test-secret-key",
            self.server.ai_service.database.config(include_secret=True)["llm_api_key"],
        )
        self.assertEqual(
            "unisound-secret-key",
            self.server.ai_service.database.config(include_secret=True)[
                "unisound_api_key"
            ],
        )

    def test_restart_queue_does_not_retry_historical_failures(self):
        pcm = struct.pack("<160h", *([120] * 160))
        failed_recording = self.server.store.save_recording(
            DEVICE_ID, pcm, 16000, 1, 16
        )
        failed_turn = self.server.ai_service.database.turn_by_recording(
            failed_recording["id"]
        )
        self.server.ai_service.database.update_turn(
            failed_turn["id"], status="asr_failed", error_message="test failure"
        )

        pending_recording = self.server.store.save_recording(
            DEVICE_ID, pcm, 16000, 1, 16
        )
        pending_turn = self.server.ai_service.database.turn_by_recording(
            pending_recording["id"]
        )
        waiting_ids = {
            item["id"] for item in self.server.ai_service.database.waiting_turns()
        }

        self.assertNotIn(failed_turn["id"], waiting_ids)
        self.assertIn(pending_turn["id"], waiting_ids)

    def test_custom_http_asr_and_tts_contracts(self):
        received = {}
        pcm = struct.pack("<320h", *([321] * 320))

        class CustomProviderHandler(BaseHTTPRequestHandler):
            def do_POST(self):  # noqa: N802
                body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
                received[self.path] = {
                    "body": body,
                    "authorization": self.headers.get("Authorization", ""),
                    "content_type": self.headers.get("Content-Type", ""),
                }
                if self.path == "/asr":
                    response = json.dumps({"text": "自定义识别成功"}).encode("utf-8")
                else:
                    request_payload = json.loads(body)
                    received["tts_payload"] = request_payload
                    response = json.dumps(
                        {
                            "audio_base64": base64.b64encode(pcm).decode("ascii"),
                            "format": "pcm_s16le",
                        }
                    ).encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(response)))
                self.end_headers()
                self.wfile.write(response)

            def log_message(self, *_args):
                return

        provider = ThreadingHTTPServer(("127.0.0.1", 0), CustomProviderHandler)
        provider_thread = threading.Thread(target=provider.serve_forever, daemon=True)
        provider_thread.start()
        try:
            base_url = f"http://127.0.0.1:{provider.server_port}"
            config = self.server.ai_service.database.config(include_secret=True)
            config.update(
                {
                    "asr_provider": "custom_http",
                    "asr_api_url": f"{base_url}/asr",
                    "asr_api_key": "asr-key",
                    "asr_api_model": "asr-model",
                    "tts_provider": "custom_http",
                    "tts_api_url": f"{base_url}/tts",
                    "tts_api_key": "tts-key",
                    "tts_api_model": "tts-model",
                    "tts_prompt": "轻快地朗读",
                }
            )
            wav_path = Path(self.temp_dir.name) / "custom-input.wav"
            with wave.open(str(wav_path), "wb") as output:
                output.setnchannels(1)
                output.setsampwidth(2)
                output.setframerate(16000)
                output.writeframes(pcm)

            transcript = self.server.ai_service._run_transcriber(wav_path, config)
            self.assertEqual("自定义识别成功", transcript)
            self.assertIn(b'name="file"', received["/asr"]["body"])
            self.assertEqual("Bearer asr-key", received["/asr"]["authorization"])

            pcm_path = Path(self.temp_dir.name) / "custom-output.pcm"
            audio = self.server.ai_service._run_tts("你好", pcm_path, config)
            self.assertEqual(pcm, pcm_path.read_bytes())
            self.assertEqual(320, audio["sample_count"])
            self.assertEqual("轻快地朗读", received["tts_payload"]["prompt"])
            self.assertEqual("Bearer tts-key", received["/tts"]["authorization"])
        finally:
            provider.shutdown()
            provider.server_close()
            provider_thread.join(timeout=2)

    def test_unisound_asr_llm_and_tts_contracts(self):
        received = []
        pcm = struct.pack("<320h", *([654] * 320))

        class UnisoundProviderHandler(BaseHTTPRequestHandler):
            def send_json(self, payload):
                response = json.dumps(payload, ensure_ascii=False).encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(response)))
                self.end_headers()
                self.wfile.write(response)

            def remember(self, body=b""):
                received.append(
                    {
                        "method": self.command,
                        "path": self.path,
                        "authorization": self.headers.get("Authorization", ""),
                        "content_type": self.headers.get("Content-Type", ""),
                        "body": body,
                    }
                )

            def do_POST(self):  # noqa: N802
                body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
                self.remember(body)
                if self.path == "/v1/files/upload":
                    self.send_json(
                        {
                            "file": {
                                "file_id": 101,
                                "bytes": len(body),
                                "filename": "recording.wav",
                                "purpose": "a2t_async_input",
                            },
                            "base_resp": {"status_code": 0, "status_msg": "success"},
                        }
                    )
                elif self.path == "/v1/audio/asr/tasks":
                    self.send_json(
                        {
                            "task_id": "asr-task",
                            "base_resp": {"status_code": 0, "status_msg": "success"},
                        }
                    )
                elif self.path == "/v1/chat/completions":
                    self.send_json(
                        {
                            "id": "chatcmpl-test",
                            "object": "chat.completion",
                            "model": "u2",
                            "choices": [
                                {
                                    "index": 0,
                                    "finish_reason": "stop",
                                    "message": {
                                        "role": "assistant",
                                        "content": "云知声 U2 回复成功",
                                    },
                                }
                            ],
                            "usage": {
                                "prompt_tokens": 10,
                                "completion_tokens": 6,
                                "total_tokens": 16,
                            },
                        }
                    )
                elif self.path == "/v1/files/delete":
                    self.send_json(
                        {
                            "file_id": 101,
                            "base_resp": {"status_code": 0, "status_msg": "success"},
                        }
                    )
                elif self.path == "/v1/audio/speech/tasks":
                    self.send_json(
                        {
                            "task_id": "tts-task",
                            "usage_characters": 2,
                            "base_resp": {"status_code": 0, "status_msg": "OK"},
                        }
                    )
                else:
                    self.send_error(404)

            def do_GET(self):  # noqa: N802
                self.remember()
                if self.path == "/v1/audio/asr/tasks/asr-task":
                    self.send_json(
                        {
                            "status": "Success",
                            "results": [{"text": "云知声识别成功"}],
                            "base_resp": {"status_code": 0, "status_msg": "success"},
                        }
                    )
                elif self.path == "/v1/audio/speech/tasks?task_id=tts-task":
                    self.send_json(
                        {
                            "task_id": "tts-task",
                            "status": "Success",
                            "file_id": 202,
                            "base_resp": {"status_code": 0, "status_msg": "success"},
                        }
                    )
                elif self.path == "/v1/files/retrieve_content?file_id=202":
                    response = b"fake-mp3-for-contract-test"
                    self.send_response(200)
                    self.send_header("Content-Type", "application/octet-stream")
                    self.send_header("Content-Length", str(len(response)))
                    self.end_headers()
                    self.wfile.write(response)
                else:
                    self.send_error(404)

            def log_message(self, *_args):
                return

        provider = ThreadingHTTPServer(("127.0.0.1", 0), UnisoundProviderHandler)
        provider_thread = threading.Thread(target=provider.serve_forever, daemon=True)
        provider_thread.start()
        service = self.server.ai_service
        original_converter = service._convert_audio_to_pcm
        try:
            config = service.database.config(include_secret=True)
            config.update(
                {
                    "asr_provider": "unisound",
                    "llm_provider": "unisound",
                    "tts_provider": "unisound",
                    "unisound_base_url": f"http://127.0.0.1:{provider.server_port}/v1",
                    "unisound_api_key": "unisound-key",
                    "unisound_timeout_seconds": 5,
                    "unisound_poll_interval_ms": 100,
                    "tts_rate": 20,
                    "tts_volume": -20,
                }
            )
            wav_path = Path(self.temp_dir.name) / "unisound-input.wav"
            with wave.open(str(wav_path), "wb") as output:
                output.setnchannels(1)
                output.setsampwidth(2)
                output.setframerate(16000)
                output.writeframes(pcm)

            transcript = service._run_transcriber(wav_path, config)
            self.assertEqual("云知声识别成功", transcript)

            response, usage = service._run_llm(
                [{"role": "user", "content": "你好"}], config
            )
            self.assertEqual("云知声 U2 回复成功", response)
            self.assertEqual(16, usage["total_tokens"])

            service._convert_audio_to_pcm = (
                lambda _source, target, _timeout: target.write_bytes(pcm)
            )
            pcm_path = Path(self.temp_dir.name) / "unisound-output.pcm"
            audio = service._run_tts("你好", pcm_path, config)
            self.assertEqual(pcm, pcm_path.read_bytes())
            self.assertEqual(320, audio["sample_count"])

            paths = [item["path"] for item in received]
            self.assertIn("/v1/files/upload", paths)
            self.assertIn("/v1/audio/asr/tasks/asr-task", paths)
            self.assertIn("/v1/chat/completions", paths)
            self.assertIn("/v1/files/delete", paths)
            self.assertIn("/v1/audio/speech/tasks?task_id=tts-task", paths)
            self.assertIn("/v1/files/retrieve_content?file_id=202", paths)
            self.assertTrue(
                all(item["authorization"] == "Bearer unisound-key" for item in received)
            )
            upload = next(item for item in received if item["path"] == "/v1/files/upload")
            self.assertIn(b"a2t_async_input", upload["body"])
            chat = next(
                item for item in received if item["path"] == "/v1/chat/completions"
            )
            chat_payload = json.loads(chat["body"])
            self.assertEqual("u2", chat_payload["model"])
            self.assertFalse(chat_payload["stream"])
            self.assertNotIn("thinking", chat_payload)
            tts_create = next(
                item
                for item in received
                if item["method"] == "POST"
                and item["path"] == "/v1/audio/speech/tasks"
            )
            tts_payload = json.loads(tts_create["body"])
            self.assertEqual("u2-tts", tts_payload["model"])
            self.assertEqual(60, tts_payload["voice_setting"]["speed"])
            self.assertEqual(40, tts_payload["voice_setting"]["volume"])
        finally:
            service._convert_audio_to_pcm = original_converter
            provider.shutdown()
            provider.server_close()
            provider_thread.join(timeout=2)

    def test_sse_stream_announces_readiness(self):
        connection = http.client.HTTPConnection(self.host, self.port, timeout=3)
        connection.request("GET", "/api/stream")
        response = connection.getresponse()
        self.assertEqual(200, response.status)
        self.assertIn("text/event-stream", response.getheader("Content-Type"))
        self.assertEqual(b"event: ready\n", response.fp.readline())
        data_line = response.fp.readline()
        self.assertTrue(data_line.startswith(b"data: "))
        payload = json.loads(data_line[6:])
        self.assertIn("worker_running", payload)
        connection.close()

    def test_pcm_upload_is_persisted_as_playable_wav(self):
        pcm = struct.pack("<8h", -1000, -500, 0, 500, 1000, 500, 0, -500)
        headers = {
            "Content-Type": "audio/L16",
            "Content-Length": str(len(pcm)),
            "X-Device-Token": TOKEN,
            "X-Sample-Rate": "16000",
            "X-Channels": "1",
            "X-Bits-Per-Sample": "16",
        }
        status, _, body = self.request(
            "POST", f"/api/devices/{DEVICE_ID}/recordings", pcm, headers
        )
        self.assertEqual(201, status)
        recording = json.loads(body)
        self.assertEqual(8, recording["sample_count"])

        wav_path = Path(self.temp_dir.name) / "recordings" / recording["filename"]
        with wave.open(str(wav_path), "rb") as saved:
            self.assertEqual(1, saved.getnchannels())
            self.assertEqual(2, saved.getsampwidth())
            self.assertEqual(16000, saved.getframerate())
            self.assertEqual(8, saved.getnframes())
            self.assertEqual(pcm, saved.readframes(8))

        status, headers, wav_body = self.request("GET", recording["audio_url"])
        self.assertEqual(200, status)
        self.assertEqual("audio/wav", headers["Content-Type"])
        self.assertTrue(wav_body.startswith(b"RIFF"))

        status, _, listing = self.json_request("GET", "/api/recordings")
        self.assertEqual(200, status)
        self.assertEqual(recording["id"], listing["recordings"][0]["id"])

        status, _, conversations = self.json_request("GET", "/api/conversations")
        self.assertEqual(200, status)
        self.assertEqual(recording["id"], conversations["turns"][0]["recording_id"])
        self.assertEqual("pending", conversations["turns"][0]["status"])

    def test_background_asr_and_llm_pipeline_with_injected_providers(self):
        def transcriber(path, config):
            self.assertTrue(path.is_file())
            self.assertEqual("zh", config["asr_language"])
            return "你好，镜子"

        def llm_caller(messages, config):
            self.assertEqual("你好，镜子", messages[-1]["content"])
            self.assertFalse(config["llm_thinking"])
            return "你好！推荐你读《*三体*》。", {
                "prompt_tokens": 24,
                "completion_tokens": 12,
            }

        def tts_caller(text, pcm_path, config):
            self.assertEqual("你好！ 推荐你读三体。", text)
            self.assertEqual("unisound", config["tts_provider"])
            pcm_path.write_bytes(struct.pack("<320h", *([800] * 320)))
            return {"sample_rate": 16000, "sample_count": 320, "duration_ms": 20}

        self.server.ai_service._transcriber = transcriber
        self.server.ai_service._llm_caller = llm_caller
        self.server.ai_service._tts_caller = tts_caller
        self.server.ai_service.start()
        status, _, _ = self.json_request(
            "POST",
            "/api/config",
            {
                "llm_provider": "deepseek",
                "llm_api_key": "fake-key",
                "tts_sentence_streaming_enabled": False,
            },
        )
        self.assertEqual(200, status)

        pcm = struct.pack("<160h", *([120] * 160))
        headers = {
            "Content-Type": "audio/L16",
            "Content-Length": str(len(pcm)),
            "X-Device-Token": TOKEN,
            "X-Sample-Rate": "16000",
            "X-Channels": "1",
            "X-Bits-Per-Sample": "16",
        }
        status, _, body = self.request(
            "POST", f"/api/devices/{DEVICE_ID}/recordings", pcm, headers
        )
        self.assertEqual(201, status)
        recording = json.loads(body)

        deadline = time.monotonic() + 3
        turn = None
        while time.monotonic() < deadline:
            status, _, conversations = self.json_request("GET", "/api/conversations")
            self.assertEqual(200, status)
            turn = next(
                item
                for item in conversations["turns"]
                if item["recording_id"] == recording["id"]
            )
            if turn["status"] == "playback_queued":
                break
            time.sleep(0.03)

        self.assertIsNotNone(turn)
        self.assertEqual("playback_queued", turn["status"])
        self.assertEqual("你好，镜子", turn["transcript"])
        self.assertEqual("你好！推荐你读《*三体*》。", turn["assistant_response"])
        self.assertEqual(24, turn["input_tokens"])
        self.assertEqual(12, turn["output_tokens"])
        self.assertEqual(20, turn["audio_duration_ms"])
        self.assertIsNotNone(turn["processing_latency_ms"])

        status, _, command = self.json_request(
            "GET",
            f"/api/devices/{DEVICE_ID}/commands?wait_ms=0",
            headers={"X-Device-Token": TOKEN},
        )
        self.assertEqual(200, status)
        self.assertEqual("play_audio", command["type"])
        self.assertEqual(turn["id"], command["turn_id"])
        status, headers, pcm = self.request(
            "GET", command["audio_path"], headers={"X-Device-Token": TOKEN}
        )
        self.assertEqual(200, status)
        self.assertEqual("application/octet-stream", headers["Content-Type"])
        self.assertEqual(640, len(pcm))

        for event_type, details in (
            ("playback_started", {"sample_count": 320}),
            ("playback_completed", {"playback_ms": 20}),
        ):
            status, _, _ = self.json_request(
                "POST",
                f"/api/devices/{DEVICE_ID}/events",
                {
                    "command_id": command["id"],
                    "type": event_type,
                    "details": details,
                },
                headers={"X-Device-Token": TOKEN},
            )
            self.assertEqual(201, status)

        status, _, conversations = self.json_request("GET", "/api/conversations")
        turn = next(
            item for item in conversations["turns"] if item["id"] == turn["id"]
        )
        self.assertEqual("playback_completed", turn["status"])
        self.assertEqual(20, turn["playback_latency_ms"])
        self.assertIsNotNone(turn["end_to_end_latency_ms"])

        status, _, logs = self.json_request("GET", "/api/logs")
        self.assertEqual(200, status)
        components = {entry["component"] for entry in logs["logs"]}
        self.assertIn("asr", components)
        self.assertIn("llm", components)
        self.assertIn("tts", components)
        self.assertIn("timing", components)

    def test_sentence_tts_queues_chunks_and_tracks_final_playback(self):
        tts_chunks = []

        def transcriber(_path, _config):
            return "测试问题"

        def llm_caller(_messages, _config):
            return "第一句。第二句！", {}

        def tts_caller(text, pcm_path, _config):
            tts_chunks.append(text)
            pcm_path.write_bytes(struct.pack("<160h", *([800] * 160)))
            return {"sample_rate": 16000, "sample_count": 160, "duration_ms": 10}

        self.server.ai_service._transcriber = transcriber
        self.server.ai_service._llm_caller = llm_caller
        self.server.ai_service._tts_caller = tts_caller
        self.server.ai_service.start()
        status, _, _ = self.json_request(
            "POST",
            "/api/config",
            {
                "unisound_api_key": "fake-key",
                "tts_sentence_max_chars": 16,
                "end_of_speech_reply_enabled": False,
            },
        )
        self.assertEqual(200, status)
        pcm = struct.pack("<160h", *([120] * 160))
        recording = self.server.store.save_recording(DEVICE_ID, pcm, 16000, 1, 16)

        deadline = time.monotonic() + 3
        turn = None
        while time.monotonic() < deadline:
            turn = self.server.ai_service.database.turn_by_recording(recording["id"])
            if turn and turn["status"] == "playback_queued" and len(tts_chunks) >= 2:
                break
            time.sleep(0.03)
        # TTS synthesis is intentionally concurrent; playback order is
        # asserted below through chunk/command sequencing, not synthesis
        # worker completion order.
        self.assertCountEqual(["第一句。", "第二句！"], tts_chunks)
        self.assertIsNotNone(turn)
        first = self.server.store.take_command(DEVICE_ID, 0)
        self.assertIsNotNone(first)
        self.assertEqual("/api/tts/%s/chunk/0/pcm" % turn["id"], first["audio_path"])
        # The next sentence must wait for the ESP32 to finish the first stream;
        # sending the second command while audio is buffered would truncate it.
        self.assertIsNone(self.server.store.take_command(DEVICE_ID, 20))

        self.server.ai_service.device_event(
            self.server.store.add_event(
                DEVICE_ID,
                {"command_id": first["id"], "type": "playback_started", "details": {}},
            )
        )
        self.server.ai_service.device_event(
            self.server.store.add_event(
                DEVICE_ID,
                {"command_id": first["id"], "type": "playback_completed", "details": {"playback_ms": 10}},
            )
        )

        second = None
        deadline = time.monotonic() + 1
        while second is None and time.monotonic() < deadline:
            second = self.server.store.take_command(DEVICE_ID, 20)
        self.assertIsNotNone(second)
        self.assertEqual("/api/tts/%s/chunk/1/pcm" % turn["id"], second["audio_path"])
        self.server.ai_service.device_event(
            self.server.store.add_event(
                DEVICE_ID,
                {"command_id": second["id"], "type": "playback_started", "details": {}},
            )
        )
        self.server.ai_service.device_event(
            self.server.store.add_event(
                DEVICE_ID,
                {"command_id": second["id"], "type": "playback_completed", "details": {"playback_ms": 10}},
            )
        )
        deadline = time.monotonic() + 1
        final = self.server.ai_service.database.turn(turn["id"])
        while final["status"] != "playback_completed" and time.monotonic() < deadline:
            time.sleep(0.01)
            final = self.server.ai_service.database.turn(turn["id"])
        self.assertEqual("playback_completed", final["status"])

    def test_punctuation_only_asr_result_resumes_listening_without_llm(self):
        llm_called = False

        def transcriber(_path, _config):
            return "。"

        def llm_caller(_messages, _config):
            nonlocal llm_called
            llm_called = True
            raise AssertionError("punctuation-only transcript must not reach LLM")

        self.server.ai_service._transcriber = transcriber
        self.server.ai_service._llm_caller = llm_caller
        self.server.ai_service.start()

        pcm = struct.pack("<160h", *([30] * 160))
        headers = {
            "Content-Type": "audio/L16",
            "Content-Length": str(len(pcm)),
            "X-Device-Token": TOKEN,
            "X-Sample-Rate": "16000",
            "X-Channels": "1",
            "X-Bits-Per-Sample": "16",
        }
        status, _, body = self.request(
            "POST", f"/api/devices/{DEVICE_ID}/recordings", pcm, headers
        )
        self.assertEqual(201, status)
        recording = json.loads(body)

        deadline = time.monotonic() + 3
        turn = None
        while time.monotonic() < deadline:
            _, _, conversations = self.json_request("GET", "/api/conversations")
            turn = next(
                item
                for item in conversations["turns"]
                if item["recording_id"] == recording["id"]
            )
            if turn["status"] == "ignored":
                break
            time.sleep(0.03)

        self.assertIsNotNone(turn)
        self.assertEqual("ignored", turn["status"])
        self.assertFalse(llm_called)
        status, _, command = self.json_request(
            "GET",
            f"/api/devices/{DEVICE_ID}/commands?wait_ms=0",
            headers={"X-Device-Token": TOKEN},
        )
        self.assertEqual(200, status)
        self.assertEqual("continue_listening", command["type"])
        self.assertEqual(turn["id"], command["turn_id"])

    def test_voice_end_command_skips_llm_and_requests_session_end(self):
        llm_called = False
        tts_called = False

        def transcriber(_path, _config):
            return "请退出长聊天模式吧！"

        def llm_caller(_messages, _config):
            nonlocal llm_called
            llm_called = True
            raise AssertionError("voice end command must not reach LLM")

        def tts_caller(_text, _pcm_path, _config):
            nonlocal tts_called
            tts_called = True
            raise AssertionError("voice end command must not reach TTS")

        self.server.ai_service._transcriber = transcriber
        self.server.ai_service._llm_caller = llm_caller
        self.server.ai_service._tts_caller = tts_caller
        self.server.ai_service.start()
        status, _, _ = self.json_request(
            "POST",
            "/api/config",
            {
                "llm_provider": "deepseek",
                "llm_api_key": "fake-key",
                "end_of_speech_reply_enabled": False,
            },
        )
        self.assertEqual(200, status)

        pcm = struct.pack("<160h", *([120] * 160))
        headers = {
            "Content-Type": "audio/L16",
            "Content-Length": str(len(pcm)),
            "X-Device-Token": TOKEN,
            "X-Sample-Rate": "16000",
            "X-Channels": "1",
            "X-Bits-Per-Sample": "16",
        }
        status, _, body = self.request(
            "POST", f"/api/devices/{DEVICE_ID}/recordings", pcm, headers
        )
        self.assertEqual(201, status)
        recording = json.loads(body)

        deadline = time.monotonic() + 3
        turn = None
        while time.monotonic() < deadline:
            _, _, conversations = self.json_request("GET", "/api/conversations")
            turn = next(
                item
                for item in conversations["turns"]
                if item["recording_id"] == recording["id"]
            )
            if turn["status"] == "session_end_queued":
                break
            time.sleep(0.03)

        self.assertIsNotNone(turn)
        self.assertEqual("session_end_queued", turn["status"])
        self.assertEqual("请退出长聊天模式吧！", turn["transcript"])
        self.assertEqual("（已执行：退出长聊天模式）", turn["assistant_response"])
        self.assertFalse(llm_called)
        self.assertFalse(tts_called)

        status, _, command = self.json_request(
            "GET",
            f"/api/devices/{DEVICE_ID}/commands?wait_ms=0",
            headers={"X-Device-Token": TOKEN},
        )
        self.assertEqual(200, status)
        self.assertEqual("end_conversation", command["type"])
        self.assertEqual(turn["id"], command["turn_id"])

    def test_device_auth_command_and_event_round_trip(self):
        status, _, payload = self.json_request(
            "POST",
            f"/api/devices/{DEVICE_ID}/commands",
            {"type": "get_status"},
        )
        self.assertEqual(202, status)
        command_id = payload["id"]

        status, _, command = self.json_request(
            "GET",
            f"/api/devices/{DEVICE_ID}/commands?wait_ms=0",
            headers={"X-Device-Token": TOKEN},
        )
        self.assertEqual(200, status)
        self.assertEqual(command_id, command["id"])
        self.assertEqual("get_status", command["type"])

        status, _, event = self.json_request(
            "POST",
            f"/api/devices/{DEVICE_ID}/events",
            {
                "command_id": command_id,
                "type": "status",
                "details": {"free_heap": 123456},
            },
            headers={"X-Device-Token": TOKEN},
        )
        self.assertEqual(201, status)
        self.assertEqual(123456, event["details"]["free_heap"])

        status, _, events = self.json_request("GET", "/api/events")
        self.assertEqual(200, status)
        self.assertEqual("status", events["events"][0]["type"])

        status, _, _ = self.json_request(
            "GET",
            f"/api/devices/{DEVICE_ID}/commands?wait_ms=0",
            headers={"X-Device-Token": "wrong"},
        )
        self.assertEqual(401, status)

    def test_websocket_pcm_upload_is_saved_and_acknowledged(self):
        pcm = struct.pack("<8h", -1200, -600, 0, 600, 1200, 600, 0, -600)
        connection = websocket_connect(
            self.host,
            self.port,
            f"/ws/devices/{DEVICE_ID}/recordings",
            TOKEN,
        )
        try:
            metadata = {
                "type": "recording_pcm",
                "protocol_version": 1,
                "transport": "turn",
                "session_id": "session-test-001",
                "turn_index": 2,
                "sample_rate": 16000,
                "channels": 1,
                "bits_per_sample": 16,
                "sample_count": 8,
            }
            websocket_send(connection, 0x1, json.dumps(metadata).encode("utf-8"))
            websocket_send(connection, 0x2, pcm)
            opcode, payload = websocket_receive(connection)
            self.assertEqual(0x1, opcode)
            acknowledgement = json.loads(payload)
            self.assertEqual("recording_saved", acknowledgement["type"])
            self.assertEqual(8, acknowledgement["recording"]["sample_count"])
            self.assertEqual("session-test-001", acknowledgement["recording"]["session_id"])
            self.assertEqual(2, acknowledgement["recording"]["turn_index"])
        finally:
            try:
                websocket_send(connection, 0x8, struct.pack("!H", 1000))
            finally:
                connection.close()

        wav_path = (
            Path(self.temp_dir.name)
            / "recordings"
            / acknowledgement["recording"]["filename"]
        )
        with wave.open(str(wav_path), "rb") as saved:
            self.assertEqual(1, saved.getnchannels())
            self.assertEqual(16000, saved.getframerate())
            self.assertEqual(pcm, saved.readframes(8))

    def test_websocket_stream_upload_accepts_multiple_pcm_chunks(self):
        pcm = struct.pack("<8h", -1200, -600, 0, 600, 1200, 600, 0, -600)
        connection = websocket_connect(
            self.host,
            self.port,
            f"/ws/devices/{DEVICE_ID}/recordings",
            TOKEN,
        )
        try:
            websocket_send(
                connection,
                0x1,
                json.dumps(
                    {
                        "type": "stream_session_start",
                        "protocol_version": 2,
                        "transport": "stream",
                        "stream_id": "stream-test-001",
                        "session_id": "session-test-001",
                        "turn_index": 3,
                        "sample_rate": 16000,
                        "channels": 1,
                        "bits_per_sample": 16,
                    }
                ).encode("utf-8"),
            )
            opcode, payload = websocket_receive(connection)
            self.assertEqual(0x1, opcode)
            self.assertEqual("stream_session_started", json.loads(payload)["type"])
            websocket_send(connection, 0x2, pcm[:8])
            websocket_send(connection, 0x2, pcm[8:])
            websocket_send(
                connection,
                0x1,
                b'{"type":"stream_session_end","stream_id":"stream-test-001"}',
            )
            opcode, payload = websocket_receive(connection)
            self.assertEqual(0x1, opcode)
            acknowledgement = json.loads(payload)
            self.assertEqual("stream_session_ended", acknowledgement["type"])
            self.assertEqual(8, acknowledgement["recording"]["sample_count"])
            self.assertEqual("stream", acknowledgement["recording"]["transport"])
        finally:
            try:
                websocket_send(connection, 0x8, struct.pack("!H", 1000))
            finally:
                connection.close()

    @unittest.skipUnless(
        HAS_PLAYWRIGHT_BROWSER, "Playwright and Edge/Chrome are required"
    )
    def test_recording_keeps_playing_across_dashboard_refresh(self):
        self.server.store.save_recording(
            DEVICE_ID, bytes(16000 * 5 * 2), 16000, 1, 16
        )

        from playwright.sync_api import sync_playwright

        with sync_playwright() as playwright:
            browser = playwright.chromium.launch(
                executable_path=str(BROWSER_EXECUTABLE),
                headless=True,
                args=["--autoplay-policy=no-user-gesture-required"],
            )
            try:
                page = browser.new_page()
                page.goto(
                    f"http://{self.host}:{self.port}/", wait_until="domcontentloaded"
                )
                page.wait_for_selector("audio")
                audio = page.query_selector("audio")
                self.assertIsNotNone(audio)
                audio.evaluate("element => element.play()")
                page.wait_for_timeout(400)
                before_refresh = audio.evaluate("element => element.currentTime")
                self.assertFalse(audio.evaluate("element => element.paused"))

                page.wait_for_timeout(2200)

                self.assertTrue(audio.evaluate("element => element.isConnected"))
                self.assertFalse(audio.evaluate("element => element.paused"))
                self.assertGreater(
                    audio.evaluate("element => element.currentTime"),
                    before_refresh + 1,
                )
            finally:
                browser.close()

if __name__ == "__main__":
    unittest.main()
