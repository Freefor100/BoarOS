# 内核时间模块

本文描述架构无关时间子系统的稳定接口：单调时钟、实时时钟和睡眠 deadline 换算。构建期 timer 操作见 `include/arch/timer.h`，RV timebase 与 SBI 定时器见 [RISC-V Timer 模块](riscv-timer.md)，等待队列与阻塞唤醒见[内核调度与进程生命周期模块](kernel-scheduler.md)。

## 范围与入口

| 文件 | 当前职责 |
|---|---|
| `include/kernel/time.h`、`kernel/time.c` | 固定点乘数换算、单调/实时时钟、timer 驱动 coarse 快照、deadline 换算 |
| `arch/riscv/virt_rtc.c`、`include/arch/riscv/virt_rtc.h` | QEMU virt goldfish RTC 读取与合理性探测 |
| `kernel/main.c`、`arch/loongarch/main.c` | 在 timer 启动前读取 RTC 并调用 `kernel_time_init` |
| `platform/loongarch_rtc.c`、`include/platform/loongarch_rtc.h` | QEMU virt LS7A TOY 启用、一致 UTC 读取与明确失败 |
| RV/LA 的 `trap.c` | 每次真实 timer interrupt 更新 coarse 快照、expire deadlines 和调度；LA 同时轮询 UART 唤醒 |

## 换算与精度

时间源由架构提供：RV 为 `time` CSR（QEMU virt 10 MHz，来自 DTB），LA 为 `RDTIME.D`，频率由 CPUCFG 4/5 计算。LA one-shot TCFG 保留 tick 相位并取 scheduler 最早期限，启动墙钟来自 LS7A TOY。换算使用启动时预算的两个 32.32 定点乘数：

- ns = ticks × `ceil(1e9·2^32/freq)` ≫ 32；
- deadline 增量 = `ceil(ns × ceil(freq·2^32/1e9) / 2^32)`。

热路径使用 64×64→128 乘法，不依赖运行期 128 位除法。乘数向上取整的绝对系数误差小于 `2^-32`，相对误差取决于 frequency，不能把两者混同。deadline 最终也向上取整，避免不足一个 tick 的请求被提前唤醒。

syscall 将有效的非负 timespec 饱和到 `INT64_MAX` 纳秒，比较目标是否已经过去时使用无符号绝对时间；转换后的未来 tick 距离限制在 `INT64_MAX` 内，再使用有符号回绕差值比较。0 是“没有 deadline”的保留值。原始时钟接口仍是 64 位纳秒表示，不宣称已实现数百年运行后的时钟回绕管理。

## 时钟语义

- `kernel_time_monotonic_ns()`：自启动的单调纳秒。
- `kernel_time_realtime_ns()`：启动时 RTC 读数加单调值；不在运行期重读 RTC，也没有 settimeofday/adjtime 语义。
- `kernel_time_boot_realtime_offset()`：暴露启动 RTC 读数，供绝对 REALTIME deadline 换算到单调域。
- `kernel_time_deadline_from_monotonic()`：把单调域目标时间换成 `time` CSR 绝对 deadline；目标不在未来时返回 `DEADLINE_PASSED`。
- `kernel_time_coarse_monotonic_ns()` / `kernel_time_coarse_realtime_ns()`：只读取初始化和最近一次 timer interrupt 发布的样本，不在查询时读取 counter。单个原子 monotonic 样本加不可变 RTC offset，保证两种 coarse 时间来自同一采样域，避免分开发布两套状态。

`kernel_time_init` 返回前就发布首个有效样本。运行期 `kernel_time_update_coarse` 每次读取真实 counter；延迟或合并多个 tick 时只采一次实际时间，不重复增加虚构的 10ms。当前 timer 为 `KERNEL_TICKS_PER_SECOND=100`，`kernel_time_coarse_resolution_ns()` 因而返回 10,000,000ns。分辨率表示正常更新周期，不承诺关中断或阻塞 timer delivery 时快照的最大陈旧时间；不会把 fine 时钟截断成 10ms 冒充 timer 快照。

CLOCK_REALTIME 的墙钟完全取决于启动时的 RTC 探测。goldfish RTC 读数必须两次采样非递减、间隔小于 1 秒且不早于 2020-01-01，否则视为无 RTC，CLOCK_REALTIME 退化为自启动相对时间（固定从epoch 起算的值不可得）。DTB 解析 RTC 节点是演进路径；当前地址按 `virt_uart.h` 先例硬编码 `0x101000`。

## 消费者与验证

syscall 层的 `clock_gettime(113)`、`clock_getres(114)` 支持 `CLOCK_REALTIME`、`CLOCK_MONOTONIC` 和两种 `COARSE`（5、6），其余 clockid（含 CPU-time 时钟）返回 `-EINVAL`。coarse getres 如实返回 10ms，NULL 输出成功，坏输出指针返回 EFAULT。`gettimeofday(169)` 仍使用 fine realtime。

睡眠时钟校验独立于可读时钟：`clock_nanosleep(115)` 的 coarse 时钟返回 `-EOPNOTSUPP`，在读取用户 timespec 前拒绝；不把它们接入 sleep deadline 换算。其余既有 `CLOCK_REALTIME` / `CLOCK_MONOTONIC` 睡眠和 `nanosleep(101)` 保持不变。

`make test-syscall-riscv` 覆盖换算、coarse 初始化一致性、无 timer 时的稳定读数、延迟采样与 10ms 分辨率；`tests/diff-abi/coarse.c` 覆盖真实 U-mode timer 推进、查询错误和拒绝 coarse sleep。固定 Linux 必须启用 `CONFIG_POSIX_TIMERS` 才能对照 raw coarse syscall；Linux HZ=250 的 4ms 与 BoarOS HZ=100 的 10ms 分辨率按各自真实周期验证，不要求裸值相等。`make test-userland-riscv` 用 musl 验证真实 trap→expire→wake 闭环和 RTC 墙钟。

当前限制：无 NTP/阶跃调整、无 CPU-time clockid；timekeeper只在启动采样RTC，此后由CSR换算推进，用户RTC_RD_TIME仍读取设备当前值；无 SMP timekeeper 写入协议。

fine时钟和deadline换算精度不等于到期唤醒精度。普通blocked deadline由timer路径
检查；`kernel/sched/scheduling.c::scheduler_rearm_timer`的架构timer事件取min(实时预算、
RR时间片、最早睡眠deadline)，到期由按(deadline, tid)排序的索引只弹出已到期者，
短睡眠不再固定等到下一次100Hz tick。IRQ延迟、关中断段和更高优先级任务仍会延后
实际运行，不宣称硬上界；提高HZ的取舍不再用于唤醒精度。

`times(153)`与proc的user/kernel统计来自timer采样：trap按当时的U/S状态记账，
不是逐段结算的运行时间。频繁短syscall、关中断与延迟tick会影响归属，不能把
`tms_stime=0`解释成没有内核工作；CPU-time clock或PROF timer扩展前需核对该边界。

Goldfish的真实墙钟另由10:135 RTC字符后端提供只读RTC_RD_TIME；UTC转换、OFD独占和限制见[文件设备](kernel-files.md#仅读rtc与ofd设备资格)。这没有增加RTC写入或clock_settime能力。

## 应用 alarm 与进程组实时时间定时器

原 netperf 2.7.0 用 libc `alarm()` 结束定时发送。RV64 libc 经 `setitimer(103)`
实现它；缺少这个接口会让 UDP_STREAM 一直发送，不能用外部超时替代程序完成。
`getitimer(102)` / `setitimer(103)` 已接入 ITIMER_REAL：相对单调时间到期后发送
进程定向 SIGALRM（SI_KERNEL），一次性或周期性。周期在信号被消费时重设，
阻塞的标准信号只保留一笔 pending，不累积每个周期的事件。

定时状态归线程组身份，不是设置它的线程。只有已启用的定时器进入期限有序的
非持引用链；timer interrupt 只检查到期项。fork 不继承，exec 保留；非组长 exec
转移状态与链资格，组长线程退出而其他成员存活时仍可交付，最后成员退出时摘链。
查询/变更在短 IRQ 临界区完成，用户复制在外侧。Linux 先安装新值再复制旧值，
因此旧值输出 EFAULT 不回滚新 alarm；NULL 新值按固定 Linux 取消。时间取整与
100 Hz timer delivery 的界限如上，不宣称高精度到期交付。

依据本地 `references/linux/kernel/time/itimer.c`（Linux 7.2，版本见来源清单）。
ITIMER_VIRTUAL / ITIMER_PROF 的 CPU 时间定时器仍返回 ENOSYS；未知 which 返回
EINVAL。重建 `python3 -B tests/network-riscv.py --workload timer`：同 ELF 对照
Linux，核对阻塞 read 的 EINTR、周期、坏参数/指针、fork、线程组长退出及非组长
exec 后的交付和最终物理页/堆回收。

## LA LS7A RTC

固定 QEMU 地址是物理 `0x100d0100`，通过平台 uncached DMW 访问。启动 owner
将 TOYTRIM 写零、CTRL.TEN/EO 置位，然后读取完整 READ1/READ0 两次；一致才接受，
最多八次，前后检查控制位。`tm_year`、日期、闰年、时间和纳秒表示范围都验证，失败
不改写输出，也不分配资源；查询不能重新启用被关闭的 RTC。初始化失败明确记录
unavailable，realtime offset 为零。固定 QEMU 的 TOY 读数只有整秒，按 **1 秒**记录
实际 RTC 分辨率；fine CLOCK_REALTIME 的运行期增量仍来自 CPU counter。

共用 RTC 字符设备仍只提供 RTC_RD_TIME、10:135 别名和 OFD 独占。BoarOS 写时钟、
告警和事件 read 返回既有 ENOTSUP；派生模拟器的 Linux 告警验证不改变这个用户接口。
`make test-rtc-loongarch-host test-environment-loongarch` 覆盖日期失败、真实 UTC、
OFD/fork/exec/错误指针和原 BusyBox hwclock/dmesg/df；两项清日志测试各用独立启动。
派生 QEMU 与固定 Linux 告警的验证见 [LA 平台](loongarch-boot.md#ls7a-rtc-与派生模拟器)。
