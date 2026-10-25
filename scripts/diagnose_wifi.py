#!/usr/bin/env python3
"""Read Wi-Fi recovery diagnostics; optionally compare before/after idle.

Sends only read-only command 0. During --idle-seconds no PC traffic is sent.
Defaults to DIAG2 (firmware 0.4.0 or later). Use --legacy for firmware 0.3.1.
"""
import argparse
import json
import secrets
import socket
import struct
import time

try:
    from .probe_pico import bounded_integer, positive_timeout
except ImportError:
    from probe_pico import bounded_integer, positive_timeout

RESPONSE = struct.Struct("<4sBBBBIIIIIIIIiiiIII")
RESPONSE_V2 = struct.Struct(RESPONSE.format + "IIII")


def decode(data, token):
    if len(data) not in (RESPONSE.size, RESPONSE_V2.size):
        return None
    values = RESPONSE.unpack(data[:RESPONSE.size])
    version = values[1]
    if (values[0] != b"PICD" or values[5] != token or
            (version, len(data)) not in ((1, RESPONSE.size), (2, RESPONSE_V2.size))):
        return None
    names = ("flags", "queue_depth", "queue_capacity", "token", "uptime_ms",
             "accepted", "rejected", "connect_attempts", "radio_resets",
             "last_rx_age_ms", "pm_value", "pm_result", "join_result",
             "link_status", "watchdog_boot", "missed_probes", "recovery_reason")
    result = dict(zip(names, values[2:]))
    result["diagnostic_version"] = version
    result["dualcore"] = bool(result["flags"] & 0x80)
    result["power_save_disabled"] = result["pm_result"] == 0 and (result["pm_value"] & 15) == 0
    if version == 2:
        extra = struct.unpack_from("<IIII", data, RESPONSE.size)
        result.update(zip(("network_heartbeat_age_ms", "epoch", "acknowledged_epoch",
                           "admission_open"), extra))
    return result


def query(host, port=8888, timeout=2, legacy=False):
    token = secrets.randbits(32)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.connect((host, port))
        deadline = time.monotonic() + timeout
        tag = b"DIAG" if legacy else b"DIAG2"
        sock.send(struct.pack("<BI", 0, token) + tag)
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                requirement = "firmware >= 0.3.1" if legacy else "firmware >= 0.4.0; use --legacy for 0.3.1"
                raise TimeoutError(f"No diagnostic reply (requires {requirement})")
            sock.settimeout(remaining)
            result = decode(sock.recv(65535), token)
            if result is not None:
                return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("host", nargs="?", default="192.168.11.7")
    parser.add_argument("--port", type=bounded_integer(1, 65535), default=8888)
    parser.add_argument("--timeout", type=positive_timeout, default=2)
    parser.add_argument("--idle-seconds", type=bounded_integer(1, 86400))
    parser.add_argument("--legacy", action="store_true", help="Use the 64-byte DIAG v1 query")
    args = parser.parse_args(argv)
    try:
        before = query(args.host, args.port, args.timeout, args.legacy)
        print(json.dumps({"before": before}, indent=2), flush=True)
        if args.idle_seconds:
            print(f"Idle for {args.idle_seconds}s; no PC probes or HID actions.", flush=True)
            time.sleep(args.idle_seconds)
            after = query(args.host, args.port, args.timeout, args.legacy)
            print(json.dumps({"after": after}, indent=2))
            if after["uptime_ms"] < before["uptime_ms"]:
                print("Uptime decreased: reboot or 32-bit uptime rollover detected.")
            else:
                print(f"Radio resets during idle: {after['radio_resets'] - before['radio_resets']}")
    except OSError as exc:
        print(f"Diagnostic query failed: {exc}")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
