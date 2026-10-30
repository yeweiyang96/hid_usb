# ADRV v4 运动窗口

版本 0.6.0-motion；UDP 8888。v3 保留 40 字节请求 / 48 字节响应。v4 请求的原 40 字节头只改变 version=4，WINDOW=6 使用额外头与分段，响应固定 80 字节。诊断继续使用独立 v3 op=5，报告协议范围 3–4、capabilities=0x7f。

WINDOW 附加头从字节 40 起为 `<IIHHI>`：revision、一次性 event_id、count、保留 0、not_after_ms。每段 `<HB6sB>`：duration_ms、modifier、keys[6]、保留 0。容量 16，每段 20–200 ms，总时长 ≤200 ms，所以当前最多实际装入 10 个最短分段。客户端租约为 250 ms。

响应字节 48 起为 `<6I4BI>`：接收 revision、USB 提交 revision、USB 完成 revision、首段开始时刻、窗口剩余毫秒、已提交 event_id、当前段、总段数、提交段、完成段、能力值 1。前三种确认不等于游戏执行。

接收核完整解析后在短锁内原子替换未来序列。USB 核以单调时钟执行，从 USB 首次接受开始；每段从实际提交计算持续时间，不插入全释放。边界背压达到 5 ms 则取消，绝不压缩补执行；绝对 not_after 不延长。到期、STOP、USB/Wi-Fi 故障清空序列并释放，取消 epoch 防止迟到输入复活。停止原因 9=窗口耗尽，10=时间轴延误。

重复相同 seq 幂等，内容不符/旧 seq/revision 拒绝。event_id 在接收时消耗，重复新窗口不能重放。RENEW 不修改时间轴、窗口截止、动作标识或执行进度。所有 uint32 时间/序号按半区间比较，覆盖回绕。

构建不刷写：`scripts/test_engine.ps1`、`scripts/build_host_lease.ps1`、`scripts/build.ps1`。相邻 AutonomousDriving 项目 `tests/test_motion_protocol.py` 使用生产 C 主机 DLL 测试 v4。部署产物为 `build-verified/hid_usb.uf2`，工具链和 SHA 记录在该目录的 `build-manifest.json`；本机 Wi-Fi 配置沿用且不输出内容。

完整字段表、客户端时效规则和实机验收步骤见相邻项目 `docs/pico-v4.md` 与 `docs/continuous-motion.md`。离线测试不证明板上 IRQ、无线、USB 和游戏动作时序。
