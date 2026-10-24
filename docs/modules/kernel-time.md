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

syscall 层的 `clock_gettime(113)`、`clock_getres(114)` 支持 `CLOCK_REALTIME`、`CLOCK_MONOTONIC`、两种 `COARSE`（5、6）及[进程/线程 CPU 时钟](kernel-accounting.md#cpu-时钟)。未知 clockid 返回 `-EINVAL`。coarse getres 如实返回 10ms，NULL 输出成功，坏输出指针返回 EFAULT。`gettimeofday(169)` 使用 fine realtime；依次写出 timeval 和可选 timezone，后者为 UTC 的两个零值。先前输出已成功时，后续用户地址错误返回 EFAULT 并保留该输出。

`clock_nanosleep(115)` 支持 REALTIME、MONOTONIC、进程 CPU 时钟及编码后的其他线程 CPU 时钟。CPU 睡眠在调用者 CPU 登记和返回，复用 POSIX CPU 期限索引，按目标实际执行时间到期；不建立用户可见 timer ID，也不预留信号队列额度。内建 CLOCK_THREAD_CPUTIME_ID 与 coarse 时钟在读取 timespec 前返回 EOPNOTSUPP，编码后的本线程时钟返回 EINVAL。进程时钟可以由同组其他线程推进；只有睡眠线程且没有其他执行者时，CPU 时间不会因墙钟流逝增加。

CPU 睡眠对象由调用线程持有，期限检查和取消在共同调度保护内交接；到期直接完成等待，强制退出先摘除期限，再释放请求。目标退出只摘除其期限，不将退出当成到期；后续信号中断取消睡眠。相对睡眠中断报告剩余量，绝对睡眠不改 remaining；相对睡眠重启保留原 CPU 到期期限，绝对睡眠经 stop/continue 后重新读取用户请求。普通墙钟睡眠沿原期限路径。

`make test-syscall-riscv` 覆盖换算、coarse 初始化一致性、无 timer 时的稳定读数、延迟采样与 10ms 分辨率；`tests/diff-abi/coarse.c` 覆盖真实 U-mode timer 推进、查询错误和拒绝 coarse sleep。固定 Linux 必须启用 `CONFIG_POSIX_TIMERS` 才能对照 raw coarse syscall；Linux HZ=250 的 4ms 与 BoarOS HZ=100 的 10ms 分辨率按各自真实周期验证，不要求裸值相等。`make test-userland-riscv` 用 musl 验证真实 trap→expire→wake 闭环和 RTC 墙钟。

当前限制：无 NTP/阶跃调整；POSIX 定时器的期限推进见下节；timekeeper只在启动采样RTC，此后由CSR换算推进，用户RTC_RD_TIME仍读取设备当前值；SMP的coarse快照由CPU0发布。

fine时钟和deadline换算精度不等于到期唤醒精度。普通blocked deadline由timer路径
检查；`kernel/sched/scheduling.c::scheduler_rearm_timer`的架构timer事件取min(实时预算、
RR时间片、最早睡眠deadline)，到期由按(deadline, tid)排序的索引只弹出已到期者，
短睡眠不再固定等到下一次100Hz tick。IRQ延迟、关中断段和更高优先级任务仍会延后
实际运行，不宣称硬上界；提高HZ的取舍不再用于唤醒精度。

任务CPU执行时间、`times`及等待返回的rusage使用共同统计，执行边界、单位和归属见[资源记账](kernel-accounting.md)。

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

未知 which 返回EINVAL。重建 `python3 -B tests/network-riscv.py --workload timer`：同 ELF 对照
Linux，核对阻塞 read 的 EINTR、周期、坏参数/指针、fork、线程组长退出及非组长
exec 后的交付和最终物理页/堆回收。

### 进程 CPU 间隔定时器

`ITIMER_VIRTUAL`累计全组用户执行时间，发送SIGVTALRM；`ITIMER_PROF`累计用户与内核执行时间，发送SIGPROF。两者使用[共同CPU记账](kernel-accounting.md)，含退出成员和各CPU尚未结算的执行片段，睡眠及IRQ不计入。状态由组身份持有，设置线程退出后继续；fork清零，exec保留，非组长exec迁移，最后成员清理时摘除活动索引。REAL、VIRTUAL、PROF相互独立。

CPU0在既有timer路径只遍历具有活动CPU期限或有限CPU限额的目标；进程目标与POSIX CPU定时器共用一次组用量快照，并在短调度域内按各CPU的真实`accounting_owner`判断该组是否正在执行。整组睡眠时保留CPU阈值；恢复执行后再检查，避免微秒周期的历史欠账按墙钟继续产生信号。用量快照在调度域外按现有成员借用与CPU发布序列取得。未启用时，系统调用进出与CPU本地记账保持原路径；空索引只在原timer处理处检查。

按固定Linux的`kernel/time/itimer.c`与`posix-cpu-timers.c`，首次到期额外加一个实际100Hz记账tick；每次到期的周期阈值增加原interval，标准信号按pending位合并。CPU定时器停用时保留interval查询值，过期尚待处理时返回一个tick的剩余量。输出旧值EFAULT保留已安装的新值。CPU0处理时机和IRQ延迟决定实际递送时刻。

`make test-itimer-abi-riscv test-itimer-abi-loongarch TEST_MEMORIES="512M 1G"`检查三类独立性、部分失败、睡眠、微秒周期、信号合并、真实handler及默认终止、设置者/组长退出和exec迁移。`make test-itimer-smp-riscv QEMU_RISCV_FIRMWARE=/absolute/fw_dynamic.bin`另验证真实两核：各线程分别不足180ms，合计超过阈值时进程定时器触发，同时保留单核对照。

### CPU 执行限额

`RLIMIT_CPU` 默认软、硬限额均无限，按全组累计用户加内核时间（PROF）执行。组内共享，fork 复制限额但从子进程自己的 CPU 用量开始，exec 保留；非组长 exec 随组身份转移。软限额到达时递送 SIGXCPU，并将软值提高一秒；硬限额先于软限额判断，到达后以 SIGKILL 终止全组。下调限额保留已用时间，在下一次 CPU 检查时处理。

有限软值加入现有进程 CPU 活动索引，与 itimer、POSIX timer 共用一次组用量采样。停用所有 CPU 期限且限额恢复无限后摘除；普通 syscall 进出不增加限额采样。实际递送精度受原 timer 周期、IRQ 延迟和执行调度影响。`tests/network_runner.py --workload cpu` 用同 ELF 对照 Linux，覆盖限额、线程/exec、CPU 睡眠及 getcpu。

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

## timerfd期限

`timerfd_create/settime/gettime`提供MONOTONIC和REALTIME匿名OFD。设置、查询、计数读取和等待由发起CPU执行；CPU0在真实到期中断中发布可读状态。已启用对象进入侵入式AVL期限索引，索引和对象状态由同一短锁保护。发布的最早tick期限供调度器读取，调度锁内不再进入索引锁；远端更新通过既有deadline IPI要求CPU0重装硬件事件。空索引只有常数空检查。

首次到期只记一次并通知。周期在read/gettime中按原绝对期限累计漏过次数、前移和重装，保留周期相位，避免短周期造成密集IRQ。settime完整导入输入后交换设置，输出旧值EFAULT不撤销新设置。timerfd read消费计数后复制，部分输出返回实际完成前缀，区别于eventfd的EFAULT返回。

末次关闭从索引摘除对象后，等待已经取得的到期通知借用和等待游标结束，再归还对象。dup/fork共享设置和计数。BOOTTIME、ALARM时钟仍在能力目录；CANCEL_ON_SET接受并保存，当前墙钟只在启动初始化，运行时调钟的取消通知随调钟接口增加。

## POSIX 定时器对象

RV64/LA64使用107～111号入口和原生64字节sigevent、32字节itimerspec布局；用户态timer ID为32位，libc的timer_t由库自行封装。线程组按需持有64桶注册表，单调分配非负ID，回绕时检查冲突。注册表与共享disposition分开，线程退出不撤销组定时器；fork/vfork不继承，成功exec和最终组清理释放对象，失败exec保留。

创建为每只定时器预留通知记录和SIGPENDING额度，SIGEV_NONE同样计费。需要通知时先准备真实目标的pending索引。创建候选在ID输出成功后才发布，用户输出故障归还候选与额度；其他线程查询尚未发布的ID返回EINVAL。普通信号仍在消费时归还额度，预留额度由定时器对象持有。对象和PID身份的最后归还在状态保护外释放内存。

REALTIME、MONOTONIC以及进程/线程CPU时钟均在调用CPU创建、设置、查询和删除，由CPU0推进到期。CPU clock目标解析与查询共用，保留PROF、VIRT、SCHED编码，分别使用用户加内核、仅用户、用户加内核时间。SIGEV_SIGNAL和原生SIGEV_THREAD向组投递，SIGEV_THREAD_ID绑定真实线程身份；libc的SIGEV_THREAD通过库的辅助线程完成回调。

每个任务尾部内嵌进程及线程阈值索引，仅活动目标进入共同AVL；每目标按三种CPU时间维护最早阈值，旧VIRTUAL/PROF与原生进程定时器共用一次组采样。遍历按PID代次和角色递增，任务与PID借用覆盖锁外采样，回来后重新核对身份、设置代次和阈值。空集合在原timer路径只检查摘要，普通trap保持既有记账路径。CPU目标退出时摘除阈值，其他组持有的句柄继续有效：gettime归零、settime返回ESRCH，delete归还资源。非组长exec在TGID角色交接时移交进程阈值，创建者、通知目标和CPU时钟目标分别保活。

通知记录区分普通分配和原生定时器预留：每只定时器最多有一条待决记录，不同定时器可同时使用同一标准信号。到期只使用创建时准备的存储。普通内联事件与排队记录共存时，索引保留内联事件的先后位置；原生记录消费不重复计费。用户提供的SI_TIMER仍是普通载荷，不据此执行重装。

REALTIME/MONOTONIC使用共享AVL算法和各自对象保护，调度器只读取最早期限摘要，远端提前设置沿既有deadline IPI通知CPU0。消费先在调度保护内取得记录及原设置快照，锁外计算进度，再核对设置代次决定重装；处理函数、sigwaitinfo和signalfd取得同一份最终overrun。删除或重设后的旧事件被丢弃，已取得的消费借用覆盖最后释放。周期按原期限推进，不按处理完成时刻重新起算。单次到期入队后查询剩余量为零，待决记录仍保留到消费或清除。STOP/CONT沿共同进程组流程清理相反待决信号并恢复停止组；KILL请求整组退出，这些组操作在状态保护外执行。

SIG_IGN的周期事件停放并保留相位，恢复disposition时移回待决队列；单独屏蔽不会重新投递已经停放的事件。SIGEV_NONE只按查询推进，停用后的旧单次期限仍可能返回剩余量，沿固定Linux的common_timer_get行为。timer_settime只解释TIMER_ABSTIME位，其余位按参考Linux忽略；NULL新值返回EINVAL，旧值输出EFAULT保留新设置。exec清除原生及用户提供的SI_TIMER记录，保留其他排队消息的顺序。

`make test-posix-timer-host`以4/16KiB页检查真实通知记录、额度、借用、代次及CPU阈值；`tests/network_runner.py --workload posix`提供两架构与固定Linux的原生ABI、真实handler/libc回调和生命周期检查。四核观察构建复用composition观察点核对调用CPU与CPU0到期位置。相同短例还以官方静态glibc在两架构的Linux/BoarOS运行，核对libc真实SIGEV_THREAD回调、输出EFAULT后的通知消费及输入输出别名。CPU时钟反例按被测时钟推进工作量，VIRT使用用户时间，PROF/SCHED使用合计时间。
