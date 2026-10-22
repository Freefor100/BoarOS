# 内核信号模块

本文记录标准信号、用户 handler 和可中断等待的当前契约。任务生命周期见[调度模块](kernel-scheduler.md)，用户态边界见[系统调用模块](kernel-syscall.md)。

## 职责与入口

- `kernel/signal_stack.c`：每线程备用栈登记、选择、边界与自动停用；
- `kernel/sched/signal.c`：pending/blocked、disposition、默认动作、stop/continue、进程通知和重启策略。
- `arch/riscv/signal.c`、`include/arch/riscv/signal.h`：RISC-V 信号帧编码、寄存器恢复和用户返回尾。
- `arch/loongarch/signal.c`、`include/arch/loongarch/signal.h`：LA 整数/FP/LSX/LASX帧、END终止记录与ERA/GPR恢复；同样调用通用选择和重启策略。
- `kernel/syscall/signal.c`：信号 syscall 的输入快照、状态提交及 errno；`kernel/syscall/time.c`：等待 deadline 和 restart_syscall。
- `kernel/sched/private.h`：任务私有信号状态。syscall 不直接访问任务布局。

## 状态与生命周期

每个线程使用 64 位 pending/blocked 位图，位 `sig-1` 对应信号；组长另持进程定向 pending。重复标准信号合并并保留首个 sender，handler 由一个未屏蔽成员执行。disposition 表按需分配一页并在组内引用共享；fork 复制当时的表和调用线程 mask，清空 child pending。exec 收拢组后重置自定义 handler，保留忽略 disposition、mask 和仍有效 pending。SIGKILL/SIGSTOP 不能捕获、忽略或阻塞。

`SA_RESETHAND` 在交付时只将处理动作恢复为默认值，保留已登记的 action flags 和 mask，后续 rt_sigaction 查询仍可观察这些设置。

用户返回直接观察线程及组 pending 中未屏蔽的位；没有可交付信号、故障、退出请求或临时掩码恢复时立即返回，不取得锁；完整事件交付保留独立调用边界，使其临时载荷和保存寄存器留在有事件的路径。等待者的 wait_mask 不参与交付判断，被屏蔽的信号仍留给 sigtimedwait。需要处理事件时在调度 raw 内重取位与载荷；并发新增信号仍通过既有通知请求返回检查。

默认动作包括忽略、继续、停止、终止和core动作；当前提供CORE限额状态，core文件写出仍列为缺口，wait状态反映实际是否产生转储。SIGCONT 清除所有 pending stop 信号并恢复 stopped task；发送任一 stop 信号清除 pending SIGCONT。这些取消规则不依赖信号是否被阻塞。孤儿停止组的退出/reparent 事件发送 SIGHUP 后 SIGCONT；SI_KERNEL=128 同时贯通 sigtimedwait 与 SA_SIGINFO 用户帧。已孤儿组的默认 TSTP/TTIN/TTOU 不再停止任务，SIGSTOP 不受此规则影响。

SIGCHLD 的默认忽略不同于显式 SIG_IGN：默认仍保留 zombie 供 wait4 回收；显式 SIG_IGN 或 SA_NOCLDWAIT 在全组退出资源清理完成后自动回收。SA_NOCLDWAIT 不抑制退出通知，包括被阻塞的默认 SIGCHLD；SA_NOCLDSTOP 单独控制 stop/continue 通知。默认 SIGCHLD 被阻塞时，停止和继续也进入 pending，显式 SIG_IGN 则不进入。清理失败保留 task/owner，由 cleanup context 重试，不能丢失父进程的唤醒。kill 使用组 pending，tkill/tgkill 与同步产生的 SIGPIPE 使用线程 pending；SIGKILL 和默认致命动作终止全组。

## 待决记录与计费

pending位图是就绪摘要。普通标准信号继续在任务原有字段保存首条来源；实时信号及完整用户载荷使用按需分配的记录。每个线程和线程组各有一个按信号编号分槽的FIFO索引，出队不遍历其他信号的记录；handler与sigtimedwait共用一次出队和128字节siginfo快照。同类标准信号合并，同类实时信号按FIFO；线程pending先于共享pending，每个集合按固定Linux的同步信号优先规则选择。

候选记录及索引在锁外分配，位图、记录和额度在调度短锁内共同发布。消费或批量清除在保护内摘链、扣除计费，锁外释放；空索引随所属线程／线程组退出归还。组长交接在同一保护内移动共享索引和计费状态。fork的child无待决记录，exec保留有效pending；忽略动作、stop/continue取消和退出均使用共同清除路径。

`RLIMIT_SIGPENDING(11)`默认软／硬额度4096，同组共享、fork复制、exec保留。当前固定真实UID0，使用量汇总该UID的全部信息记录；接纳按目标软额度判断，`/proc/<pid>/status`的`SigQ`展示使用量／额度。下调额度保留已有消息。实时排队发送额度不足返回EAGAIN；普通kill保留至少一个待决位，并按Linux允许丢失附带信息。标准信号合并不重复计费。`make test-signal-pending-host`在4/16KiB页检查FIFO、跨组额度、候选竞争、分配失败、交接和最终归还。

## SMP上的状态和执行位置

RV USER_SMP中的普通信号在实际执行线程所在CPU递送。pending及其来源、共享disposition、blocked和SA_RESETHAND的修改由调度raw保护：选择信号时一次取走pending、复制完整action、记录恢复mask、设置handler期间的屏蔽位，并重置一次性动作。锁外只使用线程栈上的delivery快照。后续rt_sigaction替换同一项时，本次递送继续使用已选版本；替换的新动作也保留。rt_sigaction的旧动作和新动作在同一提交点交换，fork复制与exec重置同样使用该保护。

构造信号帧、用户复制、寄存器/FPU恢复由当前线程执行。任务执行所有权持有本线程存储、MM及disposition表引用；uaccess借用实际物理页，缺页和等待沿共同MM路径处理。调度raw不跨用户复制、缺页或等待。sigreturn完整导入并检查用户上下文后，在短锁内恢复mask，再更新本线程寄存器和FPU。

既有返回通知字区分“CPU0业务”和“本地信号”两种工作。发布者在保护下置相应位，需要时通知远端CPU；共同返回准备读取一次通知字，已有本地信号时才消费本批通知并要求信号选择，普通返回复用原检查。消费期间的新发布留在下一轮，两种消费者只清除各自的位。mask解除屏蔽时重新检查旧pending，覆盖sigprocmask、sigreturn、sigsuspend和临时等待mask；一批信号中仍有可交付项时保留后续检查。新增屏蔽位若挡住仍pending的组信号，锁外复用组游标通知并唤醒另一合适成员；发布时也重查候选，退出者在proc_exiting提交后、组拆除前转交未消费的组信号。sigtimedwait的接收集合在同一raw内发布，结束等待时也转交因退出集合而被本线程屏蔽的组pending。SIGCONT恢复READY成员时重新发布已有可递送信号的通知。游标持有真实task存储，短锁不跨等待唤醒，单成员路径无需遍历。普通周期CPU定时器的handler和sigreturn沿该本地路径运行。

组停止/退出仍进入进程生命周期的CPU0范围。默认停止动作先迁移、再重新选择和消费pending，途中SIGCONT清除的停止请求会被丢弃；REAL定时器消费后的重装仍进入CPU0的墙钟期限索引。临时mask尚需等待真正handler时沿原等待路径阻塞，相应操作在短锁外取得执行资格。新增屏蔽位转交与停止取消的Linux依据为固定`references/linux/kernel/signal.c`中的`retarget_shared_pending`、`__set_task_blocked`和`get_signal`。调度交接见[RV两核](riscv-smp.md#真实用户态与cpu0业务交接)，CPU定时器统计见[时间模块](kernel-time.md#进程-cpu-间隔定时器)。

## 输入、信号帧与恢复

### 每线程备用栈

`sigaltstack(132)`使用RV64/LA64共同的24字节`stack_t`布局，地址、尺寸及原始标志存在线程对象尾部，由实际执行线程维护。syscall请求携带本次trap的真实用户SP；查询按`base < SP <= base+size`计算`SS_ONSTACK`，`SS_AUTODISARM`配置视为未在栈上。最小登记尺寸为RV 2048字节、LA 4096字节。登记只保存用户地址，实际构帧时通过uaccess检查映射和权限。

设置先完整导入输入，检查当前SP后再检查标志和尺寸；栈内修改为EPERM，尺寸不足为ENOMEM。新状态提交后输出旧值，输出EFAULT保留新设置；输入输出可以别名。fork复制调用线程配置，`CLONE_VM`且未指定`CLONE_VFORK`的child停用；vfork保留。成功exec停用，失败exec保留。配置不随共享disposition传播。

`SA_ONSTACK`首次从登记区顶端构帧；已在备用栈上的嵌套处理从当前SP继续向下。两架构按实际对齐后的整数、FP/SIMD帧检查范围，再通过共同uaccess复制；不可写、未映射或空间耗尽终止用户线程组并报告SIGSEGV。`uc_stack`保存交付前的原始登记状态，与查询时计算的`SS_ONSTACK`含义不同。完整构帧成功后，`SS_AUTODISARM`停用原配置，处理函数可以重新设置。sigreturn完整导入`uc_stack`和其他恢复输入后，按恢复后的SP应用登记；字段不可读为坏帧，可读设置产生的EINVAL、ENOMEM、EPERM按固定Linux的`restore_altstack`忽略。恢复后寄存器、mask及浮点状态继续沿原路径处理。

rt_sigaction/rt_sigprocmask 先完整复制输入，再提交状态，最后输出旧状态，允许输入输出地址别名。输入 EFAULT 不提交；输出 EFAULT 不回滚已经提交的有效动作。复制 helper 返回真实 uaccess 状态，handler 映射 errno。rt_sigpending 返回组与线程 pending 的并集再与 blocked 取交集。`rt_sigtimedwait` 校验 8 字节 mask 和相对 timespec，在当前线程登记临时等待集合；匹配的标准信号可以唤醒线程，但只由等待 syscall 从线程或组 pending 中取走，不经普通 handler。返回 `siginfo` 的 signo、SI_USER/SI_TKILL/SI_KERNEL、发送者进程 ID 和 uid；SIGCHLD 另携带 CLD_EXITED/KILLED/DUMPED/STOPPED/CONTINUED 与对应退出码或信号。pending 位、来源和事件状态在调度 raw 保护内共同发布与取走，标准信号合并保留首个未消费事件。每个线程和组只各补充一份紧凑的子事件状态；`rt_sigtimedwait` 与 RV/LA 信号帧共用 128 字节 ABI 编码。`siginfo` 输出 EFAULT 发生在取走信号之后。超时返回 EAGAIN，其他可递送信号打断返回 EINTR，SA_RESTART也保留这一返回。实时信号位明确返回 ENOTSUP，因为现有位图不能保存其队列语义。

RISC-V 交付 frame 共 1088 字节，16 字节对齐：128 字节 siginfo 加 960 字节 ucontext。ucontext 内 sigmask 偏移 40、mcontext 偏移 176；mcontext 包含 32 个整数寄存器和按 Q 扩展容量保留的 528 字节、16 字节对齐 FP union。当前只填写 D 寄存器与 32 位 fcsr，其余扩展存储清零。内核静态断言和真实 musl ucontext 共同核对布局，不能仅用同一套手写偏移自证正确。

handler 的 a0/a1/a2 分别为信号、siginfo 地址和 ucontext 地址；ra 指向固定 RX VDSO 的 rt_sigreturn ecall。用户帧不保存可由用户修改的特权 sstatus。sigreturn 先复制并检查恢复输入，再恢复 signal mask、整数和 FP 状态，保持受控的 S-mode 返回状态。恢复输入不要求 SP 仍为 16 字节对齐，可读的 8 字节偏移帧按固定 Linux 接受；不可读取或不支持的扩展帧以 SIGSEGV 终止任务。`test-sigreturn-boundary-riscv/loongarch` 检查正常/偏移/不可读恢复帧、mask 以及 RV F/D 与 FCSR。

LA 整数帧为 592 字节、16 字节对齐：128 字节 siginfo、448 字节 ucontext 和16字节
END。ucontext 的 mask/mcontext 偏移仍为40/176，sigcontext 是 PC、32个GPR、32位
flags，再对齐至272字节。未used_math时只发布END；否则发布最高live宽度的
FPU、LSX或LASX记录，完整布局、重复记录优先级及pending FCSR见[LA浮点/SIMD](loongarch-fpu.md)。
END有效输入为8字节；不要求尾padding可读。未知或过短扩展（含未支持LBT）为坏帧。
恢复先快照mask、context及选中有效payload，再提交用户寄存器；PRMD、内核TP不来自用户，r0保持零。
handler 的 a0/a1/a2、ra 与 SP 使用 LA ABI，VDSO 执行 syscall 139；sigreturn 不再推进 ERA。
内核交付帧保持16字节对齐；用户提供的恢复帧不另加这一拒绝条件，Linux接受
可读的8字节偏移帧。仍完整检查范围、复制和不支持扩展，先快照后提交。

## 等待与重启

wait4、console read 和 pipe I/O 可被未阻塞信号唤醒；handler 的 SA_RESTART 决定这些可重启调用是重执行还是 EINTR。进入 handler 前清除任务内的外层重启状态，重执行所需参数保存在被中断的用户寄存器现场中。

无超时 FUTEX_WAIT 使用 generic restart，handler 的 SA_RESTART 决定重试或 EINTR；重试会重新读取 futex word，值已变化返回 EAGAIN。带超时 FUTEX_WAIT 与 nanosleep 一样使用独立 tagged restart state：进入任何用户 handler 都返回 EINTR，不由 SA_RESTART 自动继续；没有进入 handler 的 stop/continue 等路径由 restart_syscall 使用首次调用保存的 monotonic absolute deadline，不能重新获得完整 relative timeout。

nanosleep/clock_nanosleep 遇到用户 handler 始终返回 EINTR，不受 SA_RESTART 影响。只有没有进入 handler 的 stop/continue 等路径使用 restart_syscall 恢复原绝对 deadline；绝对睡眠不写 remaining。纳秒输入及 tick deadline 使用明确的饱和范围，tick 的未来距离不超过 INT64_MAX，避免远期时间被有符号差值误判为过去。

sigsuspend 在等待和选择 handler 时保留临时 mask，把原 mask 写入信号恢复上下文，由 sigreturn 恢复；不能先恢复旧 mask 再选择信号。vfork 使用具体子进程的一次性完成条件，不由普通信号解除等待。

## 验证与当前边界

RV 构帧复用 prefix/mcontext 存储，不在内核栈放置整份1088字节frame；LA整数帧使用
592字节整数帧，FP/LSX/LASX按实际宽度构造，最大1664字节；按片段复制而非整帧
留在内核栈。编译帧预算由下面门禁读取实际产物，不沿用标量阶段数值。故障记录内嵌在线程
控制块中，不为交付分配内存。函数与调用链预算通过 `make test-stack-usage`、
`make test-stack-usage-la`、真实任务 canary 与退出时栈统计核对；单函数统计和
一次回归不能证明所有深层 I/O/fault 清理链。

`make test-signal-wait-abi-riscv test-signal-wait-abi-loongarch`提供等待的聚焦差分：包含空等待集合和SA_RESTART，子进程通过真实`/proc/<pid>/stat`睡眠状态握手后发信号，直接检查syscall返回，区分libc自身的EINTR重试。

`test-sched-concurrency-host`在真实选择解锁后让另一宿主线程替换action，检查旧delivery快照、mask、SA_RESETHAND及新动作均保持正确；旧实现会读到后来的handler。`test-user-smp-host`检查两类通知分别消费及晚发布、handler新增屏蔽位、接收者退出和结束sigtimedwait后转交组信号、真实COMMITTED等待者被唤醒及消费，以及CPU0换栈窗口中的SIGCONT取消STOP、恢复成员重新取得信号通知。`test-user-smp-riscv`的静态/动态musl案例另覆盖屏蔽后已pending的信号经sigsuspend解除屏蔽、嵌套handler和sigreturn恢复原mask，运行真实1/2CPU及双RAM。

`make test-signal-riscv` 覆盖 syscall 复制失败、状态提交与 errno；`make test-diff-abi-riscv` 以同一 RISC-V ELF 对照等待信号的参数、真实超时、线程/进程定向来源、阻塞送达、siginfo EFAULT 消费和其他 handler 打断；`tests/diff-abi/signals.c` 还对照 SIGCHLD 的停止/继续、退出/信号终止、首个事件合并、SA_NOCLDSTOP、SA_NOCLDWAIT、显式忽略及 SA_SIGINFO handler；`make test-userland-riscv` 以真实静态 musl 验证 handler/sigreturn、libc ucontext、sigsuspend、睡眠 EINTR、vfork、SIGCHLD 回收及 pipe 等待。架构和调度边界由 `make test-riscv` 回归。

标准信号使用线程组与线程位图 pending，发布和消费共用调度保护；signalfd尚未接入。libc内部信号与其他实时信号共用线程定向队列。siginfo 提供 SI_USER/SI_TKILL sender、孤儿组 SI_KERNEL 来源以及下述同步故障信息。组 stop/continue 与致命取消不能直接释放睡眠中的任务栈；不可中断的 vfork 有独立取消握手。

## 同步故障

LA 页故障同样经通用 force_fault 选择 handler，保留 ERA；未映射/权限为 SEGV
MAPERR/ACCERR，EOF 为 BUS/ADRERR。ADE/ALE 的 si_addr 是 BADV，break 0 为
TRAP/BRKPT、si_addr=ERA。固定 Linux LA do_ri 使用 SI_KERNEL=128、空地址，不能
沿用 RV 的 ILL_ILLOPC。SC_ADDRERR_RD/WR 根据 Linux thread.error_code 的保留契约
编码，后续异步帧和 clone 保留该值；PRMD 不发布在用户上下文。
break 6/7 分别为 SIGFPE/FPE_INTOVF、FPE_INTDIV，按真实指令 immediate 解码，
不能把整数运算 fault 全部归为 TRAP。两个类型也由同 ELF 的双侧 handler 验证。
解码使用已驻留页的 USER/EXEC 资格借用物理owner，不要求普通数据READ权限；
仅执行映射中的break6也由双侧真实程序保护，不全局放宽数据uaccess。
`make test-signal-loongarch` 在512MiB/1GiB分别运行同一个真实 LP64S musl ELF，
验证布局、来源/mask、嵌套、故障映射修复、整数寄存器/PC恢复、坏帧、pipe
SA_RESTART/EINTR、nanosleep EINTR 和 sigsuspend。每次 BoarOS 退出要求根盘 owner、
用户页/页表/任务栈和堆恢复基线。整数范围另由LP64D FPU及SIMD探针扩展，关闭扩展的CPU profile也已验收；用户态排队发送入口随后接入。

U-mode 未映射/权限页故障分别记录 SIGSEGV/SEGV_MAPERR、SEGV_ACCERR，文件 EOF/I/O fault 记录 SIGBUS/BUS_ADRERR，`si_addr` 为故障 VA。非法指令与断点为 SIGILL/ILL_ILLOPC、SIGTRAP/TRAP_BRKPT；access/misaligned cause 按固定 Linux 映射，`si_addr` 为 PC。来源为本地 `references/linux/arch/riscv/kernel/traps.c`、`arch/riscv/mm/fault.c`、`kernel/signal.c::force_sig_info_to_task`，commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e`。

线程有一个独立同步故障记录，经统一用户返回路径优先选取；交付 handler 时保留故障 PC，可由 handler 修复映射后重试，或修改 ucontext 跳过指令。被阻塞或忽略的致命同步信号改为默认动作并解除阻塞；默认动作终止全组。坏 signal frame 同样终止全组。fork/exec 清除故障记录；syscall 用户复制仍返回 EFAULT，内核自身 trap 仍 fatal。

`tests/userland/fault_signals.h` 验证 SEGV/BUS/ILL/TRAP siginfo、修复/上下文返回、默认组退出、阻塞/忽略和坏 frame；`tests/diff-abi/signals.c` 的 5 条真实故障对照固定 Linux，累计 1091 条差分一致。ptrace及后续信号接口仍列在能力目录；RV真实两核的递送与恢复使用上面的共同协议。

TTY 的前台终端信号使用稳定 PGID 发向整个进程组；背景 TTIN/TTOU 和会话 HUP/CONT
的 owner、忽略/阻塞/orphan 条件及生命周期见 [Serial TTY](kernel-tty.md)。终端 read/write
实际已交付前缀后不留下 restart 标记，只有整次无进展的 ERESTARTSYS 由 trap 登记重试。

FP/LSX/LASX记录、pending异常、首用与宽度升级和owner切换见[LA浮点/SIMD模块](loongarch-fpu.md)。

`tests/workloads/signal/altstack.c`对照两架构Linux，覆盖登记ABI、嵌套、普通栈溢出、坏备用栈、可读无效与不可读恢复字段、fork/vfork/线程及非组长exec。LA在激活FPU/LSX/LASX后，另检查4KiB备用栈上的实际帧（FPU 880、LSX 1136、LASX 1648字节）及不足该宽度一帧的边界，父进程先取得成功构帧记录，再判定后续SIGSEGV。RV观察构建记录各在线核的构帧和sigreturn；复用composition的真实页pin暂停点检查mprotect已park等待，以及munmap返回后物理页仍由pin保活，再放行复制。共同状态检查入口为`make test-signal-stack-host`（4/16KiB页）；真实多核入口复用`tests/network_runner.py --workload altstack --observe`。
