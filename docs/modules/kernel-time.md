# 内核时间模块

本文描述架构无关时间子系统的稳定接口：单调时钟、实时时钟和睡眠 deadline 换算。timebase 读取与 SBI 定时器见 [RISC-V Timer 模块](riscv-timer.md)，等待队列与阻塞唤醒见[内核调度与进程生命周期模块](kernel-scheduler.md)。

## 范围与入口

| 文件 | 当前职责 |
|---|---|
| `include/kernel/time.h`、`kernel/time.c` | 固定点乘数换算、单调/实时时钟、deadline 换算 |
| `arch/riscv/virt_rtc.c`、`include/arch/riscv/virt_rtc.h` | QEMU virt goldfish RTC 读取与合理性探测 |
| `kernel/main.c` | 在 timer 启动前读取 RTC 并调用 `kernel_time_init` |

## 换算与精度

时间源是 RISC-V `time` CSR 的原始计数（QEMU virt 10 MHz，来自 DTB）。换算使用启动时预算的两个 32.32 定点乘数：

- ns = ticks × `ceil(1e9·2^32/freq)` ≫ 32；
- deadline 增量 = `ceil(ns × ceil(freq·2^32/1e9) / 2^32)`。

热路径使用 64×64→128 乘法，不依赖运行期 128 位除法。乘数向上取整的绝对系数误差小于 `2^-32`，相对误差取决于 frequency，不能把两者混同。deadline 最终也向上取整，避免不足一个 tick 的请求被提前唤醒。

syscall 将有效的非负 timespec 饱和到 `INT64_MAX` 纳秒，比较目标是否已经过去时使用无符号绝对时间；转换后的未来 tick 距离限制在 `INT64_MAX` 内，再使用有符号回绕差值比较。0 是“没有 deadline”的保留值。原始时钟接口仍是 64 位纳秒表示，不宣称已实现数百年运行后的时钟回绕管理。

## 时钟语义

- `kernel_time_monotonic_ns()`：自启动的单调纳秒。
- `kernel_time_realtime_ns()`：启动时 RTC 读数加单调值；不在运行期重读 RTC，也没有 settimeofday/adjtime 语义。
- `kernel_time_boot_realtime_offset()`：暴露启动 RTC 读数，供绝对 REALTIME deadline 换算到单调域。
- `kernel_time_deadline_from_monotonic()`：把单调域目标时间换成 `time` CSR 绝对 deadline；目标不在未来时返回 `DEADLINE_PASSED`。

CLOCK_REALTIME 的墙钟完全取决于启动时的 RTC 探测。goldfish RTC 读数必须两次采样非递减、间隔小于 1 秒且不早于 2020-01-01，否则视为无 RTC，CLOCK_REALTIME 退化为自启动相对时间（固定从epoch 起算的值不可得）。DTB 解析 RTC 节点是演进路径；当前地址按 `virt_uart.h` 先例硬编码 `0x101000`。

## 消费者与验证

syscall 层的 `clock_gettime(113)`、`clock_getres(114)`、`gettimeofday(169)` 只支持 `CLOCK_REALTIME` 与 `CLOCK_MONOTONIC`，其余 clockid（含 CPU-time 时钟）准确返回 `-EINVAL`；`clock_nanosleep(115)`/`nanosleep(101)` 通过 deadline 换算在等待队列上睡眠。`make test-syscall-riscv` 覆盖换算数学与参数边界，`make test-userland-riscv` 用 musl 的 `clock_gettime`/`gettimeofday`/`nanosleep` 验证真实 trap→expire→wake 闭环和 RTC 墙钟（断言不早于 2020-01-01）。

当前限制：无 NTP/阶跃调整、无粗粒度/CPU-time clockid、RTC 只在启动读一次。
