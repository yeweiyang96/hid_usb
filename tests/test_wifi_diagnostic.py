import socket
import struct
import threading
import unittest

from scripts import diagnose_wifi


def reply(token, **overrides):
    fields = dict(magic=b"PICD", version=1, flags=0x37, depth=0, capacity=8,
                  token=token, uptime=30000, accepted=2, rejected=0,
                  connects=2, resets=1, age=7, pm=0x10, pm_result=0,
                  join_result=-5, link=3, watchdog=1, missed=0, reason=2)
    fields.update(overrides)
    return diagnose_wifi.RESPONSE.pack(*fields.values())


class DiagnosticTests(unittest.TestCase):
    def test_wire_fields_and_signed_errors(self):
        data = reply(123)
        self.assertEqual(len(data), 64)
        result = diagnose_wifi.decode(data, 123)
        self.assertEqual(result["connect_attempts"], 2)
        self.assertEqual(result["radio_resets"], 1)
        self.assertEqual(result["last_rx_age_ms"], 7)
        self.assertEqual(result["join_result"], -5)
        self.assertEqual(result["recovery_reason"], 2)
        self.assertEqual(result["watchdog_boot"], 1)
        self.assertTrue(result["power_save_disabled"])
        for fields in ({"pm": 0x12}, {"pm_result": -1}):
            self.assertFalse(diagnose_wifi.decode(reply(123, **fields), 123)["power_save_disabled"])

    def test_malformed_old_and_stale_responses(self):
        for packet in (b"", reply(123)[:-1], reply(123) + b"x", reply(123, magic=b"PICO"),
                       reply(123, version=2), reply(124)):
            self.assertIsNone(diagnose_wifi.decode(packet, 123))

    def test_loopback_query_is_read_only_and_ignores_stale(self):
        errors = []
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as server:
            server.bind(("127.0.0.1", 0))
            server.settimeout(2)

            def serve():
                try:
                    packet, peer = server.recvfrom(64)
                    cmd, token, tag = struct.unpack("<BI4s", packet)
                    self.assertEqual((cmd, tag), (0, b"DIAG"))
                    server.sendto(reply(token ^ 1), peer)
                    server.sendto(reply(token), peer)
                except Exception as exc:
                    errors.append(exc)

            thread = threading.Thread(target=serve, daemon=True)
            thread.start()
            result = diagnose_wifi.query("127.0.0.1", server.getsockname()[1], 1)
            thread.join(2)
            self.assertFalse(thread.is_alive())
            self.assertFalse(errors, errors)
            self.assertEqual(result["uptime_ms"], 30000)

    def test_timeout(self):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as server:
            server.bind(("127.0.0.1", 0))
            with self.assertRaises(TimeoutError):
                diagnose_wifi.query("127.0.0.1", server.getsockname()[1], .03)


if __name__ == "__main__":
    unittest.main()
