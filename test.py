import socket
import struct
import time
import random
import math
import ctypes
from ctypes import windll, Structure, c_long, byref

# ================= 配置区域 =================
PICO_IP = "192.168.11.7"  # 请修改为你 Pico 连接 WiFi 后分配的 IP
PICO_PORT = 8888           # 必须与 main.c 中的端口一致
# ===========================================

# 定义 Windows 坐标结构体
class POINT(Structure):
    _fields_ = [("x", c_long), ("y", c_long)]

def enable_dpi_awareness():
    """
    强制开启高 DPI 感知。
    这是核心步骤！否则在 125%/150% 缩放的屏幕上，Python 获取的坐标
    是“逻辑坐标”，而 Pico 移动的是“物理像素”，会导致移动距离偏短。
    """
    try:
        # Windows 10 Creators Update (1703) 及更高版本
        windll.shcore.SetProcessDpiAwareness(1) # 1 = PROCESS_SYSTEM_DPI_AWARE
        print("[系统] DPI 感知模式已开启 (物理像素 1:1 映射)")
    except Exception:
        # 旧版 Windows Fallback
        windll.user32.SetProcessDPIAware()
        print("[系统] 旧版 DPI 感知已开启")

def get_mouse_pos():
    """通过底层 API 获取当前鼠标的绝对物理坐标"""
    pt = POINT()
    windll.user32.GetCursorPos(byref(pt))
    return pt.x, pt.y

def send_human_move(sock, dx, dy):
    """
    封装 UDP 协议并发送
    协议结构: <Bhhff (Cmd, dx, dy, speed, curve)
    """
    # ---------------------------------------------------------
    # 拟人化参数生成 (调整这里的参数来观察 Pico 的不同表现)
    # ---------------------------------------------------------
    
    # 1. 速度 (Speed): 0.0 (最慢) ~ 100.0 (最快)
    # 模拟：从 65% 到 85% 的随机速度，模拟轻松但专注的操作
    speed_pct = random.uniform(65.0, 85.0) 

    # 2. 曲率 (Curvature): -100.0 (左/上凸) ~ 100.0 (右/下凸)
    # 模拟：人类很少走直线，通常会有 +/- 20% 到 40% 的弧度
    # 我们随机生成一个正负值
    curve_pct = random.uniform(20.0, 50.0)
    if random.choice([True, False]):
        curve_pct *= -1  # 随机决定是向左弯还是向右弯

    print(f"[指令] 目标位移: ({dx}, {dy}) | 速度: {speed_pct:.1f}% | 曲率: {curve_pct:.1f}%")
    
    # 3. 打包数据 (CMD_MOVE_HUMAN = 4)
    # 格式对应 C 代码: int8 cmd, int16 dx, int16 dy, float speed, float curve
    packet = struct.pack("<Bhhff", 4, int(dx), int(dy), float(speed_pct), float(curve_pct))
    
    sock.sendto(packet, (PICO_IP, PICO_PORT))
    
    # 估算物理移动所需的时间 (用于脚本等待验证)
    # 这是一个粗略估算，距离越长等待越久，留给 Pico 执行时间
    distance = math.sqrt(dx*dx + dy*dy)
    estimated_time = (distance / 500.0) + 0.5 
    time.sleep(estimated_time)

def main():
    enable_dpi_awareness()
    
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    
    print("\n====== Pico 物理鼠标拟人移动测试 ======")
    print("注意：测试前请务必在 Windows 设置中关闭【提高指针精确度】！\n")

    while True:
        try:
            input(">> 第一步：请将鼠标移动到你想去的【目标位置】，然后按回车记录...")
            target_x, target_y = get_mouse_pos()
            print(f"   [记录] 目标终点: ({target_x}, {target_y})\n")

            input(">> 第二步：请将鼠标移开到任意【起始位置】，准备测试移动，按回车开始...")
            start_x, start_y = get_mouse_pos()
            print(f"   [记录] 起始位置: ({start_x}, {start_y})")

            # 计算物理差值 (Delta)
            dx = target_x - start_x
            dy = target_y - start_y
            
            # 发送指令
            print("   [执行] 发送 Pico 指令中...")
            start_time = time.time()
            send_human_move(sock, dx, dy)
            
            # 验证结果
            final_x, final_y = get_mouse_pos()
            
            # 计算误差
            err_x = final_x - target_x
            err_y = final_y - target_y
            total_err = math.sqrt(err_x**2 + err_y**2)

            print(f"\n   [结果验证]")
            print(f"   期望终点: ({target_x}, {target_y})")
            print(f"   实际终点: ({final_x}, {final_y})")
            print(f"   过程耗时: {time.time() - start_time:.2f}秒")
            
            if total_err == 0:
                print("   ✅ 完美！0 像素误差！")
            elif total_err < 5:
                print(f"   ⚠️ 极小误差 ({int(total_err)} 像素)，属于 Windows 丢包正常范围。")
            else:
                print(f"   ❌ 偏差较大 ({int(total_err)} 像素)。请检查是否关闭了鼠标加速或 DPI 设置不匹配。")
            
            print("-" * 50)
            
        except KeyboardInterrupt:
            print("\n测试结束。")
            break
        except Exception as e:
            print(f"\n发生错误: {e}")
            break

if __name__ == "__main__":
    main()
