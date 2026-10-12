# Pico 2 W UDP HID 固件

Pico SDK 2.3.1 + TinyUSB，UDP 默认端口 `8888`。键盘和鼠标使用两个独立的 USB HID 接口，USB 轮询间隔为 1 ms。Wi-Fi 关闭省电模式；这会增加功耗。1 ms 是 USB 轮询间隔，实际端到端延迟需要在板上测量。

## 配置、构建和部署

在本目录执行：

```powershell
Copy-Item wifi_config.h.example wifi_config.h
# 编辑 wifi_config.h，填写 WIFI_SSID / WIFI_PASSWORD；也可修改 UDP_PORT。
./scripts/build.ps1
```

脚本使用本机 Pico VS Code 扩展安装的 SDK 2.3.1、GCC 15_2_Rel1、CMake v4.3.4 和 Ninja v1.13.2，输出 `build-verified/hid_usb.uf2`。`wifi_config.h` 已加入忽略规则。修改配置后重新构建，将 UF2 复制到处于 BOOTSEL 模式的 Pico 2 W，再正常启动。

构建脚本检查 Wi-Fi 名称和密码均已填写，并拒绝 `CHANGE_ME` 占位配置。直接使用 CMake 且缺少配置文件时，仍可完成无凭据的编译验证；这种占位固件无法连接实际 Wi-Fi。

本次可烧录固件已使用本地 `wifi_config.h` 编译，文件为 `release/hid_usb-pico2w-v0.2.1-static-ip.uf2`，同目录提供 SHA256 校验文件。按住 BOOTSEL 接入 USB，将此 UF2 复制到出现的 Pico 磁盘，复制完成后设备会重启。

沿用旧固件的静态网络配置：IP `192.168.11.7`、子网掩码 `255.255.255.0`、网关 `192.168.11.1`、DNS `8.8.8.8`。lwIP DHCP 客户端已关闭，启动与 Wi-Fi 重新连接均由 CYW43 驱动应用这组地址，不向路由器申请地址。配置项位于 CMake 的 `PICO_STATIC_IP`、`PICO_STATIC_NETMASK`、`PICO_STATIC_GATEWAY`、`PICO_STATIC_DNS` 缓存变量。

USB 初始化独立于 Wi-Fi 连接；Wi-Fi 失败不会让 USB 主循环退出。连接在后台状态机中推进，断线后尝试重新连接。硬件初始化或 UDP 服务创建失败时，USB 仍运行，但网络探测不会成功。

## 查询 Pico 是否可访问

```powershell
python scripts/probe_pico.py 192.168.11.7
python scripts/probe_pico.py 192.168.11.7 --port 8888 --count 10 --timeout 1 --json
```

默认访问静态 IP `192.168.11.7`；自行更改编译配置后，请使用修改后的地址。脚本只发送查询命令，不提交键盘或鼠标动作。它分别显示：

- 网络是否可达、响应数量、丢包率，以及 RTT 最小/平均/最大值。
- USB 是否已枚举，键盘/鼠标端点是否可发送，以及动作执行/恢复是否忙。
- 等待队列占用、启动时间、接收和拒绝计数。

有效回应返回退出码 `0`；没有有效回应返回 `1`；参数错误返回 `2`。网络可达而 USB 未枚举仍返回 `0`，同时明确显示 USB 状态。端点瞬时 busy 可以是正常的报告发送过程，不能据此判断 USB 故障。网络超时也可能来自错误 IP、未刷入新固件、防火墙或 Wi-Fi 客户端隔离。

查询使用同一个 UDP 端口，所有整数均为小端：

请求为 5 字节 `struct.pack('<BI', 0, token)`，`token` 是调用方随机生成的 `uint32`。响应为 24 字节 `struct.unpack('<4sBBBBIIII', response)`：

| 偏移 | 字段 | 含义 |
| --- | --- | --- |
| 0 | `magic[4]` | ASCII `PICO` |
| 4 | `version` | 查询协议版本 `1` |
| 5 | `flags` | bit0 USB mounted，bit1 键盘 ready，bit2 鼠标 ready，bit3 busy，bit4 Wi-Fi up，bit5 static IPv4 |
| 6 | `queueDepth` | 等待执行数量，不含正在执行的命令 |
| 7 | `queueCapacity` | `8` |
| 8 | `token` | 原样回传查询 token |
| 12 | `uptimeMs` | 启动时间，约 49.7 天回绕 |
| 16 | `acceptedCommands` | 已进入动作队列数量 |
| 20 | `rejectedCommands` | 非法包、队列满、不可用时拒绝，以及掉线/错误时中止的计数 |

查询绕过动作队列，长按或移动期间仍可响应。计数不是动作完成确认；同一个动作可能先被 accepted，随后被 aborted 而计入 rejected。

## 动作协议与执行规则

保留原动作命令号，数据包要求长度精确匹配，浮点为 IEEE-754 binary32 小端：

| 命令 | Python 打包格式 | 字段与范围 |
| --- | --- | --- |
| `1` 按键 | `<BBH` | cmd、USB HID usage keycode `4..0xE7`、保持时间 `0..5000` ms；`0xE0..0xE7` 转换为 Ctrl/Shift/Alt/GUI 修饰位 |
| `3` 直接移动 | `<Bhh` | cmd、相对 `dx`、`dy`；完整 int16 范围 |
| `4` 曲线移动 | `<Bhhff` | cmd、相对 `dx`、`dy`、速度 `0..100`、曲率 `-100..100`；拒绝 NaN/Inf |
| `8` 点击 | `<BB` | cmd、按钮掩码 `1..31`；1=左、2=右、4=中、8/16=侧键；保持时间随机 60..100 ms |

按键保持时间保留 ±5 ms 抖动，非零最小为 1 ms。直接移动和曲线移动均拆分成 `[-127,127]` 的报告，只有 TinyUSB 接受报告后才累计位移。曲线采用三次贝塞尔、缓起缓停和时间抖动，最后明确补齐整数终点；累计 HID 位移精确不代表屏幕像素精确，操作系统指针速度、加速和屏幕边界仍会影响光标位置。

UDP 回调只复制、校验并入队，主循环持续执行 `tud_task()` 和 `cyw43_arch_poll()`，通过时间戳非阻塞调度动作。等待队列容量 8，另有 1 个执行中的命令。队列满或 USB 不可用时拒绝新动作。USB 断连/挂起、Wi-Fi 掉线、传输失败或连续 2 秒无法提交报告会中止动作和队列；USB 可用时独立重试键盘/鼠标释放。合法保持时间不计入 USB 停滞超时。Boot 模式使用无 Report ID 的标准 8 字节键盘和 3 字节鼠标报告；鼠标侧键只在 Report 模式支持。

当前动作协议仍是无序号、无完成 ACK 的 UDP 数据包，可能丢包、重复或乱序；调用方不能仅凭 `sendto()` 成功判断动作完成，也不能盲目重发点击或按键。需要完成确认与去重时，应另加带序号的动作协议。UDP 服务没有鉴权，应限定在可信局域网使用。

AutoFish 当前的 `crates/autofish-infra/src/pico.rs` 使用 HTTP `:5000` 的 `/device`、移动和按键接口，与此 UDP 固件不兼容。此处提供独立 UDP 查询接口与脚本；接入现有桌面应用需要配套实现 UDP 客户端及所需命令。

## 已修复的问题与验证

原实现链接 `threadsafe_background`，却让 Core 1 调用 `cyw43_arch_poll()`；耗时动作实际上在网络回调上下文执行，且与主循环重入 TinyUSB。现改为单核 `pico_cyw43_arch_lwip_poll`。原先 USB busy 时跳过报告、可能漏发释放；大步长转 int8、int16 极限平方溢出、浮点非法值、直接移动未实现和修饰键错误也已修复。按 `pbuf->tot_len` 复制完整链式报文，并在释放 pbuf 前保留源地址。移除 `-ffast-math`，确保参数的有限值检查有效。

```powershell
./scripts/test_engine.ps1
python -m unittest discover -s tests -p test_probe.py -v
```

真实 C 引擎在本机 MSVC C11、`/W4 /WX` 下通过 9 组测试，包含 442 组边界/随机位移、USB 背压、释放重试与独立接口恢复、队列、断连、超时和协议编码。探测脚本通过 11 项回环 UDP 测试。Pico 2 W Release 交叉编译开启 `-Wall -Wextra -Werror`。这些检查不替代实机 USB 枚举、Wi-Fi 重连、物理移动和端到端延迟测量。

原源码和原 UF2 备份：`backups/original-20261013-041947/`。

参考：[Pico SDK 网络模型](https://www.raspberrypi.com/documentation/pico-sdk/networking.html)、[TinyUSB 任务模型](https://docs.tinyusb.org/en/latest/reference/architecture.html)。
