# 内核时间模块

本文描述架构无关时间子系统的稳定接口：单调时钟、实时时钟和睡眠 deadline 换算。构建期 timer 操作见 `include/arch/timer.h`，RV timebase 与 SBI 定时器见 [RISC-V Timer 模块](riscv-timer.md)，等待队列与阻塞唤醒见[内核调度与进程生命周期模块](kernel-scheduler.md)。

## 范围与入口

| 文件 | 当前职责 |
|---|---|
| `include/kernel/time.h`、`kernel/time.c` | 定点乘数换算、软件时钟频率与相位校准、单调/实时时钟、timer 驱动 coarse 快照、运行时墙钟与时区、deadline 换算 |
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
- `kernel_time_realtime_ns()`：当前墙钟偏移加单调值；偏移由启动 RTC 初始化，可由 `clock_settime/settimeofday` 阶跃修改，不在运行期重读 RTC。
- `kernel_time_boot_realtime_offset()`：返回当前 REALTIME 与 MONOTONIC 的偏移。函数沿用原名，但返回值不再是不可变的启动读数。
- `kernel_time_deadline_from_monotonic()`：把单调域目标时间换成 `time` CSR 绝对 deadline；目标不在未来时返回 `DEADLINE_PASSED`。
- `kernel_time_coarse_monotonic_ns()` / `kernel_time_coarse_realtime_ns()`：只读取初始化和最近一次 timer interrupt 发布的样本，不在查询时读取 counter。单个原子 monotonic 样本加当前墙钟偏移，偏移与读取计数器的组合通过发布序列复查，避免在调钟中间拼出不一致的时间。

`kernel_time_init` 返回前就发布首个有效样本。运行期 `kernel_time_update_coarse` 每次读取真实 counter；延迟或合并多个 tick 时只采一次实际时间，不重复增加虚构的 10ms。当前 timer 为 `KERNEL_TICKS_PER_SECOND=100`，`kernel_time_coarse_resolution_ns()` 因而返回 10,000,000ns。分辨率表示正常更新周期，不承诺关中断或阻塞 timer delivery 时快照的最大陈旧时间；不会把 fine 时钟截断成 10ms 冒充 timer 快照。

CLOCK_REALTIME 的初始墙钟来自启动 RTC 探测，运行期可由有权限的程序设置。goldfish RTC 读数必须两次采样非递减、间隔小于 1 秒且不早于 2020-01-01，否则视为无 RTC，CLOCK_REALTIME 退化为自启动相对时间（固定从epoch 起算的值不可得）。DTB 解析 RTC 节点是演进路径；当前地址按 `virt_uart.h` 先例硬编码 `0x101000`。

## 运行时调钟与绝对等待

`clock_settime(112)` 当前只设置 CLOCK_REALTIME；`settimeofday(170)` 支持 timeval 和可选 timezone。两者要求当前线程具有 CAP_SYS_TIME。时间输入采用原生 64 位秒及纳秒／微秒，遵循固定 Linux 的有效范围，并拒绝把墙钟设置到当前 MONOTONIC 之前。MONOTONIC、BOOTTIME、CPU 时钟、相对睡眠与 ITIMER_REAL 不随调钟改变。

clock_settime 先检查时钟编号，再导入并验证 timespec，最后检查权限。settimeofday 先导入 timeval 并检查微秒，再导入 timezone，检查时间范围与权限，随后检查 minuteswest 在 -900～900 内。timezone 先发布，因此后续墙钟下限检查失败不撤销时区。第一次成功提供 timezone 时，若没有 timeval，则按 minuteswest 将软件墙钟调整一次；同时提供 timeval 只消费首次资格、不执行额外调整。dsttime 原样保存；此接口不配置地域时区数据库。

调钟使用一个任务互斥串行完成发布和各期限索引调整。现有调度短保护内同时提交新偏移、更新 vDSO 数据并重排已提交的绝对 REALTIME 等待；记录保存原始墙钟期限，尚未阻塞的请求在提交时重新换算。MQ、futex BITSET/waitv/PI 及绝对 clock_nanosleep 因而不需要撤销登记再重排队，也不会把调钟冒充实际唤醒。相对请求继续使用 MONOTONIC。信号重启保留原时钟域和期限；处理函数可观察的 EINTR 与无处理函数的自动重启规则不变。

timerfd 和 POSIX 墙钟索引在各自已有保护内更新，通知和信号投递在保护外完成；远端修改通过既有 deadline IPI 让 CPU0 重新编程事件。期限处理再次核对有效时钟，覆盖偏移已经提交、对象索引尚未更新时的倒拨。正常时钟查询不取得调钟互斥，也不扫描等待者。硬件实时时钟只在启动时提供初始墙钟。

功能消费者为 `python3 -B tests/userland/api8.py --program system_control --arch riscv --libc glibc --only boaros`，LA 使用 `--arch loongarch`。它通过实际 BLOCKED 状态握手检查等待期间调钟，并直接调用 vDSO 符号与 syscall 对照，不以 libc 回退掩盖入口缺失。

## 软件时钟校准

`adjtimex(171)` 和 `clock_adjtime(266, CLOCK_REALTIME)` 使用相同的 208 字节原生 `timex` 结构。零模式查询和 `ADJ_OFFSET_SS_READ` 余量查询向普通用户开放，修改要求系统校时能力 `CAP_SYS_TIME`。两种接口都先从用户空间读取整个结构。`clock_adjtime` 随后区分未知时钟编号（`EINVAL`）和可查询但缺少校时方法的时钟（`EOPNOTSUPP`）。

原始硬件时间构成 `CLOCK_MONOTONIC_RAW`。内核以最近一次修改参数时的一对原始时间与单调时间为基准，把此后的计数增量换算为校准后的单调时间。频率参数采用百万分之一的 1/65536 为单位，范围为 ±500 ppm（parts per million，百万分比）。`ADJ_TICK` 可在每百分之一秒 9000 至 11000 微秒之间设置名义时长。网络时间协议（Network Time Protocol，NTP）的锁相环（phase-locked loop，PLL）按时间常数逐秒衰减剩余相位，在采样间隔较长时还可使用锁频环（frequency-locked loop，FLL）更新频率。单次渐调独立保存尚待施加的有符号微秒数，每秒最多消耗 500 微秒。各项校正以定点数相加后统一取整，保证负频率与负相位同时生效时单调时钟仍向前推进。

`ADJ_MAXERROR` 和 `ADJ_ESTERROR` 设置最大误差与估计误差。最大误差按每秒 500 微秒累积，超过 16 秒时进入未同步状态。查询用已经过去的秒数一次计算累计量。成功调用返回同步状态 `TIME_OK`（0）或 `TIME_ERROR`（5，时钟未同步或请求的每秒脉冲信号不可用）。`ADJ_SETOFFSET` 以秒和非负秒内偏移之和调整墙钟，清除尚未施加的渐调和相位，并将时钟标为未同步。

参数修改时，调度器根据等待记录保存的时钟域和目标纳秒，重新计算已提交等待的硬件期限。`ppoll`、`pselect6`、两种 `epoll_pwait` 和 `semtimedop` 在调用时把相对时限换算为单调时钟的绝对目标，后续重新登记仍使用同一目标。定时器文件描述符和可移植操作系统接口（Portable Operating System Interface，POSIX）定时器分别重排自己的期限索引。只有墙钟阶跃才会向设置了 `TFD_TIMER_CANCEL_ON_SET` 的定时器发出取消通知。虚拟动态共享对象（virtual dynamic shared object，vDSO）与系统调用使用同一套时钟换算公式和序列检查。调度执行预算与中央处理器（central processing unit，CPU）运行时间始终使用原始计数。计时中断沿用已有粗粒度时钟采样与到期处理，校准不增加周期性任务遍历。

`tests/host/clock_discipline.c` 用可控计数器检查渐调余量、误差增长、组合校正的单调性和逆向换算。`tests/userland/clock-discipline.c` 检查两种系统调用、权限、vDSO 查询、调速前已阻塞的睡眠和定时器，以及墙钟阶跃取消通知。闰秒和国际原子时（International Atomic Time，TAI）偏移设置返回 `EOPNOTSUPP`。

## 用户态 vDSO

RV64 和 LA64 的 exec 映射一个只读可执行 ELF 页和紧随其后的只读 `[vvar]` 页。auxv 的 `AT_SYSINFO_EHDR` 指向 ELF 头；三个时间符号 `__vdso_clock_gettime`、`__vdso_clock_getres`、`__vdso_gettimeofday` 分别采用 RV 的 `LINUX_4.15` 与 LA 的 `LINUX_5.10` 版本。信号处理器的返回地址指向同一 ELF 内的 `__vdso_rt_sigreturn`。

vDSO 的独立 C 编译规则同样启用 `-fstack-usage`，生成 `vdso/time.su`；内核构建目录的完整栈检查要求该对象有报告。报告缺失是构建覆盖错误，不表示 vDSO ELF 或运行时函数不存在。该编译选项只输出用量记录，不改变 vDSO 的代码或用户态 ABI。

REALTIME、MONOTONIC、BOOTTIME 和 MONOTONIC_RAW 直接读取用户可用的架构计数器。前三种应用共享校准参数，RAW 只使用硬件换算乘数。两种 COARSE 读取 CPU0 最近发布的原子样本；CPU 时钟和其他时钟编号交给 syscall。vDSO 在用户态写出结果，因此错误输出地址按普通用户内存访问处理。乘数在初始化后不变；调钟和时区修改更新共享数据页的序列，vDSO 在同一序列内读取偏移和 counter，发现写者介入则重读。内核 REALTIME 查询同样复查序列。计时中断只更新 coarse 单调样本，不遍历进程。

各 MM 共享一页时钟数据，每个映射 PTE 与 MM 的恢复后备分别持有引用。fork 继承后备，`MADV_DONTNEED` 只丢弃驻留映射；再次访问时，代码从内核 ELF 重新装入，数据重新接入同一时钟页。页表销毁后归还 MM 的后备引用，最后一个 MM 释放时先停止时间写者对该页的访问。

直接检查：`python3 -B tests/userland/run.py --workload vdso_time --arch riscv --cpus 1 --memory 512M --libc glibc`，LA 用 `--arch loongarch`。该程序检查 auxv、符号版本、各时钟的取值及分辨率、线程 CPU 时钟回退与 BOOTTIME 查询、fork 后丢页再访问和信号返回。

## 消费者与验证

syscall 层的 `clock_gettime(113)`、`clock_getres(114)` 支持 `CLOCK_REALTIME`、`CLOCK_MONOTONIC`、`CLOCK_MONOTONIC_RAW`（4）、`CLOCK_BOOTTIME`（7）、两种 `COARSE`（5、6）及[进程/线程 CPU 时钟](kernel-accounting.md#cpu-时钟)。RAW 返回原始硬件时间，MONOTONIC 返回校准后的时间。RAW 支持查询。系统持续运行期间，BOOTTIME 和 MONOTONIC 返回同一时钟域的时间，两者分辨率均为 1ns。未知 clockid 返回 `-EINVAL`。coarse getres 如实返回 10ms，NULL 输出成功，坏输出指针返回 EFAULT。`gettimeofday(169)` 使用 fine realtime；依次写出 timeval 和可选 timezone，后者返回系统保存的 timezone，两字段初始均为零。先前输出已成功时，后续用户地址错误返回 EFAULT 并保留该输出。

`clock_nanosleep(115)` 支持 REALTIME、MONOTONIC、进程 CPU 时钟及编码后的其他线程 CPU 时钟。CPU 睡眠在调用者 CPU 登记和返回，复用 POSIX CPU 期限索引，按目标实际执行时间到期；不建立用户可见 timer ID，也不预留信号队列额度。内建 CLOCK_THREAD_CPUTIME_ID、MONOTONIC_RAW 与 coarse 时钟在读取 timespec 前返回 EOPNOTSUPP，编码后的本线程时钟返回 EINVAL。进程时钟可以由同组其他线程推进；只有睡眠线程且没有其他执行者时，CPU 时间不会因墙钟流逝增加。

CPU 睡眠对象由调用线程持有，期限检查和取消在共同调度保护内交接；到期直接完成等待，强制退出先摘除期限，再释放请求。目标退出只摘除其期限，不将退出当成到期；后续信号中断取消睡眠。相对睡眠中断报告剩余量，绝对睡眠不改 remaining；相对睡眠重启保留原 CPU 到期期限，绝对睡眠经 stop/continue 后重新读取用户请求。普通墙钟睡眠沿原期限路径。

`tests/diff-abi/coarse.c` 覆盖真实 U-mode timer 推进、查询错误和拒绝 coarse sleep。固定 Linux 必须启用 `CONFIG_POSIX_TIMERS` 才能对照 raw coarse syscall；Linux HZ=250 的 4ms 与 BoarOS HZ=100 的 10ms 分辨率按各自真实周期验证，不要求裸值相等。`make test-userland-riscv` 用 musl 验证真实 trap→expire→wake 闭环和 RTC 墙钟。

硬件实时时钟只在启动时为软件墙钟提供初值。用户通过 `RTC_RD_TIME` 仍可读取设备当前值。多处理器系统的粗粒度时钟样本由 CPU0 发布。

fine时钟和deadline换算精度不等于到期唤醒精度。普通blocked deadline由timer路径
检查；`kernel/sched/scheduling.c::scheduler_rearm_timer`的架构timer事件取min(实时预算、
RR时间片、最早睡眠deadline)，到期由按(deadline, tid)排序的索引只弹出已到期者，
短睡眠不再固定等到下一次100Hz tick。IRQ延迟、关中断段和更高优先级任务仍会延后
实际运行，不宣称硬上界；提高HZ的取舍不再用于唤醒精度。

任务CPU执行时间、`times`及等待返回的rusage使用共同统计，执行边界、单位和归属见[资源记账](kernel-accounting.md)。

Goldfish的真实墙钟另由10:135 RTC字符后端提供只读RTC_RD_TIME；UTC转换、OFD独占和限制见[文件设备](kernel-files.md#仅读rtc与ofd设备资格)。软件墙钟设置不修改 RTC 硬件，RTC 字符设备仍只读。

## 应用 alarm 与进程组实时时间定时器

原 netperf 2.7.0 用 libc `alarm()` 结束定时发送。RV64 libc 经 `setitimer(103)`
实现它；缺少这个接口会让 UDP_STREAM 一直发送，不能用外部超时替代程序完成。
`getitimer(102)` / `setitimer(103)` 已接入 ITIMER_REAL：相对单调时间到期后发送
进程定向 SIGALRM（SI_KERNEL），一次性或周期性。周期在信号被消费时重设，
阻塞的标准信号只保留一笔 pending，不累积每个周期的事件。

定时状态归线程组身份，不是设置它的线程。只有已启用的定时器进入期限有序的
非持引用链；timer interrupt 只检查到期项。fork 不继承，exec 保留；非组长 exec
转移状态与链资格，组长线程退出而其他成员存活时仍可交付，最后成员退出时摘链。
查询和变更在调用者所在核取得调度状态保护，一次提交链中位置和最早期限，用户复制在保护外完成。
时钟中断用期限摘要跳过空队列和未到期的项。取走到期项时先借用线程组长的存储，
释放调度状态保护后再递送信号，覆盖并行撤销和组退出。Linux 先安装新值再复制旧值，
因此旧值输出 EFAULT 不回滚新 alarm；NULL 新值按固定 Linux 取消。时间取整与
100 Hz timer delivery 的界限如上，不宣称高精度到期交付。

未知 which 返回EINVAL。重建 `python3 -B tests/userland/run.py --workload timer`：同 ELF 对照
Linux，核对阻塞 read 的 EINTR、周期、坏参数/指针、fork、线程组长退出及非组长
exec 后的交付和最终物理页/堆回收。

### 进程 CPU 间隔定时器

`ITIMER_VIRTUAL`累计全组用户执行时间，发送SIGVTALRM；`ITIMER_PROF`累计用户与内核执行时间，发送SIGPROF。两者使用[共同CPU记账](kernel-accounting.md)，含退出成员和各CPU尚未结算的执行片段，睡眠及IRQ不计入。状态由组身份持有，设置线程退出后继续；fork清零，exec保留，非组长exec迁移，最后成员清理时摘除活动索引。REAL、VIRTUAL、PROF相互独立。

CPU0在既有timer路径只遍历具有活动CPU期限或有限CPU限额的目标；进程目标与POSIX CPU定时器共用一次组用量快照，并在短调度域内按各CPU的真实`accounting_owner`判断该组是否正在执行。整组睡眠时保留CPU阈值；恢复执行后再检查，避免微秒周期的历史欠账按墙钟继续产生信号。用量快照在调度域外按现有成员借用与CPU发布序列取得。未启用时，系统调用进出与CPU本地记账保持原路径；空索引只在原timer处理处检查。

按固定Linux的`kernel/time/itimer.c`与`posix-cpu-timers.c`，首次到期额外加一个实际100Hz记账tick；每次到期的周期阈值增加原interval，标准信号按pending位合并。CPU定时器停用时保留interval查询值，过期尚待处理时返回一个tick的剩余量。输出旧值EFAULT保留已安装的新值。CPU0处理时机和IRQ延迟决定实际递送时刻。

`make test-itimer-abi-riscv test-itimer-abi-loongarch TEST_MEMORIES="512M 1G"`检查三类独立性、部分失败、睡眠、微秒周期、信号合并、真实handler及默认终止、设置者/组长退出和exec迁移。`make test-itimer-smp-riscv QEMU_RISCV_FIRMWARE=/absolute/fw_dynamic.bin`另验证真实两核：各线程分别不足180ms，合计超过阈值时进程定时器触发，同时保留单核对照。

### CPU 执行限额

`RLIMIT_CPU` 默认软、硬限额均无限，按全组累计用户加内核时间（PROF）执行。组内共享，fork 复制限额但从子进程自己的 CPU 用量开始，exec 保留；非组长 exec 随组身份转移。软限额到达时递送 SIGXCPU，并将软值提高一秒；硬限额先于软限额判断，到达后以 SIGKILL 终止全组。下调限额保留已用时间，在下一次 CPU 检查时处理。

有限软值加入现有进程 CPU 活动索引，与 itimer、POSIX timer 共用一次组用量采样。停用所有 CPU 期限且限额恢复无限后摘除；普通 syscall 进出不增加限额采样。实际递送精度受原 timer 周期、IRQ 延迟和执行调度影响。`tests/userland/run.py --workload cpu` 用同 ELF 对照 Linux，覆盖限额、线程/exec、CPU 睡眠及 getcpu。

## LA LS7A RTC

固定 QEMU 地址是物理 `0x100d0100`，通过平台 uncached DMW 访问。启动 owner
将 TOYTRIM 写零、CTRL.TEN/EO 置位，然后读取完整 READ1/READ0 两次；一致才接受，
最多八次，前后检查控制位。`tm_year`、日期、闰年、时间和纳秒表示范围都验证，失败
不改写输出，也不分配资源；查询不能重新启用被关闭的 RTC。初始化失败明确记录
unavailable，realtime offset 为零。固定 QEMU 的 TOY 读数只有整秒，按 **1 秒**记录
实际 RTC 分辨率；fine CLOCK_REALTIME 的运行期增量仍来自 CPU counter。

共用 RTC 字符设备仍只提供 RTC_RD_TIME、10:135 别名和 OFD 独占。RTC 硬件写入、
告警和事件 read 返回既有 ENOTSUP；派生模拟器的 Linux 告警验证不改变这个用户接口。
`make test-rtc-loongarch-host test-environment-loongarch` 覆盖日期失败、真实 UTC、
OFD/fork/exec/错误指针和原 BusyBox hwclock/dmesg/df；两项清日志测试各用独立启动。
派生 QEMU 与固定 Linux 告警的验证见 [LA 平台](loongarch-boot.md#ls7a-rtc-与派生模拟器)。

## timerfd期限

`timerfd_create/settime/gettime`提供MONOTONIC和REALTIME匿名OFD。设置、查询、计数读取和等待由发起CPU执行；CPU0在真实到期中断中发布可读状态。已启用对象进入侵入式AVL期限索引，索引和对象状态由同一短锁保护。发布的最早tick期限供调度器读取，调度锁内不再进入索引锁；远端更新通过既有deadline IPI要求CPU0重装硬件事件。空索引只有常数空检查。

首次到期只记一次并通知。周期在read/gettime中按原绝对期限累计漏过次数、前移和重装，保留周期相位，避免短周期造成密集IRQ。settime完整导入输入后交换设置，输出旧值EFAULT不撤销新设置。timerfd read消费计数后复制，部分输出返回实际完成前缀，区别于eventfd的EFAULT返回。

末次关闭从索引摘除对象后，等待已经取得的到期通知借用和等待游标结束，再归还对象。dup/fork共享设置和计数。BOOTTIME、ALARM时钟仍在能力目录。相对 REALTIME 设置使用 MONOTONIC，绝对 REALTIME 设置随墙钟偏移重新计算期限。

REALTIME、ABSTIME、CANCEL_ON_SET 三者同时成立时登记取消通知，即使当前已停用。偏移改变后发布可读状态，下一次 read 返回一次 ECANCELED 并消费取消标记；底层尚未到期的计时器继续运行，后续调钟仍可再次通知。未消费取消时重新设置非零期限，先安装新设置，再返回 ECANCELED，不输出旧值；零期限不消费旧取消标记。取消资格被移除则不返回该错误。周期计时器到期后倒拨到原期限之前，read 可返回零字节并在原期限重装，不凭空前移一周期。

## POSIX 定时器对象

RV64/LA64使用107～111号入口和原生64字节sigevent、32字节itimerspec布局；用户态timer ID为32位，libc的timer_t由库自行封装。线程组按需持有64桶注册表，单调分配非负ID，回绕时检查冲突。注册表与共享disposition分开，线程退出不撤销组定时器；fork/vfork不继承，成功exec和最终组清理释放对象，失败exec保留。

创建为每只定时器预留通知记录和SIGPENDING额度，SIGEV_NONE同样计费。需要通知时先准备真实目标的pending索引。创建候选在ID输出成功后才发布，用户输出故障归还候选与额度；其他线程查询尚未发布的ID返回EINVAL。普通信号仍在消费时归还额度，预留额度由定时器对象持有。对象和PID身份的最后归还在状态保护外释放内存。

REALTIME、MONOTONIC以及进程/线程CPU时钟均在调用CPU创建、设置、查询和删除，由CPU0推进到期。CPU clock目标解析与查询共用，保留PROF、VIRT、SCHED编码，分别使用用户加内核、仅用户、用户加内核时间。SIGEV_SIGNAL和原生SIGEV_THREAD向组投递，SIGEV_THREAD_ID绑定真实线程身份；libc的SIGEV_THREAD通过库的辅助线程完成回调。

每个任务尾部内嵌进程及线程阈值索引，仅活动目标进入共同AVL；每目标按三种CPU时间维护最早阈值，旧VIRTUAL/PROF与原生进程定时器共用一次组采样。遍历按PID代次和角色递增，任务与PID借用覆盖锁外采样，回来后重新核对身份、设置代次和阈值。空集合在原timer路径只检查摘要，普通trap保持既有记账路径。CPU目标退出时摘除阈值，其他组持有的句柄继续有效：gettime归零、settime返回ESRCH，delete归还资源。非组长exec在TGID角色交接时移交进程阈值，创建者、通知目标和CPU时钟目标分别保活。

通知记录区分普通分配和原生定时器预留：每只定时器最多有一条待决记录，不同定时器可同时使用同一标准信号。到期只使用创建时准备的存储。普通内联事件与排队记录共存时，索引保留内联事件的先后位置；原生记录消费不重复计费。用户提供的SI_TIMER仍是普通载荷，不据此执行重装。

相对 REALTIME 设置的有效时钟为 MONOTONIC；只有绝对 REALTIME 设置随墙钟变化。REALTIME/MONOTONIC使用共享AVL算法和各自对象保护，调度器只读取最早期限摘要，远端提前设置沿既有deadline IPI通知CPU0。消费先在调度保护内取得记录及原设置快照，锁外计算进度，再核对设置代次决定重装；处理函数、sigwaitinfo和signalfd取得同一份最终overrun。删除或重设后的旧事件被丢弃，已取得的消费借用覆盖最后释放。周期按原期限推进，不按处理完成时刻重新起算。单次到期入队后查询剩余量为零，待决记录仍保留到消费或清除。STOP/CONT沿共同进程组流程清理相反待决信号并恢复停止组；KILL请求整组退出，这些组操作在状态保护外执行。

已排队或已领取的信号不因调钟改变设置代次或消失。周期推进允许零次；pending/claim 保留已经观察到的推进位置和累计 overrun，避免“前拨、gettime、倒拨、消费”丢失已累计的次数。

SIG_IGN的周期事件停放并保留相位，恢复disposition时移回待决队列；单独屏蔽不会重新投递已经停放的事件。SIGEV_NONE只按查询推进，停用后的旧单次期限仍可能返回剩余量，沿固定Linux的common_timer_get行为。timer_settime只解释TIMER_ABSTIME位，其余位按参考Linux忽略；NULL新值返回EINVAL，旧值输出EFAULT保留新设置。exec清除原生及用户提供的SI_TIMER记录，保留其他排队消息的顺序。

`make test-posix-timer-host`以4/16KiB页检查真实通知记录、额度、借用、代次及CPU阈值；`tests/userland/run.py --workload posix`提供两架构与固定Linux的原生ABI、真实handler/libc回调和生命周期检查。四核观察构建复用composition观察点核对调用CPU与CPU0到期位置。相同短例还以官方静态glibc在两架构的Linux/BoarOS运行，核对libc真实SIGEV_THREAD回调、输出EFAULT后的通知消费及输入输出别名。CPU时钟反例按被测时钟推进工作量，VIRT使用用户时间，PROF/SCHED使用合计时间。
