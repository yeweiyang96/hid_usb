#!/usr/bin/env python3
"""Check the Pico UDP service without sending keyboard or mouse actions.

Protocol v1 uses little-endian integers. A request is command 0 followed by a
random 32-bit token; a response echoes that token along with device status.
"""

from __future__ import annotations

import argparse
from dataclasses import asdict, dataclass
import json
import math
import secrets
import socket
import statistics
import struct
import sys
import time
from typing import Sequence


REQUEST = struct.Struct("<BI")
RESPONSE = struct.Struct("<4sBBBBIIII")
MAGIC = b"PICO"
VERSION = 1
COMMAND_PROBE = 0


@dataclass(frozen=True)
class DeviceStatus:
    flags: int
    queue_depth: int
    queue_capacity: int
    token: int
    uptime_ms: int
    accepted_commands: int
    rejected_commands: int

    @property
    def usb_mounted(self) -> bool:
        return bool(self.flags & 0x01)

    @property
    def keyboard_ready(self) -> bool:
        return bool(self.flags & 0x02)

    @property
    def mouse_ready(self) -> bool:
        return bool(self.flags & 0x04)

    @property
    def busy(self) -> bool:
        return bool(self.flags & 0x08)

    @property
    def wifi_up(self) -> bool:
        return bool(self.flags & 0x10)

    def to_dict(self) -> dict:
        result = asdict(self)
        for name in ("usb_mounted", "keyboard_ready", "mouse_ready", "busy", "wifi_up"):
            result[name] = getattr(self, name)
        return result


@dataclass(frozen=True)
class Sample:
    number: int
    rtt_ms: float | None
    status: DeviceStatus | None
    error: str | None = None


@dataclass(frozen=True)
class ProbeResult:
    host: str
    port: int
    requested: int
    sent: int
    samples: list[Sample]
    error: str | None = None

    @property
    def received(self) -> int:
        return sum(sample.status is not None for sample in self.samples)

    @property
    def network_reachable(self) -> bool:
        return self.received > 0

    @property
    def loss_percent(self) -> float:
        return 100.0 * (self.sent - self.received) / self.sent if self.sent else 100.0

    @property
    def last_status(self) -> DeviceStatus | None:
        return next((sample.status for sample in reversed(self.samples) if sample.status), None)

    @property
    def rtt_ms(self) -> dict[str, float] | None:
        timings = [sample.rtt_ms for sample in self.samples if sample.rtt_ms is not None]
        if not timings:
            return None
        return {"min": min(timings), "avg": statistics.fmean(timings), "max": max(timings)}

    def to_dict(self) -> dict:
        return {
            "host": self.host,
            "port": self.port,
            "requested": self.requested,
            "sent": self.sent,
            "received": self.received,
            "network_reachable": self.network_reachable,
            "loss_percent": self.loss_percent,
            "rtt_ms": self.rtt_ms,
            "status": self.last_status.to_dict() if self.last_status else None,
            "error": self.error,
            "samples": [
                {
                    "number": sample.number,
                    "rtt_ms": sample.rtt_ms,
                    "status": sample.status.to_dict() if sample.status else None,
                    "error": sample.error,
                }
                for sample in self.samples
            ],
        }


def decode_response(data: bytes, expected_token: int) -> DeviceStatus | None:
    """Ignore other protocol versions, malformed packets and stale replies."""
    if len(data) != RESPONSE.size:
        return None
    magic, version, flags, depth, capacity, token, uptime, accepted, rejected = RESPONSE.unpack(data)
    if magic != MAGIC or version != VERSION or token != expected_token:
        return None
    return DeviceStatus(flags, depth, capacity, token, uptime, accepted, rejected)


def probe(host: str, port: int = 8888, timeout: float = 1.0, count: int = 5) -> ProbeResult:
    """Send only command 0, accepting replies exclusively from the target peer.

    The receive deadline is fixed for each sample, so unrelated or malformed
    packets cannot extend the requested timeout.
    """
    if not math.isfinite(timeout) or timeout <= 0:
        raise ValueError("timeout must be a finite positive number")
    if not 1 <= count <= 1000:
        raise ValueError("count must be between 1 and 1000")
    if not 1 <= port <= 65535:
        raise ValueError("port must be between 1 and 65535")

    samples: list[Sample] = []
    sent = 0
    try:
        # Pico's Wi-Fi stack uses IPv4. Connecting UDP also filters source
        # address/port in the OS before responses reach the decoder.
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.settimeout(timeout)
            sock.connect((host, port))
            for number in range(1, count + 1):
                token = secrets.randbits(32)
                started = time.monotonic()
                deadline = started + timeout
                try:
                    sock.send(REQUEST.pack(COMMAND_PROBE, token))
                    sent += 1
                    while True:
                        remaining = deadline - time.monotonic()
                        if remaining <= 0:
                            raise socket.timeout()
                        sock.settimeout(remaining)
                        data = sock.recv(65535)
                        status = decode_response(data, token)
                        if status is not None:
                            samples.append(Sample(number, (time.monotonic() - started) * 1000, status))
                            break
                except socket.timeout:
                    samples.append(Sample(number, None, None, "timeout"))
                except OSError as exc:
                    samples.append(Sample(number, None, None, str(exc)))
    except OSError as exc:
        return ProbeResult(host, port, count, sent, samples, str(exc))
    return ProbeResult(host, port, count, sent, samples)


def positive_timeout(value: str) -> float:
    try:
        result = float(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("must be a finite positive number") from exc
    if not math.isfinite(result) or result <= 0:
        raise argparse.ArgumentTypeError("must be a finite positive number")
    return result


def bounded_integer(low: int, high: int):
    def parse(value: str) -> int:
        try:
            result = int(value)
        except ValueError as exc:
            raise argparse.ArgumentTypeError(f"must be an integer between {low} and {high}") from exc
        if not low <= result <= high:
            raise argparse.ArgumentTypeError(f"must be between {low} and {high}")
        return result
    return parse


def format_result(result: ProbeResult) -> str:
    yes_no = lambda value: "yes" if value else "no"
    lines = [
        f"Pico UDP probe: {result.host}:{result.port}",
        f"Network reachable: {yes_no(result.network_reachable)}",
        f"Replies: {result.received}/{result.sent} sent ({result.requested} requested); loss: {result.loss_percent:.1f}%",
    ]
    if result.rtt_ms:
        timings = result.rtt_ms
        lines.append(f"RTT min/avg/max: {timings['min']:.3f}/{timings['avg']:.3f}/{timings['max']:.3f} ms")
    status = result.last_status
    if status:
        lines.extend([
            f"USB mounted: {yes_no(status.usb_mounted)}",
            f"Keyboard ready: {yes_no(status.keyboard_ready)}; mouse ready: {yes_no(status.mouse_ready)}",
            f"Wi-Fi up: {yes_no(status.wifi_up)}; busy: {yes_no(status.busy)}; queue: {status.queue_depth}/{status.queue_capacity}",
            f"Uptime: {status.uptime_ms} ms; accepted commands: {status.accepted_commands}; rejected commands: {status.rejected_commands}",
        ])
        if not status.usb_mounted:
            lines.append("UDP service is reachable; USB is not mounted on a host.")
    else:
        lines.append("USB status: unknown (no valid probe reply)")
    if result.error:
        lines.append(f"Network error: {result.error}")
    errors = [sample.error for sample in result.samples if sample.error and sample.error != "timeout"]
    if errors:
        lines.append(f"Last sample error: {errors[-1]}")
    return "\n".join(lines)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("host", help="Pico IPv4 address or hostname")
    parser.add_argument("--port", type=bounded_integer(1, 65535), default=8888)
    parser.add_argument("--timeout", type=positive_timeout, default=1.0, metavar="SECONDS")
    parser.add_argument("--count", type=bounded_integer(1, 1000), default=5)
    parser.add_argument("--json", action="store_true", help="write machine-readable JSON")
    args = parser.parse_args(argv)
    result = probe(args.host, args.port, args.timeout, args.count)
    print(json.dumps(result.to_dict(), ensure_ascii=False) if args.json else format_result(result))
    return 0 if result.network_reachable else 1


if __name__ == "__main__":
    sys.exit(main())
