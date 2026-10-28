# 连续键盘租约协议 v3

固件版本：`0.5.1-diagnostics`。独立于旧版动作协议 v2，保留旧健康查询和动作数据格式。新通道只控制键盘，使用原 UDP 端口 `8888`，不需要将行走拆成每 200 毫秒一次的按下/释放。

## 生命周期

1. `QUERY` 只读取当前 `boot/epoch` 和 `CAN_OPEN`。查询不续租、不产生输入。
2. `OPEN` 带查询到的 `boot/epoch` 和新的非零随机 `session`。只有 USB、Wi-Fi 可用且旧引擎与队列空闲、取消已确认时才成功。成功后 `epoch` 加一，旧引擎先全释放。OPEN 本身不按键；客户端必须观察释放完成后才 SET。
3. `SET` 用新递增序号提交完整 modifier + 六键状态；不排入历史动作队列，最新状态覆盖尚未提交的旧目标。`hold_ms=0` 表示持续保持，`20..200` 表示短转，有限保持从 TinyUSB 首次接受目标报告时计时。
4. `RENEW` 仅续会话租约，保持当前目标，不重发 USB、不重启短转计时。`lease_ms` 允许 `100..300`，建议每 50 毫秒续 250 毫秒。每个续租需要新序号；查询和重复包均不续租。
5. 有限保持到期时固件安排全零键盘报告，置 `HOLD_EXPIRED`，会话仍可续租；新 SET 清除此标志。
6. `STOP` 立即关闭会话、推进 epoch、清空旧动作队列，Core 0 尽快提交全释放。它不排队等待当前动作完成。会话超期、USB/Wi-Fi 断开和传输失败使用相同的取消屏障。实际释放仍取决于 USB 可用和主循环进度；没有宣称即时物理生效。

超期检查在每次新协议收包和 Core 0 tick 前进行，迟到续租不能在超期后挽救旧会话。停止或超期后必须重新 QUERY/OPEN；旧 OPEN/SET/RENEW 无法恢复已停止会话。新会话开始后，旧 STOP 也不会中断新会话。设备每次启动生成新的随机 boot 标识，避免重启前的数据报沿用旧身份。boot 是 32 位随机标识，不是密码学认证。

## 字节格式

所有整数小端，保留字段必须为零。定长请求 40 字节，Python：`<4sBBHHHIIIIIB6sB`。

| 偏移 | 字段 | 类型/含义 |
|---|---|---|
| 0 | magic | `ADRV` |
| 4 | version | u8，3 |
| 5 | op | u8，0 QUERY / 1 OPEN / 2 SET / 3 STOP / 4 RENEW / 5 DIAGNOSTIC |
| 6 | lease_ms | u16；OPEN/SET/RENEW 为 100..300，其余为 0 |
| 8 | hold_ms | u16；仅 SET 可为 20..200，否则为 0 |
| 10 | reserved | u16，0 |
| 12 | token | u32，只关联本次响应 |
| 16 | boot | u32，QUERY 可填 0 |
| 20 | epoch | u32，QUERY 可填 0；OPEN 用预期 epoch，后续使用 OPEN 响应的新 epoch |
| 24 | session | u32，OPEN 非零新随机值，后续保持一致 |
| 28 | seq | u32；OPEN=0，SET/RENEW 必须前进，允许正常回绕 |
| 32 | modifier | u8，USB modifier 位掩码；仅 SET 使用 |
| 33 | keys | 6 字节；0 为空，普通键范围 4..0xdf，非零键不得重复 |
| 39 | reserved | u8，0 |

普通控制响应 48 字节；op5 为独立160字节只读响应，见[诊断协议](diagnostics-v1.md)。普通响应Python：`<4sBBBB7IHHB6sB`。

| 偏移 | 字段 | 含义 |
|---|---|---|
| 0 | magic | `ADRS` |
| 4/5/6/7 | version/result/flags/reserved | 均 u8；版本 3，保留 0 |
| 8/12/16/20 | token/boot/epoch/session | 均 u32 |
| 24/28/32 | accepted_seq/applied_seq/uptime_ms | 均 u32 |
| 36/38 | lease_remaining_ms/hold_remaining_ms | 均 u16 |
| 40/41/47 | actual_modifier/actual_keys[6]/reserved | 本租约通道最后提交的键盘状态，保留 0 |

result：0 OK，1 报文无效，2 boot/epoch 过期，3 会话关闭或不匹配，4 当前不可用/忙，5 序号过旧或重复序号内容不一致。

flags：bit0 USB available，bit1 Wi-Fi up，bit2 session active，bit3 USB/释放待处理，bit4 hold expired，bit5 can open。

**确认语义：**`accepted_seq` 是最近接受的 SET 或 RENEW；`applied_seq` 只对应最近被 TinyUSB 接受的 SET。RENEW 绝不推进 applied_seq，以免丢失 SET 后收到 RENEW 被误当成 SET 已执行。自动释放保留原 SET 的 applied_seq；需同时检查实际键、HOLD_EXPIRED 和 PENDING。TinyUSB 接受报告不等于 USB 主机或游戏已经消费，移动是否成功仍由游戏遥测判断。

同序号、相同语义的重复 SET/RENEW 返回 OK，但不重复执行、不续租、不重启 hold。同序号改变内容被拒绝。相同 OPEN 的重复包仅可在原会话仍存活且匹配其原预期 epoch 时返回 OK；不续租。STOP 的重复包在旧会话仍关闭时可返回 OK，但不推进额外取消，也不影响后续新会话。

## 与旧协议互斥

旧命令 10 已强化为抢占全释放和取消，不再排队。租约会话活跃及释放待处理期间，其他旧输入命令会被拒绝。旧健康查询仍为动作格式版本 2，24 字节；新能力通过 ADRV QUERY 检测，不借用旧 version 字段。

旧动作协议没有会话序号，仍不能保证乱序重复下的恰好一次输入。执行导航时必须独占设备。连续控制必须使用新协议，不能用旧命令 2 的无限按下来替代租约。

## 验证

运行 `./scripts/test_engine.ps1`：原 23 组、7组租约及6组诊断生产 C 主机测试，MSVC C11 `/W4 /WX`。新增组覆盖 40/48 字节黄金格式、边界、连续续租无松键、丢 SET 后续租不假确认、待 USB 的 SET 不被续租改号、重复/乱序包、有限 hold 与 USB 背压、STOP 及旧命令 10 抢占、USB/Wi-Fi 丢失、boot/epoch/序号/时间回绕。

交叉编译和主机模拟不能替代刷入后实测。需要测量查询 RTT、持续行走、按键实际释放延迟、断网/失焦/遥测冻结停止，以及短转角度。用户已提供本机 wifi_config.h，本次完成联网版 `build-verified/hid_usb.uf2` 交叉编译，SHA256 `e73d3219eabf48a16a1be019fd9debc2d2418d92b48be872f7ab85f969e97556`；尚未刷入、没有实机验证。`-CompileOnly` 产物仍不含 Wi-Fi 凭据，不可作为可部署固件刷入。


客户端可使用真实生产 C 状态机做无硬件互通测试：运行 `./scripts/build_host_lease.ps1`，得到 `build-host-tests/hid_lease_host.dll`。DLL 原样链接 hid_bridge/hid_lease/hid_engine，测试适配层只模拟 USB 接受报告和旧引擎空闲，不创建网络套接字、不发送真实 USB 输入。导出 reset、exchange(request40, now_ms, response48)、tick、copy_keyboard、keyboard_reports、keyboard_releases 和模拟连接开关；这验证两端字节格式及状态衔接，不能验证硬件 IRQ、无线收发或真实 USB 时序。
