# 进程跟踪

RV64 和 LA64 提供 Linux `ptrace(117)`。跟踪关系绑定线程。被跟踪线程保留原有父进程，`/proc/<tid>/status` 的 `TracerPid` 给出跟踪者 TID。跟踪者从自己的跟踪列表等待事件，普通父进程仍通过子进程列表回收。跟踪者与真实父进程分属不同线程组时，跟踪者先取得最终退出状态，随后真实父进程可取得该状态并回收进程。

`PTRACE_TRACEME` 以真实父线程作为跟踪者。`PTRACE_ATTACH` 和 `PTRACE_SEIZE` 按调用者真实 UID、GID、能力集合以及目标映像的 dumpable 属性检查权限。目标正在准备 exec 时，附加请求等待凭据和映像提交或准备失败，醒来后重新核对目标线程与权限。`ATTACH` 请求停止目标，`SEIZE` 可用 `PTRACE_INTERRUPT` 在不发送用户信号的情况下请求停止。`PTRACE_LISTEN` 等待已 seize 的线程发生组停止状态变化。

## 停止与恢复

被跟踪线程进入系统调用、退出系统调用、递交信号或产生已启用的事件时，在自己的内核栈上准备等待。旧 CPU 交出执行权并保存浮点上下文后，调度器发布停止状态，通知跟踪者的等待队列并发送 SIGCHLD。跟踪者可用 wait4 或 waitid 取得事件；waitid 的 WNOWAIT 保留事件。停止期间可读取或修改寄存器、信号掩码与递交中的 siginfo。信号递交停止可通过继续命令的信号号选择递交、替换或抑制。在系统调用停止时指定非零信号，内核另行排入一条线程信号，它可形成新的信号递交停止。fork、exec、exit 等事件停止忽略继续命令附带的信号。

`PTRACE_SYSCALL` 请求在系统调用入口和出口停止。`PTRACE_O_TRACESYSGOOD` 使这两类停止带有 `SIGTRAP | 0x80`，`PTRACE_GET_SYSCALL_INFO` 返回对应阶段、参数或结果。fork、vfork、clone、vfork 完成、exec 和 exit 事件由各自选项控制。fork 类事件的消息是新 TID，exec 消息是执行 exec 的线程原先的 TID。创建子任务时先建立继承的跟踪关系，再发布新任务。跟踪者退出时解除所属关系，启用 EXITKILL 的目标收到 SIGKILL。

## 寄存器与内存

`GETREGSET` 和 `SETREGSET` 接受 `NT_PRSTATUS` 和 `NT_PRFPREG`。RV64 整数布局为 PC 后跟 x1 至 x31。LA64 整数布局含 32 个通用寄存器、原始参数 a0、ERA、BADV 和 ABI 保留区。系统调用入口停止后，实际参数及 seccomp 使用原始 a0；修改普通 a0 不替换该参数。
LA64 可单独修改 regset 的原始 a0 槽，RV64 的整数 regset 不暴露该槽；出口停止仍可修改 a0 返回值。
LA64 基础浮点写入保留各向量寄存器的高位。`PEEKUSER` 与 `POKEUSER` 在 LA64 接受原生索引 0 至 34，RV64 对这两项请求返回 EIO。

`PEEKTEXT` 和 `PEEKDATA` 的原始系统调用把机器字复制到 data 指针并返回零；libc 的 ptrace 包装返回读取的机器字。`POKETEXT` 和 `POKEDATA` 使用传入的机器字更新目标。停止期间的访问固定目标的执行状态，私有映射的写入使用 COW，写入可执行页后发布指令缓存。映射与页所有权见 [内存管理](kernel-mm.md)。

单步、硬件断点和 seccomp 事件均超出此接口当前支持的跟踪范围。

## 直接消费者与工具

`tests/userland/api8.py --program ptrace --program ptrace_signal --program ptrace_tools`
运行直接调用、信号交接和原版工具消费者，架构、libc、核数及内核由同一执行器选择。
`ptrace_tools` 使用固定上游 strace 7.3，检查 `-f` 跟踪中的线程、fork、vfork、
非组长 exec、信号和文件操作记录，并对已经运行的目标执行 attach。
`tools/build-strace.py --arch riscv` 或 `--arch loongarch` 构建静态 musl 工具；
来源及构建约束见[第三方组件](../third-party.md#strace-验证工具)。
