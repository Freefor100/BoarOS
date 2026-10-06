# 内核信号模块

本文记录标准信号、用户 handler 和可中断等待的当前契约。任务生命周期见[调度模块](kernel-scheduler.md)，用户态边界见[系统调用模块](kernel-syscall.md)。

## 职责与入口

- `kernel/sched/signal.c`：pending/blocked、disposition、默认动作、stop/continue、进程通知和重启策略。
- `arch/riscv/signal.c`、`include/arch/riscv/signal.h`：RISC-V 信号帧编码、寄存器恢复和用户返回尾。
- `arch/loongarch/signal.c`、`include/arch/loongarch/signal.h`：LA 整数帧、END 扩展终止记录与 ERA/GPR 恢复；同样调用通用选择和重启策略。
- `kernel/syscall/signal.c`：信号 syscall 的输入快照、状态提交及 errno；`kernel/syscall/time.c`：等待 deadline 和 restart_syscall。
- `kernel/sched/private.h`：任务私有信号状态。syscall 不直接访问任务布局。

## 状态与生命周期

每个线程使用 64 位 pending/blocked 位图，位 `sig-1` 对应信号；组长另持进程定向 pending。重复标准信号合并并保留首个 sender，handler 由一个未屏蔽成员执行。disposition 表按需分配一页并在组内引用共享；fork 复制当时的表和调用线程 mask，清空 child pending。exec 收拢组后重置自定义 handler，保留忽略 disposition、mask 和仍有效 pending。SIGKILL/SIGSTOP 不能捕获、忽略或阻塞。

默认动作包括忽略、继续、停止、终止和 core 标志；core 位不意味着已经生成 core 文件。SIGCONT 清除所有 pending stop 信号并恢复 stopped task；发送任一 stop 信号清除 pending SIGCONT。这些取消规则不依赖信号是否被阻塞。孤儿停止组的退出/reparent 事件发送 SIGHUP 后 SIGCONT；SI_KERNEL=128 同时贯通 sigtimedwait 与 SA_SIGINFO 用户帧。已孤儿组的默认 TSTP/TTIN/TTOU 不再停止任务，SIGSTOP 不受此规则影响。

SIGCHLD 的默认忽略不同于显式 SIG_IGN：默认仍保留 zombie 供 wait4 回收；显式 SIG_IGN 或 SA_NOCLDWAIT 在全组退出资源清理完成后自动回收。SA_NOCLDWAIT 不抑制已安装 handler 的退出通知；SA_NOCLDSTOP 单独控制 stop/continue 通知。清理失败保留 task/owner，由 cleanup context 重试，不能丢失父进程的唤醒。kill 使用组 pending，tkill/tgkill 与同步产生的 SIGPIPE 使用线程 pending；SIGKILL 和默认致命动作终止全组。

## 输入、信号帧与恢复

rt_sigaction/rt_sigprocmask 先完整复制输入，再提交状态，最后输出旧状态，允许输入输出地址别名。输入 EFAULT 不提交；输出 EFAULT 不回滚已经提交的有效动作。复制 helper 返回真实 uaccess 状态，handler 映射 errno。rt_sigpending 返回组与线程 pending 的并集再与 blocked 取交集。`rt_sigtimedwait` 校验 8 字节 mask 和相对 timespec，在当前线程登记临时等待集合；匹配的标准信号可以唤醒线程，但只由等待 syscall 从线程或组 pending 中取走，不经普通 handler。返回 `siginfo` 的 signo、SI_USER/SI_TKILL/SI_KERNEL、发送者进程 ID 和 uid；`siginfo` 输出 EFAULT 发生在取走信号之后。超时返回 EAGAIN，其他可递送信号打断返回 EINTR。实时信号位明确返回 ENOTSUP，因为现有位图不能保存其队列语义。

RISC-V frame 共 1088 字节，16 字节对齐：128 字节 siginfo 加 960 字节 ucontext。ucontext 内 sigmask 偏移 40、mcontext 偏移 176；mcontext 包含 32 个整数寄存器和按 Q 扩展容量保留的 528 字节、16 字节对齐 FP union。当前只填写 D 寄存器与 32 位 fcsr，其余扩展存储清零。内核静态断言和真实 musl ucontext 共同核对布局，不能仅用同一套手写偏移自证正确。

handler 的 a0/a1/a2 分别为信号、siginfo 地址和 ucontext 地址；ra 指向固定 RX VDSO 的 rt_sigreturn ecall。用户帧不保存可由用户修改的特权 sstatus。sigreturn 先复制并检查恢复输入，再恢复 signal mask、整数和 FP 状态，保持受控的 S-mode 返回状态。不可读取或不支持的扩展帧以 SIGSEGV 终止任务。

LA 整数帧为 592 字节、16 字节对齐：128 字节 siginfo、448 字节 ucontext 和16字节
END。ucontext 的 mask/mcontext 偏移仍为40/176，sigcontext 是 PC、32个GPR、32位
flags，再对齐至272字节。只发布 END，不发布未拥有的 FP/LSX/LASX/LBT 状态；
未知扩展或 SC_USED_FP 帧以用户坏帧处理，扩展支持另行实现。恢复先快照 mask、
context 和 END，再提交用户寄存器；PRMD、内核 TP 不来自用户，r0保持零。
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
有界592字节输入，构帧路径编译栈帧736字节、恢复352字节。故障记录内嵌在线程
控制块中，不为交付分配内存。函数与调用链预算通过 `make test-stack-usage`、
`make test-stack-usage-la`、真实任务 canary 与退出时栈统计核对；单函数统计和
一次回归不能证明所有深层 I/O/fault 清理链。

`make test-signal-riscv` 覆盖 syscall 复制失败、状态提交与 errno；`make test-diff-abi-riscv` 以同一 RISC-V ELF 对照等待信号的参数、真实超时、线程/进程定向来源、阻塞送达、siginfo EFAULT 消费和其他 handler 打断；`make test-userland-riscv` 以真实静态 musl 验证 handler/sigreturn、libc ucontext、sigsuspend、睡眠 EINTR、vfork、SIGCHLD 回收及 pipe 等待。架构和调度边界由 `make test-riscv` 回归。

当前为单 hart 线程组和位图 pending；尚无实时信号队列、sigaltstack、signalfd 或 SMP 同步。libc 内部信号可走线程定向路径，但不据此宣称完整实时信号排队。siginfo 提供 SI_USER/SI_TKILL sender、孤儿组 SI_KERNEL 来源以及下述同步故障信息。组 stop/continue 与致命取消不能直接释放睡眠中的任务栈；不可中断的 vfork 有独立取消握手。

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
用户页/页表/任务栈和堆恢复基线。此范围没有 FP/SIMD、altstack 或实时队列。

U-mode 未映射/权限页故障分别记录 SIGSEGV/SEGV_MAPERR、SEGV_ACCERR，文件 EOF/I/O fault 记录 SIGBUS/BUS_ADRERR，`si_addr` 为故障 VA。非法指令与断点为 SIGILL/ILL_ILLOPC、SIGTRAP/TRAP_BRKPT；access/misaligned cause 按固定 Linux 映射，`si_addr` 为 PC。来源为本地 `references/linux/arch/riscv/kernel/traps.c`、`arch/riscv/mm/fault.c`、`kernel/signal.c::force_sig_info_to_task`，commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e`。

线程有一个独立同步故障记录，经统一用户返回路径优先选取；交付 handler 时保留故障 PC，可由 handler 修复映射后重试，或修改 ucontext 跳过指令。被阻塞或忽略的致命同步信号改为默认动作并解除阻塞；默认动作终止全组。坏 signal frame 同样终止全组。fork/exec 清除故障记录；syscall 用户复制仍返回 EFAULT，内核自身 trap 仍 fatal。

`tests/userland/fault_signals.h` 验证 SEGV/BUS/ILL/TRAP siginfo、修复/上下文返回、默认组退出、阻塞/忽略和坏 frame；`tests/diff-abi/signals.c` 的 5 条真实故障对照固定 Linux，累计 1091 条差分一致。sigaltstack、ptrace、实时队列及 SMP 不在本次交付中。

TTY 的前台终端信号使用稳定 PGID 发向整个进程组；背景 TTIN/TTOU 和会话 HUP/CONT
的 owner、忽略/阻塞/orphan 条件及生命周期见 [Serial TTY](kernel-tty.md)。终端 read/write
实际已交付前缀后不留下 restart 标记，只有整次无进展的 ERESTARTSYS 由 trap 登记重试。
