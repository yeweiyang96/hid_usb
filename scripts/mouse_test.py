"""Interactive physical-cursor test with measured pixels/HID-count calibration.

No mouse settings are changed. Commands are sent once, then only health queries
are repeated. --settings-only never submits a mouse or keyboard action.
"""

from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
import math
import random
import secrets
import socket
import struct
import sys
import time

if __package__:
    from .probe_pico import REQUEST, decode_response
else:
    from probe_pico import REQUEST, decode_response


def pixel_delta_to_counts(dx, dy, gain_x, gain_y):
    if any(not math.isfinite(g) or g <= 0 for g in (gain_x, gain_y)):
        raise ValueError("像素/HID 单位比例必须是有限正数")
    counts = round(dx / gain_x), round(dy / gain_y)
    if any(not -32768 <= value <= 32767 for value in counts):
        raise ValueError("换算后的 HID 位移超出 int16 范围")
    return counts


def measured_gain(counts, forward_pixels, reverse_pixels):
    if counts <= 0 or forward_pixels <= 0 or reverse_pixels >= 0:
        raise ValueError("标定位移方向异常：请不要在标定中移动实体鼠标")
    forward = forward_pixels / counts
    reverse = -reverse_pixels / counts
    gain = (forward + reverse) / 2
    if abs(forward - reverse) > max(0.03, gain * 0.1):
        raise ValueError("正反向比例差异过大：可能碰到屏幕边界或受到其他鼠标输入影响")
    return gain


def bounded_curvature(start, counts, gains, curvature, bounds):
    """Keep Bezier control points inside the monitor's rectangular boundary.

    A cubic Bezier lies inside its control-point convex hull. Reducing curvature
    avoids discarding motion at a screen edge even when its endpoints are valid.
    """
    dx, dy = counts
    gx, gy = gains
    left, top, right, bottom = bounds
    for _ in range(20):
        bend = curvature / 200
        controls = [
            (start[0] + gx * (dx * fraction - dy * bend),
             start[1] + gy * (dy * fraction + dx * bend))
            for fraction in (1 / 3, 2 / 3)
        ]
        if all(left <= x <= right - 1 and top <= y <= bottom - 1 for x, y in controls):
            return curvature
        curvature /= 2
    return 0.0


class PicoMouse:
    def __init__(self, host, port=8888, timeout=15):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.connect((host, port))
        self.timeout = timeout

    def close(self):
        self.sock.close()

    def status(self, timeout=1):
        token = secrets.randbits(32)
        deadline = time.monotonic() + timeout
        self.sock.send(REQUEST.pack(0, token))
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("Pico 查询超时")
            self.sock.settimeout(remaining)
            data = self.sock.recv(65535)
            status = decode_response(data, token)
            if status is not None:
                return status

    def wait_idle(self):
        deadline = time.monotonic() + self.timeout
        while time.monotonic() < deadline:
            try:
                status = self.status(min(1, deadline - time.monotonic()))
            except TimeoutError:
                continue
            if not status.usb_mounted or not status.wifi_up:
                raise RuntimeError("Pico 的 Wi-Fi 或 USB 不可用")
            if not status.busy and status.queue_depth == 0 and status.mouse_ready:
                return status
            time.sleep(0.01)
        raise TimeoutError("Pico 长时间忙，请停止其他控制客户端")

    def move(self, dx, dy, speed=75, curvature=0, direct=False):
        if not all(-32768 <= value <= 32767 for value in (dx, dy)):
            raise ValueError("HID 位移超出 int16 范围")
        before = self.wait_idle()
        packet = struct.pack("<Bhh", 3, dx, dy) if direct else struct.pack("<Bhhff", 4, dx, dy, speed, curvature)
        started = time.monotonic()
        deadline = started + self.timeout
        self.sock.send(packet)  # Exactly once: a timed-out action is never replayed.
        while time.monotonic() < deadline:
            try:
                status = self.status(min(1, deadline - time.monotonic()))
            except TimeoutError:
                continue  # Retrying a read-only query is safe.
            if status.uptime_ms < before.uptime_ms:
                raise RuntimeError("Pico 在动作期间重启，停止测试")
            if status.rejected_commands != before.rejected_commands:
                raise RuntimeError("Pico 拒绝或中止了命令；未重发动作")
            received = (status.accepted_commands - before.accepted_commands) & 0xffffffff
            if received > 1:
                raise RuntimeError("发现其他客户端同时发送动作，本次结果无效")
            if not status.usb_mounted or not status.wifi_up:
                raise RuntimeError("动作期间 USB 或 Wi-Fi 断开；未重发动作")
            if received == 1 and not status.busy and status.queue_depth == 0 and status.mouse_ready:
                # Allow the desktop cursor update to follow the USB completion.
                time.sleep(0.03)
                return time.monotonic() - started
            time.sleep(0.01)
        raise TimeoutError("未确认 Pico 接收并完成动作；未重发，请先查询设备状态")


class WindowsMouse:
    def __init__(self):
        if sys.platform != "win32":
            raise RuntimeError("此交互测试需要在 Pico USB 接入的 Windows 电脑上运行")
        self.user32 = ctypes.WinDLL("user32", use_last_error=True)
        self.user32.SystemParametersInfoW.argtypes = (wintypes.UINT, wintypes.UINT, wintypes.LPVOID, wintypes.UINT)
        self.user32.GetPhysicalCursorPos.argtypes = (ctypes.POINTER(wintypes.POINT),)
        self.user32.MonitorFromPoint.argtypes = (wintypes.POINT, wintypes.DWORD)
        self.user32.MonitorFromPoint.restype = wintypes.HANDLE
        self.user32.GetMonitorInfoW.argtypes = (wintypes.HANDLE, wintypes.LPVOID)
        # Check HRESULT instead of treating any non-exception as success.
        shcore = ctypes.WinDLL("shcore", use_last_error=True)
        result = shcore.SetProcessDpiAwareness(2)
        if result not in (0, -2147024891):  # S_OK or already configured.
            raise OSError(f"无法设置 DPI 感知模式：0x{result & 0xffffffff:08x}")

    def position(self):
        point = wintypes.POINT()
        if not self.user32.GetPhysicalCursorPos(ctypes.byref(point)):
            raise ctypes.WinError(ctypes.get_last_error())
        return point.x, point.y

    def bounds(self, point):
        class MonitorInfo(ctypes.Structure):
            _fields_ = [("size", wintypes.DWORD), ("monitor", wintypes.RECT),
                        ("work", wintypes.RECT), ("flags", wintypes.DWORD)]
        handle = self.user32.MonitorFromPoint(wintypes.POINT(*point), 2)
        info = MonitorInfo()
        info.size = ctypes.sizeof(info)
        if not self.user32.GetMonitorInfoW(handle, ctypes.byref(info)):
            raise ctypes.WinError(ctypes.get_last_error())
        rect = info.monitor
        return rect.left, rect.top, rect.right, rect.bottom

    def settings(self):
        speed = ctypes.c_int()
        acceleration = (ctypes.c_int * 3)()
        if not self.user32.SystemParametersInfoW(0x70, 0, ctypes.byref(speed), 0):
            raise ctypes.WinError(ctypes.get_last_error())
        if not self.user32.SystemParametersInfoW(3, 0, acceleration, 0):
            raise ctypes.WinError(ctypes.get_last_error())
        return speed.value, acceleration[2], self.user32.GetDpiForSystem()


def calibrate(device, mouse, direct=False):
    input("\n请将鼠标移到待测屏幕中央，停止其他控制程序，按回车开始两轴往返标定…")
    initial = mouse.position()
    bounds = mouse.bounds(initial)
    left, top, right, bottom = bounds
    margins = (min(initial[0] - left, right - 1 - initial[0]),
               min(initial[1] - top, bottom - 1 - initial[1]))
    if min(margins) < 128:
        raise ValueError("标定起点距屏幕边缘太近，请在屏幕中央重新启动测试")
    gains = []
    for axis in (0, 1):
        counts = min(512, int(margins[axis] / 4))
        readings = [mouse.position()]
        for sign in (1, -1):
            delta = [0, 0]
            delta[axis] = counts * sign
            device.move(*delta, speed=75, curvature=0, direct=direct)
            current = mouse.position()
            if not (left + 2 < current[0] < right - 3 and top + 2 < current[1] < bottom - 3):
                raise ValueError("标定中碰到屏幕边界，本次样本无效")
            readings.append(current)
        forward = readings[1][axis] - readings[0][axis]
        reverse = readings[2][axis] - readings[1][axis]
        orthogonal = 1 - axis
        if any(abs(p[orthogonal] - readings[0][orthogonal]) > 2 for p in readings[1:]):
            raise ValueError("标定中出现非预期方向的移动，请不要操作实体鼠标")
        gain = measured_gain(counts, forward, reverse)
        gains.append(gain)
        print(f"[标定 {'XY'[axis]}] +{counts}/-{counts} HID 单位 → {forward}/{reverse} 像素，比例 {gain:.6f} 像素/单位")
    return tuple(gains), bounds


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="192.168.11.7")
    parser.add_argument("--port", type=int, default=8888)
    parser.add_argument("--direct", action="store_true", help="测试直接移动，默认测试拟人曲线")
    parser.add_argument("--settings-only", action="store_true", help="只读系统和 Pico 状态，不移动")
    args = parser.parse_args(argv)
    device = None
    try:
        mouse = WindowsMouse()
        settings = mouse.settings()
        speed, acceleration, dpi = settings
        print(f"[Windows] 指针速度 {speed}/20（默认10），加速度 {acceleration}，系统 DPI {dpi}（{dpi / 96:.0%}）")
        print("HID 相对位移单位不等于屏幕像素；本脚本不修改 Windows 鼠标设置。")
        device = PicoMouse(args.host, args.port)
        status = device.wait_idle()
        print(f"[Pico] UDP 可达、USB 就绪、静态 IP {'是' if status.static_ip else '未报告'}")
        if args.settings_only:
            return 0
        if acceleration:
            raise ValueError("请先关闭提高指针精确度；加速度开启时不能用固定比例换算")
        print("\n====== Pico 屏幕位移标定与移动测试 ======")
        print("标定会移动鼠标四次，不会点击或按键；过程中不要移动实体鼠标。")
        gains, bounds = calibrate(device, mouse, args.direct)
        while True:
            if mouse.settings() != settings:
                raise ValueError("系统鼠标速度或 DPI 设置已变化，请重新标定")
            input("\n>> 将鼠标移动到目标位置，按回车记录…")
            target = mouse.position()
            print(f"   目标: {target}")
            input(">> 将鼠标移到起始位置，按回车开始测试…")
            start = mouse.position()
            if mouse.settings() != settings:
                raise ValueError("系统鼠标设置已变化，请重新标定")
            if mouse.bounds(start) != bounds or mouse.bounds(target) != bounds:
                raise ValueError("起点和终点必须位于标定时的同一块屏幕")
            delta = target[0] - start[0], target[1] - start[1]
            counts = pixel_delta_to_counts(*delta, *gains)
            speed = random.uniform(65, 85)
            requested_curve = random.choice((-1, 1)) * random.uniform(20, 50)
            curve = bounded_curvature(start, counts, gains, requested_curve, bounds)
            print(f"   起点: {start}；屏幕位移: {delta}；换算 HID 位移: {counts}")
            if not args.direct:
                print(f"   速度: {speed:.1f}%；曲率: {curve:.1f}%（原请求 {requested_curve:.1f}%，必要时缩小以避开边界）")
            elapsed = device.move(*counts, speed=speed, curvature=curve, direct=args.direct)
            final = mouse.position()
            error = math.hypot(final[0] - target[0], final[1] - target[1])
            print(f"   Pico 已接收、队列空闲、鼠标 USB 报告完成；等待耗时: {elapsed:.3f} 秒")
            print(f"   期望终点: {target}；实际终点: {final}；误差: {error:.2f} 像素")
            if error > 5:
                print("   偏差仍大：此结果不能直接归因于 DPI 或丢包；请保存标定比例和本次记录继续核对。")
    except (EOFError, KeyboardInterrupt):
        print("\n测试结束。")
        return 0
    except (OSError, RuntimeError, ValueError, TimeoutError) as exc:
        print(f"\n测试停止：{exc}", file=sys.stderr)
        return 1
    finally:
        if device is not None:
            device.close()


if __name__ == "__main__":
    raise SystemExit(main())
