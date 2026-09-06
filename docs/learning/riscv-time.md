# RISC-V 时间、定时器与周期 Tick 学习总结

本文整理内核定时器开发需要掌握的时间模型、RISC-V/SBI 机制、周期策略、性能取舍和验证方法，并记录 BoarOS 当前已经确定的选择。当前接口和限制以 [RISC-V Timer 与内核 Tick 模块](../modules/riscv-timer.md) 为准。

## Timebase、时间和 Tick 不是同一个概念

RISC-V 的 `time` CSR 提供单调递增的硬件时间计数。`timebase-frequency` 表示它每秒增加多少次。例如 10 MHz 表示一秒增加 10000000，一毫秒增加 10000。这个频率描述硬件计数尺度，不表示内核每秒必须处理中断多少次。

内核 tick 是软件选择的逻辑周期。BoarOS 当前选择 100 Hz，即理想情况下每 10 ms 产生一次逻辑 tick。于是：

```text
period = timebase-frequency / tick-hz
QEMU virt:      10000000 / 100 = 100000
VisionFive 2:    4000000 / 100 =  40000
```

不要把“time CSR 计数一次”理解成“CPU trap 一次”。CPU 只在计数到达软件设置的 deadline 时产生 timer pending；绝大多数 timebase 计数没有 trap。

三个常见角色也应分开：

- clocksource：读取连续、单调的时间值，例如 RISC-V `time`；
- clockevent：安排某个未来时刻产生事件，例如 SBI `set_timer(deadline)`；
- tick：内核基于 clockevent 建立的周期策略，用于累计运行时间、超时或触发调度检查。

这一区分让底层硬件保持一次性 deadline，而上层既可以当前每 10 ms 重设，也可以以后改成“只安排最近真实事件”的 tickless 策略。

## RISC-V 怎样把 Timer 变成 S-mode Trap？

Supervisor timer interrupt 的 cause 编号是 5。接收它需要三个条件：timer pending、`sie.STIE=1`，以及 S-mode 全局中断条件成立。进入 trap 时硬件把旧 `sstatus.SIE` 保存到 SPIE 并清 SIE，所以 handler 可以先稳定保存现场，不会立即被另一个同级中断嵌套。

异步 timer interrupt 不对应一条失败指令，因此 `sepc` 表示恢复位置，通常保持不变。handler 必须先解除当前 pending 条件；否则 `sret` 恢复 SIE 后会立即再次 trap。对一次性 timer 来说，解除方式就是把比较值安排到未来。

时间读数和 timer 编程是两件事。S-mode 可以读取 `time`，但是否能直接写 supervisor 比较寄存器取决于 Sstc 等扩展和固件配置。没有 Sstc 时，S-mode 通常通过 SBI 请求 M-mode 固件操作平台 timer。

## SBI TIME 与 Sstc

SBI TIME 扩展 ID 为 `0x54494d45`，函数 0 接收绝对 `stime_value`。它不是“等待一段时间”的阻塞调用，也不是自动周期 timer；每次调用只安排一个 deadline。BoarOS 每次中断后再次调用它，才形成当前的周期行为。[SBI TIME 规范](https://github.com/riscv-non-isa/riscv-sbi-doc/blob/master/src/ext-time.adoc)规定把 deadline 设到未来会清除待处理的 supervisor timer interrupt。

[Sstc 扩展](https://docs.riscv.org/reference/isa/priv/sstc.html)提供 `stimecmp` 等 supervisor timer compare 能力，S-mode 可以避免每次经过 SBI/M-mode。OpenSBI 自身也会在硬件有 Sstc 时利用它实现 TIME 服务，因此使用 SBI 不妨碍新硬件获得固件侧优化。BoarOS 当前只保留 SBI 路径，因为 QEMU 与 VisionFive 2 的 OpenSBI 启动链都能提供它，而且一个后端已经满足当前需求；尚未测量到需要额外 Sstc 直写路径的瓶颈。

## 为什么频率必须来自 DTB？

timebase 是平台事实，不是 Sv39 或 RV64 固定常量。RISC-V CPU DT binding 要求 `/cpus/timebase-frequency` 描述该值；Linux RISC-V 启动也从该属性初始化全局 timebase，缺失时停止启动。[Linux RISC-V CPU binding](https://github.com/torvalds/linux/blob/f4cdf7ca9a1fdcca413157df19753f388a5a224e/Documentation/devicetree/bindings/riscv/cpus.yaml)和 [`arch/riscv/kernel/time.c`](https://github.com/torvalds/linux/blob/f4cdf7ca9a1fdcca413157df19753f388a5a224e/arch/riscv/kernel/time.c)可用于核对这一契约。

固定的 QEMU v11.1.0 源码在 `hw/riscv/virt.c` 使用默认 ACLINT timebase，并由 `hw/riscv/fdt-common.c` 写入 `/cpus`；当前真实 QEMU DTB 和 OpenSBI 输出都验证为 10 MHz。Linux 固定快照的 [`jh7110-common.dtsi`](https://github.com/torvalds/linux/blob/f4cdf7ca9a1fdcca413157df19753f388a5a224e/arch/riscv/boot/dts/starfive/jh7110-common.dtsi)为 VisionFive 2/JH7110 覆盖成 4 MHz。因此两块平台可以复用同一 SBI timer 代码，但不能共享硬编码 period。

BoarOS 的通用 DTB 读取允许该属性缺失并输出零，因为未来 LoongArch 不使用这一 RISC-V 属性；RISC-V timer 启动再把零频率作为精确错误。属性一旦存在，就必须是一个非零 32 位大端 cell，错误长度或重复值属于无效输入。

## 怎样从一次性 Deadline 构造稳定周期？

最直接的写法是在 handler 中设置 `next = now + period`。它虽然简单，但每次 handler 延迟都会把下一次 deadline 同样向后推，长期相位会逐次漂移。

BoarOS 保存原计划 deadline。若本次处理时刻为 `now`：

```text
elapsed = (now - previous_deadline) / period + 1
next    = previous_deadline + elapsed * period
```

这样有四个效果：

- 正常到期时推进一个周期；
- 稍晚但尚未跨过下一边界时仍保持原相位；
- 跨过多个周期时一次补记 elapsed，而不是循环制造多个 trap；
- 新 deadline 总在采样的 now 之后，避免因重设过去时间导致软件中断风暴。

算法用 64 位模运算处理自然回绕，只要求比较的时间距离小于 `2^63` 个计数。10 MHz 下这一窗口约为数万年，远大于当前内核运行边界。若 SBI 设置失败，不能先更新软件 deadline，否则软件会错误地认为硬件已接受新状态；所以候选值只在固件成功后提交。

当 frequency 不能整除 100 时，当前使用整数商。每个周期最多少于一个硬件计数，目标 QEMU 和 VisionFive 2 都不存在该误差。将来若墙钟需要更高精度，应直接基于原始 timebase 做比例换算；不应把调度 tick 当作精确墙钟。

## 周期 Tick 的性能含义

100 Hz 表示单 hart 每秒最多约 100 次正常 timer trap。一次 trap 包含整数 Trap Frame 保存/恢复、C dispatcher、SBI ecall 和少量状态更新。其粗略 CPU 占用为：

```text
开销比例 ≈ tick-hz × 单次 handler 时间
```

它还会打断 `wfi`，增加缓存活动和功耗；多 hart 时总事件数量会近似随 hart 数增加。UART 输出远比 handler 本身昂贵，因此正常 tick 绝不能逐次打印。

当前 100 Hz 为单 hart FIFO 内核线程提供 10 ms 时间片检查。无 READY 竞争者时 scheduler 不切换；有竞争者时一次 trap 最多切换一次。若以后测量发现空闲唤醒或高 hart 数成本明显，可以把周期策略换成 tickless；一次性 SBI deadline 后端正适合这一变化。不要仅凭“Linux 支持高 HZ”提高频率，也不要仅凭“中断有开销”在没有延迟指标时过早引入复杂 nohz 状态。

## xv6、rCore 与 Linux 的相关做法

- [xv6-riscv](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/start.c)当前教学实现直接使用 Sstc/stimecmp，并以简单固定间隔重设；[`trap.c`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/trap.c)在 timer interrupt 中推进全局 ticks，并在存在进程时触发调度。它强调机制清晰，周期约为 10 Hz。
- [rCore Tutorial](https://rcore-os.cn/rCore-Tutorial-Book-v3/chapter3/4time-sharing-system.html)通过 SBI 设置下一 timer，使用 100 Hz 时间片并在 trap 中进入任务切换，适合观察“one-shot 固件接口怎样构成周期 tick”。
- Linux 的 [`timer-riscv.c`](https://github.com/torvalds/linux/blob/f4cdf7ca9a1fdcca413157df19753f388a5a224e/drivers/clocksource/timer-riscv.c)把 time 作为 clocksource，把 timer 作为 one-shot clockevent；有 Sstc 时直接写 compare，否则调用 SBI。上层可选择不同 [HZ](https://github.com/torvalds/linux/blob/f4cdf7ca9a1fdcca413157df19753f388a5a224e/kernel/Kconfig.hz)，并通过 [NO_HZ](https://github.com/torvalds/linux/blob/f4cdf7ca9a1fdcca413157df19753f388a5a224e/kernel/time/Kconfig)减少空闲或指定 CPU 的周期 tick。

这些系统的共同点是把“硬件绝对 deadline”和“上层何时需要事件”分开。BoarOS 当前只实现其中最小、已经有真实消费者的部分，不提前复制完整 Linux clockevents 框架。

## 可复用的验证与调试方法

- 用 QEMU `dumpdtb` 或 `fdtget <dtb> /cpus timebase-frequency` 核对实际启动输入，不从机器型号猜频率。
- 同时观察 OpenSBI 的 timer device 频率和内核解析结果；两者不一致时先检查传入的是哪棵 DTB。
- 状态测试要覆盖“固件失败前后 CSR 和软件 armed 是否变化”，不只测成功日志。
- deadline 数学应脱离真实等待做确定性边界测试，包括恰好到期、多周期迟到和 `UINT64_MAX` 回绕。
- 真实 QEMU 测试仍必须让 timer trap 至少返回一次；只看 set_timer 返回成功不能证明 STIE/SIE、Trap Frame、dispatcher 和 `sret` 连通。
- 无根设备的正常内核预期停在 `wfi`，所以 idle 自动测试把宿主 timeout 视为预期状态并检查期间没有 fatal；挂载生产根盘后，PID 1 退出会触发资源收口和 SBI 关机，不能再把 timeout 当作成功。

## 墙钟来源与定点换算（当前结论）

- QEMU `virt` 板载 goldfish RTC 位于 MMIO `0x101000`：读 `TIME_LOW`（offset 0）会锁存 `TIME_HIGH`（offset 4），两寄存器合成为自 Unix epoch 的纳秒。先读 LOW 再读 HIGH 即得到一致的样本，无需重试循环。
- 墙钟只在启动时采样一次作为 `boot_realtime`，此后 realtime = boot_realtime + 单调值；Linux 桌面系统同样以 RTC 校准启动时刻、运行期靠时钟源推进。接收样本必须做合理性检查（非递减、间隔有界、不早于选定阈值），把"设备不存在/寄存器读回垃圾"归一成明确的不可用状态。
- ns↔ticks 换算用启动时预算的 32.32 定点乘数：`ceil(1e9·2^32/freq)` 与 `ceil(freq·2^32/1e9)`，热路径一次 `mulhu`，相对误差 < 2^-32，且不引入 libgcc 的 128 位除法（`-nostdlib` 下没有 `__udivti3`）。直接 `ns * freq` 会在大睡眠时长溢出 64 位，两个乘数方案天然避免。

## 可复用的验证与调试方法（补充）

- 布局相关的远端损坏（堆 magic 失败、lwext4 ENOENT、页分配器 STATE 同时出现且位置随无关代码增减移动）应首先怀疑栈溢出：把嫌疑栈临时放大数倍复测，若症状消失即为栈深度问题，再去量化和定尺寸。
- 深调用链（如 openat → fs context 路径解析 → lwext4 遍历）叠加在测试 harness 主流程栈帧上，单看生产路径的深度会低估需求；按最深的真实消费者预算栈。

本地核对入口包括 `references/riscv/`、`references/qemu/`、`references/opensbi/` 和 `references/linux/`。固定版本、commit 与恢复方式见 [本地参考资料](../../references/README.md)。
