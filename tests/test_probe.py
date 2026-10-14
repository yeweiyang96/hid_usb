"""Loopback-only tests: no Pico, network access or HID actions are needed."""

from contextlib import redirect_stderr, redirect_stdout
import importlib.util
import io
import json
from pathlib import Path
import socket
import struct
import subprocess
import sys
import threading
import time
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "probe_pico.py"
SPEC = importlib.util.spec_from_file_location("probe_pico", SCRIPT)
probe_pico = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = probe_pico
SPEC.loader.exec_module(probe_pico)


def response(token, *, magic=b"PICO", version=1, flags=0x17, depth=2, capacity=16):
    return struct.pack("<4sBBBBIIII", magic, version, flags, depth, capacity, token, 1234, 56, 7)


class MockPico:
    def __init__(self, handler, requests=1):
        self.handler = handler
        self.requests = requests
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.settimeout(2)
        self.port = self.sock.getsockname()[1]
        self.received = []
        self.error = None
        self.thread = threading.Thread(target=self.serve, daemon=True)

    def serve(self):
        try:
            for index in range(self.requests):
                data, peer = self.sock.recvfrom(65535)
                self.received.append(data)
                command, token = struct.unpack("<BI", data)
                if command != 0:
                    raise AssertionError("probe must never emit an HID command")
                self.handler(self.sock, peer, token, index)
        except Exception as exc:
            self.error = exc

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *_):
        self.thread.join(3)
        self.sock.close()
        if self.thread.is_alive():
            raise AssertionError("mock server did not stop")
        if self.error:
            raise self.error


class ProbeTests(unittest.TestCase):
    def test_valid_reply_and_status(self):
        with MockPico(lambda sock, peer, token, _: sock.sendto(response(token), peer), requests=3) as pico:
            result = probe_pico.probe("127.0.0.1", pico.port, 0.2, 3)
        self.assertTrue(result.network_reachable)
        self.assertEqual((result.sent, result.received, result.loss_percent), (3, 3, 0))
        self.assertEqual([len(data) for data in pico.received], [5, 5, 5])
        status = result.last_status
        self.assertTrue(status.usb_mounted)
        self.assertTrue(status.keyboard_ready)
        self.assertTrue(status.mouse_ready)
        self.assertTrue(status.wifi_up)
        self.assertFalse(status.busy)
        self.assertEqual((status.queue_depth, status.queue_capacity, status.uptime_ms), (2, 16, 1234))
        self.assertEqual((status.accepted_commands, status.rejected_commands), (56, 7))
        self.assertLessEqual(result.rtt_ms["min"], result.rtt_ms["avg"])
        self.assertLessEqual(result.rtt_ms["avg"], result.rtt_ms["max"])

    def test_ignore_malformed_stale_and_unsupported_replies(self):
        def handler(sock, peer, token, _):
            for packet in (
                b"PICO",
                response(token) + b"extra",
                response(token ^ 0xFFFFFFFF),
                response(token, magic=b"NOPE"),
                response(token, version=3),
                response(token),
            ):
                sock.sendto(packet, peer)
        with MockPico(handler) as pico:
            result = probe_pico.probe("127.0.0.1", pico.port, 0.2, 1)
        self.assertEqual(result.received, 1)

    def test_peer_source_is_filtered(self):
        def handler(sock, peer, token, _):
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as unrelated:
                unrelated.sendto(response(token, flags=0), peer)
            sock.sendto(response(token, flags=0x17), peer)
        with MockPico(handler) as pico:
            result = probe_pico.probe("127.0.0.1", pico.port, 0.2, 1)
        self.assertTrue(result.last_status.usb_mounted)

    def test_timeout_and_partial_loss(self):
        def handler(sock, peer, token, index):
            if index == 0:
                sock.sendto(response(token), peer)
        with MockPico(handler, requests=2) as pico:
            result = probe_pico.probe("127.0.0.1", pico.port, 0.04, 2)
        self.assertEqual((result.sent, result.received, result.loss_percent), (2, 1, 50))
        self.assertEqual(result.samples[-1].error, "timeout")

    def test_bad_reply_alone_times_out(self):
        with MockPico(lambda sock, peer, token, _: sock.sendto(response(token, version=3), peer)) as pico:
            result = probe_pico.probe("127.0.0.1", pico.port, 0.04, 1)
        self.assertFalse(result.network_reachable)
        self.assertIsNone(result.last_status)
        self.assertIsNone(result.rtt_ms)
        self.assertEqual(result.loss_percent, 100)

    def test_current_v2_and_legacy_v1(self):
        for version in (1, 2):
            with self.subTest(version=version):
                with MockPico(lambda sock, peer, token, _: sock.sendto(response(token, version=version), peer)) as pico:
                    result = probe_pico.probe("127.0.0.1", pico.port, 0.2, 1)
                self.assertTrue(result.network_reachable)

    def test_invalid_traffic_cannot_extend_deadline(self):
        def handler(sock, peer, token, _):
            for _ in range(20):
                sock.sendto(response(token ^ 1), peer)
                time.sleep(0.005)
        with MockPico(handler) as pico:
            started = time.monotonic()
            result = probe_pico.probe("127.0.0.1", pico.port, 0.04, 1)
            elapsed = time.monotonic() - started
        self.assertFalse(result.network_reachable)
        self.assertLess(elapsed, 0.09)

    def test_cli_json_reachable_even_without_usb(self):
        with MockPico(lambda sock, peer, token, _: sock.sendto(response(token, flags=0x10), peer)) as pico:
            process = subprocess.run(
                [sys.executable, str(SCRIPT), "127.0.0.1", "--port", str(pico.port), "--count", "1", "--timeout", "0.2", "--json"],
                capture_output=True, text=True, timeout=3,
            )
        self.assertEqual(process.returncode, 0, process.stderr)
        payload = json.loads(process.stdout)
        self.assertTrue(payload["network_reachable"])
        self.assertFalse(payload["status"]["usb_mounted"])
        self.assertFalse(payload["status"]["keyboard_ready"])
        self.assertFalse(payload["status"]["mouse_ready"])
        self.assertEqual(payload["received"], 1)

    def test_cli_text_distinguishes_network_from_usb(self):
        with MockPico(lambda sock, peer, token, _: sock.sendto(response(token, flags=0x10), peer)) as pico:
            output = io.StringIO()
            with redirect_stdout(output):
                code = probe_pico.main(["127.0.0.1", "--port", str(pico.port), "--count", "1", "--timeout", "0.2"])
        self.assertEqual(code, 0)
        self.assertIn("Network reachable: yes", output.getvalue())
        self.assertIn("USB mounted: no", output.getvalue())

    def test_cli_no_reply_returns_one(self):
        with MockPico(lambda *_: None) as pico:
            output = io.StringIO()
            with redirect_stdout(output):
                code = probe_pico.main(["127.0.0.1", "--port", str(pico.port), "--count", "1", "--timeout", "0.04", "--json"])
        self.assertEqual(code, 1)
        payload = json.loads(output.getvalue())
        self.assertFalse(payload["network_reachable"])
        self.assertIsNone(payload["status"])

    def test_cli_invalid_arguments_return_two(self):
        invalid = [
            [],
            ["127.0.0.1", "--timeout", "0"],
            ["127.0.0.1", "--timeout", "-1"],
            ["127.0.0.1", "--timeout", "nan"],
            ["127.0.0.1", "--timeout", "inf"],
            ["127.0.0.1", "--timeout", "nope"],
            ["127.0.0.1", "--count", "0"],
            ["127.0.0.1", "--count", "1001"],
            ["127.0.0.1", "--count", "1.5"],
            ["127.0.0.1", "--port", "0"],
            ["127.0.0.1", "--port", "65536"],
        ]
        for argv in invalid:
            with self.subTest(argv=argv), redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as raised:
                    probe_pico.main(argv)
                self.assertEqual(raised.exception.code, 2)

    def test_library_rejects_invalid_settings(self):
        for kwargs in ({"timeout": float("nan")}, {"timeout": 0}, {"count": 0}, {"count": 1001}, {"port": 0}):
            with self.subTest(kwargs=kwargs), self.assertRaises(ValueError):
                probe_pico.probe("127.0.0.1", **kwargs)


if __name__ == "__main__":
    unittest.main()
