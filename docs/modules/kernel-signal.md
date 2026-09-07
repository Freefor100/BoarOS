# 内核信号模块

本文记录 BoarOS 当前标准信号、用户 handler 和可中断等待的稳定契约。系统调用编号见[系统调用解码模块](kernel-syscall.md)，任务状态与 wait 回收见[内核调度与进程生命周期模块](kernel-scheduler.md)，RISC-V 返回入口见[RISC-V Trap 模块](riscv-trap.md)。

## 范围与入口

| 文件 | 当前职责 |
|---|---|
| `include/kernel/signal.h` | 信号号、`sigaction`/`sigprocmask` ABI 前缀和内核信号接口 |
| `kernel/sched/private.h` | task 内 pending/blocked/restart 状态和懒分配 disposition 表布局 |
| `kernel/sched/signal.c` | 发送、合并、默认动作、stop/continue、handler frame、sigreturn 和 syscall restart |
| `kernel/syscall.c` | `kill/tkill/tgkill`、`rt_sig*`、`restart_syscall` 与 `nanosleep` 解码 |
| `arch/riscv/trap.c`、`arch/riscv/trap_entry.S` | ecall 后公共用户返回尾、`rt_sigreturn` 分派和返回前处理 |
| `tests/riscv/signal_cases.c`、`tests/userland/real.c` | 调度/信号模块测试和真实静态 musl 组合测试 |

## 状态、继承与默认动作

每个用户 task 用两个 64 位位图表示 pending 和 blocked，位 `sig-1` 对应信号 `sig`；同一标准信号已经 pending 时再次发送只保留第一次 sender，不建立实时队列。handler 表按需分配一页，包含 64 个 disposition。`SIGKILL` 和 `SIGSTOP` 不能被捕获、忽略或阻塞。

普通 clone/fork 复制 disposition 和 blocked mask，但清空 child pending；exec 将所有自定义 handler 恢复为 `SIG_DFL`，保留 `SIG_IGN`，并清除当时已被忽略的 pending 信号。当前 SIGCHLD 只有在父进程安装真实 handler 且未设置 `SA_NOCLDWAIT` 时才产生通知；wait4 仍是子进程回收入口。

默认动作覆盖标准的忽略、继续、停止、终止和 core 标志：SIGCHLD/SIGURG/SIGWINCH 默认忽略，SIGCONT 默认继续，SIGTSTP/SIGTTIN/SIGTTOU/SIGSTOP 默认停止，SIGQUIT/SIGILL/SIGTRAP/SIGABRT/SIGBUS/SIGFPE/SIGSEGV/SIGXCPU/SIGXFSZ/SIGSYS 带 core 标志，其余可发送标准信号默认终止。停止 task 从 ready/blocked 路径摘出并保留 stopped 状态；SIGCONT 或 SIGKILL 会恢复它，父进程在允许的 `WUNTRACED/WCONTINUED` 选项下可观察状态事件。

## 发送与用户 handler

`kill(129)` 支持正 PID、调用者进程组、指定进程组和除调用者/PID 1 外的全部进程；`tkill(130)` 按 TID，`tgkill(131)` 同时检查 TGID/TID。信号为 0 时只做存在性检查。发送到默认忽略 disposition 会丢弃，发送到 blocked signal 只设置 pending；发送到 interruptible wait 的未阻塞信号会从等待队列唤醒 task。

pending 信号在 timer、ecall、用户故障或新 task 首次进入用户态前的公共 return tail 处理。默认动作在内核内完成；可捕获 handler 则在用户栈向下分配固定 816 字节 frame：前 128 字节是 `siginfo`，后 688 字节是 RISC-V Linux 形态 `ucontext`。frame 保存 PC、整数寄存器、`sstatus`、当前 signal mask 和 D 状态；handler 的 `a0/a1/a2` 分别得到 signal、`siginfo` 地址和 `ucontext` 地址。`SA_RESETHAND`、`SA_NODEFER` 和 handler mask 会影响本次投递期间的 disposition/mask；当前 `SA_ONSTACK` 不切换到备用栈。

handler 返回地址不指向用户栈上的可执行数据，而指向固定的 RX VDSO 页。该页紧邻 stack guard 上方，内容为 `li a7,139; ecall`，因此未经 BoarOS 特制的静态 musl handler 也能通过 `rt_sigreturn(139)` 恢复现场。`rt_sigreturn` 从当前用户栈复制并校验 frame，恢复整数寄存器、`sepc/sstatus`、signal mask 和 per-task FP 状态；frame 越界、不可读、PC 溢出或非法用户返回状态会以 SIGSEGV 终止当前 task。

## 可中断等待与重启

wait4、console read、nanosleep 和 pipe read/write 使用通用等待队列的 interruptible 标志；vfork 等必须等待地址空间生命周期的路径保持不可中断。信号唤醒返回内部 `ERESTARTSYS`，Trap 层暂不推进 ecall 的 `sepc`，由公共 return tail 决定结果：没有 `SA_RESTART` 的 handler 把 syscall 变为 `-EINTR` 并跳过 ecall；有 `SA_RESTART` 的 handler 保持原 ecall 位置，sigreturn 后重新执行它。nanosleep 保存绝对 deadline 和 remaining 地址，重启时使用内部 `restart_syscall(128)`，避免把相对时间重复计算成更长睡眠。`rt_sigsuspend(133)` 临时安装 mask，直到有可投递信号，再恢复原 mask 并返回 `-EINTR`。

## ABI 边界、所有权与限制

当前 `rt_sigaction(134)`、`rt_sigprocmask(135)` 和 `rt_sigpending(136)` 采用 8 字节有效 signal set；用户传入的 128 字节 sigaction 结构只消费 riscv64 所需的 handler/flags/mask 前缀。信号 frame 写入用户栈前先通过 uaccess 检查，signal table 在 task 退出清理阶段释放；清理失败保留 owner 供 idle 重试，不把已释放的物理页留在 task 字段中。

当前范围是单 hart、单成员线程组和标准信号。尚无 queued real-time `siginfo`、`sigaltstack`、signalfd、会话/控制终端的完整 SIGHUP 语义、futex 集成或 SMP 下的并发信号与 MM/TLB 协议；`SA_SIGINFO` 已接受 Linux 入口形态，但 frame 内容仍是当前固定实现的子集。

## 验证

```sh
make test-scheduler-cases-riscv
make test-signal-riscv
make test-userland-riscv
make test-riscv
```

模块测试覆盖 pending 合并、blocked/不可阻塞 signal、默认动作、handler frame/sigreturn、SIGCHLD 条件、stop/continue wait status 和 `SA_RESTART`。真实静态 musl 测试还覆盖 handler 修改、nanosleep 中断与重启、sigsuspend、SIGQUIT core 位、pipe 阻塞路径和最终资源回收。
