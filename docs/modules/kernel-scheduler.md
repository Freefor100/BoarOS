# 内核调度、线程组与生命周期

本文记录单 hart FIFO 调度、线程组资源、clone/exec/wait 和回收契约。背景见[线程组与 futex](../learning/threads-and-futex.md)，信号、文件资源、MM 的稳定边界分别见对应模块文档。

## 实现入口

| 文件 | 职责 |
|---|---|
| `kernel/sched/core.c` | 创建、ready FIFO、tick 抢占、资源借用校验 |
| `kernel/sched/process.c` | clone、线程组/父子树、wait、退出、回收与记账 |
| `kernel/sched/exec.c` | 已准备映像的提交与旧资源清理 |
| `kernel/sched/wait.c` | 全局 blocked 链、每队列 FIFO、超时和信号唤醒 |
| `kernel/sched/futex.c` | 256 桶 WAIT/WAKE/REQUEUE、clear-child-tid 唤醒 |
| `kernel/sched/signal.c` | 组/线程 pending、disposition、stop/continue 和重启 |
| `arch/riscv/process.c` | clone 寄存器、FP/TLS 继承与 exec 寄存器清零 |
| `arch/riscv/context.c`、`context_switch.S` | psABI context 和 SIE 临界区 |
| `kernel/sched/private.h` | 私有任务布局、组与队列成员关系 |

公共 scheduler 头不暴露 Trap Frame；架构 clone 入口位于 `include/arch/riscv/process.h`。syscall 通过不透明 task 接口取得 TID/TGID/PPID 和资源，不直接修改调度私有字段。

## 身份与资源

每个用户执行线程有独立 TID、FP/整数寄存器、signal mask、线程 pending、clear-child-tid、restart 状态和私有任务页。组长承载 TGID、进程组、父子树、组 pending、退出通知及已回卷记账。双向成员环包含组长容器；组长停止执行后仍留在环中，直到组结束或非组长 exec 接管身份。

普通 fork 从调用线程复制 MM 的 COW 页表/VMA、fd 表、fs context 和 disposition；OFD 仍按现有语义共享。子进程挂在调用线程的组长父子树中，并记录创建者 TID。线程 clone 通过 MM/files/fs/disposition 引用共享已有对象，不复制页表或 fd 槽。首次需要共享 disposition 而父线程尚无表时，会按需分配表页。

同组 fd 变更立即可见。阻塞 read/write/writev 在睡眠期间持有 OFD 引用；即使其他线程 close/dup 替换槽位，也不能提前回收正在使用的端点。退出请求必须使调用栈完成清理，不能直接释放阻塞任务页。

## clone 与 vfork

RISC-V clone 接收 flags、child_stack、parent_tid、tls、child_tid 和完整 syscall 入口寄存器。支持普通 SIGCHLD fork/vfork，以及共享 VM/FS/FILES/SIGHAND/THREAD 的线程组合和 SETTLS/PARENT_SETTID/CHILD_SETTID/CHILD_CLEARTID/SYSVSEM/DETACHED 兼容位。非法依赖返回 EINVAL，尚未闭环的合法资源组合返回 ENOTSUP。SYSVSEM 位不代表已经支持 SysV semaphore。

子线程继承完整 FP、整数现场和 mask；a0 为 0，PC 越过 ecall，非零 child_stack 替换 sp，SETTLS 设置 tp。SETTID 用户存储失败不回滚已经创建的任务，与固定 Linux clone 路径一致。所有内核分配失败都在发布前回滚；若回滚触及真实 VFS/block I/O owner，则由所属文件或 mount 记录，物理页和堆释放不另建重试状态；不会发布半构造子进程。

vfork 共享 MM，但复制 files/fs。父线程和具体子进程保持双向完成关联；子进程释放共享 MM 引用后一次性完成等待。普通信号不解除等待。组退出/exec 的致命取消先断开双向关联，再唤醒父线程；子进程自己的 MM 引用仍有效，之后完成也不能访问已释放父任务。

## 等待与 futex

所有 BLOCKED 任务在全局双向 blocked 链上，并可加入一个等待队列 FIFO。队列 wake-one 取队首，wake-all 唤醒全部；摘除 blocked 和 queue 成员均为 O(1)。deadline 使用原始 time ticks，0 表示无限；tick 仍扫描 blocked 链处理到期。

WAIT 在关中断内读取用户字、比较 expected、登记并阻塞。futex key 为 MM record 身份和四字节对齐地址；哈希碰撞需二次匹配，REQUEUE 保留 FIFO，返回唤醒数与迁移数之和。值不匹配为 EAGAIN，非法地址为 EFAULT，非法参数为 EINVAL，超时为 ETIMEDOUT。无超时 WAIT 的 signal 唤醒走 generic restart：用户 handler 没有 SA_RESTART 时返回 EINTR，带 SA_RESTART 时 sigreturn 后重新执行并再次比较用户字。带超时 WAIT 使用独立 tagged restart state 保存用户地址、expected、operation 和首次调用计算的 monotonic absolute deadline；用户 handler 无论 flags 均看到 EINTR，没有 handler 的 stop/continue 路径经 restart_syscall 继续剩余 deadline。未支持命令为 ENOSYS。用户访问层状态损坏不能转换成普通用户错误。

没有 MAP_SHARED 时，非 private futex 仅保证同一 MM 内语义，不提供跨 MM 共享 backing key。PI/bitset/wake-op 和 robust-list 回收尚未实现。

## 退出与 exec

exit 只退出当前线程，exit_group 和默认致命信号结束全组。退出先执行 clear-child-tid 清零/唤醒，再释放 exec/files/fs/MM 等资源。任务仍在自己的内核栈上时不释放任务页。

组长先退出进入 GROUP_DEAD，保留进程容器；普通成员资源清理成功后从组环移除并回卷时间。最后一个成员结束后，组长才成为唯一进程退出对象，向父进程产生一次 zombie/SIGCHLD。SIGCHLD 显式忽略或 NOCLDWAIT 的自动回收仍遵循信号模块契约。

退出切回保存的 idle/cleanup context，不依赖所有用户任务都阻塞。该 context 排空可完成的清理后主动派发 ready 任务；tick 对待清理标志只做 O(1) 检查并切换，不在中断热路径执行释放。只有真实 VFS/block I/O 清理失败保留原 owner，在后续清理机会重试；合法页/堆释放完成即返回，分配器不变量错误进入 fatal。任务仍在自身内核栈上时，任务页回收可以延后。

exec 完成映像验证后收拢组内其他线程。竞争 exec 或已被组终止的线程清理自己的事务并退出；准备失败仍保留原映像。非组长成功 exec 接管原 TGID、父子树位置、组 pending 和累计记账，旧 TID 被释放，旧组长容器静默回收。新映像最终只有一个执行成员，重置自定义 handler 和寄存器，按 CLOEXEC 关闭描述符。

## wait 与记账

wait4 遍历组长父子树，默认允许等待同组其他线程的子进程；线程本身不作为独立子进程出现。`__WNOTHREAD` 按创建/收养线程关系过滤。创建者退出时迁移给仍存活成员，进程退出时收养到 PID1；迁移必须在旧 TID 重用前更新记录。

支持 pid 选择、WNOHANG/WUNTRACED/WCONTINUED/__WALL/__WCLONE。组 stop 完成状态与组长执行状态分离，因此 GROUP_DEAD 容器仍能报告存活成员的组 stop/continue。不可中断等待尚未完成的成员保留 stop pending，不提前宣称整组停止。

zombie 先逻辑回收再复制 status/rusage，因此坏输出指针的 EFAULT 不让它再次可 wait。退出和 reparent 可能向退出队尾追加 orphan zombie，摘头必须使用更新后的链关系，不能丢失新增 owner。`times()` 累计所有仍在组环中的线程及已回卷成员；子进程时间保存在组长，不重复计数。

## 同步、成本与验证边界

单 hart 的 SIE 临界区串行化组关系、fd/MM 引用和 futex 登记。共享 MM 使用同一页表与本地 SFENCE.VMA；这不是 SMP 协议。多 hart 前仍须补锁、页引用原子操作和远端 TLB shootdown。

每线程任务页仍为 4 KiB，包括控制块、canary、内核栈和 Trap Frame。新增元数据减少栈余量，必须结合静态栈用量与真实 canary 检查审查调用链；单函数栈大小不是完整栈界证明。ASID 0 的切换刷新成本、FIFO/100 Hz tick、线性 wait4 与 deadline 扫描仍存在。

聚焦入口为 `make test-scheduler-cases-riscv`、`make test-scheduler-riscv`、`make test-files-riscv`、`make test-signal-riscv` 和 `make test-root-init-riscv`；组合消费者复用 `make test-userland-riscv`，阶段收口使用 `make test-riscv`。各次实际通过范围以 README 和提交验证说明为准，不把实现路径存在等同于全部线程负载已验证。

尚无 SMP、MAP_SHARED、PI futex、实时信号队列、sigaltstack、clone3、内核 robust-list 回收或 LoongArch context。固定语义依据见学习总结的 Linux commit 与 musl 归档。
