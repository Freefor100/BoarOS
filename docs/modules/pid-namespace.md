# PID namespace：身份和生命周期接线

## 当前边界

**还不支持 `CLONE_NEWPID`。** 多层编号已合入现有 `kernel/pid.c`，scheduler 的初始 `scheduler.identities` 使用有引用计数的初始 namespace registry。TID/TGID/PGID/SID role、pidfd 和所有 namespace 索引现在共享同一个 `struct kernel_pid` 生命周期，不再保留独立的 namespace identity 实现。初始层对象仍为 64 字节；只有嵌套层才在对象尾部追加祖先 binding。scheduler 已按 active/for-children 选择身份目标，记录每层 init，并限制 reparent 的 namespace 边界；这些内部路径由生产函数 fixture 验证。动态 registry 已使用堆 owner 和锁外回收，proc 的 pid/pid_for_children FD 与 setns 的 for-children 安装已接入。实际用户任务目前仍只在初始层创建：尚无用户创建 PID namespace 的入口及全部用户 PID 视图。init 终止/收割已接入内部 cleanup 路径，但尚未有嵌套 guest 的端到端验收。

这不是把数字 1 重命名后就能开放的功能：创建/wait、信号/ptrace 和 proc 挂载视图已有显式转换，资源、futex、IPC/socket 与 PGID/SID/TTY 的分阶段接线见下文，其余边界仍需全量复核和逐项验收；`child_reaper_locked` 已限制在本层 subreaper/init，内部 init 清理现在会终止并收割后代，但跨视图用户接口仍缺验收；系统 init 退出还控制整机关机。仅放开 clone 标志会同时泄露父命名空间、错误收养孤儿和错误结束系统。

## 已实现的编号基础

- 一个身份在固定头内保留 active namespace 的编号和哈希链；祖先 binding 按 root-to-parent 顺序放在尾部。每项是 namespace、编号及该 namespace 哈希链成员。输入精确编号数组采用 clone3 的 leaf-to-root 顺序；未指定的祖先层自动分配。身份的 generation 在整棵树的根统一递增，旧身份不能因复用数字而变成新身份。
- 每个 namespace 有自己的 bitmap 和 256 个哈希桶，允许兄弟 namespace 同号。查找直接在指定 namespace 索引进行，不扫描全部任务。已知身份向 namespace 转换是一次 depth 索引和指针比较，兄弟、后代以及另一棵树的视图返回 0，不回退到全局号。
- 调用者在锁外准备身份、binding 和 namespace/bitmap 存储。预留在现有 identity 锁内逐层占号；中途冲突、越界或空间不足归还此前所有编号，尚未发布的预留不出现在查找中。成功后一次锁范围内全部发布。失败可能改变 allocator 游标，但不泄漏编号，也不承诺恢复自动分配顺序。
- `disable` 只停止创建，不隐藏现有身份。reserve、publish、创建下级 namespace 都检查祖先是否仍接受创建。这样准备子任务期间 namespace init 退出的交错可以在最终发布时拒绝，而不是留下半发布的祖先身份。
- `detach` 同时摘掉全部索引并归还编号；引用只保留身份存储和历史映射。旧 pidfd 类持有者仍持有旧对象，不能按历史整数重新查找目标。最终 identity put 释放 active namespace 引用；子 namespace 持父引用，因此最后一个外部引用关闭前整条祖先链不会消失。身份最终释放统一排到根 registry 的退休链；子 namespace 归零也排到根的独立退休链，调用者取出后在锁外释放。根 owner 最终归零后，必须先排空两条退休链才可释放根存储。
- 所有操作，包括查找和引用，必须由调用者用同一个 identity 锁串行化。基础层不分配内存、不访问用户内存、不调用销毁回调；引用变成 0 后由调用者在锁外释放预先准备的存储。namespace 的父链、depth 和身份 binding 在存活期间不迁移。

空间为每 namespace 一个 bitmap 和索引、每 identity 一份固定头及每可见层一项 binding。创建/发布/退出的工作量与深度成正比；转换不遍历祖先链。初始层没有新增每任务对象或祖先数组，只在已有身份锁内维护初始 registry 引用；不能将这个复杂度说明当作实际吞吐收益。

## 接入顺序和必要不变量

### 1. 身份已统一，调用者视图仍需接入

现有 TID/TGID/PGID/SID role 链、pidfd observer、proc generation 目标使用同一份多层身份。`kernel_pid_find`、reserve/publish/detach/retire 已在共同实现中处理全部祖先索引；没有第二套身份对象或目录。scheduler 初始化使用 managed 初始 registry，现有任务创建/回收实际经过这些路径。已新增显式用户 TID/TGID 查询并在 getpid/gettid 入口使用；原 kernel_task_tid/tgid 仍为初始层编号，供内部 completion、异步操作和日志使用。getppid 按调用者 active 视图转换，父身份不可见时为 0。当前执行者的 TID/TGID 查询不增加锁或分配；远端查询继续在 identity 域内读取。

`task->tid` 如果保留，只能作为内部初始层缓存，不可再直接返回用户。调度器按身份指针和 role 链工作，热路径不增加 namespace 搜索。持久引用保活身份，不能把借来的 task 指针放进无锁异步队列。现有非 leader exec 的 role transfer 必须同时保留所有层编号及 namespace init 所指线程组。

### 2. 已接入的 active/for-children 与剩余 syscall 工作

active 直接由 TID 身份取得，非 leader exec 的旧容器必要时从转移后的 TGID 取得，不另存一份可漂移的任务 active 指针。`pid_for_children` 为 NULL 时表示 active；只有覆盖时才持有额外 registry 引用，资源退出时归还。内部安装函数仅允许自身或后代，不改变调用者 active。创建先固定目标引用、按实际深度在锁外准备身份存储，再预留全部编号；普通线程必须与 parent active 相同。内部 `task->tid` 仍是初始层编号，不能直接用作未来 namespace 用户输出。

setns 已使用 namespace FD 或 pidfd 的 PID 快照，与 MNT/UTS/IPC 一起准备、验证后原子安装；unshare 的 PID 创建标志仍拒绝。任务出生后的 active PID namespace 不改变。`unshare(CLONE_NEWPID)` 和 `setns` 只更新后续子进程的目标；不能修改调用者 getpid/gettid。setns 仅允许 active 的自身或后代视图，不允许进入祖先或兄弟；校验失败时 namespace 集合不部分安装。`CLONE_THREAD` 必须留在同一 active namespace，不能把共享线程组拆到不同视图。

当前没有 user namespace；第一阶段 owner 固定为初始 user namespace，并沿现有凭据检查 CAP_SYS_ADMIN。以后加入 user namespace 时，创建/setns/指定 TID 的权限必须按被修改的每一层 owner 计算，不能用“有效 uid 为 0”替代。clone3 在调用核一次导入数组，再检查大小、每层范围、权限和 init 是否存在，不在发布时重读用户指针。

scheduler 的 `process_identity_create_numbers` 已直接消费内核持有的 leaf-to-root 数组，按实际目标深度一次预留；scalar 创建只调用同一函数，不另维护一套规则。生产 fixture 检查 `[1, 23]` 得到本层 init=1、初始层 TID=23，以及祖先冲突后本层编号回滚。**clone3 syscall 仍只支持当前初始层的一个元素**。后续导入多层数组时，建议仅在多元素请求时准备 clone 专用拥有者，沿两种架构 action 消费/失败路径统一释放；不要把 parser 局部栈数组指针带出函数，也不要为所有 syscall 或所有任务常驻 32 个 TID。普通 clone 和单元素请求可保留无额外分配的路径。

新 namespace 的第一个进程必须为本层 1；尚无 init 时请求非 1 不能成功。基础层只负责占号，不推断哪个任务是 init，此约束必须与 child_reaper 的安装放在同一个发布事务里。`CLONE_PARENT`、线程/信号表共享冲突、深度上限和 clone3 请求数组长度应对照固定 Linux fork 路径检查。

### 3. 每 namespace 的 reaper 和终止

已在 registry 中保存 init 身份的弱引用，真实 TGID role 负责保活；role transfer 直接更新该身份的任务成员，最后 TGID 解绑时清除 reaper 并停止创建。新层 init 与外层进程进入已有子 namespace 后创建的普通子进程，都继承真实父进程的 PGID/SID 身份，不隐式执行 setsid。父组/会话在子视图不可见时，编号转换为 0；只有显式 setsid 才建立自身组/会话。孤儿优先给同线程组存活成员，再找**同 namespace 内**最近的 subreaper，最终给本层 init；生产 reparent fixture 验证外层 subreaper 不会接走本层孤儿。父进程不可见时 getppid 返回 0；等待资格仍按真实亲子/ptrace 关系判断，返回数字按等待者视图转换。

init 的 exit_group 或最后可存活线程开始退出时，已在 identity 锁内 disable；leader 单独退出而其他成员存活不会提前 disable。clone 在最终 identity 锁内、ptrace/cgroup/pidfd/亲子链提交之前再次 admission，死亡 namespace 返回 ENOMEM 并走原资源回滚；同一锁内的后续身份 commit 不再有 namespace 关闭竞态。 此保证只涉及身份、FD 安装和关系提交：pidfd 的用户地址在此之前已经可能写入预留的 FD 整数，失败不恢复该用户内存。固定 Linux `kernel/fork.c` 同样先 put_user(pidfd)，再检查 PIDNS_ADDING；失败释放 pidfile 与 FD 预留，不把已写整数回滚。调用者必须先检查 clone 返回值，不能把错误返回后输出槽里的整数当成已安装 FD。预留时也区分 namespace 已死亡与 PID 耗尽，分别映射 ENOMEM 与 EAGAIN。

嵌套 init 组的最后成员结束后，由既有 cleanup worker 完成资源清理，再开始 namespace 收割。只遍历该 namespace 的 published 身份索引，对其余真实 TGID 请求 SIGKILL；兄弟 namespace 和祖先任务不在这个索引内。init 组 leader 提前退出时仍保留 GROUP_DEAD metadata，不能提前终结整个 namespace。终止扫描也不能把目标组 leader 的 EXITED 当成整组死亡：cleanup 尚未将其转为 GROUP_DEAD 时，其他成员可能仍执行；只要组内还有成员，就必须请求整组退出。扫描从当前 TGID role 取任务，不保留角色转移前的 leader 指针。正在退出的本层 init 继续承担孤儿收养，不能因为 group_exiting 或 EXITED 就把孤儿丢给 NULL 或外层 init。

收割阶段将 init 的 SIGCHLD 处理设为忽略，并用真实 wait claim 收割已有 zombie；后续退出的 init 子进程直接回收，包括非 SIGCHLD exit_signal 的 clone 子。此例外只属于正在收割的 namespace init，其他父进程仍按原信号/等待语义处理。外层 parent 通过 setns+fork 创建的 zombie 不会被强制夺走，必须由真实 parent 等待或正常 reparent 后收割。

不能让唯一 cleanup worker 等待由它自己清理的子进程，也不在仍持 MM/FD/robust 资源的用户退出线程里等待。若 namespace 的已分配身份尚未只剩 init，清理后的 init 从 exited 工作队列移入 pending namespace 链，保留任务 metadata 与身份，不保留用户栈/资源。锁外 identity collector 在既有锁范围内检查 pending 链；预留回滚和最后 role 摘除使编号只剩 init 时，重新排入 exited 队列并唤醒 cleanup，随后才能向外层 parent/ptracer 发布最终退出。列表检查只发生在已有 collection 路径，不增加普通任务的队列或逐层通知。

保留的 ns FD/pidfd 不参与编号等待，只保活死亡对象。尚未发布的 clone 预留则参与等待，失败回滚也触发 collection，避免漏唤醒。当前 host 已验证两个真实线程的预留→shutdown/defer→admission 拒绝→回滚重排交错；完整多层 init/外部 tracer/外层 parent 的 guest 收割仍是开放前验收项，不能据此宣称完整 namespace 已可用。

init 的信号保护不能复用“系统 PID 1”的整数判断。对本层 init 只允许其安装处理器的普通信号；祖先 namespace 的 SIGKILL/SIGSTOP 例外要按发送者/目标关系判断。系统级 reboot 与容器内 reboot 的语义也要分开，在实现前明确拒绝不能执行的操作。

### 4. 用户边界分阶段接线，全部完成前不开放

clone 的内部 completion 与尚未迁移的 ptrace bookkeeping 保留 root TID；返回值与 PARENT_SETTID 使用发起父线程的 active 视图，首次子返回的 CHILD_SETTID 和 set_tid_address 使用子自身的 active 编号。两者不能共用 task->tid。wait 的 PID/PGID 输入按等待者查身份并保留 canonical 引用，输出也按等待者转换；真实亲子关系和 PIDFD 身份不变，不按转换后的整数重新查任务。proc 固定挂载视图与 ptrace 的独立视图规则见后文，其他资源接口不能据此认为已完成。

跨进程资源查询使用显式调用者 active PID 视图：`process_vm_*`、`kcmp`、sched/affinity、`prlimit64` 和 `capget` 不再把用户整数当 root TID。内部 `kernel_task_find_by_tid` / scheduler root lookup 保持原语义；新的用户 lookup 在身份锁内取得 task storage borrow。MM 和 kcmp 在资源锁后仍按同一视图重查原 task，权限检查与资源取得同锁完成，不通过重复查找切换到复用编号的新任务。sched/affinity 的查找及操作在原状态锁内；prlimit/capget 保留 task 引用而非历史整数，原 usercopy、凭据和错误顺序不变。`capset` 的非零自身 PID 也按自身可见 TID 比较。

host 的真实三层身份/lifetime/scheduler、proc MM 取得及 kcmp 兄弟 registry 检查同号隔离；kcmp 在 FD 锁前受控摘除 role，后续重查返回 ESRCH，已有凭据改变/退出并发测试仍执行。capability/prlimit 解码 fixture 只模拟 lookup 和 rlimit 执行，检查选中 task、错误前缀和 borrow 归还；不替代真实 nested syscall。RV/LA 的 process_memory、sched_control、pid_namespace 覆盖初始视图回归及显式 PID 的 capget/capset/prlimit。futex PI、socket/IPC 凭据、PGID/SID/TTY、cgroup、priority/clock 等未完成边界仍阻止开放 CLONE_NEWPID。


| 边界 | 当前主要位置 | 必须采用的视图 |
| --- | --- | --- |
| getpid/gettid/getppid、clone 返回/PARENT_SETTID/CHILD_SETTID | `kernel/sched/process.c` | 调用者、父、子分别取各自视图；不能共用一个导出整数 |
| kill/tkill/tgkill、组信号、siginfo、SIGCHLD | `kernel/sched/signal.c` | 目标参数按发送者查找；内核生成 sender 按接收者转换，不可见为 0 |
| wait4/waitid、ptrace event/GETEVENTMSG/TracerPid | `kernel/sched/process.c`、`ptrace.c` | 输入按调用者；输出按等待者/tracer；身份引用而非数字复查 |
| pidfd_open、pidfd signal/getfd/wait、fdinfo | `kernel/sched/pidfd.c`、procfs | open 参数按调用者；FD 后续继续指向同一身份；fdinfo 按 proc 挂载视图 |
| setsid/setpgid/getpgid/getsid、tty 前台组 | `process.c`、`tty.c` | 组和会话使用同样多层身份；不能用初始层整数测试组等价 |
| affinity/scheduler/nice/ioprio、CPU clock、kcmp/process_vm/prlimit/robust-list | sched 与 syscall 对应入口 | 每个输入 PID 都按调用者解析，跨睡眠持有身份/目标 owner |
| futex PI owner/robust、POSIX timer sigev_notify_thread_id | futex、signal/timer | 用户 TID 与内部 task 身份必须明确区分 |
| cgroup.procs/threads、fcntl owner/SIGIO、SCM_CREDENTIALS/SO_PEERCRED | cgroupfs、open-file/socket/signal | 按接口规定的读取者/接收者视图转换，权限凭据仍按现有规则 |
| SysV IPC shm_cpid/lpid、sem GETPID、msg_lspid/lrpid | shm/sem/msg | 持身份或保留规范要求的历史号；按读取者视图处理，不假定 IPC namespace 与 PID namespace 同步 |
| core notes、BPF 暴露的身份、netlink portid | coredump/BPF/socket | 分别审查 ABI；portid 不等同 PID，禁止全局机械替换 |

异步来源尤其要查清“何时、在哪个视图生成整数”。已经排入队列的 `si_pid` 不能在接收时误当成当前全局 PID 查找；用户提供的 siginfo 也不能不分来源地重写。

### Perf、seccomp 与 netlink 的身份边界

perf_event_open 的非负 PID 按调用者 active 视图解析 TID，0 选择调用者；查到的 task storage borrow 仍由 event 持有至销毁。后续 trace 计数比较真实 task，不拿历史 PID 重查，所以号码复用不会把旧 event 绑定到新任务。原 capability、CPU、属性和 FD 错误检查顺序不变，内部 trace common.pid 与 BPF current_pid_tgid 仍用 root 身份。

seccomp TSYNC 在原身份锁保护的失败分支，将首个不兼容线程的 TID 转为调用者视图；无法解析时为 ESRCH。成功安装/广播、过滤器引用和 exec 等待事务不变，不给正常 syscall 过滤增加转换。netlink 自动绑定只用调用者本地 TGID 作为首选 portid，碰撞仍在 socket registry 锁内选择其他地址；一旦绑定，portid 是 socket 地址，继承 FD、换观察者或进程退出都不触发 PID 翻译或重绑定。显式 portid 不作 PID 查找。

固定 Linux 来源是 [events/core.c](https://raw.githubusercontent.com/torvalds/linux/f4cdf7ca9a1fdcca413157df19753f388a5a224e/kernel/events/core.c) 的 find_lively_task_by_vpid、[seccomp.c](https://raw.githubusercontent.com/torvalds/linux/f4cdf7ca9a1fdcca413157df19753f388a5a224e/kernel/seccomp.c) 的 seccomp_can_sync_threads，以及 [af_netlink.c](https://raw.githubusercontent.com/torvalds/linux/f4cdf7ca9a1fdcca413157df19753f388a5a224e/net/netlink/af_netlink.c) 的 netlink_autobind。

Host 检查 perf lookup 边界本地/内部号不同、重新映射后旧 event 仍持原 task、并发 close 与正在执行的程序；其 task 查找是受控模型。TSYNC 用生产转换/过滤器并保留真实 pthread 安装与 exec 交错。socket fixture 用真实 sibling registry/生产 TGID helper/socket binding 验证两个本地 PID1 的地址冲突和继承地址不变。native `unshare -Urpf ... --seccomp-only` 三轮实际返回本地失败 TID；当前环境 RAW/DGRAM netlink socket 均 EPERM，完整 `--nested` 不计通过，独立 net namespace 重试亦未消除限制。RV4/LA2 在初始 PID namespace 跑实际 netlink 与 TSYNC 程序；完整 nested guest 仍待开放前验收，CLONE_NEWPID 保持关闭。

### Namespace reboot 隔离

reboot 的权限、magic 和动作校验仍在 syscall 边界执行。有效请求先区分 active PID namespace：初始层沿原全局 machine reboot 路径；nested 层只记录本层最终退出信号、借用其 init task 并提交可信 SIGKILL 等价的 group-exit，不触及全局 reboot 状态。restart/restart2 对外层父报告 SIGHUP，halt/poweroff 报告 SIGINT；发起者按 Linux do_exit(0) 选择单线程退出，不改成任意调用者的 exit_group。RESTART2 的 machine 字符串只在初始层导入，nested 请求不因坏字符串指针返回 EFAULT。

namespace init 的早期退出仍为 SIGKILL。其后代、外层 parent 持有的 zombie 和 ptrace FINAL owner 全部清退后，才在身份锁内更新 init completion 与 wait_status 为 HUP/INT，保证 SIGCHLD、wait 和后续 completion 使用同一终态。记录属于既有 namespace registry，不为普通任务或正常退出增加分配。固定来源：[reboot.c](https://raw.githubusercontent.com/torvalds/linux/f4cdf7ca9a1fdcca413157df19753f388a5a224e/kernel/reboot.c)、[pid_namespace.c](https://raw.githubusercontent.com/torvalds/linux/f4cdf7ca9a1fdcca413157df19753f388a5a224e/kernel/pid_namespace.c)。user namespace 尚未实现，能力检查沿现有单 user namespace 模型，未宣称支持跨 user namespace 权限所有者。

验证分层：syscall host 检查权限/magic/动作顺序、nested RESTART2 不读取坏指针、不调用全局请求以及 EXIT/EXIT_GROUP 区别；scheduler fixture 实际执行 nested reboot、group termination、并发预留取消、reaper drain、ptrace/外层 zombie 延迟归还，检查最终 wait_status、兄弟不变及 owner 归零，但任务执行/部分资源退出仍由模型驱动。native `unshare -Urmfp` 加新 proc 挂载，三次各覆盖 init/普通成员 × 四种命令，真实外层 wait 信号、死 namespace FD setns 后 fork ENOMEM 与兄弟继续运行。RV/LA 默认 workload 只验证初始层无效输入与能力顺序，原 system_control 补充回归；CLONE_NEWPID 仍关闭，不能将其称为 nested guest 验收。

### CPU clock 与 timer recipient

负编码 CPU clock 的 PID 在调用者 active PID namespace 查找，内部采样仍持有 canonical 身份。编码 0 保留当前线程/线程组语义，线程时钟限制为真实同线程组。固定 Linux `pid_for_clock` 的特殊规则仅允许 clock_gettime 用调用者自己的非 leader TID 代表其进程时钟；getres 和 timer_create 的严格路径仍要求 TGID。实现比较解析后的真实 TID identity，不比较用户数字和 root tid。采样在原锁外进行，回锁重新检查 role；非 leader exec 式 role transfer 后重新采样新 owner，不重查历史整数。

SIGEV_THREAD_ID 在 caller view 解析并要求真实同组。timer 保留 recipient PID 引用，准备队列后仍检查原 TID role，角色已摘除则 EINVAL 并归还候选/配额；到期递送不按用户填写的旧数字重新查找。来源是固定 Linux [posix-cpu-timers.c](https://raw.githubusercontent.com/torvalds/linux/f4cdf7ca9a1fdcca413157df19753f388a5a224e/kernel/time/posix-cpu-timers.c) 和 [posix-timers.c](https://raw.githubusercontent.com/torvalds/linux/f4cdf7ca9a1fdcca413157df19753f388a5a224e/kernel/time/posix-timers.c)。未改变 timer 的既有分配/发布顺序，也未宣称所有错误前都无用户可见副作用。

Host 使用真实多层 PID/role 检查兄弟同号、外层不可见、非 leader gettime 例外和严格拒绝、异组线程拒绝，以及受控采样期间 TGID role transfer 重试和角色消失 ESRCH。POSIX fixture 覆盖 local recipient 与 root number 不同、队列准备期间摘除 TID 的失败归还、实际 thread queue 到期消费；其中任务/lookup/会计采样边界仍是模型。native `unshare -Urpf ... --nested` 三次真实双 PID1 子进程各建 pthread，检查 strict CPU clock 差异和 SIGEV_THREAD_ID 实际接收；RV/LA 同程序默认模式与原 POSIX 全套通过。原 CPU workload 10 项虽通过并成功归还 owner 后 poweroff，现有 runner 要求正常 root exit 而标 FAIL，不能计完整套件通过。完整 nested guest 和真实 exec/clock 并发仍未验收，CLONE_NEWPID 继续关闭。

### Nice 与 I/O priority

命名 PID/PGID 在调用者 active PID namespace 中查找；PRIO_USER / IOPRIO_WHO_USER 只遍历该视图中 TID 可见的相同 UID 任务。PGID=0 则保留实际继承的 canonical 组，不因组号或其他成员对调用者不可见就过滤它们。两种集合语义不能混同：内层设置同 UID 不影响外层/兄弟，但设置实际共享组可影响这些真实成员。固定 Linux 的 [sys.c](https://raw.githubusercontent.com/torvalds/linux/f4cdf7ca9a1fdcca413157df19753f388a5a224e/kernel/sys.c) 和 [ioprio.c](https://raw.githubusercontent.com/torvalds/linux/f4cdf7ca9a1fdcca413157df19753f388a5a224e/block/ioprio.c) 分别规定了这些扫描与查找。

I/O priority 在原身份锁内取得目标 storage borrow，锁外分配仍针对该真实任务，不按旧数字再次查找；退出目标不新建 io_context 的既有行为不变。优先级权限、nice 的逐目标部分成功、ioprio 的遇错停止、未设置 raw priority 与组/用户 effective priority 的区别也不改。UID 映射尚未实现；ioprio set/get 的 who=0 UID 差异保留固定 Linux 既有语义。

验证包括真实多层身份的 nice USER/实际 PGID 集合、ioprio 同号/不可见输入、UID 隔离/权限/owner 归还，以及 pthread 在实际 lookup 解锁后执行退出与最后 io_context 释放。后者的 lookup 边界是受控模型，不是完整 syscall 并发。native `unshare -Urpf ... --nested` 三次实际创建两个 PID1 兄弟，验证 UID 修改隔离及 PGID=0 修改真实外层组；RV/LA 同一程序默认模式与原 ioprio、sched_control 验证初始层真实进程/组与权限。CLONE_NEWPID 未开放，完整 nested guest 尚未验收。

### cgroup2 成员 PID 与视图边界

cgroup2 的 `cgroup.procs` / `cgroup.threads` **在生成读取内容时**使用当前读取者 active PID namespace，不是 cgroup mount 或 OFD 打开者的 PID 视图。与固定 Linux `cgroup_procs_show` 一致，不可见的真实成员输出 `0` 行，不删除成员或过滤掉该行。原 OFD 的已生成缓冲可继续读取；seek 到 0 后重新生成时采用当时读取者视图。这与 `/proc/sysvipc` 的固定 opener PID view 不同。进程列表仍每组一行，leader 已退出但还有活线程时保留该 TGID。

写入成员文件时，非零 PID 在写入者当前视图内查找，0 表示当前任务；身份查找、权限与迁移在原身份锁事务内完成，不在解锁后重查历史数字。权限仍使用既有 OFD 打开者凭据，PID 解析不因此变成打开者视图。迁移按真实线程组进行，兄弟 namespace 同号不能误命中；原 domain-only 层级不能拆开线程组的限制不变。`/proc/<pid>/cgroup` 已由 proc mount 解析成 canonical inode 身份，内部 root 编号+generation 查询不改成 current-relative。这里没有新增 cgroup namespace 虚拟化，也未开放 CLONE_NEWPID。

固定来源为 [Linux cgroup.c](https://raw.githubusercontent.com/torvalds/linux/f4cdf7ca9a1fdcca413157df19753f388a5a224e/kernel/cgroup/cgroup.c)；不要把 cgroup-v1 的 PID 列表缓存或 cgroup namespace 层级根语义套给这些 cgroup2 PID 列。host 使用生产 membership/index/锁内迁移和实际 PID registry，permission/heap 边界建模，并在分配期间受控迁移或 unlink，检查第二次锁内快照及失败 owner 归还。Linux `unshare -UrmCpf` 可实际挂载 cgroup2 并创建两个 PID1 兄弟 namespace，继承已读 OFD 后 rewind 和重新 open 均验证当前视图与 0 行；该环境无可写迁移权限，不能把 native 列表通过称为迁移通过。RV/LA 使用初始 PID namespace 验证真实文件读写/迁移，完整 nested guest 仍待验收。

### PGID/SID、TTY 与异步 owner

getpgid/getsid/setpgid 的输入及返回使用调用者 active PID 视图，setsid 返回本层 TGID；所有权限、session 等价及组成员关系仍比较 canonical 身份。进入 PID namespace 的子进程继承真实父 PGID/SID，即使内层数字不可见为 0；它不是自动新 session。setpgid 可在同一真实 session 中建立内层组，不因继承 session 不可见而拒绝；不能通过兄弟同号访问另一组。孤儿进程组判定只排除 global init 父进程，不排除 nested init；后者可连接同一 session 的不同组，否则会把正常后台读误判成孤儿组 EIO 而非 SIGTTIN 停止。

TTY 保存真实 session/foreground 引用。TIOCGPGRP/TIOCGSID 按当前调用者输出，不可见为 0；TIOCSPGRP 在调用者索引中查身份后检查真实 session，保留 Linux 的无 PGID 成员时 TID fallback。前台组输出不要求当前有成员，因此其最后进程退出后，TTY 持有的身份仍可返回原号。TTY job control、hangup、组信号继续按真实身份，不新增 namespace 数字过滤；proc TTY 字段继续使用 proc mount 视图。这里沿用 CPU0 service/IRQ 保护 TTY 引用的契约。

F_SETOWN/EX 在当前视图查找并保留身份，F_GETOWN/EX 按当前读取者转换，既不固定设置者视图，也不把保存的历史数字重新查找。OFD 别名共享 owner。GETOWN 根据 owner 类型检查活 role，没有对应 role 或读取者不可见时返回 0。**这不是 SIGIO 是否递送的判据**：固定 Linux `send_sigio` 对 F_OWNER_PID 也先用 TID role 找任务，再向其线程组发送，所以指定 nonleader TID 时，GETOWN 可为 0 而实际仍递送。生产发送和凭据检查保留这一行为；按真实 PGID 成员发送也可跨 namespace，不能与兄弟同号不同身份混淆。SIGIO 的 fd/band payload 不按 PID 转换。

对照固定 Linux [kernel/exit.c](https://raw.githubusercontent.com/torvalds/linux/f4cdf7ca9a1fdcca413157df19753f388a5a224e/kernel/exit.c)、[kernel/sys.c](https://raw.githubusercontent.com/torvalds/linux/f4cdf7ca9a1fdcca413157df19753f388a5a224e/kernel/sys.c)、[tty_jobctrl.c](https://raw.githubusercontent.com/torvalds/linux/f4cdf7ca9a1fdcca413157df19753f388a5a224e/drivers/tty/tty_jobctrl.c)、[fs/fcntl.c](https://raw.githubusercontent.com/torvalds/linux/f4cdf7ca9a1fdcca413157df19753f388a5a224e/fs/fcntl.c)。host 使用真实 PID registry/组操作，TTY ioctl 映射边界及 async lookup/信号sink部分建模；signal fixture 使用生产发送路径检查 nonleader owner 与跨 namespace 实际组成员。Linux 实际 nested workload 验证外层 session 继承、兄弟本层 PID1、TTY 前台组、nested init 子进程的后台读 SIGTTIN stop→SIGCONT 恢复、OFD owner 与退出，并以真实 pthread 检查 GETOWN=0 的 process SIGIO 和 F_OWNER_TID 递送。pthread_join 仅同步用户线程/clear_child_tid，不当作 kernel task reap；退出 owner 检查使用真正 waitpid。RV/LA 相同程序仍为初始 PID 视图，不替代完整 nested guest。

### SysV IPC 的 PID 身份与读取者视图

sem 的 GETPID、msg 的 msg_lspid/msg_lrpid、shm 的 shm_cpid/shm_lpid 按 syscall **当前调用者** active PID namespace 转换；祖先或兄弟身份不可见时为 0。`/proc/sysvipc` 则在 **OFD 打开时**保留打开者的 IPC namespace 和 active PID namespace，后续读取使用这两个视图，既不取 proc mount 的 PID 视图，也不随当前读取者改变。dup/fork 共享原 OFD 视图；内层在继承的外层 proc mount 上重新 open，捕获内层视图。固定 Linux 的 [sem.c](https://raw.githubusercontent.com/torvalds/linux/f4cdf7ca9a1fdcca413157df19753f388a5a224e/ipc/sem.c)、[msg.c](https://raw.githubusercontent.com/torvalds/linux/f4cdf7ca9a1fdcca413157df19753f388a5a224e/ipc/msg.c)、[shm.c](https://raw.githubusercontent.com/torvalds/linux/f4cdf7ca9a1fdcca413157df19753f388a5a224e/ipc/shm.c) 分别使用 pid_vnr 或 proc 的 pid_nr_ns；[ipc/util.c](https://raw.githubusercontent.com/torvalds/linux/f4cdf7ca9a1fdcca413157df19753f388a5a224e/ipc/util.c) 的 sysvipc_proc_open/release 明确管理这两个 owner。OFD 复用仅 pathless socket/namespace 使用的 inode union 槽，不增大所有 OFD、不新增分配；仅 sysvipc open 增加一个 PID view 引用，最终 close 释放，snapshot/close 失败保留 owner 等待重试。

sem/msg 沿用已有身份 owner，msg 的同一发送/接收者快路径比较规范身份指针，不再比较 leaf PID 与 root TID。直接交付给已睡眠接收者仍记录该线程 TID，普通 dequeue 记录 TGID。SHM 创建者和最后操作者各持一个 TGID 引用；创建、VMA open/close 更新，段销毁归还。相同身份的重复更新不取全局 PID 锁、不做引用增减，不新增凭据对象分配；身份变化在 PID 锁内替换引用，堆回收在锁外。退出和数字精确复用均不会将旧 IPC 元数据重新绑定到新任务。

SHM 的普通 shmat/shmdt、VMA 拆分/合并及 fork 的 VMA open 使用执行操作的任务，fork 因而记录父进程。共享 MM 释放非最后一个引用不触发 close。异步退出清理把退出者沿 MM release、VMA destroy 显式传给 SHM close；exec 事务在私有或共享 FD 表两种情况下都保存清理主体。失败的 exec 尚未移交旧 MM，不更新旧 attachment；失败 clone 的 MM 回滚沿当前父任务路径，不能假装未发布 child 执行了 close。传入主体在调用期间由任务/exec事务现有所有权保活，不存入共享 MM，也不增加每个 VMA 的身份引用。

验证使用 Linux 实际 nested PID namespace 和共享 IPC、兄弟同号、继承/dup 的外层 proc FD 与重新 open 的不同视图、退出后元数据，另测共享 MM、VMA 拆分、失败/成功 exec。RV4/LA2 跑相同初始 PID namespace 用户程序，实际 exit 及 exec 回收后 lpid 正确；旧父版本可复现退出后 lpid=0 而非 child PID。sem/msg host 复用受控引用模型并调用真实映射函数；SHM host 使用真实 PID registry、保留/替换/释放、精确复用及双退休链，MM映射边界仍是模型。真实 open_file fixture 另测 alias、task 退出后的 view 保活、snapshot 失败、close 重试和双退休链归零。它们不替代完整 nested guest，CLONE_NEWPID 保持关闭。

### PI / robust futex 的用户编号

PI 用户 word 不携带 namespace 标签。对照固定 Linux [pi.c](https://raw.githubusercontent.com/torvalds/linux/f4cdf7ca9a1fdcca413157df19753f388a5a224e/kernel/futex/pi.c)，owner 查找和现有 PI state 的 owner 校验都使用执行 syscall 的 active 视图；CMP_REQUEUE_PI 的受益线程编号按 requeuer 视图，UNLOCK_PI 写下一 owner 时按 unlocker 视图。不能统一改成每个 owner 自身的 local TID，也不能拿最后写入的缓存数字代替当前观察者转换。已有 PI state 的非空 owner 若对执行者不可见，其转换编号为 0；因此 word 的 TID=0 即使没有 OWNER_DIED 也可能与真实 owner 匹配，不能无条件拒绝零号。没有 owner 的 detached state 仍不接纳。PI state 仍保留真实 task owner/等待边和 storage borrow；共享 futex key 不增加 namespace 字段。原有 owner 退出、bucket 发布互斥、donation/revoke 与锁外归还不变。

共享 word 跨不同 PID 视图并不透明：同号可能被解释为自身而返回 EDEADLK；外层 proxy requeue 可写入内层 waiter 的外层编号，使该 waiter 后续按自身编号 UNLOCK 返回 EPERM。固定 Linux [core.c](https://raw.githubusercontent.com/torvalds/linux/f4cdf7ca9a1fdcca413157df19753f388a5a224e/kernel/futex/core.c) 也明确说明 robust 同号跨 namespace 无法仅凭 word 区分。`pid_namespace_futex.c` 在真实 nested namespace 验证同号、proxy requeue 和不可见 owner 零号规则，不能据此宣称跨视图共享 robust mutex 完整可用。

exit robust 清理使用退出线程自身 local TID；exec 延续 BoarOS 原有策略，在身份采用前保存旧 TID，再用旧 local TID 清旧 MM 的 robust word。这里存在已测量的兼容差异：宿主 Linux 6.18.57 的 nonleader exec 在采用 leader 身份后留下旧 owner word，BoarOS 将其标记 OWNER_DIED；父提交 240b6956 用相同程序也复现后者，非本次新增。`robust_exec.c` 默认验证 BoarOS 清理，显式 `--expect-retained` 则只验证并报告 Linux 留旧值，不能把两者计为相同结果。

验证入口：`sh tests/host/pid-namespace-futex.sh` 使用真实分层 PID registry 和生产 claim/unlock/robust fixup；页固定、互斥及 wait 通知是受控单线程接缝。`pi-exit.sh` 的真实 pthread/bucket 发布交错与 `pi-priority.sh` 继续覆盖 owner 归还和优先级链。api8 的 `--program pi --program robust_exec --program pid_namespace` 在 RV/LA 初始 PID 视图执行完整 15 项 PI 和 nonleader exec；BoarOS nested guest 仍未开放。宿主可编译 `tests/workloads/resource/pid_namespace_futex.c` 后运行 `unshare -Urpf -- <binary>`；`robust_exec.c` 用 `unshare -Urpf -- <binary> --nested --expect-retained`。这两个 native 程序确实创建新 PID namespace，权限不足时失败，不静默跳过。

### 5. namespace FD 的存储所有权

动态 namespace 将 registry、allocator 和 bitmap 放在同一堆对象，初始层继续使用 scheduler 静态存储。最后 registry 引用只把对象排到根的退休链；identity 和 namespace 两条退休链统一在锁外释放。父 namespace 的引用由子 registry 持有，不能在持 identity 锁时执行通用 namespace destroy 或堆操作。

proc 快照在锁内固定 registry，再在锁外分配小型 nsfs bridge。bridge 通过通用 namespace 引用供快照/OFD 共享，并单向持有一个 registry 引用；registry 不反向持 bridge，不存在引用环。两个独立打开可有不同 bridge，但稳定 namespace inode ID 和目标 registry 相同。最终 bridge destroy 归还 registry 并在锁外释放；任务仍持 active/for-children 时可以重新打开，不需要复活归零的原 bridge。普通 clone 不创建 bridge。

`/proc/PID/ns/pid` 取 active，`pid_for_children` 取后续子进程目标；两者返回相同 nsfs 类型 `CLONE_NEWPID` 和 `pid:[inode]` 名称。按仓库固定 Linux commit f4cdf7ca 的 pidns_for_children_get，尚无 child_reaper 的 for-children 仍可取得 namespace FD；其目标身份和存储引用保持有效。这与较旧 Linux v6.12 的检查不同，不混用版本语义。proc 使用同一次多类型快照，并在锁外分配后复查任务代际、MM、安全状态和访问凭据。死亡 namespace 的 FD 仍保留存储，setns 不复活它，随后 clone admission 返回 ENOMEM。当前用户只能取得初始 namespace FD；嵌套所有权和死亡状态由生产函数 host fixture 验证。

pidfd_open 的数字现在按调用者 active namespace 查找。pidfd_send_signal 在取得 FD 身份后、导入用户 siginfo 前检查 namespace 关系：仅同层或发送者为祖先时允许，继承或 SCM_RIGHTS 不赋予向祖先/兄弟发送的权力，失败为 EINVAL；随后仍走原信号权限和角色存续检查。该限制也覆盖 proc 进程目录 FD。getfd 不套用 send 的层级限制，继续沿真实身份、凭据、MM 和 files 复核。pidfd setns 使用保留身份的 canonical root 编号与 generation 取得 namespace 快照，不把 leaf number 当作 root 重新解析。

`tests/host/pidfd_files.c` 通过生产 FD 表 fork、pin/install 转交和 pidfd 实现检查兄弟同号、外层 FD 拒绝、合法祖先访问、跨兄弟 getfd、退出后编号复用和最后引用归还；调度及 OFD 最终析构仍是 fixture 接缝。proc-index 检查实际 nested namespace 快照的 canonical 适配与已退出身份拒绝，未执行完整 nested setns 安装。RV/LA 的 pid_namespace 使用真实 fork/SCM_RIGHTS 传 pidfd 并做 signal 0 与 fdinfo 检查，仍仅初始 PID namespace；native Linux 的 fsopen/setns 权限拒绝单独报告，不算成功。完整 nested guest 和后续剩余资源边界仍不可省略。

### 6. proc 固定挂载视图

proc 实例与 FSOPEN 配置 FD 已持有创建者 active PID registry。输入数字及目录 cookie 使用该视图，inode 保存 canonical root 编号与 generation；资源查询不随读取者切换而重新解释。stat/status 的父/组/会话/tracer 与 namespace 列、self/thread-self、pidfd fdinfo 已显式转换。所有权、权限和验证范围见 [procfs 模块](procfs.md#pid-挂载视图)。完整 nested guest、独立 IPC/cgroup 输出及下列剩余接口仍是开放条件。

## 验收

`make test-pid-namespace-host` 在 4 KiB/16 KiB 配置下运行同一真实基础实现，使用 ASan/UBSan。受 ptrace 限制的宿主需要 `ASAN_OPTIONS=detect_leaks=0`；本测试使用静态/栈存储，无堆分配。覆盖三层映射、兄弟同号及不可见、桶冲突、未发布不可见、每层冲突回滚、精确/自动分配、存储引用与号码复用分离、disable 对发布交错的拒绝、祖先关闭和最大深度/代际耗尽。`make test-pid-object-host` 验证共同分配函数没有破坏既有单层路径及原 64 字节类别。`make test-pid-namespace-concurrency-host` 用四个真实宿主线程、生产 `kernel_raw_lock` 和 barrier 争用同一两层精确编号，逐轮检查只有一个预留者、所有 role 跨层指向同一身份、exec 式 role transfer、最后 role 摘除后的数字复用、远端保留旧引用和锁外可回收的退休链；不使用 sleep。socket identity 测试同时检查生产 scheduler create/commit/release 后 registry 引用回到初始 owner。

`make test-pid-namespace-lifecycle-host` 调用生产 scheduler identity 创建/提交/释放，验证父 active 不变、子目标、init 与外层进入子 namespace 的父 PGID/SID 继承、不可见编号为 0、显式 setsid 后的继承、错误目标和线程组合、死亡 namespace 发布前拒绝、堆操作均在锁外、两条退休链归零。这是顺序调用生产函数的生命周期 fixture，不是真实 guest clone/shutdown 并发；四线程编号测试只覆盖预留/发布/退休竞争。另有两个真实 pthread 用 barrier 强制生产预留完成、shutdown defer、admission ENOMEM 与身份释放，验证 collector 恰一次唤醒并重排 init、堆/引用/退休链归零；未覆盖完整 clone 资源复制和实际 guest init 退出。`test-sched-concurrency-host` 中的 reparent 用例覆盖存活兄弟、dead-leader subreaper 及 namespace 边界。生命周期 fixture 另外调用生产 user-ID helper，检查内层 TID/TGID 1 与内部 root 23 分离、不可见父0、可见内层父1及 set_tid_address；child-link fixture 调用真实 wait_target_get，确认内层2和外层24持同一 canonical 身份、内层不能用24越界查找。clone 的用户内存两种 SETTID 写入仍需嵌套 guest 端到端检查。生命周期 fixture 另外验证 namespace 终止目标选择排除 sibling/已有 zombie，动态 namespace/bridge 分配失败、两个独立 snapshot 的稳定 inode、死 namespace 保活、最后 snapshot 关闭后的根引用和堆计数回基线。四个真实 pthread 用 barrier 同时创建独立 bridge、共享一个通用 namespace 引用并关闭，每轮检查 registry/bridge 引用精确值，最终排空双退休链；这验证 bridge 与 registry 两种引用模型的交接，但不是完整 guest clone/shutdown 并发。proc fixture 覆盖 PID 与其他类型组合快照、权限/身份变化后的全部 owner 回滚，以及未出生 init 的 pid_for_children 可打开、目标 ID 正确及 owner 归还。

`tests/userland/api8.py --program pid_namespace` 检查初始视图的 pid/pid_for_children 同 inode、readlink、NS_GET_NSTYPE、setns 不改变 active PID、继承 FD 和最后关闭。默认严格要求 direct 与 pidfd 组合 setns 返回 0，并分别打印 success。只在受限宿主手动传入 --allow-eperm 时容许 EPERM，并明确打印 permission-denied，不将该分支算成 setns 成功；此检查不冒充嵌套 namespace 验收。以上测试不证明 Linux namespace ABI 已实现。接线后必须在 Linux 和 RV/LA 使用同一用户 ELF、pipe/wait 握手做以下行为对照，不用 sleep 估计进度：

1. 三层 init，记录每层 fork 返回、getpid/getppid、set_tid、两种 SETTID 写入；兄弟同号互不可见，祖先能查到后代。
2. 外层与内层分别挂 proc；继承外层 proc FD 验证视图不变，新挂 proc 只显示本层后代；旧 FD 跨数字复用仍不能别名。
3. 内外层 pidfd、wait、SIGCHLD、siginfo、ptrace stop/event；外层 parent 不可见时 pid 为 0，errno 与真实 Linux 一致。
4. 子孙形成孤儿，测试本层 subreaper 和跨层边界；init leader 先退、其他线程存活、非 leader exec、最终 init 退出杀死后代且外层继续运行。
5. 保留死亡 namespace FD，再 fork/setns；在资源准备与最终发布之间令 init 退出，验证没有部分索引/FD、owner 或 bitmap 泄漏。
6. 多层 set_tid 在每层分别冲突/权限不足/坏用户地址；回滚后同号可再用，失败不安装 pidfd；并发正常 fork 与精确预留不能拿到同一号码。

参考采用仓库固定 Linux commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e` 的 `kernel/pid.c`、`kernel/pid_namespace.c`、`include/linux/pid.h`；完整接线还须逐项核对 `kernel/fork.c`、`kernel/exit.c`、`kernel/signal.c` 和 `fs/proc/`，不能将当前基础测试作为它们的动态对照。

固定 Linux 对照：[fork.c](https://raw.githubusercontent.com/torvalds/linux/f4cdf7ca9a1fdcca413157df19753f388a5a224e/kernel/fork.c) 中 PGID/SID 继承独立于 child-reaper 标记；pidfd put_user 发生在最终 namespace admission 之前。完整 guest clone/shutdown 的确定性交错仍是开放前测试项。`tests/host/pid_namespace_shutdown.c` 将生产 drain、整组终止、wait claim/已有 zombie 退休和双队列 collector 连跑：三层 namespace，EXITED leader 加活线程，兄弟隔离，真实 pthread 在关闭前保留预留、关闭后 admission 返回 ENOMEM；内层 init 先恢复 cleanup，外层还等待真实外部 parent 持有的 zombie。关闭祖先后，即使后代 accepting 位尚未改变也拒绝创建；保留死 namespace FD 并安装 for-children 也不能复活创建。任务执行及 MM/FD/栈清理仍由 fixture 建模，物理页释放使用恰一次受控 sink；这不等同完整 guest shutdown，也未覆盖真实 ptrace 清退。调度器 forget 与退休实现在 lifetime.c 同层维护，fixture 不复制身份摘除和 wait 事务。

wait4 非退出事件在原有报告状态锁内同时快照观察者可见 PID，之后的 status/rusage 用户复制只消费标量，不在可睡眠复制后重读远端 role。child-link host fixture 在第一次生产 report 用户复制内执行真实 TID role transfer，并验证第二次复制后仍返回原 PID；这是受控 role 交错，不宣称复现完整非 leader exec 或 UAF。完整 guest wait/exec 交错仍属开放门槛。


### 信号视图接线阶段

kill/tkill/tgkill 和 rt_sigqueueinfo/rt_tgsigqueueinfo 用调用者 active 视图查找，tgkill 的 TGID 比较也在该视图；内部 scheduler root lookup 不变。kill(-1) 只选择调用者可见且本地 PID > 1 的其他线程组，兄弟 namespace 的同号不会命中。负 PGID 先查找并持有真实身份；kill(0) 持有调用者实际继承的 PGID，而不是重新解析数字 0。固定 Linux 的共享组语义允许 kill(0) 作用于该实际组的外层/兄弟成员，这与同号不同组隔离不是一回事。

固定 Linux `kernel/signal.c` 的 prepare_kill_siginfo 使用发送者自身视图 TGID；send_signal_locked 对 PID-bearing payload 仅在实际发送者对接收者不可见时置 0。不得将 SI_QUEUE 用户填写的数字当查找键或强行翻译为接收者编号。当前发送在发布锁内做不可见处理；队列存已确定的标量，signalfd/sigwait/handler 消费不再重查历史 PID。SIGCHLD 则在来源身份锁内按 parent/tracer 视图快照 child TID；异步 parent-death 在 reparent 锁内快照真实死父对孩子可见的 TGID，不误用 cleanup worker 的身份。

signal-owner host fixture 使用真实两层 registry/index/queue，检查兄弟同号、kill(-1)、TGID 不匹配、权限拒绝、普通/用户填充 sender 和父视图 SIGCHLD；在生产 queue allocation 内受控转移目标 TID role，确保原 generation 被拒绝且候选 owner 归还。它不是 guest nested syscall 验收。CLONE_NEWPID 保持关闭，namespace init 信号策略已接入下述阶段，但完整 nested syscall、proc/pidfd/futex/IPC/socket 视图及完整退出交错仍须接线/验收。


### Init 信号策略与退出结果

真实 init identity 发布时设置 group-owned `signal_unkillable`，普通子进程不继承，非 leader exec 随进程组状态转移。无 pending 的正常返回路径不增加检查。发送时，默认信号对 init 忽略；已阻塞/正在 sigwait 的信号仍可入队，解除阻塞后的默认动作再检查免疫，不能因早先入队而意外终止 init。安装 handler 的信号正常递送。不可见且带 PID 字段的来源或受信任内核来源只为 nested init 的 SIGKILL/SIGSTOP 提供 force；global init 额外拒绝这两种信号。用户自填 si_pid=0 不构成 force；SI_TIMER/SI_SIGIO 无 PID 字段，不能仅凭来源不可见获得 force；允许 self 的 SI_KERNEL 仍遵循 syscall 原有检查。默认同步 fault 在未 ptrace 时清除免疫；调试器接管的 fault 不因此永久改变 init 策略。

直接 SIGKILL、CPU 硬限制、消息通知和 POSIX timer 的入口也检查策略；被忽略的 timer SIGKILL 不能残留一个会调用 group-exit 的 wake owner。已有忽略周期 timer 的停放机制需考虑 init 的特殊默认动作：阻塞或 sigwait 后它重新可观察。仅带 timer registry 的免疫 init 在 mask 变化后检查自己的 parked 列表，不给普通任务增加 timer 扫描；显式 SIG_IGN 的既有停放策略不变。

`queued` 的真实 guest PID1 检查默认 TERM/KILL/STOP 忽略、handler、阻塞/消费、同 namespace 子进程 kill 以及非 leader exec 后再次检查，并要求最终资源清理成功。该新覆盖曾在两架构暴露 group exit completion 的创建时旧 TID：exec 已采用 PID1，exit_group 却只更新 reason/status。现于 group exit 提交时一并快照当前内部 TID/TGID，保留 reaper 身份一致性检查。`posix` 增加默认免疫 timer 停放后仅改 mask 即恢复的检查；timerfd 完成作为时序边界，不依赖随机 sleep。

Host 覆盖 nested init 与 global init 的 admission 差异、伪造 sender=0、handler 在分配期间恢复默认、blocked default 的消费、fault/ptrace 分支、timer kill 无退出回调。祖先 KILL 用生产发送路径到受控 exit sink 验证，不能视为完整 nested shutdown；STOP 的 nested force 目前是生产策略函数检查。RV4/LA2 的真实 PID1 测试不替代完整 nested guest；本机 Linux unshare PID 返回 EPERM，不宣称已做 nested Linux 行为差分。CLONE_NEWPID 继续关闭。


### Ptrace PID 与退出交接

ptrace 请求按调用者 active namespace 查 TID；attach 在可睡眠准备后重查该视图下同一目标，其他请求在取得停止访问权时也重查。TRACEME 沿保留的 creator 身份找真实 TID role，不把 creator 的本层数字当 root PID 再查。权限仍由真实 caller/target 凭据决定。内部 tracer 存在性 helper 保持 root 身份语义，不能把“对内层不可见”的 tracer 当成不存在；proc 的 TracerPid 按固定挂载 PID 视图输出真实 tracer TID，不可见为 0。

fork/clone/vfork 事件持有 child PID 身份到事件消费，GETEVENTMSG 的数字在实际 stop 发布锁内按当前 tracer 视图生成，不能凭可能已经复用的整数查回 child。普通未跟踪的非 vfork clone 不增加此引用。exec 在身份采用前快照 tracer（未跟踪时为自然父）视图中的旧 TID，随后只传标量；内部 robust/completion 的 root TID 不改。固定 Linux 的 [exec.c](https://raw.githubusercontent.com/torvalds/linux/f4cdf7ca9a1fdcca413157df19753f388a5a224e/fs/exec.c) 同样在身份改变前取得 old_vpid。

合成停止的 GETSIGINFO 使用 tracee 自身视图的 TID；已有递送或 SETSIGINFO 的 payload 原样保留，不重新解释数字。tracer 改变递送信号时，新 sender 使用 tracer 的 TID（不是 TGID），按 tracee 视图转换，祖先不可见为 0。这两条不同规则对应固定 [signal.c](https://raw.githubusercontent.com/torvalds/linux/f4cdf7ca9a1fdcca413157df19753f388a5a224e/kernel/signal.c) 的 ptrace_do_notify 和 ptrace_signal。SETSIGMASK 通过普通 mask 更新通道完成 shared pending 重定位和 immune-init parked timer 重检，而非只覆盖位图。

外部 tracer 的 FINAL wait 所有权不会因 namespace init 死亡被强行撤销。消费 FINAL 后，生产 finish_traced_wait 将 child 交回自然父；关闭中的 init 忽略 SIGCHLD，child 随之转交 cleanup。停止中的访问 owner 与 wait claim 都归还后，延迟的终止唤醒才可继续。Host 将这些生产请求、stop 发布、受控 user-copy 交错及 FINAL wait/collector 连跑，覆盖同号不可见、权限拒绝、准备期间 TID role 转移、非 leader tracer 的 sender、内外视图事件和 namespace 延迟收尾。实际 task 执行、MM/FD/栈清理仍是模型，不能代替 nested guest 的 ptrace 停止/继续与完整退出。

`resource/ptrace.c` 增加 fork、线程 clone、vfork/vfork-done、非 leader exec 的 GETEVENTMSG 检查，RV/LA 可在现有初始视图跑真实用户程序。宿主 TRACEME 返回 EPERM，未作为 Linux 动态通过；nested guest 仍须在全部视图接线后验收，CLONE_NEWPID 保持关闭。

### UNIX socket 凭据的身份与视图

`SO_PEERCRED` 在 socketpair、listen 和 stream/seqpacket connect 时保存 TGID 身份引用及当时的 effective UID/GID，不再缓存 root PID 整数。查询按执行者的 active PID namespace 转换，不可见为 0。listener 凭据更新后只影响后续 connect；已建立的两端持有各自独立快照。对端退出、角色摘除或号码重用不会将旧 socket 重新绑定到新进程。每端只多持一个现有 PID 引用，不分配额外凭据对象；listener 复制在 socket 状态锁内增引用，替换旧引用及最终释放在 socket 锁外回收。

`SCM_CREDENTIALS` 的显式 PID 先按发送者 local TGID 进行权限比较，再在发送者视图查找并保留身份；无效 UID/GID 的 EINVAL、权限 EPERM、显式零号和不存在 PID 的 ESRCH 顺序不变。自动凭据沿用发送时保留的 TGID 和 real UID/GID。收取时只转换消息持有的身份，不拿历史数字重查；SCM_RIGHTS 转移/继承 FD 不改变该身份或 SO_PEERCRED 快照。用户 namespace 尚未实现，本阶段不新增其 UID/GID 映射或 capability 作用域。

固定 Linux 来源为 [af_unix.c](https://raw.githubusercontent.com/torvalds/linux/f4cdf7ca9a1fdcca413157df19753f388a5a224e/net/unix/af_unix.c) 的 prepare/update/copy_peercred、[scm.c](https://raw.githubusercontent.com/torvalds/linux/f4cdf7ca9a1fdcca413157df19753f388a5a224e/net/core/scm.c) 的显式凭据检查，以及 [sock.c](https://raw.githubusercontent.com/torvalds/linux/f4cdf7ca9a1fdcca413157df19753f388a5a224e/net/core/sock.c) 的 SO_PEERCRED 导出。内部 root PID helper 保持不变；新的 caller-view helper 仅用于用户边界。

验证分层：`socket_identity.c` 使用真实身份/registry，检查兄弟同号、不可见查找和已退出 retained owner；socket accept host fixture 调用生产 socketpair/peercred/destroy，受控切换执行者，检查内外/兄弟视图、摘除后精确复用及旧/新引用计数分离，最后双退休队列归零。它不是完整跨 namespace syscall。`resource/pid_namespace_socket.c --nested` 在 native Linux 的 `unshare -Urpf` 内真实创建两个 CLONE_NEWPID 子进程，两者均验证 getpid()==1，双向自动/显式 SCM、父身份不可见、SCM_RIGHTS 转移后退出身份、继承兄弟 FD 和丢弃 capability 后的权限顺序。默认模式在 RV4/LA2 运行同一实际 FD/消息/退出程序，但只在初始 PID namespace；既有 unix_control 全套补充 stream/dgram/seqpacket、真实/effective UID/GID、listen/connect、copy fault 和关闭回收。完整 nested guest 仍未运行，CLONE_NEWPID 继续关闭。
