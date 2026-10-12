"""No hardware actions: geometry/calibration regressions and mock UDP only."""

import math
import socket
import struct
import threading
import unittest

from scripts.mouse_test import PicoMouse, bounded_curvature, measured_gain, pixel_delta_to_counts


class MouseMathTests(unittest.TestCase):
    def test_reported_large_motion_requires_count_conversion(self):
        self.assertEqual(pixel_delta_to_counts(-2559, -1439, 0.625, 0.625), (-4094, -2302))
        dx, dy = pixel_delta_to_counts(-2559, -1439, 0.625, 0.625)
        self.assertLessEqual(abs(2559 + dx * 0.625), 0.5)
        self.assertLessEqual(abs(1439 + dy * 0.625), 0.5)
        self.assertGreater(abs(2559 - 2559 * 0.625), 900)

    def test_gain_uses_both_directions_and_rejects_clipping(self):
        self.assertAlmostEqual(measured_gain(320, 200, -200), 0.625)
        self.assertAlmostEqual(measured_gain(320, 199, -200), 399 / 640)
        for values in [(320, 200, -100), (320, 0, -200), (320, 200, 200), (0, 200, -200)]:
            with self.assertRaises(ValueError):
                measured_gain(*values)

    def test_invalid_gain_and_hid_range(self):
        for gain in (0, -1, math.nan, math.inf):
            with self.assertRaises(ValueError):
                pixel_delta_to_counts(100, 100, gain, 1)
        with self.assertRaises(ValueError):
            pixel_delta_to_counts(10000, 10000, 0.1, 0.1)

    def test_corner_curve_is_reduced_and_path_stays_inside_screen(self):
        start = (2559, 1439)
        counts = (-4094, -2302)
        gains = (0.625, 0.625)
        curve = bounded_curvature(start, counts, gains, -49.9, (0, 0, 2560, 1440))
        self.assertLess(abs(curve), 49.9)
        dx, dy = counts
        bend = curve / 200
        controls = [(dx / 3 - dy * bend, dy / 3 + dx * bend),
                    (2 * dx / 3 - dy * bend, 2 * dy / 3 + dx * bend)]
        for step in range(1001):
            t = step / 1000
            u = 1 - t
            x = 3 * u * u * t * controls[0][0] + 3 * u * t * t * controls[1][0] + t**3 * dx
            y = 3 * u * u * t * controls[0][1] + 3 * u * t * t * controls[1][1] + t**3 * dy
            # Endpoint conversion can differ by at most half a physical pixel.
            self.assertTrue(-0.5 <= start[0] + gains[0] * x <= 2559.5)
            self.assertTrue(-0.5 <= start[1] + gains[1] * y <= 1439.5)

    def test_safe_curve_is_preserved(self):
        self.assertEqual(bounded_curvature((1000, 700), (300, -100), (0.625, 0.625), 40,
                                          (0, 0, 2560, 1440)), 40)


class MockDevice:
    def __init__(self, *, reject=False, accept=True, extra_commands=0, query_drops=0):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.settimeout(0.1)
        self.port = self.sock.getsockname()[1]
        self.reject, self.accept, self.extra_commands = reject, accept, extra_commands
        self.query_drops = query_drops
        self.moves = []
        self.polls_after_move = 0
        self.stop = threading.Event()
        self.error = None
        self.thread = threading.Thread(target=self.serve)

    def __enter__(self):
        self.thread.start()
        return self

    def serve(self):
        try:
            while not self.stop.is_set():
                try:
                    packet, peer = self.sock.recvfrom(1024)
                except socket.timeout:
                    continue
                if packet[0] in (3, 4):
                    self.moves.append(packet)
                    continue
                command, token = struct.unpack("<BI", packet)
                assert command == 0
                if self.query_drops:
                    self.query_drops -= 1
                    continue
                accepted = 1 + self.extra_commands if self.moves and self.accept else 0
                rejected = int(self.reject and bool(self.moves))
                flags = 0x37
                if self.moves:
                    self.polls_after_move += 1
                    if self.polls_after_move < 3:
                        flags |= 8
                response = struct.pack("<4sBBBBIIII", b"PICO", 1, flags, 0, 8, token, 1234, accepted, rejected)
                self.sock.sendto(response, peer)
        except Exception as error:
            self.error = error

    def __exit__(self, *_):
        self.stop.set()
        self.thread.join(2)
        self.sock.close()
        if self.error:
            raise self.error


class CompletionTests(unittest.TestCase):
    def test_idle_query_retries_after_one_lost_read_only_response(self):
        with MockDevice(query_drops=1) as server:
            client = PicoMouse("127.0.0.1", server.port, timeout=1.5)
            try:
                self.assertTrue(client.wait_idle().mouse_ready)
                self.assertEqual(server.moves, [])
            finally:
                client.close()

    def test_waits_for_accepted_command_to_finish_without_replay(self):
        with MockDevice() as server:
            client = PicoMouse("127.0.0.1", server.port, timeout=1)
            try:
                client.move(100, -50)
                self.assertEqual(len(server.moves), 1)
                self.assertGreaterEqual(server.polls_after_move, 3)
            finally:
                client.close()

    def test_rejection_concurrent_commands_and_loss_stop_without_replay(self):
        for options, expected_error in [({"reject": True}, RuntimeError),
                                        ({"extra_commands": 1}, RuntimeError),
                                        ({"accept": False}, TimeoutError)]:
            with self.subTest(options=options), MockDevice(**options) as server:
                client = PicoMouse("127.0.0.1", server.port, timeout=0.1)
                try:
                    with self.assertRaises(expected_error):
                        client.move(100, -50)
                    self.assertEqual(len(server.moves), 1)
                finally:
                    client.close()


if __name__ == "__main__":
    unittest.main()
