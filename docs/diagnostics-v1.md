# 连续控制只读诊断 ADRD v1

适用固件 `0.5.1-diagnostics`。新查询复用 40 字节 ADRV 请求，op=5；只有 token 可非零，其他控制字段必须为零。响应独立为 ADRD，定长 160 字节、格式版本 1。普通 QUERY/OPEN/SET/RENEW/STOP 仍为原来 40 字节请求和 48 字节 ADRS 响应，旧 PICO/DIAG/DIAG2 格式也不变。

诊断只读取快照，不续租、不执行按键、不作为控制确认。它不会由固件主动推送，也不在 SET/RENEW/STOP 时追加报文。

## 数据布局

所有整数小端。头部 Python 格式 `<4sBBH8I`，40 字节。

| 偏移 | 字段 | 含义 |
|---|---|---|
| 0 | magic[4] | ADRD |
| 4/5/6 | format/result/size | u8=1，u8结果码，u16=160 |
| 8/12/16 | token/boot/uptime_ms | u32，回传token、本次启动标识、当前设备毫秒时间 |
| 20 | firmware_version | u32；按 major/minor/patch/build 四个字节编码，0x00050100=0.5.1.0 |
| 24 | capabilities | u32，当前0x3f；bit0连续状态，1有限hold，2抢占停止，3诊断，4真实USB完成事件，5诊断计数 |
| 28 | lease_bounds | 低u16最小100、高u16最大300，单位ms |
| 32 | hold_bounds | 低u16最小20、高u16最大200，单位ms；hold=0另代表持续状态 |
| 36 | validity | u32，有效位与事件类型位 |
| 40 | stop | 5个u32：reason、time_ms、epoch、session、seq |
| 60 | submitted | 4个u32：epoch、session、seq、time_ms |
| 76 | completed | 同上，真实USB完成回调 |
| 92 | release_submitted | 同上，全零键盘状态提交 |
| 108 | release_completed | 同上，全零键盘状态完成回调 |
| 124 | net_send_errors | u32，回复分配、复制或UDP发送返回错误的累计次数 |
| 128 | usb_errors | u32，USB不可用转换、传输失败或发送停滞取消的累计次数 |
| 132/136 | core0_max_gap_us/core1_max_gap_us | u32，本次启动以来两核心应用循环最大观测间隔，单位us |
| 140 | usb_unmatched_completions | u32，没有对应在途报告或内容不匹配的完成回调次数 |
| 144 | usb_tracking_overruns | u32，上次报告尚未回调时又接受新报告的异常次数 |
| 148 | protocol_bounds | 低u16最小3、高u16最大3；支持的ADRV控制协议范围 |
| 152..159 | reserved | 全零 |

validity：bit0 stop有效，bit1 submitted有效，bit2 completed有效，bit3 release_submitted有效，bit4 release_completed有效；bit8说明最近submitted是全零释放，bit9说明最近completed是全零释放。时间或序号为0也可能有效，不能用0代替有效位判断。

stop.reason：1客户端STOP，2租约过期，3Wi-Fi异常，4USB不可用，5USB传输失败，6USB发送停滞，7旧命令10释放，8其他内部取消。未发生停止时无bit0，而不是把reason=0解释为成功。

## 身份和时间语义

- stop记录第一次真正结束活跃会话的事件及被撤销的epoch/session；seq是该会话最近SET的目标序号，可能尚未提交，不是最近RENEW。重复STOP或会话关闭后的网络/USB变化不覆盖原原因和时间。
- submitted只在TinyUSB实际接受键盘报告后记录。租约通道复用当前Core0处理轮次的设备毫秒时间；旧引擎的取消释放在实际提交处采样。它是毫秒粒度的设备事件记录，不能当作亚毫秒硬件测量。
- completed只能由 `tud_hid_report_complete_cb` 产生，说明USB栈报告传输成功，仍不说明游戏已经消费。与提交时保存的在途epoch/session/SET序号关联；停止、续租和新会话不会把旧回调改名为新动作。
- release记录实际全零键盘报告，停止时沿用被撤销会话的身份。有限hold自动释放仍使用原SET序号。一次停止后的重复全零报告不会把首次取消释放伪装成另一个SET。
- 四种事件各自保留最近有效记录，可能属于不同序号甚至不同会话。计算耗时时必须比较boot及对应epoch/session/seq，不能仅因有效位存在就把两条记录相减。
- 时间戳为1ms粒度的uint32设备运行时间，约49.7天回绕。用同boot、匹配身份的uint32差值，并仅接受小于2^31ms的合理时间窗。最大循环间隔用uint32微秒差值，首次采样不计间隔；8秒硬件看门狗仍独立工作。计数器uint32自然回绕，跨重启不可直接相减。
- 完成报告内容必须匹配在途键盘状态。异常覆盖或不匹配会增加计数并停止产生可疑完成记录，直到USB失败/不可用重置跟踪。控制行为不依赖这些诊断字段；缺少完成事件明确表示未知。

## 延迟边界

诊断状态在原桥接锁内做定长快照，160字节编码在锁外完成。计数与时间字段使用固定大小数据，无新动作队列、阻塞等待、字符串格式化或Flash写入。正常控制每包响应次数和40/48字节大小不变；网络失败只增加计数，不递归发送诊断。Core0/Core1循环间隔复用原publish/heartbeat锁；时间单位换算在进锁之前完成。

这并不等于绝对零开销。最终生产主机DLL `1ca4b812df495efaca41f42ea906bcac8c1290ad8bab8c489f9665b10f8822e2` 的7轮×3000样本离线对比中，SET/RENEW/STOP中位数旧→新分别为1.9→1.9、1.9→1.9、1.9→2.0微秒；p95分别为3.8→4.3、3.5→4.1、4.1→4.3微秒。RENEW、DIAGNOSTIC及heartbeat不增加USB报告。Core1 heartbeat的独立主机调用中位数0.6微秒，包含测试调用开销，不能解释为单纯新增固件开销。

上述为Windows主机C状态机与ctypes基准，不测量RP2350 IRQ、无线往返、真实USB传输或游戏响应。真实延迟仍需刷入后验证。

## 回归和产物

`./scripts/test_engine.ps1` 当前36组生产C主机测试：原30组加6组诊断测试，覆盖160字节布局、只读性、提交/完成分离、跨取消和跨会话完成身份、首次停止原因保持、失败/拔线/在途覆盖不伪造完成、循环时间及计数回绕、有限hold释放。

原27项Python本机UDP模拟测试全部通过。`./scripts/build_host_lease.ps1` 构建无USB/网络能力的生产状态机DLL，新增exchange_diagnostic、complete_keyboard、complete_report、note_send_error、network_heartbeat_us接口，供客户端真实协议互通测试。

本次联网版产物：`build-verified/hid_usb.uf2`，SHA256 `e73d3219eabf48a16a1be019fd9debc2d2418d92b48be872f7ab85f969e97556`。旧UF2及manifest已保存在 `backups/pre-diagnostics-20261029-045812`。本次没有刷入设备，没有发送真实按键或查询；用户手动刷机后验收。
