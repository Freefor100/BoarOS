# 内核调度、线程组与生命周期

本文记录RV/LA单CPU的OTHER/FIFO/RR 调度、线程组资源、clone/exec/wait 和回收契约。背景见[线程组与 futex](../learning/threads-and-futex.md)，信号、文件资源、MM 的稳定边界分别见对应模块文档。

## 实现入口

| 文件 | 职责 |
|---|---|
| `kernel/sched/core.c` | 创建、切换、tick 入口、资源借用校验 |
| `kernel/sched/policy.c`、`runqueue.c` | 纯策略/预算状态机、分级 ready 队列 |
| `kernel/sched/scheduling.c`、`kernel/syscall/sched.c` | 实际运行时间记账、抢占、安全返回边界、策略与 CPU0 affinity ABI |
| `kernel/sched/process.c` | clone、线程组/父子树、wait、退出、回收与记账 |
| `kernel/pid.c` | 统一身份对象、编号/代次、引用与 TID/TGID/PGID/SID 角色成员 |
| `kernel/sched/proc.c` | 进程快照、对象路径与缺页/磁盘读取统计 |
| `kernel/sched/exec.c` | 已准备映像的提交与旧资源清理 |
| `kernel/sched/sync.c` | 任务 owner 的 mutex/RWlock、锁序与 FIFO 资格交接 |
| `kernel/sched/park.c`、`wait.c` | 代次等待、借用游标、blocked/期限索引及完成仲裁 |
| `kernel/sched/futex.c` | 256 桶 WAIT/WAKE/REQUEUE、robust-list 退出清理、clear-child-tid 唤醒 |
| `kernel/sched/signal.c` | 组/线程 pending、disposition、stop/continue 和重启 |
| `include/arch/task.h`、`arch/riscv/process.c`、`arch/loongarch/context.c` | 构建期线程、地址空间与 trap 操作；各架构初始/clone/exec 寄存器契约 |
| `arch/riscv/`、`arch/loongarch/` 的 context/trap 汇编 | 架构 ABI context、用户异常返回与中断状态 |
| `kernel/sched/private.h` | 私有任务布局、组与队列成员关系 |

公共 scheduler 头不暴露 Trap Frame；架构 clone 入口由 `include/arch/task.h` 选择，旧 RV 入口保留包装。syscall 通过不透明 task 接口取得 TID/TGID/PPID 和资源，不直接修改调度私有字段。

等待登记、阻塞提交、期限索引和runnable发布使用统一rank40短锁；heap10→physical20、
对象内部30→调度40是raw锁序。raw不跨架构切换，不允许调度、分配、I/O、用户复制或外部回调。
业务条件、进程组及缓存/网络数据仍按单CPU纪律保护；等待原语互斥不表示整个内核支持SMP。

`kernel_wait_prepare`为当前任务建立不回退的全局代次token，登记后复查条件，释放对象保护再调用
`kernel_wait_park`；`kernel_wait_finish`同步撤销绑定节点。wake/期限/signal第一个完成者决定
原因，旧代次与重复通知无效，任务页复用也不重用代次。多队列节点通过`kernel_wait_node_bind`共享token。
代次耗尽返回INVALID_STATE且不发布新登记，重复等待和错误owner属于fatal。

任务的on_cpu与CPU切换前驱共同保护旧栈：阻塞提交后的提前wake只记录ready_pending，
park自行保存IRQ并保持关闭跨过切换交接，返回时恢复原状态；
新栈的`kernel_scheduler_switch_finish`才发布旧任务或允许回收。恢复已有上下文的C切换尾部、
首次kernel/user trampoline及退出切换经过同一完成点；无切换的普通异常返回不重复查询。
调度请求用原子exchange消费，完成点不清除后来请求。

wake在调度锁内判断空队列；为空直接完成，非空才建立跨解锁的借用游标。
被摘节点保留有引用的退休后继，引用链迭代归还，每段最多16项。
callback在调度锁外同步执行，临时禁止抢占，不能阻塞；自身只能非阻塞remove。
释放context前必须remove_sync等待借用归零。futex过滤遍历使用同一借用游标；
requeue先借task、放当前游标，再收完旧node借用并迁移登记，不在raw内归还后备引用。
queue_close停止新登记并借用队列直到通知完成，空队列也不能在close的解锁间隙销毁。
queue_destroy在注册或操作/游标/回调借用未归零时返回BUSY，调用者继续持有queue及业务owner。

聚焦入口：`make test-wait-host test-wait-riscv test-wait-loongarch`。host直接链接park.c、
真实raw/runqueue，在2/4线程握手下检查提前wake、单次仲裁、旧代次、多队列与退休游标；
双侧512MiB/1GiB检查真实首次/退出切换、timeout及任务页回到基线。此处是原语与单CPU证据，
不替代共享MM、业务对象或多核客体的并发验收。

唯一任务节点的通知在同一raw区间完成，仍验证queue/borrow_owner身份，不建立跨解锁游标；
回调保留借用并在锁外执行。
finish每段最多摘16个登记，未借用的小批登记可在同一保护下结束；存在借用才等待归还。

可睡眠RWlock每实例持有rank30内部raw；资格判断与token登记在同一保护下完成，释放raw再park。
未竞争取得不在热路径栈上构造waiter；慢路径持有其栈waiter至token结束及pending摘除。
FIFO先授资格再通知，连续读者按原边界分段，每段最多16个等待者，handoff owner覆盖分段间隙。
已经授予但尚未运行的读者/写者仍占用锁，后来读者不能越过writer。
通知不等于资格：额外唤醒只更新等待token，不重排FIFO；旧登记结束与重取对象raw之间也允许授资格。guard的rank/key和任务owner
保持原契约；等待仍不可中断，没有新增timed/interruptible API。

调用方无raw业务锁时，`KERNEL_WAIT_RECHECK`在登记后求值一次非阻塞条件，再park/finish。
poll的多个节点绑定一个token；epoll停止item通知后同步撤销node才释放context。
TTY/lwext4等通用通道仍由所属调用方在单CPU IRQ域内持条件owner；宏不把业务数据变成跨核安全。
宿主RW门禁为`make test-sleep-lock-host`，包含1/8/32等待者、writer边界、分段中到达的读者、
IRQ恢复及非法owner/上下文。真实I/O等待继续用`make test-io-sleep-riscv`验证DMA、超时与回收。

## 策略、就绪队列与 RT 预算

OTHER 保留 100 Hz tick 轮转，内核 worker 使用 OTHER。FIFO/RR 的用户优先级为 1–99，数值越大越优先；FIFO 不因 tick 同级轮转，RR 每片 100ms，按实际在 CPU 上运行的纳秒扣除。更高优先级抢占、阻塞和 yield 不重新赠送 RR 时间片；耗尽后才重装。唤醒及策略修改标记 need_resched，在 IRQ 或返回用户态的安全边界切换。降优先级排到新级队首，升优先级排队尾，同级修改保留位置；高优先级抢占的当前任务回原级队首。

ready 节点独立于 blocked/cleanup 链。全局优先级排序双链保留遍历视图，100 个等级各存 head/tail；同级插入、OTHER 尾追加、删除和选取均 O(1)，新增空的中间等级最多扫描 99 个等级，不扫描任务数。RT 被节流时只取 OTHER 队首；没有 OTHER 则 idle，不能借机运行已耗尽的 RT。

FIFO yield和RR到期只轮转同级候选；未节流且没有同级竞争者时，不能把CPU下让给低优先级任务。
旧任务仍on_cpu而暂未入队，决策必须显式比较当前与ready候选，不能把“有next”当作可轮转。
`test-scheduler-handoff-host`直接链接生产决策、队列/策略/raw，覆盖低/同/高优先级、节流和idle；
`test-scheduler-handoff-riscv/test-scheduler-handoff-loongarch`在512MiB/1GiB以同ELF对照固定Linux，
高优先级yield/跨RR片时低任务计数保持0，随后高任务阻塞才允许低任务前进并正常回收。

退出在发布 completion、清 TID 并唤醒等待者后，若已注册 cleanup worker，按同一
ready 策略直接选择下一任务。退出栈只切离，回收仍由 worker 或 join owner 在可信栈
执行；不能强制返回 idle，使已经 ready 的 joiner 在 `wfi` 中等待下一次 timer。
没有 cleanup worker 的模块 fixture 继续返回负责回收的 idle。
`test-scheduler-cases-riscv` 在未启动 timer 时验证退出、join 和清理的连续进展，
以及除启动 owner 所持 cleanup worker 外的页回到基线。

全局预算默认 period=1,000,000us、runtime=950,000us，仅运行中的 FIFO/RR 扣费，阻塞及 idle 不扣。周期补充不积累旧余额；runtime=-1 禁用节流，0 不允许运行 RT。控制更新在 IRQ 临界区先结算旧配置下的实际运行，校验 period>0、runtime>=-1 且 runtime<=period（-1除外）、两值均不超过 INT_MAXus。runtime 修改不清消费；相同 period 写入不重开周期，改变 period 时从当前时刻建立新长度但保留已消费值。proc 控件见 [procfs](procfs.md)。

硬件 timer 取普通 tick、RT 配额耗尽/补充和 RR 片尾中的最早期限。预算 IRQ 可以返回 elapsed_ticks=0；此时仍重新记账调度，但不推进 tick、CPU tick 统计或 coarse clock。实际运行时间由切换、策略/配置修改和 timer 入口结算，精度受关中断临界区与模拟器 IRQ 延迟限制。用户访问复制与可睡眠路径不放在调度状态锁内。

sched_setparam/setscheduler/getparam/getscheduler、priority_min/max、rr_get_interval 和 CPU0 affinity 使用真实任务状态；未发布 child 不可被按 TID 查询。BATCH/IDLE/DEADLINE/EXT 的 min/max 查询按固定 Linux 返回 0，设置这些策略仍显式拒绝。RESET_ON_FORK 在子任务清除并将继承的 RT 变为 OTHER；exec 保留执行线程的策略。没有实现 nice 调度、SMP、cgroup 配额或 deadline scheduler。

确定性 host 测试 `tests/host/sched_{policy,runqueue,syscall}_test.c` 覆盖预算、排队、ABI 和随机队列模型。`tests/diff-abi/sched_policy.c` 的 23 条同 ELF Linux 对照覆盖参数及真实 FIFO/yield/更高优先级抢占/RR轮转；`sched_stat.c` 另有5条策略stat和STOPPED/zombie wchan对照。`tests/sched-bandwidth-riscv.sh` 使用真实 FIFO 忙循环验证耗尽、OTHER 进展、周期恢复、同值写不补预算和 -1 切换保留消费；500us/250us 场景先等待新 coarse tick，再检查8ms以内被抢占并报告实际超出量。RR 在高优先级任务运行60ms后仍只剩约40ms，能检出错误重装100ms的实现。两个先阻塞后赋RR策略的子进程由同一pipe write唤醒，在100ms quota/300ms period下验证同时到期仍把CPU交给同级peer；旧队首重排已由该测试复现失败。该测试直接调用 SYS_sched_setscheduler，固定 musl 的同名包装函数是 ENOSYS 存根。测试用 guest monotonic 计时，不由 QEMU 墙钟速度推断调度正确性。

## 身份与资源

TID、TGID、PGID 和 SID 共用 `kernel_pid` 对象：编号、不可回退的代次、引用计数和四类成员链只有一份真实身份。每个线程挂接 TID；线程组代表挂接 TGID、PGID、SID，其他线程通过代表取得组身份。`tid` 仅为对象编号的派生缓存，由身份事务维护，并由调度校验检查一致性。fork 继承父进程的 PGID/SID 对象；线程 clone 只新增 TID。`__WNOTHREAD` 的创建者关系持有 TID 对象引用，退出、收养和非组长 exec 转移关系，不按可能复用的整数判等。

编号仅在全部角色成员和临时引用都释放后归还 bitmap。对象仍有引用不表示 TID 对应线程仍可收信号；线程 lookup、组 lookup 和 proc 可见性分别检查其角色及生命周期。组长先退出保留身份容器，直到最后线程退出或非组长 exec 原子接管四类成员。zombie 在 wait/reap 前保留进程身份；旧 proc 节点用同一对象的代次防止编号复用后命中新进程。

对象存储来自调度器私有 slab heap；准备分配可以睡眠，但必须先于身份发布，发布、角色迁移和摘除区间禁止分配或 yield。单 hart 的 SIE=0 本身不代表禁止睡眠：既有 syscall/回收路径可调度，不能因此把跨分配的裸身份指针当作受保护引用。最后引用先摘编号索引、登记待释放对象，再在身份变更结束后归还存储。该机制不宣称 SMP 安全。固定依据为 `references/linux/kernel/pid.c` 的角色成员和 exec 转移，以及 `references/linux/kernel/sys.c`、`kernel/exit.c`，commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e`；本实现按已选契约额外让临时引用保留编号。

每个用户执行线程有独立 TID、FP/整数寄存器、signal mask、线程 pending、clear-child-tid、robust-list 注册地址、restart 状态、私有元数据页和独立的连续物理内核栈。组长承载 TGID、进程组、父子树、组 pending、退出通知、已回卷记账及 `RLIMIT_NOFILE`/`RLIMIT_STACK`。两项限制在组内线程间共享，普通 fork 复制，exec 保留；非组长 exec 接管组身份时一并转移。双向成员环包含组长容器；组长停止执行后仍留在环中，直到组结束或非组长 exec 接管身份。

普通 fork 从调用线程复制 MM 的 COW 页表/VMA、fd 表、fs context 和 disposition；OFD 仍按现有语义共享。子进程挂在调用线程的组长父子树中，并记录创建者 TID。线程 clone 通过 MM/files/fs/disposition 引用共享已有对象，不复制页表或 fd 槽。首次需要共享 disposition 而父线程尚无表时，会按需分配表页。

同组 fd 变更立即可见。阻塞 read/write/writev 在睡眠期间持有 OFD 引用；即使其他线程 close/dup 替换槽位，也不能提前回收正在使用的端点。退出请求必须使调用栈完成清理，不能直接释放阻塞任务页。

## 会话与进程组

`setpgid/getpgid/getsid/setsid` 使用同一身份对象的 PGID/SID 角色。显式 `fork_no_exec` 与 `session_leader` 状态分别约束父进程改子组和会话组长，不能用 SID 数值等于 TGID 代替曾成功 setsid 的状态。父进程可在子 exec 前修改其组；成功 exec 后同会话返回 EACCES，跨会话先返回 EPERM。查询可指定非组长 TID，setpgid 指定非组长 TID 返回 EINVAL；setsid 从任一组内线程发起都作用于进程代表。普通 fork 继承组/会话但不继承会话组长标记，exec 保留该标记，非组长 exec 一并转移。

组长退出或被收割不销毁仍有 PGID/SID 成员的身份。zombie 在被收割前允许查询和符合条件的 setpgid；kill 对仍存在的 zombie 进程或组成功但不向已结束线程排队。kill 的正值按 TID 查找所属进程，零/负 PGID 按角色成员查找，wait4 的零/负 PGID 按当前真实组关系选择子进程；proc stat/status 从对象获取相同组/会话信息。

进程退出与跨进程 reparent 检查最后一个同会话、不同组的父关系是否消失；忽略 init 父关系和真正结束的成员。新孤儿组含已完成 group stop 的任务时，全组依次收到内核 SIGHUP、SIGCONT（SI_KERNEL），恢复停止任务。仅有 TID 的线程回收不触发进程级孤儿事件。固定 Linux `kernel/sys.c` 的 setpgid/setsid 只改角色，不主动调用孤儿 HUP/CONT 检查；`kernel/exit.c` 的调用点是 reparent_leader 和 exit_notify，本实现保持这一范围。已孤儿组的默认 TSTP/TTIN/TTOU 丢弃，SIGSTOP 仍停止；[Serial TTY](kernel-tty.md) 通过稳定 SID/前台 PGID 引用接入 ctty、终端组信号与线程组生命周期；完整凭据权限模型仍未实现。

`tests/diff-abi/session.c` 使用 pipe/wait4 握手与专用 exec probe，覆盖父子 exec errno、线程目标、非组长 setsid/exec、session 边界、zombie/proc、组定向 kill/wait、进程组长先收割后的组存续、退出/reparent 两条孤儿路径，以及 sigtimedwait 和 SA_SIGINFO 的内核信号来源。启动上下文不同：固定 Linux 裸 PID 1 初始 PGID/SID 为 0，BoarOS 初始身份为 1；测试先建立真实非零会话，再比较用户操作，不把启动整数差异混入会话机制断言。

## clone 与 vfork

通用进程策略由构建期 `arch_process_*` 后端准备寄存器。LA64的系统调用顺序为
flags、child_stack、parent_tid、child_tid、tls，由LA trap重排成下述通用顺序；
SETTLS写r2/TP、child stack写r3/SP、返回a0=0并使ERA前进4字节。LA已保存独立FP/SIMD owner，区分启用宽度、live宽度和used_math，其首用、信号和成本边界见[LA浮点](loongarch-fpu.md)，不套用RV的FS Dirty或SIMD范围。真实LP64S musl线程/TLS/取消/非PI robust与
组生命周期由 `make test-pthread-loongarch` 双侧验证，两处构造OOM和重试由
`make test-pthread-oom-loongarch` 验证；任务页/栈及根盘owner要求恢复基线。

RISC-V clone 接收 flags、child_stack、parent_tid、tls、child_tid 和完整 syscall 入口寄存器。支持普通 SIGCHLD fork/vfork（均可额外指定 CLONE_FS 共享 cwd/root，或指定 CHILD_SETTID/CHILD_CLEARTID 管理子进程私有 MM 中的 TID），以及共享 VM/FS/FILES/SIGHAND/THREAD 的线程组合和 SETTLS/PARENT_SETTID/CHILD_SETTID/CHILD_CLEARTID/SYSVSEM/DETACHED 兼容位。非法依赖返回 EINVAL，尚未闭环的合法资源组合返回 ENOTSUP。SYSVSEM 位不代表已经支持 SysV semaphore。

子线程继承完整 FP、整数现场和 mask；a0 为 0，PC 越过 ecall，非零 child_stack 替换 sp，SETTLS 设置 tp。CHILD_SETTID 在子任务首次返回用户态前、子 MM 激活后写入，因此可正常处理 COW 和缺页；PARENT_SETTID 仍在父任务发布 clone 时写入。SETTID 用户存储失败不回滚已经创建的任务，与固定 Linux clone 路径一致。所有内核分配失败都在发布前回滚；若回滚触及真实 VFS/block I/O owner，则由所属文件或 mount 记录，物理页和堆释放不另建重试状态；不会发布半构造子进程。

vfork 共享 MM，复制 files；fs 默认复制，显式 CLONE_FS 时共享。父线程和具体子进程保持双向完成关联；子进程释放共享 MM 引用后一次性完成等待。普通信号不解除等待。组退出/exec 的致命取消先断开双向关联，再唤醒父线程；子进程自己的 MM 引用仍有效，之后完成也不能访问已释放父任务。

## 等待与 futex

所有 BLOCKED 任务在全局双向 blocked 链上，并可加入一个等待队列 FIFO。队列 wake-one 取队首，wake-all 唤醒全部；摘除 blocked 和 queue 成员均为 O(1)。deadline 使用原始 time ticks，0 表示无限；带期限任务同时进入以 (deadline, tid) 为键的有序索引，tick 只弹出已到期者，全部唤醒路径统一摘除索引项。

WAIT 在关中断内读取用户字、比较 expected、登记并阻塞。private key 使用不复用的 MM 身份号与四字节对齐地址；共享匿名 key 使用后备对象身份与连续字节偏移，等待者持有对象引用直到原等待调用恢复。非 private 操作先从用户映射读取并解析后备，坏页在 WAKE 中也返回 EFAULT。哈希碰撞需二次匹配，REQUEUE 将目标 key 的引用交给迁移的等待者、保留 FIFO，返回唤醒数与迁移数之和。WAIT_BITSET 为等待者保存非零掩码；WAKE_BITSET 只唤醒掩码相交者，普通 WAKE 匹配全部，REQUEUE 保留原掩码。值不匹配为 EAGAIN，非法地址为 EFAULT，零掩码等非法参数为 EINVAL，超时为 ETIMEDOUT。

WAIT 的超时为相对 monotonic；WAIT_BITSET 的超时为绝对 monotonic 或带 `FUTEX_CLOCK_REALTIME` 的绝对 realtime。当前没有 `clock_settime`/`settimeofday` 写入口，启动后的 realtime offset 不变，故可一次转换为 monotonic deadline；将来加入可调时钟时必须重新审查阻塞中的实时截止时刻。无超时等待的 signal 唤醒走 generic restart：用户 handler 没有 SA_RESTART 时返回 EINTR，带 SA_RESTART 时 sigreturn 后重新执行并再次比较用户字。带超时等待使用 tagged restart state 保存用户地址、expected、operation、掩码和首次调用的 monotonic absolute deadline；用户 handler 无论 flags 均看到 EINTR，没有 handler 的 stop/continue 路径经 restart_syscall 继续剩余 deadline。未支持命令为 ENOSYS。用户访问层状态损坏不能转换成普通用户错误。

`set_robust_list` 只核对 RV64 链头长度 24 字节并保存线程私有地址，允许空指针注销，不在注册时预读用户链。`get_robust_list` 支持当前线程及存活目标 TID；当前所有用户线程均为 root，查询其他线程按这一固定凭据模型可访问，输出先写长度再写指针。普通 clone/fork 不继承注册，失败 exec 保留，成功 exec 清理旧链并重置。链头与节点是可变用户内存，退出时才有界读取，不作为内核对象持有引用。

退出清理在原 MM 与原 TID 有效时同步完成，早于 clear-child-tid 和资源释放。成功的非组长 exec 在身份接管前保存旧 TID，切换到新页表前、旧 MM 仍激活时清理；可返回的 exec 失败不清理旧链。遍历最多 2048 项，下一链接先于字更新读取；pending 项不会因已在链上而处理两次。owner 等于退出 TID 时通过可处理缺页/COW 的 32 位原子比较交换保留 WAITERS 并置 OWNER_DIED，必要时唤醒一个 waiter；pending 的非 owner 解锁窗口按 Linux 规则补唤醒。坏地址、错位、超长链和内存不足只结束该次尽力清理，不把用户错误升级为内核 fatal 或保留无 owner 的重试状态。

带 `FUTEX_PRIVATE_FLAG` 的操作仍只在同一 MM 内匹配；未置该标记的共享匿名映射可跨 fork 的独立 MM 唤醒或 requeue。其他已支持的私有映射仍使用 MM key。共享文件映射已实现，其 futex 尚无跨 MM 后备 key。PI 标记项不按普通 robust 字更新；PI、WAKE_OP 和 futex2 仍未实现。单 hart 的 SIE 临界区不构成 SMP 锁协议。

## 退出与 exec

exit 只退出当前线程，exit_group 和默认致命信号结束全组。退出先完成本线程的 robust-list 清理，仅当同一 MM 仍有其他活跃使用者时执行 clear-child-tid 清零/唤醒，完成可能睡眠的用户复制后再注销本使用者，最后释放 exec/files/fs/MM 等资源。任务仍在自己的内核栈上时不释放栈；切回可信清理上下文后检查 canary、高水位并回收旧栈；运行期需要存储的资源释放由可调度内核清理任务执行。

BoarOS 的 PID 1 最终退出启动整机收口：cleanup worker 保存它的完成状态，通过
`kernel_scheduler_stop_users()` 对剩余用户组请求致命退出，等待正常取消/回收完成，
再停止网络与存储 worker、卸载根盘及检查内存基线。内核 worker 在用户 I/O owner
释放之前继续服务；不强制卸载活引用，也不按 daemon 名称清理。只在关机阶段枚举
现有任务表，等待仍复用 cleanup 队列，不轮询忙等。实际子进程、STOPPED、组长先
退出与关机中 fork 的回归为 `python3 -B tests/root-shutdown-riscv.py`；先用通用
`INIT_CONFIG=config/init.json` 构建。根因与旧基线反例见[收口记录](../learning/threads-and-futex.md#pid-1-关机与存活后台-owner2026-10-06)。

组长先退出进入 GROUP_DEAD，保留进程容器；普通成员资源清理成功后从组环移除并回卷时间。最后一个成员结束后，组长才成为唯一进程退出对象，向父进程产生一次 zombie/SIGCHLD。SIGCHLD 显式忽略或 NOCLDWAIT 的自动回收仍遵循信号模块契约。

退出切回保存的 idle/cleanup context，不依赖所有用户任务都阻塞。该 context 排空可完成的清理后主动派发 ready 任务；tick 对待清理标志只做 O(1) 检查并切换，不在中断热路径执行释放。只有真实 VFS/block I/O 清理失败保留原 owner，在后续清理机会重试；合法页/堆释放完成即返回，分配器不变量错误进入 fatal。真实 I/O 清理重试、GROUP_DEAD 和 zombie 只保留元数据，均不保留已经停止执行的内核栈；再次进入退出队列的旧组长不会重复释放栈。

exec 完成映像验证后收拢组内其他线程。竞争 exec 或已被组终止的线程清理自己的事务并退出；准备失败仍保留原映像。非组长成功 exec 接管原 TGID、父子树位置、组 pending 和累计记账，旧 TID 被释放，旧组长容器静默回收。新映像最终只有一个执行成员，重置自定义 handler 和寄存器，按 CLOEXEC 关闭描述符。

## wait 与记账

wait4 遍历组长父子树，默认允许等待同组其他线程的子进程；线程本身不作为独立子进程出现。`__WNOTHREAD` 按创建/收养线程关系过滤。创建者退出时迁移给仍存活成员，进程退出时收养到 PID1；迁移必须在旧 TID 重用前更新记录。

支持 pid 选择、WNOHANG/WUNTRACED/WCONTINUED/__WALL/__WCLONE。组 stop 完成状态与组长执行状态分离，因此 GROUP_DEAD 容器仍能报告存活成员的组 stop/continue。不可中断等待尚未完成的成员保留 stop pending，不提前宣称整组停止。

zombie 先逻辑回收再复制 status/rusage，因此坏输出指针的 EFAULT 不让它再次可 wait。退出和 reparent 可能向退出队尾追加 orphan zombie，摘头必须使用更新后的链关系，不能丢失新增 owner。`times()` 累计所有仍在组环中的线程及已回卷成员；子进程时间保存在组长，不重复计数。

## 同步、成本与验证边界

单 hart 的 SIE 临界区串行化组关系、fd/MM 引用和 futex 登记。共享 MM 使用同一页表与本地 SFENCE.VMA；这不是 SMP 协议。多 hart 前仍须补锁、页引用原子操作和远端 TLB shootdown。

RV每线程拥有独立的 4 KiB 元数据页和 8 KiB 连续物理内核栈（buddy order 1）；调度器初始化要求分配器已进入 finalized buddy 模式，栈分配、构造回滚与正常释放使用同一 order。RV生产内核把栈映射到内核栈窗口（`RISCV_KERNEL_STACK_WINDOW_BASE` 起 128 MiB，12 KiB 槽：前置 4 KiB 页不建 PTE，作为未映射 guard，后接 8 KiB 栈）；空窗口骨架在页表构建期预留，所有用户根按值继承同一子树，叶子由受限的运行期接口插入/删除，空 level-0 表在最后一个叶撤除时释放，guard 页不占物理页。无活动内核页表的测试 fixture 继续使用 direct-map 栈。栈底留 16 字节对齐区和 canary，剩余 8176 字节包含 Trap Frame 与 C 调用链。新栈填充固定字节，初始用户 Trap Frame 显式清零。退出后只在其他可信栈扫描未覆盖前缀；累计最小剩余空间和最大已用空间由只读统计接口提供，生产 PID 1 完成时报告。构造回滚同样释放独立栈，不让资源清理失败保留它。

LA生产任务也沿现有内核栈窗口接口运行：PGDH共享骨架持有永久boot页，任务叶
仅借用原连续物理栈，PLV0/RW/NX且下方16KiB未映射。异常入口在写frame前
核对SP是否容得下304字节；不足时切到保留的可信异常栈输出fatal。合法栈上界
按SP下方字节定位槽位。用户PGDL切换不改PGDH，回收先撤叶/TLB同步再归还栈。
该硬件保护和映射失败/骨架OOM回滚由`test-stack-guard-loongarch`验证。

`make test-stack-usage` 强制重建隔离的生产对象，编译器 `-fstack-usage` 产出逐函数记录；host probe 从实际栈配置和 Trap Frame 头计算容量、guard、汇编 Frame 与余量预算，避免测试大栈或旧报告污染门禁。`tests/stack-usage.py` 拒绝超出“栈容量减 16 字节、288 字节汇编 Trap Frame、1024 字节余量”的单帧及无界动态栈；该检查不能证明完整调用链。真实 root-init、静态 musl 与动态 pthread 测试另要求已退出任务的最小实测余量至少 1024 字节，不足时必须扩大栈后重新运行。Canary 用于发现破坏，填充测量用于观察高水位；生产栈的未映射 guard 只对经窗口 VA 的 SP 式越界生效，direct-map 别名仍可达同一批物理页，且二者都不证明未执行分支的栈界。idle/boot 栈仍在 .bss 高半区别名内，没有 guard。ASID 0 的切换刷新成本、OTHER 的 100 Hz tick 与线性 wait4 仍存在。

聚焦入口为 `make test-stack-usage`、`make test-scheduler-cases-riscv`、`make test-scheduler-riscv`、`make test-files-riscv` 和 `make test-signal-riscv`；`make test-userland-riscv` 验证真实 pthread、共享匿名 futex、bitset 绝对 realtime 等待在 stop/continue 后保留掩码和截止时刻。`make test-diff-abi-riscv` 用同一 ELF 对照固定 Linux 的零掩码、超时、错误、按掩码唤醒和 requeue；`make test-glibc-riscv` 验证 glibc 2.44 的 `pthread_join` 消费路径。阶段收口使用 `make test-riscv`。各次实际通过范围以 README 和提交验证说明为准，不把实现路径存在等同于全部线程负载已验证。

尚无 SMP、共享文件 futex、PI futex、实时信号队列、sigaltstack 或 clone3。LoongArch 已验证整数 context、timer 抢占、信号、musl TLS/pthread、动态 musl/DSO TLS、标量 FPU 与 LSX/LASX；更广程序与多核仍需独立验收。固定语义依据见学习总结的 Linux commit 与 musl 归档。

活动普通文件/TCP I/O 的单页暂存由任务持有并跨调用复用：首次使用时分配，调用期间登记在任务的 `io_buffer`，正常调用完成只解除登记。任务资源清理在 socket read/write reservation 之后、MM/文件表和任务栈释放之前解除登记；常驻页在任务最终存储释放时归还。页释放错误遵循物理分配器 fatal 不变量，不进入历史 cleanup 重试链。

## 存储等待与清理任务

`kernel/sync.h` 的 mutex/RWlock guard 记录任务 io_context owner 与锁序，非法释放、递归误用、逆序及读转写触发 fatal。争用者按 FIFO 排队；释放时先给队首写者预留独占资格，或给连续队首读者预留共享资格，再定向唤醒。预留者尚未运行时也阻止新到任务抢占；排队写者后的读者及 try_read 不能越过队列。等待不可中断，信号退出在取得与释放后处理，栈上的等待记录不被提前销毁。只有队列/引用/状态发布使用短关中断区，持锁跨设备等待允许其他任务运行。分配器回收深度及 lwext4 重入深度属于任务上下文。

外部IRQ的公共trap返回在S-mode仅允许空闲任务、且未持I/O锁时消费need_resched，防止中断在开中断与WFI之间已处理却继续入睡。这条限制不适用于既有timer调度：开启SIE的普通内核线程仍可在tick切换，持有I/O锁不等于禁止抢占。syscall进入时SIE关闭，但显式block/yield仍能切换；共享元数据必须有独立临界区和睡眠前引用，不能以单hart或持锁作为整段不可切换的证明。buddy/slab已补对应保护，见[物理页模块](physical-pages.md)。`test-scheduler-cases-riscv` 不借助 timer 验证 idle 返回调度，并验证读写 FIFO、后到者不抢锁及 1/8/32 等待者；原实现分别出现一次 idle 失败和两次交接顺序失败。成本锁持有时间包含资格已交接但获得者尚未运行的区间。

存储等待以 `interruptible=0` 登记：pending 信号与组退出不能拆除 DMA owner；设备完成或 reset 后原调用栈先释放资源，再在用户返回边界处理退出。指定的 cleanup task 排空已退出任务和 root-boot 收尾，阻塞时正常调度；idle/IRQ 不进入运行期可睡眠存储。无块设备的纯模块 fixture 可继续由 idle 回收不含存储的任务。清理结束检查任务锁/backend 状态均为空。

`make test-scheduler-cases-riscv test-scheduler-riscv test-io-sleep-riscv test-userland-riscv` 保护 FIFO 资格交接、设备等待/唤醒与真实任务组合行为。

`make test-diff-abi-riscv` 的 `tid.*` 使用同一 ELF 对照固定 Linux，覆盖私有 fork、共享页但独立 MM、vfork 退出/成功与失败 exec、坏地址/只读地址、线程 futex 等待。成功 exec 与退出共用旧 MM 的注销和清 TID 顺序；延迟资源清理不增加活跃使用者数。

## 系统统计与可等待内核任务

调度器独立登记所有已发布任务，直到最后 metadata 回收才注销；构造失败不发布。`kernel_scheduler_system_statistics()` 返回真实任务数（含尚未回收的 zombie，16 位饱和）和 16 位小数的负载。每 501 个 100 Hz tick 按固定 Linux 的 1884/2014/2037（11 位小数）更新 1/5/15 分钟指数平均。READY/RUNNING 与不可中断 BLOCKED 计入活动量；空闲 worker/cleanup 的工作队列等待标为 interruptible，不凭空形成空闲负载，实际存储与压力等待仍计入。

`kernel_thread_create_joinable()` 的调用者持有 join handle；被等待线程退出不向普通 completion 队列发布业务完成。`kernel_thread_join()` 等在途调用展开到 EXITED 后，从可信调用栈摘取并释放目标栈/metadata；清理任务也可安全等待，不依赖它自己稍后执行 reap。通用 reap 同样清空并唤醒 handle。页缓存 stop 先禁止新压力提交，唤醒并 join worker，再释放专用快照页。

`test-scheduler-cases-riscv` 验证任务数、活动负载、空闲等待衰减及不可中断等待增长，末引用释放回到页基线；`test-io-sleep-riscv` 覆盖 worker 构造 OOM 与退出清理。

身份对象独立契约验证：`cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -Iinclude tests/pid-object-host.c kernel/pid.c -o /tmp/boaros-pid-object-test && /tmp/boaros-pid-object-test`；覆盖角色转移、最后成员与 pin 分离、编号复用后的新代次、容量与代次耗尽回滚。调度组合验证沿用 `make test-scheduler-cases-riscv test-scheduler-riscv` 与 `make test-userland-riscv`。

真实消费者的独立调用链诊断见 [session-consumers](../learning/session-consumers.md)：
固定 BusyBox setsid/chrt/taskset、musl daemon 和原 iperf3/cyclictest，
使用 QEMU 用户态 ecall 入口观察器，不修改原 ELF、不计分。

默认关闭的成本观测见[成本模块](kernel-cost.md)。开启后在实际调度事件结算运行、blocked/ready
时间及按rank的锁等待/持有、唤醒/重阻塞；标量48字节加内嵌guard16字节不持有新owner。
独立假时钟测试保护睡眠排除及前后台归属；13窗口三启动锁对照与四组合三启动低内存fixture
保护原来的阻塞、取消和资源回收。测量不改变队列/公平策略。

C4 另计 queue validation 次数、shape/thread检查、deadline到期处理数与最大单轮到期数、
实际timeout数量、到期到下一次运行的直方图。到期时复用已有 blocked_start 标量存期限，
调度后清除，不增加任务字节。timer计时在可能切换前收口，跨切换的暂停栈不成为诊断在途owner。
`make test-cost-riscv COST_CASE=deadline` 固定4期限任务加0/32/128/256无期限blocked，
pipe/proc状态双握手确认，覆盖同期限、提前信号、默认信号、取消和对象复用。
到期处理数必须与timeout数量一致且不随无期限blocked总数增长；队列shape检查只查头尾，
计数与索引弹出分开，不将它误报成全队列扫描。


## SMP前置的同步与等待契约（2026-10-09）

当前生产实现仍为单CPU。等待/就绪发布和对象内部资格已有raw互斥，业务状态尚未完成跨核保护。四种职责必须分开：

| 机制 | 保护与限制 | 当前状态 |
|---|---|---|
| 本地IRQ控制 | 保存/恢复本CPU中断；单CPU短元数据发布，允许显式切换 | arch IRQ与KERNEL_IRQ_SCOPE已有 |
| 抢占/迁移控制 | CPU本地指针借用和current稳定，不提供共享对象互斥 | cpu.h的嵌套深度；timer延后切换，既有安全点消费请求 |
| 跨核raw短锁 | 共享状态和内存序；不得持锁阻塞、I/O或调用会等待的分配路径 | raw_lock.h的32位acquire/release原子字、CPU guard链与rank/key序 |
| 可睡眠对象锁 | 任务guard、rank/key、资格交接和对象owner跨睡眠存活 | 内部raw30→调度raw40；业务数据仍由guard或原单CPU纪律负责 |

内核tp仍指向任务，架构前缀之后的CPU关联供当前CPU查询；current与need_resched由CPU记录持有。
启动任务与idle显式绑定，RV采用固件hart ID并在高地址重定位后重绑，LA读取CPUID CSR。
bootstrap I/O上下文与任务I/O上下文分开，任务guard和回收深度不迁入CPU。raw guard必须按LIFO由同CPU释放，
递归、逆序、错误owner和计数损坏fatal；block/yield/退出及可睡眠锁获取拒绝禁止抢占上下文。
`make test-sync-host`验证真实四线程争用、IRQ恢复、显式preempt在raw内的合法嵌套及fatal反例。
延期tick同时保存OTHER轮转原因，安全点消费；普通唤醒不自动变成同级轮转，切换后不把旧原因交给新任务。
RV scheduler cases分别验证idle和两个同级OTHER任务的延期请求，恢复后peer必须进展并归还全部owner。
`test-sync-riscv/test-sync-loongarch`在512M/1G运行真实原子/IRQ正例，以及raw内block/yield/退出/睡眠锁的fatal。
runner从Make接收实际构建目录，COST入口不能借用普通产物；host保护目录覆盖和缺失输入不回退。
这些结果不证明调度策略/任务索引、共享MM或设备业务已经具备SMP安全性。

`kernel_rwlock`的栈waiter由阻塞调用持有；授予资格先于wake，新任务不得抢走已授予
资格。持有guard期间锁对象不能移动/销毁；现有等待不可中断。未来取消/超时要明确
撤销注册与未消费资格的owner，不能先释放调用栈再摘队。raw锁只可保护短队列修改，
释放后再park；不能把当前整个IRQ-off acquire机械包进raw spinlock。

登记、通知、阻塞和切换完成由本模块的短调度域保护；条件及task/callback/context的
业务owner仍由调用者保持。完整SMP须把条件保护、MM页存活、对象最后释放与这套交接连接，
不能只移植等待原语就撤掉业务IRQ纪律。当前销毁入口与借用规则见本文开头。

已核对的IRQ-off显式切换与owner边界如下；这是调用盘点，不是跨核安全证明：

| 调用位置 | 切换/工作 | 睡眠期间的owner |
|---|---|---|
| kernel/sched/sync.c acquire | 对象raw内准备token，锁外park | 调用栈waiter、任务io_context及锁对象 |
| kernel/sched/park.c prepare/park | 仲裁登记与BLOCKED发布，锁外switch | task元数据/栈；queue资源由调用者保持 |
| kernel/physical_page.c allocate_order | EMPTY后回收/pressure_wait | 不持buddy半成品；回调固定cache group，深度在任务context |
| fs/page_cache.c lookup/get/readahead/writeback/pressure | loading、后台work、代次进展等待 | entry users、cache/runtime或group pin，停止先join |
| drivers/virtio/block.c | 等descriptor/span/request完成与reset | request token及业务buffer；DMA停止未确认不释放 |
| net/ethernet.c worker；net/socket.c协议入口 | IRQ关闭下NIC/串行lwIP批次，末尾重查后yield/block | root借设备，worker持网络；TX/RX loan直到归还/停DMA |
| mm/mm.c文件fault | 源I/O可显式睡眠，恢复后重查 | 来源操作pin、VMA代次和候选页，失败准确回收 |

等待交接须覆盖wake发生于检查前、登记后、释放保护后/park前；timeout和cancel
与grant竞争；destroy与callback遍历；拒绝raw锁下阻塞；每种交错不能漏wake、双授予、
重复运行或使用已销毁队列。原单CPU测试只作为退化配置，不能替代真实多CPU内存序。
固定依据为本地references/linux/Documentation/locking/locktypes.rst，commit
f4cdf7ca9a1fdcca413157df19753f388a5a224e；lwIP raw串行边界见references/lwip/doc/doxygen/main_page.h（pitfalls/multithreading），
commit77dcd25a72509eb83f72b033d219b1d40cd8eb95。实现依赖只在[路线图](../goals.md#p6b-锁等待与中断规则)维护。
