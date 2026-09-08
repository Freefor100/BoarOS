# 内核信号模块

本文记录标准信号、用户 handler 和可中断等待的当前契约。任务生命周期见[调度模块](kernel-scheduler.md)，用户态边界见[系统调用模块](kernel-syscall.md)。

## 职责与入口

- `kernel/sched/signal.c`：pending/blocked、disposition、默认动作、stop/continue、进程通知和重启策略。
- `arch/riscv/signal.c`、`include/arch/riscv/signal.h`：RISC-V 信号帧编码、寄存器恢复和用户返回尾。
- `kernel/syscall/signal.c`：信号 syscall 的输入快照、状态提交及 errno；`kernel/syscall/time.c`：等待 deadline 和 restart_syscall。
- `kernel/sched/private.h`：任务私有信号状态。syscall 不直接访问任务布局。

## 状态与生命周期

每个任务使用 64 位 pending/blocked 位图，位 `sig-1` 对应信号。重复标准信号合并并保留首个 sender；disposition 表按需分配一页。SIGKILL/SIGSTOP 不能捕获、忽略或阻塞。fork 继承 disposition 和 blocked mask，清空 child pending；exec 重置自定义 handler，保留忽略 disposition、blocked mask 和 pending。

默认动作包括忽略、继续、停止、终止和 core 标志；core 位不意味着已经生成 core 文件。SIGCONT 清除所有 pending stop 信号并恢复 stopped task；发送任一 stop 信号清除 pending SIGCONT。这些取消规则不依赖信号是否被阻塞。

SIGCHLD 的默认忽略不同于显式 SIG_IGN：默认仍保留 zombie 供 wait4 回收；显式 SIG_IGN 或 SA_NOCLDWAIT 在退出资源清理完成后自动回收。SA_NOCLDWAIT 不抑制已安装 handler 的退出通知；SA_NOCLDSTOP 单独控制 stop/continue 通知。清理失败保留 task/owner，由 idle 重试，不能丢失父进程的唤醒。

## 输入、信号帧与恢复

rt_sigaction/rt_sigprocmask 先完整复制输入，再提交状态，最后输出旧状态，允许输入输出地址别名。输入 EFAULT 不提交；输出 EFAULT 不回滚已经提交的有效动作。复制 helper 返回真实 uaccess 状态，handler 映射 errno。rt_sigpending 返回 pending 与 blocked 的交集。

RISC-V frame 共 1088 字节，16 字节对齐：128 字节 siginfo 加 960 字节 ucontext。ucontext 内 sigmask 偏移 40、mcontext 偏移 176；mcontext 包含 32 个整数寄存器和按 Q 扩展容量保留的 528 字节、16 字节对齐 FP union。当前只填写 D 寄存器与 32 位 fcsr，其余扩展存储清零。内核静态断言和真实 musl ucontext 共同核对布局，不能仅用同一套手写偏移自证正确。

handler 的 a0/a1/a2 分别为信号、siginfo 地址和 ucontext 地址；ra 指向固定 RX VDSO 的 rt_sigreturn ecall。用户帧不保存可由用户修改的特权 sstatus。sigreturn 先复制并检查恢复输入，再恢复 signal mask、整数和 FP 状态，保持受控的 S-mode 返回状态。不可读取或不支持的扩展帧以 SIGSEGV 终止任务。

## 等待与重启

wait4、console read 和 pipe I/O 可被未阻塞信号唤醒；handler 的 SA_RESTART 决定这些可重启调用是重执行还是 EINTR。进入 handler 前清除任务内的外层重启状态，重执行所需参数保存在被中断的用户寄存器现场中。

nanosleep/clock_nanosleep 遇到用户 handler 始终返回 EINTR，不受 SA_RESTART 影响。只有没有进入 handler 的 stop/continue 等路径使用 restart_syscall 恢复原绝对 deadline；绝对睡眠不写 remaining。纳秒输入及 tick deadline 使用明确的饱和范围，tick 的未来距离不超过 INT64_MAX，避免远期时间被有符号差值误判为过去。

sigsuspend 在等待和选择 handler 时保留临时 mask，把原 mask 写入信号恢复上下文，由 sigreturn 恢复；不能先恢复旧 mask 再选择信号。vfork 使用具体子进程的一次性完成条件，不由普通信号解除等待。

## 验证与当前边界

当前 RV64 `-O2` 的静态栈检查中，构帧函数使用 880 字节、sigreturn 使用 864 字节；构帧复用 784 字节存储分别编码前缀与 mcontext，不在内核栈放置整份 1088 字节 frame。任务控制块为 1088 字节；4 KiB 页扣除控制块、canary/对齐和 288 字节 Trap Frame 后剩余 2704 字节。单函数统计不是完整调用链栈界证明，真实回归同时检查 canary；深层缺页/失败清理链和未来功能仍需沿调用链复查预算。

`make test-signal-riscv` 覆盖 syscall 复制失败、状态提交与 errno；`make test-userland-riscv` 以真实静态 musl 验证 handler/sigreturn、libc ucontext、sigsuspend、睡眠 EINTR、vfork、SIGCHLD 回收及 pipe 等待。架构和调度边界由 `make test-riscv` 回归。

当前为单 hart、单成员线程组和标准信号；尚无实时信号队列、sigaltstack、signalfd、完整会话/控制终端语义或 SMP 同步。siginfo 当前主要提供 SI_USER 信号与 sender，不宣称完整的故障 siginfo/用户故障 handler 路径。
