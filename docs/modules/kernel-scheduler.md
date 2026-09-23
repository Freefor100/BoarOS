# 内核调度、线程组与生命周期

本文记录单 hart FIFO 调度、线程组资源、clone/exec/wait 和回收契约。背景见[线程组与 futex](../learning/threads-and-futex.md)，信号、文件资源、MM 的稳定边界分别见对应模块文档。

## 实现入口

| 文件 | 职责 |
|---|---|
| `kernel/sched/core.c` | 创建、ready FIFO、tick 抢占、资源借用校验 |
| `kernel/sched/process.c` | clone、线程组/父子树、wait、退出、回收与记账 |
| `kernel/sched/exec.c` | 已准备映像的提交与旧资源清理 |
| `kernel/sched/wait.c` | 全局 blocked 链、每队列 FIFO、超时和信号唤醒 |
| `kernel/sched/futex.c` | 256 桶 WAIT/WAKE/REQUEUE、robust-list 退出清理、clear-child-tid 唤醒 |
| `kernel/sched/signal.c` | 组/线程 pending、disposition、stop/continue 和重启 |
| `arch/riscv/process.c` | clone 寄存器、FP/TLS 继承与 exec 寄存器清零 |
| `arch/riscv/context.c`、`context_switch.S` | psABI context 和 SIE 临界区 |
| `kernel/sched/private.h` | 私有任务布局、组与队列成员关系 |

公共 scheduler 头不暴露 Trap Frame；架构 clone 入口位于 `include/arch/riscv/process.h`。syscall 通过不透明 task 接口取得 TID/TGID/PPID 和资源，不直接修改调度私有字段。

## 身份与资源

每个用户执行线程有独立 TID、FP/整数寄存器、signal mask、线程 pending、clear-child-tid、robust-list 注册地址、restart 状态、私有元数据页和独立的连续物理内核栈。组长承载 TGID、进程组、父子树、组 pending、退出通知、已回卷记账及 `RLIMIT_NOFILE`/`RLIMIT_STACK`。两项限制在组内线程间共享，普通 fork 复制，exec 保留；非组长 exec 接管组身份时一并转移。双向成员环包含组长容器；组长停止执行后仍留在环中，直到组结束或非组长 exec 接管身份。

普通 fork 从调用线程复制 MM 的 COW 页表/VMA、fd 表、fs context 和 disposition；OFD 仍按现有语义共享。子进程挂在调用线程的组长父子树中，并记录创建者 TID。线程 clone 通过 MM/files/fs/disposition 引用共享已有对象，不复制页表或 fd 槽。首次需要共享 disposition 而父线程尚无表时，会按需分配表页。

同组 fd 变更立即可见。阻塞 read/write/writev 在睡眠期间持有 OFD 引用；即使其他线程 close/dup 替换槽位，也不能提前回收正在使用的端点。退出请求必须使调用栈完成清理，不能直接释放阻塞任务页。

## clone 与 vfork

RISC-V clone 接收 flags、child_stack、parent_tid、tls、child_tid 和完整 syscall 入口寄存器。支持普通 SIGCHLD fork/vfork（均可额外指定 CLONE_FS 共享 cwd/root），以及共享 VM/FS/FILES/SIGHAND/THREAD 的线程组合和 SETTLS/PARENT_SETTID/CHILD_SETTID/CHILD_CLEARTID/SYSVSEM/DETACHED 兼容位。非法依赖返回 EINVAL，尚未闭环的合法资源组合返回 ENOTSUP。SYSVSEM 位不代表已经支持 SysV semaphore。

子线程继承完整 FP、整数现场和 mask；a0 为 0，PC 越过 ecall，非零 child_stack 替换 sp，SETTLS 设置 tp。SETTID 用户存储失败不回滚已经创建的任务，与固定 Linux clone 路径一致。所有内核分配失败都在发布前回滚；若回滚触及真实 VFS/block I/O owner，则由所属文件或 mount 记录，物理页和堆释放不另建重试状态；不会发布半构造子进程。

vfork 共享 MM，复制 files；fs 默认复制，显式 CLONE_FS 时共享。父线程和具体子进程保持双向完成关联；子进程释放共享 MM 引用后一次性完成等待。普通信号不解除等待。组退出/exec 的致命取消先断开双向关联，再唤醒父线程；子进程自己的 MM 引用仍有效，之后完成也不能访问已释放父任务。

## 等待与 futex

所有 BLOCKED 任务在全局双向 blocked 链上，并可加入一个等待队列 FIFO。队列 wake-one 取队首，wake-all 唤醒全部；摘除 blocked 和 queue 成员均为 O(1)。deadline 使用原始 time ticks，0 表示无限；tick 仍扫描 blocked 链处理到期。

WAIT 在关中断内读取用户字、比较 expected、登记并阻塞。futex key 为 MM record 身份和四字节对齐地址；哈希碰撞需二次匹配，REQUEUE 保留 FIFO，返回唤醒数与迁移数之和。值不匹配为 EAGAIN，非法地址为 EFAULT，非法参数为 EINVAL，超时为 ETIMEDOUT。无超时 WAIT 的 signal 唤醒走 generic restart：用户 handler 没有 SA_RESTART 时返回 EINTR，带 SA_RESTART 时 sigreturn 后重新执行并再次比较用户字。带超时 WAIT 使用独立 tagged restart state 保存用户地址、expected、operation 和首次调用计算的 monotonic absolute deadline；用户 handler 无论 flags 均看到 EINTR，没有 handler 的 stop/continue 路径经 restart_syscall 继续剩余 deadline。未支持命令为 ENOSYS。用户访问层状态损坏不能转换成普通用户错误。

`set_robust_list` 只核对 RV64 链头长度 24 字节并保存线程私有地址，允许空指针注销，不在注册时预读用户链。`get_robust_list` 支持当前线程及存活目标 TID；当前所有用户线程均为 root，查询其他线程按这一固定凭据模型可访问，输出先写长度再写指针。普通 clone/fork 不继承注册，失败 exec 保留，成功 exec 清理旧链并重置。链头与节点是可变用户内存，退出时才有界读取，不作为内核对象持有引用。

退出清理在原 MM 与原 TID 有效时同步完成，早于 clear-child-tid 和资源释放。成功的非组长 exec 在身份接管前保存旧 TID，切换到已验证新页表后、退休旧 MM 前清理；可返回的 exec 失败不清理旧链。遍历最多 2048 项，下一链接先于字更新读取；pending 项不会因已在链上而处理两次。owner 等于退出 TID 时通过可处理缺页/COW 的 32 位原子比较交换保留 WAITERS 并置 OWNER_DIED，必要时唤醒一个 waiter；pending 的非 owner 解锁窗口按 Linux 规则补唤醒。坏地址、错位、超长链和内存不足只结束该次尽力清理，不把用户错误升级为内核 fatal 或保留无 owner 的重试状态。

没有 MAP_SHARED 时，非 private futex 仅保证同一 MM 内语义，不提供跨 MM 共享 backing key。PI 标记项不按普通 robust 字更新；PI/bitset/wake-op、跨 MM 共享 futex 仍未实现。单 hart 的 SIE 临界区不构成 SMP 锁协议。

## 退出与 exec

exit 只退出当前线程，exit_group 和默认致命信号结束全组。退出先完成本线程的 robust-list 清理，再执行 clear-child-tid 清零/唤醒，最后释放 exec/files/fs/MM 等资源。任务仍在自己的内核栈上时不释放栈；切回可信 idle 栈后，先检查 canary、记录高水位并释放栈，再继续处理元数据和其他资源。

组长先退出进入 GROUP_DEAD，保留进程容器；普通成员资源清理成功后从组环移除并回卷时间。最后一个成员结束后，组长才成为唯一进程退出对象，向父进程产生一次 zombie/SIGCHLD。SIGCHLD 显式忽略或 NOCLDWAIT 的自动回收仍遵循信号模块契约。

退出切回保存的 idle/cleanup context，不依赖所有用户任务都阻塞。该 context 排空可完成的清理后主动派发 ready 任务；tick 对待清理标志只做 O(1) 检查并切换，不在中断热路径执行释放。只有真实 VFS/block I/O 清理失败保留原 owner，在后续清理机会重试；合法页/堆释放完成即返回，分配器不变量错误进入 fatal。真实 I/O 清理重试、GROUP_DEAD 和 zombie 只保留元数据，均不保留已经停止执行的内核栈；再次进入退出队列的旧组长不会重复释放栈。

exec 完成映像验证后收拢组内其他线程。竞争 exec 或已被组终止的线程清理自己的事务并退出；准备失败仍保留原映像。非组长成功 exec 接管原 TGID、父子树位置、组 pending 和累计记账，旧 TID 被释放，旧组长容器静默回收。新映像最终只有一个执行成员，重置自定义 handler 和寄存器，按 CLOEXEC 关闭描述符。

## wait 与记账

wait4 遍历组长父子树，默认允许等待同组其他线程的子进程；线程本身不作为独立子进程出现。`__WNOTHREAD` 按创建/收养线程关系过滤。创建者退出时迁移给仍存活成员，进程退出时收养到 PID1；迁移必须在旧 TID 重用前更新记录。

支持 pid 选择、WNOHANG/WUNTRACED/WCONTINUED/__WALL/__WCLONE。组 stop 完成状态与组长执行状态分离，因此 GROUP_DEAD 容器仍能报告存活成员的组 stop/continue。不可中断等待尚未完成的成员保留 stop pending，不提前宣称整组停止。

zombie 先逻辑回收再复制 status/rusage，因此坏输出指针的 EFAULT 不让它再次可 wait。退出和 reparent 可能向退出队尾追加 orphan zombie，摘头必须使用更新后的链关系，不能丢失新增 owner。`times()` 累计所有仍在组环中的线程及已回卷成员；子进程时间保存在组长，不重复计数。

## 同步、成本与验证边界

单 hart 的 SIE 临界区串行化组关系、fd/MM 引用和 futex 登记。共享 MM 使用同一页表与本地 SFENCE.VMA；这不是 SMP 协议。多 hart 前仍须补锁、页引用原子操作和远端 TLB shootdown。

每线程拥有独立的 4 KiB 元数据页和 8 KiB 连续物理内核栈（buddy order 1）；调度器初始化要求分配器已进入 finalized buddy 模式，栈分配、构造回滚与正常释放使用同一 order。栈底留 16 字节对齐区和 canary，剩余 8176 字节包含 Trap Frame 与 C 调用链。新栈填充固定字节，初始用户 Trap Frame 显式清零。退出后只在其他可信栈扫描未覆盖前缀；累计最小剩余空间和最大已用空间由只读统计接口提供，生产 PID 1 完成时报告。构造回滚同样释放独立栈，不让资源清理失败保留它。

`make test-stack-usage` 强制重建隔离的生产对象，编译器 `-fstack-usage` 产出逐函数记录；host probe 从实际栈配置和 Trap Frame 头计算容量、guard、汇编 Frame 与余量预算，避免测试大栈或旧报告污染门禁。`tests/stack-usage.py` 拒绝超出“栈容量减 16 字节、288 字节汇编 Trap Frame、1024 字节余量”的单帧及无界动态栈；该检查不能证明完整调用链。真实 root-init、静态 musl 与动态 pthread 测试另要求已退出任务的最小实测余量至少 1024 字节，不足时必须扩大栈后重新运行。Canary 用于发现破坏，填充测量用于观察高水位；两者都不等价于未映射 guard page，也不证明未执行分支的栈界。ASID 0 的切换刷新成本、FIFO/100 Hz tick、线性 wait4 与 deadline 扫描仍存在。

聚焦入口为 `make test-stack-usage`、`make test-scheduler-cases-riscv`、`make test-scheduler-riscv`、`make test-files-riscv`、`make test-signal-riscv` 和 `make test-root-init-riscv`；组合消费者复用 `make test-userland-riscv`，其中真实 pthread 探针从工作线程修改两项组限额、在主线程观察并恢复，还检查未实现资源不伪造成功。阶段收口使用 `make test-riscv`。各次实际通过范围以 README 和提交验证说明为准，不把实现路径存在等同于全部线程负载已验证。

尚无 SMP、MAP_SHARED、PI futex、跨 MM 共享 futex、实时信号队列、sigaltstack、clone3 或 LoongArch context。固定语义依据见学习总结的 Linux commit 与 musl 归档。
