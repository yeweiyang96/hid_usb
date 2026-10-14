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

当前固件版本为 `0.3.1-wifi-recovery`，使用本地 `wifi_config.h` 编译后的发布文件为 `release/hid_usb-pico2w-v0.3.1-wifi-recovery.uf2`，同目录提供 SHA256 校验文件。按住 BOOTSEL 接入 USB，将此 UF2 复制到出现的 Pico 磁盘，复制完成后设备会重启。固件源码、测试、编译和发布均在本原项目进行。

沿用旧固件的静态网络配置：IP `192.168.11.7`、子网掩码 `255.255.255.0`、网关 `192.168.11.1`、DNS `8.8.8.8`。lwIP DHCP 客户端已关闭，启动与 Wi-Fi 重新连接均由 CYW43 驱动应用这组地址，不向路由器申请地址。配置项位于 CMake 的 `PICO_STATIC_IP`、`PICO_STATIC_NETMASK`、`PICO_STATIC_GATEWAY`、`PICO_STATIC_DNS` 缓存变量。

USB 初始化独立于 Wi-Fi 连接；Wi-Fi 失败不会让 USB 主循环退出。连接通过主循环中的异步状态机推进；Wi-Fi 驱动初始化和配置调用本身可能短暂阻塞。硬件初始化或 UDP 服务创建失败时每 30 秒重试。网络不通期间 USB 仍可能保持已枚举状态，这不能证明 Wi-Fi 正常。

## 待机断网恢复与诊断

`0.3.1` 补上了旧实现只检查驱动连接标志的恢复缺口。连接后每 15 秒向静态网关发 ARP 请求，观察真实的网关 ARP 回应或发给本机的 IPv4 数据包。广播和无关 ARP 不算恢复证据。探测等待 3 秒，失败后隔 5 秒重试；连续 3 次没有有效接收会重新初始化 CYW43 驱动、STA 网卡和 UDP 服务，重新应用静态地址并连接。不依赖网关允许 ICMP ping。

驱动明确报告掉线时约 5 秒后尝试重新关联；30 秒仍未连接则重新初始化 Wi-Fi。每次初始化、连接成功后设置 SDK 的 `CYW43_NONE_PM` 并读取模式验证，设置失败会在后续网络探测时重试。连接成功还发送 gratuitous ARP，刷新局域网邻居缓存。恢复会中止动作并释放按键，不重放未完成命令。

主循环看门狗超时为 8 秒，只有 USB/网络循环完成一轮后才喂狗。驱动调用或主循环卡住时整板复位，USB 会重新枚举；普通 Wi-Fi 恢复只重建网络。路由器关闭或 Wi-Fi 信号不可用时，这些机制不能让网络立刻恢复。ARP 保活是预防和恢复措施，尚需刷入后实机待机验证，不能仅凭主机测试认定原断线根因已确定。

```powershell
python scripts/diagnose_wifi.py
python scripts/diagnose_wifi.py --idle-seconds 300
```

诊断脚本只发只读查询；第二条在查询前后间隔 5 分钟，期间不从电脑发探测或输入动作，适合验证 Pico 自身的待机保活。测试期间请停止其他客户端主动查询。对比 `radio_resets` 判断是否发生了 Wi-Fi 重建，`watchdog_boot` 判断本次是否由看门狗启动，`power_save_disabled` 判断模式读取是否确认关闭省电。网关不可用也可能导致 Wi-Fi 重建；复位计数不等于已证明驱动故障。

扩展诊断请求为 9 字节 `<BI4s`：`0`、随机 token、`b'DIAG'`。响应 64 字节，前 24 字节沿用健康查询字段布局，但 magic 为 `PICD`、独立格式版本为 `1`；其后 `<IIIIiiiIII` 为 connect_attempts、radio_resets、last_rx_age_ms、pm_value、pm_result、join_result、link_status、watchdog_boot、missed_probes、recovery_reason。reason：0 未触发，1 驱动掉线/连接超时，2 没有有效收包。计数从本次启动累计，connect_attempts 包括首次连接。旧固件不支持该扩展查询。

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
| 4 | `version` | 当前动作扩展固件版本 `2`；查询脚本兼容 `1` 和 `2` |
| 5 | `flags` | bit0 USB mounted，bit1 键盘 ready，bit2 鼠标 ready，bit3 busy，bit4 Wi-Fi up，bit5 static IPv4，bit6 本次由看门狗启动 |
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
| `2` 按键按下/释放 | `<BBBH` | cmd、keycode、down `0/1`、动作后等待 `0..5000` ms |
| `3` 直接移动 | `<Bhh` | cmd、相对 `dx`、`dy`；完整 int16 范围 |
| `4` 曲线移动 | `<Bhhff` | cmd、相对 `dx`、`dy`、速度 `0..100`、曲率 `-100..100`；拒绝 NaN/Inf |
| `5` 组合键 | `<BBBH` | cmd、modifier 掩码、keycode、保持时间 `0..5000` ms |
| `6` 滚轮 | `<Bh` | cmd、完整 int16 相对滚动量 |
| `8` 点击 | `<BB` 或 `<BBH` | cmd、按钮掩码 `1..31`，可选保持时间 `0..5000` ms；1=左、2=右、4=中、8/16=侧键；省略或为零时随机 60..100 ms |
| `9` 鼠标按下/释放 | `<BBBH` | cmd、按钮掩码、down `0/1`、动作后等待 `0..5000` ms |
| `10` 全部释放 | `<B` | cmd；清除已保持的键盘和鼠标状态 |

按键保持时间保留 ±5 ms 抖动，非零最小为 1 ms。直接移动和曲线移动均拆分成 `[-127,127]` 的报告，只有 TinyUSB 接受报告后才累计位移。曲线采用三次贝塞尔、缓起缓停和时间抖动，最后明确补齐整数终点；累计 HID 位移精确不代表屏幕像素精确，操作系统指针速度、加速和屏幕边界仍会影响光标位置。

UDP 回调只复制、校验并入队，主循环持续执行 `tud_task()` 和 `cyw43_arch_poll()`，通过时间戳非阻塞调度动作。等待队列容量 8，另有 1 个执行中的命令。队列满或 USB 不可用时拒绝新动作。USB 断连/挂起、Wi-Fi 掉线、传输失败或连续 2 秒无法提交报告会中止动作和队列；USB 可用时独立重试键盘/鼠标释放。合法保持时间不计入 USB 停滞超时。Boot 模式使用无 Report ID 的标准 8 字节键盘和 3 字节鼠标报告；鼠标侧键只在 Report 模式支持。

当前动作协议仍是无序号、无完成 ACK 的 UDP 数据包，可能丢包、重复或乱序；调用方不能仅凭 `sendto()` 成功判断动作完成，也不能盲目重发点击或按键。需要完成确认与去重时，应另加带序号的动作协议。UDP 服务没有鉴权，应限定在可信局域网使用。

客户端需要实现本页的 UDP 协议；旧 HTTP `:5000` 接口不适用于此固件。本次网络恢复修改保留原项目已有的 v2 动作扩展，不改变上述动作命令格式。

## 屏幕位移标定与交互测试

在 Pico USB 接入的同一台 Windows 电脑运行：

```powershell
python test.py --settings-only
python test.py
python test.py --direct
```

`--settings-only` 只读取 Windows 设置和 Pico 状态，不发送输入动作。默认测试拟人曲线，`--direct` 测试直接移动。脚本先显示实际指针速度、加速度和系统 DPI，再要求将鼠标移至屏幕中央。按回车后执行 X/Y 两轴的四次标定移动，不点击、不按键；请保持实体鼠标不动，并停止其他控制客户端。随后仍按原流程手动记录目标和起点。

HID 相对位移是报告计数，不是屏幕像素。即使关闭提高指针精确度，Windows 指针速度和显示环境仍会影响桌面上的像素位移。DPI 感知解决坐标虚拟化，不能把 HID 计数转换成一比一像素。新版使用实际测得的每轴「像素/HID 单位」比例换算，不修改系统鼠标设置，也不要求 Pico 提供 DPI 设置。具体影响见 [Microsoft Raw Mouse 文档](https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-rawmouse)。

脚本通过动作接收计数、busy、队列和鼠标端点就绪状态等待动作结束，替代旧版按距离固定 sleep 的估计。只重试只读查询，不重发动作。当前动作协议无请求序号，测试时必须独占设备。Pico 拒绝/中止动作、重启、断连或出现并发命令时，脚本停止测试。

起点和终点必须位于标定时的同一块屏幕，系统速度/加速度/DPI 改变后需重新标定。曲线控制点越过屏幕边缘时自动减小曲率，以减少边界裁剪；整数报告与标定误差仍可能带来少量像素偏差。较大偏差需继续诊断，不能直接标为「正常丢包」或一概归因于 DPI。

## 已修复的问题与验证

原实现链接 `threadsafe_background`，却让 Core 1 调用 `cyw43_arch_poll()`；耗时动作实际上在网络回调上下文执行，且与主循环重入 TinyUSB。现改为单核 `pico_cyw43_arch_lwip_poll`。原先 USB busy 时跳过报告、可能漏发释放；大步长转 int8、int16 极限平方溢出、浮点非法值、直接移动未实现和修饰键错误也已修复。按 `pbuf->tot_len` 复制完整链式报文，并在释放 pbuf 前保留源地址。移除 `-ffast-math`，确保参数的有限值检查有效。

```powershell
./scripts/test_engine.ps1
python -m unittest discover -s tests -p 'test_*.py' -v
```

真实 C 引擎在本机 MSVC C11、`/W4 /WX` 下通过 9 组测试，包含 442 组边界/随机位移、USB 背压、释放重试与独立接口恢复、队列、断连、超时和协议编码。Wi-Fi 监测状态机另有 5 组测试，覆盖正常长时间待机、驱动假 UP、丢包、重连超时、初始化失败及报文过滤。Python 查询、诊断与鼠标标定共 24 项测试。Pico 2 W Release 交叉编译开启 `-Wall -Wextra -Werror`。这些检查不替代实机 USB 枚举、Wi-Fi 重连、物理移动和端到端延迟测量。

原源码和原 UF2 备份：`backups/original-20261013-041947/`。

参考：[Pico SDK 网络模型](https://www.raspberrypi.com/documentation/pico-sdk/networking.html)、[TinyUSB 任务模型](https://docs.tinyusb.org/en/latest/reference/architecture.html)。
