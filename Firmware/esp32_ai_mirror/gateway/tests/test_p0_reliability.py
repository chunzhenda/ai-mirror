import os
import tempfile
import time
import unittest
from pathlib import Path

from gateway.src.app import build_server
from gateway.src.maintenance import cleanup_tree
from gateway.src.resilience import RetryPolicy, run_with_retry


TOKEN = "p0-test-token"
DEVICE_ID = "p0-device"


class P0ReliabilityTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp_dir = tempfile.TemporaryDirectory()
        self.server = build_server(
            "127.0.0.1", 0, Path(self.temp_dir.name), TOKEN
        )

    def tearDown(self) -> None:
        self.server.server_close()
        self.temp_dir.cleanup()

    def test_retry_policy_retries_transient_failure_with_bounded_attempts(self):
        attempts = []

        def operation():
            attempts.append(len(attempts) + 1)
            if len(attempts) < 3:
                raise TimeoutError("provider timed out")
            return "ok"

        result = run_with_retry(
            operation,
            RetryPolicy(max_attempts=3, base_delay_seconds=0, max_delay_seconds=0),
            retryable=lambda error: isinstance(error, TimeoutError),
        )

        self.assertEqual("ok", result)
        self.assertEqual([1, 2, 3], attempts)

    def test_command_enqueue_is_idempotent_for_same_key(self):
        first = self.server.store.enqueue_command(
            DEVICE_ID,
            "play_audio",
            {
                "turn_id": "a" * 32,
                "chunk_index": 0,
                "idempotency_key": "turn-a-chunk-0",
            },
        )
        second = self.server.store.enqueue_command(
            DEVICE_ID,
            "play_audio",
            {
                "turn_id": "a" * 32,
                "chunk_index": 0,
                "idempotency_key": "turn-a-chunk-0",
            },
        )

        self.assertEqual(first["id"], second["id"])
        queued = self.server.store.take_command(DEVICE_ID, 0)
        self.assertIsNotNone(queued)
        self.assertIsNone(self.server.store.take_command(DEVICE_ID, 0))

    def test_command_queue_rejects_overflow_instead_of_dropping_oldest(self):
        for index in range(20):
            self.server.store.enqueue_command(
                DEVICE_ID,
                "ping",
                {"idempotency_key": f"ping-{index}"},
            )
        with self.assertRaises(OverflowError):
            self.server.store.enqueue_command(
                DEVICE_ID,
                "ping",
                {"idempotency_key": "ping-overflow"},
            )

    def test_health_endpoints_expose_liveness_readiness_and_devices(self):
        import http.client
        import json
        import threading

        thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        thread.start()
        host, port = self.server.server_address[:2]
        try:
            connection = http.client.HTTPConnection(host, port, timeout=2)
            connection.request("GET", "/health/live")
            live = connection.getresponse()
            live_payload = json.loads(live.read())
            connection.close()

            connection = http.client.HTTPConnection(host, port, timeout=2)
            connection.request("GET", "/health/ready")
            ready = connection.getresponse()
            ready_payload = json.loads(ready.read())
            connection.close()

            connection = http.client.HTTPConnection(host, port, timeout=2)
            connection.request("GET", "/health/devices")
            devices = connection.getresponse()
            devices_payload = json.loads(devices.read())
            connection.close()
        finally:
            self.server.shutdown()
            thread.join(timeout=2)

        self.assertEqual(200, live.status)
        self.assertTrue(live_payload["ok"])
        self.assertIn("checks", ready_payload)
        self.assertIn("devices", devices_payload)

    def test_stale_turn_recovery_marks_old_active_turn_failed(self):
        recording = self.server.store.save_recording(
            DEVICE_ID,
            b"\x00\x00" * 160,
            16000,
            1,
            16,
        )
        turn = self.server.ai_service.database.turn_by_recording(recording["id"])
        self.assertIsNotNone(turn)
        with self.server.ai_service.database._lock, self.server.ai_service.database._connection:
            self.server.ai_service.database._connection.execute(
                "UPDATE conversation_turns SET status='playing', updated_at=? WHERE id=?",
                ("2000-01-01T00:00:00+00:00", turn["id"]),
            )

        recovered = self.server.ai_service.recover_stale_turns()

        self.assertEqual(1, recovered)
        recovered_turn = self.server.ai_service.database.turn(turn["id"])
        self.assertEqual("failed", recovered_turn["status"])
        self.assertIn("stale", recovered_turn["error_message"])

    def test_structured_log_keeps_trace_fields(self):
        entry = self.server.ai_service.log(
            "INFO",
            "test",
            "p0 trace",
            {"retryable": True},
            turn_id="b" * 32,
            request_id="request-1",
            command_id="command-1",
            device_id=DEVICE_ID,
            attempt=2,
            elapsed_ms=17,
        )

        self.assertEqual("request-1", entry["request_id"])
        self.assertEqual("command-1", entry["command_id"])
        self.assertEqual(2, entry["attempt"])
        self.assertEqual(17, entry["elapsed_ms"])

        logs = self.server.ai_service.database.logs(1)
        self.assertEqual("request-1", logs[0]["request_id"])
        self.assertEqual(DEVICE_ID, logs[0]["device_id"])

    def test_cleanup_tree_removes_expired_files_but_keeps_active_files(self):
        root = Path(self.temp_dir.name) / "artifacts"
        root.mkdir()
        expired = root / "old.tmp"
        active = root / "active.pcm"
        expired.write_bytes(b"old")
        active.write_bytes(b"active")
        old_time = time.time() - 3600
        os.utime(expired, (old_time, old_time))

        result = cleanup_tree(
            root,
            max_age_seconds=60,
            protected_names={active.name},
        )

        self.assertEqual(1, result["deleted_files"])
        self.assertFalse(expired.exists())
        self.assertTrue(active.exists())


if __name__ == "__main__":
    unittest.main()
