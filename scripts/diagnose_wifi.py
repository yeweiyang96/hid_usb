#!/usr/bin/env python3
"""Read Wi-Fi recovery diagnostics; optionally compare before/after idle.

Sends only read-only command 0. During --idle-seconds no PC traffic is sent.
Requires firmware 0.3.1 or later; older firmware may ignore this query.
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


def decode(data, token):
    if len(data) != 64:
        return None
    values = RESPONSE.unpack(data)
    if values[0] != b"PICD" or values[1] != 1 or values[5] != token:
        return None
    names = ("flags", "queue_depth", "queue_capacity", "token", "uptime_ms",
             "accepted", "rejected", "connect_attempts", "radio_resets",
             "last_rx_age_ms", "pm_value", "pm_result", "join_result",
             "link_status", "watchdog_boot", "missed_probes", "recovery_reason")
    result = dict(zip(names, values[2:]))
    result["power_save_disabled"] = result["pm_result"] == 0 and (result["pm_value"] & 15) == 0
    return result


def query(host, port=8888, timeout=2):
    token = secrets.randbits(32)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.connect((host, port))
        deadline = time.monotonic() + timeout
        sock.send(struct.pack("<BI4s", 0, token, b"DIAG"))
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("No diagnostic reply (requires firmware >= 0.3.1)")
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
    args = parser.parse_args(argv)
    try:
        before = query(args.host, args.port, args.timeout)
        print(json.dumps({"before": before}, indent=2), flush=True)
        if args.idle_seconds:
            print(f"Idle for {args.idle_seconds}s; no PC probes or HID actions.", flush=True)
            time.sleep(args.idle_seconds)
            after = query(args.host, args.port, args.timeout)
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
