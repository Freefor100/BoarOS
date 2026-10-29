# 内核调度、线程组与生命周期

本文记录RV/LA默认单CPU的OTHER/FIFO/RR调度、线程组资源、clone/exec/wait和回收契约；独立RV用户1～4核配置见[执行与服务边界](riscv-smp.md#真实用户态与cpu0业务交接)。背景见[线程组与 futex](../learning/threads-and-futex.md)，信号、文件资源、MM的稳定边界分别见对应模块文档。

## 实现入口

| 文件 | 职责 |
|---|---|
| `kernel/sched/core.c` | 创建、切换、tick 入口、资源借用校验 |
| `kernel/sched/policy.c`、`runqueue.c` | 纯策略/预算状态机、分级 ready 队列 |
| `kernel/sched/scheduling.c`、`kernel/syscall/sched.c` | 实际运行时间记账、抢占、安全返回边界、策略与请求affinity；独立用户SMP的CPU0服务交接 |
| `kernel/sched/process.c` | clone、线程组/父子树、wait、退出、回收与记账 |
| `kernel/pid.c`、`kernel/sched/identity.c` | 私有编号保留、身份发布/角色引用，以及锁外身份存储释放 |
| `kernel/sched/lifetime.c` | 任务存储借用、退休后继、join/reaper 资格与锁外页归还 |
| `kernel/sched/proc.c` | 进程快照、对象路径与缺页/磁盘读取统计 |
| `kernel/credentials.c`、`kernel/syscall/credentials.c`、`kernel/syscall/capability.c` | 不可变凭据、UID 账户及身份／capability 控制 |
| `kernel/sched/exec.c` | 已准备映像的提交与旧资源清理 |
| `kernel/sched/sync.c` | 任务 owner 的 mutex/RWlock、锁序与 FIFO 资格交接 |
| `kernel/sched/park.c`、`wait.c` | 代次等待、借用游标、blocked/期限索引及完成仲裁 |
| `kernel/sched/futex.c`、`futex_pi.c` | 256 桶普通 futex、PI mutex 及优先级传播、robust-list 退出清理、clear-child-tid 唤醒 |
| `kernel/sched/signal.c` | 组/线程 pending、disposition、stop/continue 和重启 |
| `include/arch/task.h`、`arch/riscv/process.c`、`arch/loongarch/context.c` | 构建期线程、地址空间与 trap 操作；各架构初始/clone/exec 寄存器契约 |
| `arch/riscv/`、`arch/loongarch/` 的 context/trap 汇编 | 架构 ABI context、用户异常返回与中断状态 |
| `include/kernel/task_state.h` | 任务存储的共同内部布局 |
| `kernel/sched/private.h` | 调度器状态、组与队列成员关系 |

USER_SMP保留来源CPU的实时用户资格，并单独索引CPU0实时服务；普通服务与已有worker按OTHER队列轮转，迁移排到目标核队尾。用户`sched_yield`在发起CPU调用共同的`kernel_scheduler_yield_current()`，等待游标和回调借用归还也使用真实内核调度。组停止的最终调度锁段重查执行owner：已被远核接纳的成员取得持久停止通知，尚未执行的成员才可摘队并停车。具体边界及尺寸见[用户两核](riscv-smp.md#真实用户态与cpu0业务交接)。

公共 scheduler 头不暴露 Trap Frame；架构 clone 入口由 `include/arch/task.h` 选择，旧 RV 入口保留包装。syscall 通过不透明 task 接口取得 TID/TGID/PPID 和资源，不直接修改调度私有字段。

`boot_cpu`、`kernel_wait_domain` 和调度器首个 cacheline 按此顺序放在现有 BSS 首页，CPU 本地状态与共享锁各自按 64 字节隔开。对象及其所有权保持独立，普通陷入、身份查询与返回准备因此只需触达一个静态调度控制页。链接布局检查保护同页与隔离要求；它减少必要的 TLB 工作集，不保证任意动态页和链接地址都没有冲突。

等待登记、阻塞提交、期限索引、运行记账、策略更新、队列校验和runnable发布使用统一rank40短锁；heap10→physical20、
对象内部30→调度40是raw锁序。raw不跨架构切换，不允许调度、分配、I/O、用户复制或外部回调。
同一 API 按 CPU 数编译：UP 关闭中断、保持抢占嵌套并使用编译器屏障，SMP 另以 acquire/release 原子操作互斥。
`LOCK_DEBUG=1` 开启递归扫描和等级审计，默认关闭；持有链始终保留，用于禁止等待、嵌套恢复和最外层解锁后的通知发布。pthread 宿主夹具明确使用 SMP 配置，`test-sync-host` 覆盖 UP/SMP 与锁调试的四种组合。

短对象保护按实际访问者选择：`KERNEL_TASK_LOCK_SCOPE` 用于只由任务修改的 ext4
缓冲元数据，`KERNEL_IRQ_LOCK_SCOPE` 用于任务和设备中断共享的 VirtIO 请求状态。
默认 UP 构建中，前者通过抢占深度和编译器屏障排除其他任务，保留原中断状态；
后者再关闭本地中断。UP 的互斥由抢占深度提供，检查嵌套归还并禁止阻塞，无需读写对象锁字。
SMP、锁调试或成本诊断构建直接使用完整 raw 实现，保留持有链和对象初始化检查，递归及等级审计由 `LOCK_DEBUG` 开启。调度器和分配器继续使用原 raw
持有链；`test-lwext4-exclusion-host` 检查四种 UP/SMP、debug 组合及真实共享更新。

任务表、身份角色、父子/组成员关系、join及退出队列也使用该调度域；私有准备和资源清理在锁外。
受控任务可绑定匿名及私有arena tmpfs映射 MM；RV 用户路径中的信号递送、sigreturn 和 ext4 冷读在实际执行 CPU 推进，命名空间修改、日志和挂载管理继续由 CPU0 执行。具体交接见[用户两核](riscv-smp.md#真实用户态与cpu0业务交接)。

`kernel_wait_prepare`为当前任务建立不回退的全局代次token，登记后复查条件，释放对象保护再调用
`kernel_wait_park`；`kernel_wait_finish`同步撤销绑定节点。wake/期限/signal第一个完成者决定
原因，旧代次与重复通知无效，任务页复用也不重用代次。多队列节点通过`kernel_wait_node_bind`共享token。
代次耗尽返回INVALID_STATE且不发布新登记，重复等待和错误owner属于fatal。
事件队列只由实际等待节点保存，期限、原因和可中断性只由wait record保存。通知先按原期限
摘除AVL，再发布完成原因；finish收完节点及借用后才清空record，futex requeue直接迁移节点。

任务的on_cpu与CPU切换前驱共同保护旧栈：阻塞提交后的提前wake只记录ready_pending，
park自行保存IRQ并保持关闭跨过切换交接，返回时恢复原状态；恢复后的执行者已经取得本任务CPU，NOTIFIED原因到它执行finish前保持不变，读取结果不再重复取得调度保护。finish仍在保护内拆除登记并等待真实借用归还。
新栈的`kernel_scheduler_switch_finish`才发布旧任务或允许回收。恢复已有上下文的C切换尾部、
首次kernel/user trampoline及退出切换经过同一完成点；无切换的普通异常返回不重复查询。
调度请求用原子exchange消费，完成点不清除后来请求。

通知准备先以acquire读取原子发布的实际登记数，空队列直接完成；非空再在等待域锁内复查并取得游标借用。登记和摘除仍在等待域串行完成，以release发布计数，不增加重复状态。调用者在业务保护内登记，或绑定后重新查询业务条件；与通知发布共同保证休眠前事件不会丢失。
被摘节点保留有引用的退休后继，引用链迭代归还，每段最多16项。游标前进在同一等待域内借用后继并归还前驱；结束时先归还节点，再结束队列借用。
callback在调度锁外同步执行，临时禁止抢占，不能阻塞；自身只能非阻塞remove。
释放context前必须remove_sync等待借用归零。futex的短链过滤通知不跨调度锁，
不建立游标借用；长链和requeue使用同一借用游标。
requeue先借task、放当前游标，再在同一raw临界区更新key与节点位置；同桶保持链上次序。源后备引用延后到桶操作锁和用户pin都归还后释放。
queue_close停止新登记并借用队列直到通知完成，空队列也不能在close的解锁间隙销毁。
queue_destroy在注册或操作/游标/回调借用未归零时返回BUSY，调用者继续持有queue及业务owner。

聚焦入口：`make test-wait-host test-wait-riscv test-wait-loongarch`。host直接链接park.c、
真实raw/runqueue，在2/4线程握手下检查提前wake、单次仲裁、旧代次、多队列与退休游标；
COMMITTED任务的模拟backend与生产一样在通知中摘除默认节点，另验证任务/回调混合队列的FIFO和新增登记的sequence边界。
双侧512MiB/1GiB检查真实首次/退出切换、timeout及任务页回到基线。此处是原语与单CPU证据，
不替代共享MM、业务对象或多核客体的并发验收。

唯一任务节点的通知在同一raw区间完成，仍验证queue/borrow_owner身份，不建立跨解锁游标；
回调保留借用并在锁外执行。
finish每段最多摘16个登记，未借用的小批登记可在同一保护下结束；存在借用才等待归还。

可睡眠RWlock每实例持有rank30内部raw；资格判断与token登记在同一保护下完成，释放raw后等待。
`kernel_rwlock_try_write()`及mutex包装只尝试取得，不登记等待者；已有读/写owner、pending或handoff均返回忙，guard和IRQ状态保持。它保留相同rank/key、owner与上下文检查，不能用于越过FIFO或在raw内取得任务锁。
未竞争取得不在热路径栈上构造waiter；慢路径持有其栈waiter至token结束、pending摘除及重新取得对象raw，覆盖通知者的最后访问。
FIFO先授资格再通知，连续读者按原边界分段，每段最多16个等待者，handoff owner覆盖分段间隙。
已经授予但尚未运行的读者/写者仍占用锁，后来读者不能越过writer。
排在其他等待者之后的写者直接 park：下一次释放不会给它资格，因此不扫描运行 CPU，也不主动自旋。其余已登记争用者在实际在线至少两 CPU 时，在锁外恢复入口 IRQ 状态，短时间轮询自己的 release/acquire 资格标记。基础上限为50µs；仅FIFO队首且独占writer对应另一在线CPU的RUNNING/on_cpu任务时，上限为1ms。CPU/current只用于当时的预算提示，锁外不保留或解引用该任务；任务随后阻塞或切换仍受同一个绝对截止约束。到界沿原park路径等待，提前通知直接完成；单次取得只使用一次预算。默认UP构建消去此路径，SMP内核仅启动一CPU时也直接park。
通知不等于资格：额外唤醒只更新等待token，不重排FIFO；旧登记结束与重取对象raw之间也允许授资格。guard的rank/key和任务owner
保持原契约；等待仍不可中断，没有新增timed/interruptible API。

调用方无raw业务锁时，`KERNEL_WAIT_RECHECK`在登记后求值一次非阻塞条件，再park/finish。
poll的多个节点绑定一个token；epoll停止item通知后同步撤销node才释放context。
TTY/lwext4等通用通道仍由所属调用方在单CPU IRQ域内持条件owner；宏不把业务数据变成跨核安全。
宿主RW门禁为`make test-sleep-lock-host`，包含1/8/32等待者、writer边界、分段中到达的读者、
IRQ恢复及非法owner/上下文。真实I/O等待继续用`make test-io-sleep-riscv`验证DMA、超时与回收。

## 策略、就绪队列与 RT 预算

独立[RV受控两核配置](riscv-smp.md)在同一调度域内维护每CPU资格索引，只有显式mask的joinable内核任务参与跨CPU选择。
用户及业务worker默认CPU0；全局runnable成员与on_cpu仍是唯一owner，资格节点只借用成员。
任务构造、栈窗口和实际回收限CPU0，停止时受控存储owner归零后才确认HSM STOPPED。

OTHER 保留 100 Hz tick 轮转，内核 worker 使用 OTHER。FIFO/RR 的用户优先级为 1–99，数值越大越优先；FIFO 不因 tick 同级轮转，RR 每片 100ms，按实际在 CPU 上运行的纳秒扣除。更高优先级抢占、阻塞和 yield 不重新赠送 RR 时间片；耗尽后才重装。唤醒及策略修改标记 need_resched，在 IRQ 或返回用户态的安全边界切换。降优先级排到新级队首，升优先级排队尾，同级修改保留位置；高优先级抢占的当前任务回原级队首。

挑选、摘队列与取得on_cpu执行owner在同一个短锁发布点完成，普通切换与退出复用该入口；锁外才切换架构状态，旧owner仍由新栈交接释放。未使用的拆分pop入口已删除。
yield、预算tick与普通tick在同一次决策raw内完成队列校验、运行记账、挑选和最早期限计算，
释放raw后才调用timer backend；切换、阻塞和退出也按这个顺序提交硬件事件。
普通tick只在CPU0推进load采样余量，不为未到采样期的事件额外取锁。
到采样期才借任务分段遍历，遍历前检查和决策时重查分别保留。无效队列不消费待调度请求。

ready 节点独立于 blocked/cleanup 链。全局优先级排序双链保留遍历视图，100 个等级各存 head/tail；同级插入、OTHER 尾追加、删除和选取均 O(1)，新增空的中间等级最多扫描 99 个等级，不扫描任务数。RT 被节流时只取 OTHER 队首；没有 OTHER 则 idle，不能借机运行已耗尽的 RT。

FIFO yield和RR到期只轮转同级候选；未节流且没有同级竞争者时，不能把CPU下让给低优先级任务。
旧任务仍on_cpu而暂未入队，决策必须显式比较当前与ready候选，不能把“有next”当作可轮转。
`test-scheduler-handoff-host`直接链接生产决策、队列/策略/raw，覆盖低/同/高优先级、节流和idle；
`test-scheduler-handoff-riscv/test-scheduler-handoff-loongarch`在512MiB/1GiB以同ELF对照固定Linux，
高优先级yield/跨RR片时低任务计数保持0，随后高任务阻塞才允许低任务前进并正常回收。
该用例在放行前准备计时输出，阻塞后通过pipe确认真实子任务进展；不以5ms睡眠
推断调度完成。另运行子任务延迟20ms的同契约反例，原160ms RR窗口和退出核对保持。

退出在发布 completion、清 TID 并唤醒等待者后，若已注册 cleanup worker，按同一
ready 策略直接选择下一任务。退出栈只切离，回收仍由 worker 或 join owner 在可信栈
执行；不能强制返回 idle，使已经 ready 的 joiner 在 `wfi` 中等待下一次 timer。
没有 cleanup worker 的模块 fixture 继续返回负责回收的 idle。
`test-scheduler-cases-riscv` 在未启动 timer 时验证退出、join 和清理的连续进展，
以及除启动 owner 所持 cleanup worker 外的页回到基线。

全局预算配置默认 period=1,000,000us、runtime=950,000us，消费量和结算起点按CPU保存；仅运行中的 FIFO/RR 扣费，阻塞及 idle 不扣。周期补充不积累旧余额；runtime=-1 禁用节流，0 不允许运行 RT。控制更新在调度短锁内先结算各CPU在旧配置下的实际运行，校验 period>0、runtime>=-1 且 runtime<=period（-1除外）、两值均不超过 INT_MAXus。runtime 修改不清消费；相同 period 写入不重开周期，改变 period 时从当前时刻建立新长度但保留已消费值。proc 控件见 [procfs](procfs.md)。

硬件 timer 的最后编程期限由当前CPU持有，取普通 tick、该CPU的RT配额耗尽/补充和当前任务RR片尾中的最早期限。预算 IRQ 可以返回 elapsed_ticks=0；此时仍重新记账调度，但不推进 tick、CPU tick 统计或 coarse clock。实际运行时间由切换、策略/配置修改和 timer 入口结算，精度受关中断临界区与模拟器 IRQ 延迟限制。用户访问复制与可睡眠路径不放在调度状态锁内。

IRQ关闭的短入口复用同一次CPU/current定位，借用在切换或可睡眠服务前结束；
缺页服务使用当前执行owner固定的task/MM，不把CPU指针带过睡眠。RV/LA timer
后端保留已确认的相同事件，避免每次yield重复写硬件，周期IRQ仍自行重装。

任务的`scheduling.priority`保存实际运行优先级，既有就绪/每核资格/CPU0服务与来源保留队列按这个值排序。继承期间才在任务尾部保存基础优先级；sched查询、RTPRIO授权和fork使用基础值，proc的priority展示实际值、rt_priority展示基础值。优先级变更在调度保护内重新发布相关队列并请求目标CPU调度；实际优先级未变时保留FIFO位置。每核就绪与保留队列记录被提升的成员，存在此类持锁者时解除该核RT带宽节流，使其能释放锁。

sched_setparam/setscheduler/getparam/getscheduler、priority_min/max、rr_get_interval 和 CPU0 affinity 使用真实任务状态；未发布 child 不可被按 TID 查询。BATCH/IDLE/DEADLINE/EXT 的 min/max 查询按固定 Linux 返回 0，设置这些策略仍显式拒绝。RESET_ON_FORK 在子任务清除并将继承的 RT 变为 OTHER；exec 保留执行线程的策略。RV 用户执行支持真实1～4核，亲和性与执行资格见 [RV SMP](riscv-smp.md)；nice 调度、cgroup 配额和 deadline scheduler另行实现。


RTPRIO、RTTIME限额属于线程组共享的资源记录，普通fork复制、线程clone共享，exec保留。没有CAP_SYS_NICE时，调度修改先满足同属主规则；进入或切换RT策略要求目标组的RTPRIO软值非零，提升优先级可使用目标软值或保留已经取得的更高RT优先级。降低优先级和退出RT沿Linux规则处理；清除RESET_ON_FORK仍需CAP_SYS_NICE。资源数值不裁成99，实际FIFO/RR优先级范围仍为1～99。

RTTIME对每线程连续RT执行计量。各CPU的timer tick仅在当前用户线程运行FIFO/RR且软限额有限时推进watchdog；普通优先级任务在读取全局tick前跳过watchdog调用。同一全局tick跨核只计一次。超过按100Hz向上取整的软／硬阈值后，线程进入现有CPU timer活动索引，由CPU0检查并向线程组发送SIGXCPU，软限额增加1,000,000us，或在硬限额到达时发送SIGKILL。仅有RTTIME待检查的条目不采样完整CPU clock；普通trap不新增watchdog锁或采样。

连续计数在调度器确定任务带着BLOCKED或STOPPED状态离开CPU时清零。计数为零时不写RTTIME计数或资源标记；待检查标记只会在非零计数后发布。已经退出RT策略或把限额改为无限的任务仍按历史计数清理，并移除仅由RTTIME产生的CPU timer索引。PREPARED通知、COMMITTED后在切换前收到通知、停止后在切离前收到SIGCONT，以及抢占、yield、迁移、CPU0服务和RT带宽节流都保留计数；此时状态已经或仍为READY，未完成一次睡眠/停止。直接host检查以手动tick覆盖跨CPU去重、软硬阈值及三种等待交错，真实消费者通过父子握手确认阻塞后继续执行。

`make test-sched-concurrency-host`直接执行生产策略/选择/等待交接，在2/4线程握手中验证唯一执行owner、旧栈交接及IRQ恢复；保护CPU独立记账和公开策略入口的同域修改。该证据不替代客户机多核。

确定性 host 测试 `tests/host/sched_{policy,runqueue,syscall}_test.c` 覆盖预算、排队、ABI 和随机队列模型。`tests/diff-abi/sched_policy.c` 的 23 条同 ELF Linux 对照覆盖参数及真实 FIFO/yield/更高优先级抢占/RR轮转；`sched_stat.c` 另有5条策略stat和STOPPED/zombie wchan对照。`tests/sched-bandwidth-riscv.sh` 使用真实 FIFO 忙循环验证耗尽、OTHER 进展、周期恢复、同值写不补预算和 -1 切换保留消费；500us/250us 场景先等待新 coarse tick，再检查8ms以内被抢占并报告实际超出量。RR 在高优先级任务运行60ms后仍只剩约40ms，能检出错误重装100ms的实现。两个先阻塞后赋RR策略的子进程由同一pipe write唤醒，在100ms quota/300ms period下验证同时到期仍把CPU交给同级peer；旧队首重排已由该测试复现失败。该测试直接调用 SYS_sched_setscheduler，固定 musl 的同名包装函数是 ENOSYS 存根。测试用 guest monotonic 计时，不由 QEMU 墙钟速度推断调度正确性。

`getcpu(168)` 在调用者执行位置返回实际逻辑 CPU，单 NUMA 节点编号为0；两个可选输出独立复制，后一个 fault 保留已完成前缀，第三个缓存参数按 Linux 忽略。

## 身份与资源

### 用户凭据

每个实际线程在任务尾部持有一个不可变 `kernel_credentials`，包含 real/effective/saved/fs UID/GID、排序的补充组、effective/permitted/inheritable/bounding/ambient capability 与 securebits。系统使用单一用户命名空间；初始 UID/GID 为 0、补充组为空，E/P/B 为固定 Linux 的有效 41 位，I/A 为零。

设置接口先导入输入并构造完整候选，再在调度短锁内替换任务指针；旧对象和分配失败的候选在锁外释放。线程 clone 最初共享同一对象，后续 raw setxid 只替换调用线程的指针。musl/glibc 的全线程 setxid 由其信号和 futex 协作完成。普通 fork/vfork 则复制小型凭据对象，并继续引用共享的补充组和 UID 账户：Linux 的空路径 `linkat` 会比较打开时与当前的凭据对象身份，仅复制数值而保留同一对象会错误放行子进程。

`no_new_privs` 是独立的线程标志，设置后保持为 1，clone 继承、exec 保留。它不改变凭据指针，因此仅设置 NNP 后，先前打开的 FD 仍满足空路径 `linkat` 的打开身份条件。OFD 持有打开时凭据引用，直至最后一次实际后端关闭完成；已有 FD 的正常数据读写继续使用打开时取得的访问权利。

当前线程直接借用自己的稳定对象；跨线程查询在调度锁内取得引用后于锁外复制或格式化。退出后任务凭据保留至任务索引、等待者和存储借用全部归还的最终 collect 阶段。同步 I/O worker 显式借用原调用者凭据，计账任务与权限对象分别维护。

`RLIMIT_NPROC` 复用凭据的真实 UID 账户，计数对象是用户线程；内核 worker 不计入。clone 在复制凭据后、复制 MM/FD 前原子接纳，构造失败立即归还。真实 UID 0、有效 CAP_SYS_RESOURCE 或 CAP_SYS_ADMIN 可以越过调用者软限制，仍计入账户使用量。僵尸到 wait/reap 时才减数；普通退出线程在实际 reap 移出组目录的同一调度域边界归还计数，其他任务在撤销身份时归还。EXITED/GROUP_DEAD 只是执行状态，不提前隐藏仍发布的 proc 身份。后续 proc/pid 等存储借用不延长配额占用。非组长 exec 在转移身份的同一调度域边界归还旧组长容器的计数。

真实 UID 改变在同一调度域内将一个线程从旧账户转入新账户，凭据引用保持账户存活。按固定 Linux 的顺序，转账前目标账户已有数量严格大于调用者 NPROC 软限制且目标非 root 时，标记延后失败，setuid 本身仍成功；随后 exec 重新检查当前账户，仍超限才返回 EAGAIN，降至限制内则清除标记。通过 fork 接纳检查也清除此标记。有效/文件系统 UID 改变不转移 NPROC 使用量。

`RLIMIT_SIGPENDING` 的使用量归真实 UID 账户，普通记录和 POSIX timer 预留记录保持接纳时的账户引用。线程改 UID 影响后续接纳，旧记录消费和删除仍归还原账户；队列细节见[信号模块](kernel-signal.md)。dumpable 由任务持有的执行安全对象保存，共享 MM 的线程与 vfork 共享它；普通 fork 复制状态，exec 替换，最终任务回收才释放。身份变化先更新共享状态再替换凭据；exec 的新 MM、候选凭据与执行安全状态一起发布，见[映像替换](kernel-exec.md)。

### PID 身份

TID、TGID、PGID 和 SID 共用 `kernel_pid` 对象：编号、不可回退的代次、引用计数和四类成员链只有一份真实身份。每个线程挂接 TID；线程组代表挂接 TGID、PGID、SID，其他线程通过代表取得组身份。`tid` 仅为对象编号的派生缓存，由身份事务维护，并由调度校验检查一致性。fork 继承父进程的 PGID/SID 对象；线程 clone 只新增 TID。`__WNOTHREAD` 的创建者关系持有 TID 对象引用，退出、收养和非组长 exec 转移关系，不按可能复用的整数判等。

全部 TID/TGID/PGID/SID 角色消失时撤下编号索引并归还 bitmap；pidfd、排队消息、定时器等引用只保留身份存储。新进程可以取得相同编号，但其代次和对象与旧引用分别保持。对象仍有引用不表示 TID 对应线程仍可收信号；线程 lookup、组 lookup 和 proc 可见性分别检查其角色及生命周期。组长先退出保留身份容器，直到最后线程退出或非组长 exec 原子接管四类成员。zombie 在 wait/reap 前保留进程身份；旧 proc 节点用同一对象的代次防止编号复用后命中新进程。

对象存储来自调度器私有slab heap。准备分配可以睡眠；reserve仅占用数字/代次，不进入查找桶。
TID 与私有 TGID 先构造。最终发布在同一调度域中读取实际父进程的 PGID/SID 并挂接角色、进入任务索引及 runnable；准备期间不以临时引用阻止已消失进程组撤号。失败归还私有保留项，不发布半成品。
最后 PID 引用登记存储退休，heap 释放在锁外执行；已经撤号的对象不再次归还编号。
PID记录为64字节，状态与代次共用一字；合法代次仍为proc身份的34位范围，引用保持32位。
状态 0/1/2/3 分别为私有预留、已发布、已撤号及存储退休。已撤号对象可以继续取得存活存储引用，但不再挂接角色或重新发布；退休对象禁止 get。所有状态修改仍在调度域；真实 PID 引用覆盖代次读取，
任务存储借用不能替代PID引用或锁内快照。`test-pid-object-host`覆盖编码边界、非法引用/状态，
以及4/16KiB下32/128个实际PID的单页slab预算和最终页/堆归还。

`scheduler_task_lookup`在发现任务的同一保护下取得存储借用，归还使用`scheduler_task_put`；
locked变体仅供已持调度域的短操作使用。借用不固定MM、文件表、权限、角色或代次。
退休先摘身份和任务索引，资源由可信栈清理；最后等待/执行/查找使用结束后才归还元数据页。
组退出沿本组的借用游标通知成员，不扫描全局任务表；单成员的退出标记和状态发布
在同一raw内完成，仅实际跨解锁通知时增加借用。`test-sched-concurrency-host`
用1/2/4成员及64个无关任务保护工作量、退出标记和全部借用归还。
长枚举借用当前节点，退休节点保留有owner的后继；每段至多16项，后继引用迭代归还。
父子链在实际插入、移除和收养当前节点时检查相邻连接与首尾 owner，不在退出或 reparent 前扫描整条链。`children_epoch` 继续供 wait 选择在成员变化后重查。组遍历持有实际成员的存储引用，成员代次变化时重新从组长开始；proc 调度/身份值受域保护，MM/TTY 内容仍按单 CPU 业务契约读取。

join和reaper在同一域竞争唯一清理资格，运行中的旧栈不能取得回收资格。

### kcmp

RV64/LA64 的 kcmp(272) 在发起 CPU 比较 FILE、VM、FILES、FS、SIGHAND 和 SYSVSEM；同对象返回 0，不同对象返回可用于排序的 1/2。启动时从已有随机源取得每类置换键，以异或和奇数乘法隐藏内核地址顺序。查找按实际 PID/TID 取得任务存储借用，先检查双方 REALCREDS、dumpable 和 capability 子集，再判断 type 和 FD；PID 已回收返回 ESRCH，有身份的僵尸仍可比较。IO/EPOLL_TFD 返回 EOPNOTSUPP，非法 type 返回 EINVAL，FILE 的 ulong 索引按 Linux 原生 unsigned int 宽度截断。

每任务 files_access 的短访问同时覆盖远端资源比较：两目标按地址排序，同目标只取一次；FS 在 VFS 状态保护内读取，其余身份在调度域内读取。FILE 再按地址排序取得真实文件表的槽锁，在调度域重新核对双方 PID/TID 和权限后直接比较槽持有的 OFD，dup/fork 保持相等，重新 open 同 inode 则不同。查询无需创建整表或 OFD 临时 owner，也不触发最后 close 或后端工作。退出先发布 proc_exiting，再在调度域外等待短访问结束，由原 owner 释放资源；僵尸 VM/FILES/FS/SYSVSEM 比较空资源，SIGHAND 寿命见[信号状态](kernel-signal.md#状态与生命周期)。SEM_UNDO 尚未建立时按真实 NULL 比较，CLONE_SYSVSEM 在子任务发布前建立并共享撤销组。

`sh tests/host/kcmp.sh` 检查锁序、真实 OFD 槽、懒分配身份、权限复核和退出等待；`tests/workloads/resource/kcmp.c` 用管道握手覆盖 fork、CLONE_FS、pthread、vfork、SEM_UNDO 最后共享者、真实/文件系统凭据差异、非组长 exec、僵尸与 PID 回收。clone 支持组合仍按本模块原范围。

### pidfd

RV64/LA64 提供 pidfd_open(434)、pidfd_send_signal(424) 和 pidfd_getfd(438)。 send_signal 同时接受按旧 Linux ABI 打开的 proc 进程目录；该节点持有解析时的真实 PID 引用，退出或编号复用后不会指向新进程。proc 根、task/TID 目录、普通文件及 O_PATH 不参与此转换，getfd 和 waitid 仍要求实际 pidfd。pidfd OFD 持有真实 PID 身份存储；dup、fork 和 UNIX FD 传递沿共同 OFD 引用规则。open 接受 PIDFD_NONBLOCK、PIDFD_THREAD，新 FD 设置 CLOEXEC；普通 open 要求 TGID 角色，THREAD 则保留实际 TID。身份代次作为同一目标 pidfd 的 inode 标识，fstat 沿固定 Linux pidfs getattr 输出 0700 并去掉文件类型位。数据 read/write 及定位读写返回 EINVAL，lseek 返回 ESPIPE。

有 pidfd 的身份才分配共享观察源，按身份指针索引；PID 对象保持 64 字节。poll/epoll 在发起 CPU 查询状态和登记既有等待源；组长提前退出仍等待最后线程，使退出真正可等待后才发布可读，wait/reap 撤下 TID 后同时报告 HUP。reap、wait 消费及非组长 exec 在修改角色前取得原 TID 存储 owner；状态修改完成后，由同一执行者在返回前锁外通知，再归还该 owner。等待游标和观察源引用覆盖通知及最后关闭的交错，不增加 worker 或待通知协议。

waitid(P_PIDFD) 持有实际身份选择子进程，编号复用不改变目标。NONBLOCK pidfd 的子进程尚无所需事件时返回 EAGAIN；显式 WNOHANG 则成功返回空信息。open 和 getfd 在发起 CPU 执行；分配 FD 前读取调用者当前 NOFILE 软限制。getfd 按实际 TID 查找，检查 REALCREDS、dumpable 和 capability 子集，取得真实 OFD 后重新核对目标身份、MM、凭据、执行安全对象及文件表。任务存储借用覆盖目标查找；每任务的 files_access 短保护覆盖授权快照、真实 FD pin 和安全复核，FD 锁在调度域之外取得。退出先在调度域发布 proc_exiting，再在域外等待这份短保护结束，随后才允许原 files owner 释放表存储；临时读者不持有整表清理引用。接收 FD 设置 CLOEXEC，offset、状态标志和打开权利继续共享；失败时 OFD 清理仍由调用者既有 FD cleanup 路径保留 owner。无 epoll 登记的 pidfd 最后关闭在发起 CPU 完成，有登记时沿原控制路径处理通知。

发送默认采用 pidfd 的线程／线程组属性，支持单一的 THREAD、THREAD_GROUP、PROCESS_GROUP scope 标志及 SELF_THREAD/SELF_THREAD_GROUP 特殊值。可选 siginfo 沿共同导入和 pending 机制处理；目标在导入前解析并持有真实身份，不在用户缺页后按复用的 FD 或 PID 重新选目标。用户载荷来源、零信号探测及权限沿固定 Linux。pidfd 专用 namespace/info ioctl 和 CLONE_PIDFD 扩展另行接入。

sh tests/host/pidfd.sh 检查最后角色撤号、64 字节分配类、旧身份引用、通知早于登记／休眠，以及回调期间最后关闭与存储归还。真实 FD 表用例在授权与 pin 之间握手触发退出，检查表存储保留至短访问结束、NOFILE 错误顺序及失败 OFD owner；OFD 用例检查本地 poll、最后关闭及 epoll 通知路径。直接用户消费者为 tests/workloads/resource/pidfd.c，核对真实进程／线程退出、getfd、权限、信号和 proc 字段。
wait4的STOP/CONT报告也持临时资格，复制可以睡眠；释放资格后通知父等待队列，复制失败时事件仍可选。
退出队头仍有事件/等待/执行owner时，reaper返回BUSY；cleanup等待真实归还通知，不将待归还借用当作可立即回收的工作。
PID1收尾另等退休存储owner全部归还，最后释放通知已登记的cleanup；空索引不能代替页基线。
退休元数据的最后借用在普通可信调用栈上归还页；禁止抢占的回调范围只通知已有cleanup owner，raw持有者不得触发释放。
摘索引时pending即覆盖清理owner，retire只改变可释放资格；摘队列再将页owner交给collector，
实际锁外归还完成后，才在调度域内减少pending。
最后一次完成保留关机进展标记，cleanup登记前后都复查，避免通知先到而永久睡眠；
标记只要求重新检查，下一次stop_users扫描前清除，不表示live user已结束，也不绕过retry deadline。
2/4线程门禁握手覆盖页服务未完成及通知在检查、登记、park各窗口到达，检查注册与借用归零。
父通知的空队列或直接token通知在域内结束，实际跨解锁的
游标才固定父任务容器。发送信号给当前已固定的leader复用既有owner；不同leader
仍在同一保护下取得存储借用，GROUP_DEAD容器和真实成员遍历的寿命不变。
退出通知cleanup后再选择next；join在新栈完成点通知，避免既提前释放旧栈又丢失无timer进展。
`make test-sched-concurrency-host`涵盖2/4线程的选择、策略竞争、查找/退休与join/reaper争用；
`make test-sched-lifecycle-riscv test-sched-lifecycle-loongarch`在512MiB/1GiB验证真实换栈、
借用延后归还及trusted-stack资源基线。宿主模型不替代客户机多核。
固定依据仍为`references/linux/kernel/pid.c`、`kernel/fork.c`、`kernel/exit.c`和`fs/exec.c`，
commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e`；本实现保留临时PID引用对数字的owner。


每个用户执行线程有独立 TID、FP/整数寄存器、signal mask、线程 pending、clear-child-tid、robust-list 注册地址、restart 状态、私有元数据页和独立的连续物理内核栈。组长承载 TGID、进程组、父子树、组 pending、退出通知、已回卷记账及进程资源限制。每个用户任务持有一个含 Linux 16 项限额的 `kernel_resource_limits` 引用；组内线程共享记录，普通 fork 在调度域下复制取值，exec 和非组长 exec 保留该记录。最后任务存储引用回收时在锁外释放，内核 worker 不分配限额记录。双向成员环包含组长容器；组长停止执行后仍留在环中，直到组结束或非组长 exec 接管身份。

普通 fork 从调用线程复制 MM 的 COW 页表/VMA、fd 表、fs context 和 disposition；OFD 仍按现有语义共享。子进程挂在调用线程的组长父子树中，并记录创建者 TID。线程 clone 通过 MM/files/fs/disposition 引用共享已有对象，不复制页表或 fd 槽。首次需要共享 disposition 而父线程尚无表时，会按需分配表页。

同组 fd 变更立即可见。阻塞 read/write/writev 在睡眠期间持有 OFD 引用；即使其他线程 close/dup 替换槽位，也不能提前回收正在使用的端点。退出请求必须使调用栈完成清理，不能直接释放阻塞任务页。

## 会话与进程组

`setpgid/getpgid/getsid/setsid` 使用同一身份对象的 PGID/SID 角色。显式 `fork_no_exec` 与 `session_leader` 状态分别约束父进程改子组和会话组长，不能用 SID 数值等于 TGID 代替曾成功 setsid 的状态。父进程可在子 exec 前修改其组；成功 exec 后同会话返回 EACCES，跨会话先返回 EPERM。查询可指定非组长 TID，setpgid 指定非组长 TID 返回 EINVAL；setsid 从任一组内线程发起都作用于进程代表。普通 fork 继承组/会话但不继承会话组长标记，exec 保留该标记，非组长 exec 一并转移。

组长退出或被收割不销毁仍有 PGID/SID 成员的身份。zombie 在被收割前允许查询和符合条件的 setpgid；kill 对仍存在的 zombie 进程或组成功但不向已结束线程排队。kill 的正值按 TID 查找所属进程，零/负 PGID 按角色成员查找，wait4 的零/负 PGID 按当前真实组关系选择子进程；proc stat/status 从对象获取相同组/会话信息。

进程退出与跨进程 reparent 检查最后一个同会话、不同组的父关系是否消失；忽略 init 父关系和真正结束的成员。新孤儿组含已完成 group stop 的任务时，全组依次收到内核 SIGHUP、SIGCONT（SI_KERNEL），恢复停止任务。仅有 TID 的线程回收不触发进程级孤儿事件。固定 Linux `kernel/sys.c` 的 setpgid/setsid 只改角色，不主动调用孤儿 HUP/CONT 检查；`kernel/exit.c` 的调用点是 reparent_leader 和 exit_notify，本实现保持这一范围。已孤儿组的默认 TSTP/TTIN/TTOU 丢弃，SIGSTOP 仍停止；[Serial TTY](kernel-tty.md) 通过稳定 SID/前台 PGID 引用接入 ctty、终端组信号与线程组生命周期；用户发送与特权控制按调用者当前凭据检查。

`tests/diff-abi/session.c` 使用 pipe/wait4 握手与专用 exec probe，覆盖父子 exec errno、线程目标、非组长 setsid/exec、session 边界、zombie/proc、组定向 kill/wait、进程组长先收割后的组存续、退出/reparent 两条孤儿路径，以及 sigtimedwait 和 SA_SIGINFO 的内核信号来源。启动上下文不同：固定 Linux 裸 PID 1 初始 PGID/SID 为 0，BoarOS 初始身份为 1；测试先建立真实非零会话，再比较用户操作，不把启动整数差异混入会话机制断言。

## clone 与 vfork

通用进程策略由构建期 `arch_process_*` 后端准备寄存器。LA64的系统调用顺序为
flags、child_stack、parent_tid、child_tid、tls，由LA trap重排成下述通用顺序；
SETTLS写r2/TP、child stack写r3/SP、返回a0=0并使ERA前进4字节。LA已保存独立FP/SIMD owner，区分启用宽度、live宽度和used_math，其首用、信号和成本边界见[LA浮点](loongarch-fpu.md)，不套用RV的FS Dirty或SIMD范围。真实LP64S musl线程/TLS/取消/非PI robust与
组生命周期由 `make test-pthread-loongarch` 双侧验证，两处构造OOM和重试由
`make test-pthread-oom-loongarch` 验证；任务页/栈及根盘owner要求恢复基线。

RISC-V clone 接收 flags、child_stack、parent_tid、tls、child_tid 和完整 syscall 入口寄存器。支持普通 SIGCHLD fork/vfork（可指定CLONE_FS共享cwd/root、CHILD_SETTID/CHILD_CLEARTID管理子进程TID，或CLONE_SYSVSEM共享独立撤销组），以及共享VM/FS/FILES/SIGHAND/THREAD的线程组合和SETTLS/PARENT_SETTID/CHILD_SETTID/CHILD_CLEARTID/SYSVSEM/DETACHED标志。CLONE_SYSVSEM具有实际引用与最后共享者退出补偿，机制见[SysV信号量](sysv-sem.md)。非法依赖及NEWIPC与SYSVSEM冲突返回EINVAL，其他尚未支持的合法资源组合返回ENOTSUP。

子线程继承完整 FP、整数现场和 mask；a0 为 0，PC 越过 ecall，非零 child_stack 替换 sp，SETTLS 设置 tp。CHILD_SETTID 在子任务首次返回用户态前、子 MM 激活后写入，因此可正常处理 COW 和缺页；PARENT_SETTID 仍在父任务发布 clone 时写入。SETTID 用户存储失败不回滚已经创建的任务，与固定 Linux clone 路径一致。所有内核分配失败都在发布前回滚；若回滚触及真实 VFS/block I/O owner，则由所属文件或 mount 记录，物理页和堆释放不另建重试状态；不会发布半构造子进程。

vfork 共享 MM，复制 files；fs 默认复制，显式 CLONE_FS 时共享。父线程和具体子进程保持双向完成关联；子进程释放共享 MM 引用后一次性完成等待。普通信号不解除等待。组退出/exec 的致命取消先断开双向关联，再唤醒父线程；子进程自己的 MM 引用仍有效，之后完成也不能访问已释放父任务。

跨核软件事件发布与硬件IPI发送分开：发送者以release原子操作合入事件位，
同类事件已经待处理时复用现有通知；接收者以原子交换取走事件后，新发布重新请求IPI。
发送者关闭本地中断，实际IPI由当前raw保护退出或既有通知批次结束时发出，
因此合并保留已经排队的发送者或接收者负责推进，不改变任务READY与执行者交接。
RV与LA使用同一事件位发布原语，各自保留硬件确认及IPI发送。

## 等待与 futex

所有 BLOCKED 任务在全局双向 blocked 链上，并可加入一个等待队列 FIFO。队列 wake-one 取队首，wake-all 唤醒全部；摘除 blocked 和 queue 成员均为 O(1)。deadline 使用原始 time ticks，0 表示无限；带期限任务同时进入以 (deadline, tid) 为键的有序索引，tick 只弹出已到期者，全部唤醒路径统一摘除索引项。

MM 一次准备至多两个 key 和一个用户字 pin。private key 使用不复用的 MM 身份号与四字节对齐地址，不取得映射编辑 gate；WAKE 和不读字的 REQUEUE 不要求地址仍有映射。private WAIT、CMP_REQUEUE 和 WAKE_OP 沿共同用户 pin 协议，在 metadata raw 内检查当前 PTE、访问许可并取得实际页引用；新许可关闭时先等待开放，缺页或 COW 先调用 resolver，再重新取得 pin。共享 key 需要 operation gate 固定整组映射视图，再由 metadata raw 检查当前 PTE 并取得页许可；所有缺页和 COW 先在没有部分 pin、key 后备引用的状态完成，再重新解析整组地址。共享匿名和 SysV key 使用内存对象与对象内字节偏移，共享文件 key 使用按文件系统实例和 inode 唯一化的活 VFS node 与文件内字节偏移。非 private 操作按实际 PTE 检查身份解析资格：驻留的纯执行叶可用于 WAKE，PROT_NONE 不可；需要读字或写字的命令另需相应许可，未驻留页沿缺页权限检查。等待者持有后备引用直到原等待调用恢复，文件描述符关闭或映射撤销不提前结束它。

等待键、bitset及消费状态归每个futex等待项，普通WAIT使用栈上单项；任务仅保存当前请求的清理owner。请求及后备引用在token结束、节点借用排空后归还，强制退出沿共同I/O清理在释放旧栈前完成。此后请求由原执行者或独占退出清理者拥有，清空任务请求句柄直接由该owner完成。现有futex/PI/信号量请求字段占用原单地址futex槽位，后续信号、文件和MM热字段偏移保持原值。

每个哈希桶有可睡眠操作 mutex，双地址操作按桶索引递增取得，同桶只取得一次。桶表在 boot CPU 初始化 wait_domain 后、任务和 secondary 发布前一次初始化。每次操作只解析一组桶指针，比较、登记和扫描直接使用这些指针；锁内不再进入 MM 准备或缺页。WAIT 在桶锁内比较已 pin 的用户字，并在等待短保护内直接登记 token；任务、桶和栈token已由该调用取得，无需再次经过公共登记包装的参数检查。通知因此不会插入比较与登记之间；随后释放桶锁、归还 pin，再 park。token 登记后不再等待其他 mutex。WAKE、比较后 requeue 和 WAKE_OP 的原子更新与唤醒也受相应桶锁保护。桶碰撞仍按完整 key 匹配。WAKE 在至多 16 个登记的短链上，只取得一次 wait_domain，完成 key/bitset 匹配和通知；队列只包含任务节点，节点使用始终在保护内，因而无需锁外游标借用。真实WAKE在同一短保护内标记并摘除相应等待项，扫描先保存后继。等待项的消费独立于任务token通知：超时或信号已通知但调用者尚未出队时，真实WAKE仍计数并优先完成该项。长链与 requeue 沿借用游标线性前进，每个节点只短暂持有 wait_domain。requeue 移动前以 task 借用覆盖游标归还，在同一 wait_domain 内发布 key 与节点位置，已经出队的项不再迁移；仅收到超时/信号通知且仍排队的项保留迁移与真实唤醒资格。所有可能成为最后一个的后备引用都在桶锁和用户 pin 之外释放。

SMP下，WAKE/WAKE_OP及退出唤醒在取得全部桶资格后，借用当前调用栈上的通知批次。READY和目标CPU事件位立即发布；实际IPI目标暂存于该批次，全部桶锁归还后才合并发送，避免发送者在持桶期间等待固件。批次保存并恢复原IRQ及抢占深度，期间保持CPU固定且不允许阻塞；撤销CPU上的栈指针后再发送。原始raw释放路径不增加分支，UP编译为空操作。共享key最终释放与用户pin归还在批次结束后执行。

REQUEUE 返回唤醒数与迁移数之和，CMP_REQUEUE 先比较源字，不相等返回 EAGAIN 并保留原队列；同 key 也允许 requeue。同桶迁移保持原链上位置，跨桶追加到目标 FIFO。WAIT_BITSET 为等待者保存非零掩码；WAKE_BITSET 只唤醒掩码相交者，普通 WAKE 匹配全部，requeue 保留原掩码。WAKE_OP 对第二个用户字执行原生 32 位 SET、ADD、OR、ANDN 或 XOR，以更新前的有符号值执行 EQ、NE、LT、LE、GT、GE 比较，条件成立时追加唤醒第二组 waiter；12 位操作数和比较数先符号扩展，SHIFT 操作按 Linux 的低五位移位规则处理。未知比较返回 ENOSYS，但保留已完成的原子更新。第四参数的计数按低 32 位解释；REQUEUE/CMP_REQUEUE 拒绝负计数，legacy WAKE/WAKE_OP 的非正计数仍可唤醒一个匹配者。错误与副作用顺序依据固定 Linux futex 实现。

值不匹配为 EAGAIN，非法地址或权限为 EFAULT，准备时缺页分配耗尽为 ENOMEM，零掩码及负 requeue 计数等非法参数为 EINVAL，超时为 ETIMEDOUT。

WAIT 的超时为相对 monotonic；WAIT_BITSET 的超时为绝对 monotonic 或带 `FUTEX_CLOCK_REALTIME` 的绝对 realtime。当前没有 `clock_settime`/`settimeofday` 写入口，启动后的 realtime offset 不变，故可一次转换为 monotonic deadline；将来加入可调时钟时必须重新审查阻塞中的实时截止时刻。无超时等待的 signal 唤醒走 generic restart：用户 handler 没有 SA_RESTART 时返回 EINTR，带 SA_RESTART 时 sigreturn 后重新执行并再次比较用户字。带超时等待使用 tagged restart state 保存用户地址、expected、operation、掩码和首次调用的 monotonic absolute deadline；用户 handler 无论 flags 均看到 EINTR，没有 handler 的 stop/continue 路径经 restart_syscall 继续剩余 deadline。未支持命令为 ENOSYS。用户访问层状态损坏不能转换成普通用户错误。

`set_robust_list` 只核对 RV64 链头长度 24 字节并保存线程私有地址，允许空指针注销，不在注册时预读用户链。`get_robust_list` 支持当前线程及存活目标 TID；跨组查询按 Linux READ_REALCREDS 检查 UID/GID、capability 和目标执行安全状态的 dumpable，输出先写长度再写指针。普通 clone/fork 不继承注册，失败 exec 保留，成功 exec 清理旧链并重置。链头与节点是可变用户内存，退出时才有界读取，不作为内核对象持有引用。

退出清理在原 MM 与原 TID 有效时同步完成，早于 clear-child-tid 和资源释放。成功的非组长 exec 在身份接管前保存旧 TID，切换到新页表前、旧 MM 仍激活时清理；可返回的 exec 失败不清理旧链。遍历最多 2048 项，下一链接先于字更新读取；pending 项不会因已在链上而处理两次。owner 等于退出 TID 时通过可处理缺页/COW 的 32 位原子比较交换保留 WAITERS 并置 OWNER_DIED，必要时唤醒一个 waiter；pending 的非 owner 解锁窗口按 Linux 规则补唤醒。坏地址、错位、超长链和内存不足只结束该次尽力清理，不把用户错误升级为内核 fatal 或保留无 owner 的重试状态。

带 `FUTEX_PRIVATE_FLAG` 的操作仍只在同一 MM 内匹配；未置该标记的共享匿名、SysV 和共享文件映射使用上述后备 key，可跨 MM、不同 VA、独立打开和硬链接唤醒或 requeue。私有映射仍使用 MM key。PI 标记项更新 OWNER_DIED，实际 PI owner 随后由其持锁列表交接；futex2 尚未接入。futex 入口在调用者 CPU 执行，文件或缺页后端需要的 CPU0 业务交接由对应 MM/文件层负责。

### 多地址等待

`futex_waitv(449)`导入1～128项24字节描述符，每项是32位期望值、地址、PRIVATE/shared标志及零reserved。返回实际被唤醒项的原始索引；重复地址保留独立排队与WAKE计数，普通REQUEUE移动各项而保留索引。NUMA/MPOL扩展为EOPNOTSUPP，其他非法字段为EINVAL。期限是MONOTONIC或REALTIME绝对时间，没有timeout时忽略clockid。

数组和请求归本次调用，任务只保留强制退出清理owner。先取得所有key，再比较任一值；因此后项key错误先于前项值不符。登记阶段以非阻塞MM资格重查key与实际pin，逐项比较并绑定同一token。一次只持有一个页pin和一个桶；缺页、pin许可关闭或桶竞争时，放桶、归还pin并结束所有登记后再等待或装页。全部节点借用排空后读取实际唤醒项，它优先于已经通知的超时/信号；没有真实唤醒时按原绝对期限及重启规则处理。私人WAKE和普通单项等待沿各自原路径执行。

### PI mutex

`WAIT_REQUEUE_PI(11)`将等待者登记在条件字，`CMP_REQUEUE_PI(12)`比较源值并把它交给指定PI锁；源和目标使用不同实际key，nr_wake固定为1。成功返回时等待线程已取得目标锁。源队列按登记时基础实时优先级、同优先级FIFO排列，目标队列沿现有继承优先级规则处理。普通WAKE或普通REQUEUE遇到PI条件等待项返回EINVAL，已完成的前缀保留，旧source引用仍按实际迁移数归还。

源等待、目标PI等待、已授予与完成由同一请求保存，复用现有token和owner引用。源阶段信号走NOINTR重启；目标阶段尚未获锁时返回EAGAIN；已授予优先完成用户字修复。共享目标key先取得写访问；私有源比较成功后，即使空队列也检查目标可读，真正代理取得才要求可写。准备缺页或等待时先释放桶和所有pin，重取后复核。退出与已授予请求的致命取消取得目标桶资格再复核owner，避免在重排发布中途摘除仍需接纳后续等待者的状态。

RV/LA 的 futex(98) 接受 LOCK_PI、TRYLOCK_PI、UNLOCK_PI 和 LOCK_PI2。未竞争时只原子修改用户字，不常驻分配锁对象，保留 libc 的用户态快速解锁。竞争时，在原桶内发布按真实 key 标识的状态；每个在途调用持有状态引用，状态持有 owner 的任务存储借用及共享 key 的后备引用。调用栈保存等待请求；用户页 pin 只覆盖比较／更新，睡眠前归还，持有者退出后的用户字修复重新取得 pin。

等待者按实际优先级、同优先级的登记顺序排列，链式传播在调度保护内更新 owner 的实际优先级和队列位置。实际优先级和基础值未变时直接结束传播，不重复采样CPU记账；FIFO/RR等策略变更仍在原策略下结算。基础调度策略、权限和查询值保持独立；fork 继承基础值。等待者撤销、优先级更新、解锁和退出都重新计算受影响链；新等待操作按固定 Linux 的 1024 层检查上限排除环；逐步追加可以形成更长的既有无环链，优先级更新沿实际链完整传播，成本为链长。内部可睡眠锁仍按原 FIFO 契约运行，PI 作用于这里的用户互斥锁。

正常解锁先原子发布下一个 TID，再转交内核 owner、通知等待者。IPI沿普通futex的通知批次，在桶锁释放后发送；请求注销与其状态引用归还在同一调度保护内发布，未发布候选由原调用直接释放。SMP队首若观察到owner正在远端CPU执行，可在释放桶及页pin后等待最多50µs，并受原deadline和本核调度请求约束；锁外只读取本请求的原子grant，不继续访问旧owner。轮询不改变优先级/FIFO选择，真正交接仍由原handoff和等待token完成；未取得交接再按原路径睡眠。超时／信号先通知原 token，等待线程与解锁方在同一保护内竞争最终移除／授予；已授予时返回取得锁。LOCK_PI 使用绝对 REALTIME，LOCK_PI2 默认绝对 MONOTONIC、可带 REALTIME 标志；信号处理后按原绝对期限无条件重启，不依赖 SA_RESTART。

退出与 exec 在旧 MM 释放前发布清理状态，先走 robust 用户链，再依次交接真实 PI owner；新的竞争者通过现有组等待源等待清理完成。退出持有者交接时，获准线程修复 OWNER_DIED 和自己的 TID；地址失效返回 EFAULT；只要还有等待者，仍存活的获准线程继续是内核 owner，随后通过实际退出流程交接。所有最后引用和堆存储在短保护外释放。普通 WAKE／REQUEUE 源遇到活动 PI 状态返回 EINVAL；PI 接纳也检查同 key 的普通等待者。

公共`kernel_wait_prepare`以256字节对齐，最终RV布局检查要求它完整位于一个代码页；这个124字节路径曾因跨页使每次调用多执行一个QEMU翻译块。实际调用者直接使用已持有保护的接口时沿`kernel_wait_prepare_locked`。

`make test-pi-priority-host` 检查 4/16 KiB 下实际队列、CPU0 服务资格、RT 节流及链式传播；`tests/workloads/futex/pi.c` 检查原始字、libc robust mutex、超时／信号、优先级倒置、环、共享别名及退出修复故障。`tests/network_runner.py --workload pi` 复用两架构 Linux 对照，`--libc glibc` 使用现有目标静态工具链。

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

zombie 先逻辑回收再复制 status/rusage，因此坏输出指针的 EFAULT 不让它再次可 wait。退出和 reparent 可能向退出队尾追加 orphan zombie，摘头必须使用更新后的链关系，不能丢失新增 owner。`times()` 只遍历目标组环，累计所有仍在环中的线程及已回卷成员，不扫描无关任务。组代次变化导致游标重走时先清除旧累计；子进程时间保存在组长，不重复计数。

## 同步、成本与验证边界

短调度域保护组与身份元数据。FD 槽和 OFD 引用使用共同的 raw/原子引用规则；MM 的修改、pin 和远端 TLB 确认由其 operation gate、metadata raw 与退休批次负责。futex 身份解析取得实际后备引用，等待队列沿本模块的代次协议处理；这些业务对象不依赖整次系统调用关闭中断。尚未并行化的命名空间、设备和协议后端仍在各自 CPU0 业务边界执行，具体见[文件资源](kernel-files.md)、[MM](kernel-mm.md)及[网络模块](kernel-network.md)。

RV每线程拥有独立的 4 KiB 元数据页和 8 KiB 连续物理内核栈（buddy order 1）；调度器初始化要求分配器已进入 finalized buddy 模式，栈分配、构造回滚与正常释放使用同一 order。RV生产内核把栈映射到内核栈窗口（`RISCV_KERNEL_STACK_WINDOW_BASE` 起 128 MiB，12 KiB 槽：前置 4 KiB 页不建 PTE，作为未映射 guard，后接 8 KiB 栈）；空窗口骨架在页表构建期预留，所有用户根按值继承同一子树，叶子由受限的运行期接口插入/删除，空 level-0 表在最后一个叶撤除时释放，guard 页不占物理页。无活动内核页表的测试 fixture 继续使用 direct-map 栈。栈底留 16 字节对齐区和 canary，剩余 8176 字节包含 Trap Frame 与 C 调用链。新栈填充固定字节，初始用户 Trap Frame 显式清零。退出后只在其他可信栈扫描未覆盖前缀；累计最小剩余空间和最大已用空间由只读统计接口提供，生产 PID 1 完成时报告。构造回滚同样释放独立栈，不让资源清理失败保留它。

栈窗口优先选择首栈页与物理栈页号的低位一致的空槽，范围为一张末级页表的页数
（RV为512，LA为2048）；同类槽用尽后沿位图查找任意空槽，保留完整容量。
这样分散栈别名，减少实测QEMU中热栈与ext4对象的soft-TLB槽冲突；这项测量对应
软件地址转换，真机收益尚未测量。RV的早期任务可能分布到三张L0表，相比原紧密
布局最多多占两页，最后叶撤除后按原规则归还；LA沿用预建表。

页/栈初始化只写已独占、对齐的私有存储，完整初始化后才发布。可信栈回收时用整字
跳过全为填充值的前缀，再逐字节定位首处差异；不跨canary或高边界，余量统计仍精确
到字节。host在4/16KiB下遍历全部前缀边界并检查坏canary拒绝回收，真实guard另测。

LA生产任务也沿现有内核栈窗口接口运行：PGDH共享骨架持有永久boot页，任务叶
仅借用原连续物理栈，PLV0/RW/NX且下方16KiB未映射。异常入口在写frame前
核对SP是否容得下304字节；不足时切到保留的可信异常栈输出fatal。合法栈上界
按SP下方字节定位槽位。用户PGDL切换不改PGDH，回收先撤叶/TLB同步再归还栈。
该硬件保护和映射失败/骨架OOM回滚由`test-stack-guard-loongarch`验证。

`make test-stack-usage` 强制重建隔离的生产对象，编译器 `-fstack-usage` 产出逐函数记录；host probe 从实际栈配置和 Trap Frame 头计算容量、guard、汇编 Frame 与余量预算，避免测试大栈或旧报告污染门禁。`tests/stack-usage.py` 拒绝超出“栈容量减 16 字节、288 字节汇编 Trap Frame、1024 字节余量”的单帧及无界动态栈；该检查不能证明完整调用链。真实 root-init、静态 musl 与动态 pthread 测试另要求已退出任务的最小实测余量至少 1024 字节，不足时必须扩大栈后重新运行。Canary 用于发现破坏，填充测量用于观察高水位；生产栈的未映射 guard 只对经窗口 VA 的 SP 式越界生效，direct-map 别名仍可达同一批物理页，且二者都不证明未执行分支的栈界。idle/boot 栈仍在 .bss 高半区别名内，没有 guard。ASID 0 的切换刷新成本、OTHER 的 100 Hz tick 与线性 wait4 仍存在。

futex 的直接宿主入口 `make test-futex-host` 运行真实 MM、park、桶锁和原子操作，覆盖 4/16 KiB pin、MM gate 被另一执行者持有时 private 操作继续完成、关闭新许可后的等待与撤权、冷页和 COW、同桶/跨桶与同 key requeue、六种有符号比较、bitset、短链单次调度锁通知、同桶碰撞与 17 节点游标续批、PREPARED/COMMITTED 通知及 owner 归还；`tests/workloads/futex/caller.c` 可在 Linux 与两种架构上运行相同用户探针。

聚焦入口为 `make test-stack-usage`、`make test-scheduler-cases-riscv`、`make test-scheduler-riscv`、`make test-files-riscv` 和 `make test-signal-riscv`；`make test-userland-riscv` 验证真实 pthread、共享匿名 futex、bitset 绝对 realtime 等待在 stop/continue 后保留掩码和截止时刻。`make test-diff-abi-riscv` 用同一 ELF 对照固定 Linux 的零掩码、超时、错误、按掩码唤醒和 requeue，以及共享文件别名、独立打开、硬链接、偏移隔离、SysV 和共享 robust 退出；`make test-glibc-riscv` 验证 glibc 2.44 的 `pthread_join` 消费路径。阶段收口使用 `make test-riscv`。各次实际通过范围以 README 和提交验证说明为准，不把实现路径存在等同于全部线程负载已验证。

RV普通用户任务、共享匿名/文件MM及ext4文件操作支持真实1～4核，执行位置和后端并发范围见[多核模块](riscv-smp.md)。实时信号队列和sigaltstack已提供；PI条件重排与futex_waitv已接入；NUMA/MPOL等futex2扩展及clone3继续后续安排。LoongArch默认单核，`LA_SMP=1`支持QEMU virt上的真实1/2核执行，启动/IPI/TLB与停止规则见[LA模块](loongarch-boot.md#双核执行)；已覆盖整数context、timer抢占、信号、musl TLS/pthread、动态musl/DSO TLS、标量FPU与LSX/LASX。固定语义依据见学习总结的Linux commit与musl归档。

活动普通文件/TCP I/O 的单页暂存由任务持有并跨调用复用：首次使用时分配，调用期间登记在任务的 `io_buffer`，正常调用完成只解除登记。正常回收与构造失败共用栈上请求清理：先结束 epoll、socket read/write reservation 和 TTY 请求，再解除暂存登记，随后才释放 MM/文件表和任务栈；常驻页在任务最终存储释放时归还。页释放错误遵循物理分配器 fatal 不变量，不进入历史 cleanup 重试链。

`make test-sched-policy-host` 的系统调用夹具链接生产 affinity 实现，检查有效掩码、空交集、PID、长度与用户地址错误；该既有目标纳入宿主 CI。

读写锁按 FIFO 交接，读者每 16 项释放 metadata raw，并在分段之间恢复调用者原本的 IRQ 状态；再次取得短保护后继续原来的边界。`handoff_owner` 保留交接资格，新读者不能越过已排队的 writer。调用者进入时已关闭 IRQ，则交接不会自行开启。FIFO 交接当前不实现优先级继承，RT 带宽预算也不传递锁依赖。

## 存储等待与清理任务

任务的 I/O 上下文随当前执行栈保存。RV/LA 从内核 TP 取得实际任务；调度器可以在切栈前将 `cpu.current` 发布为下一任务，此时 I/O owner 仍属于旧栈。RV 用编译器的固定 TP 寄存器变量表达同一状态，切换和陷入汇编沿原规则保存／恢复；CPU 字段仍在需要时从当前任务读取。热点直接按共同任务布局取得嵌入字段，普通外部入口与宿主替身保留相同行为。

`kernel/sync.h` 的 mutex/RWlock guard 记录任务 io_context owner 与锁序，非法释放、递归误用、逆序及读转写触发 fatal。争用者按 FIFO 排队；释放时先给队首写者预留独占资格，或给连续队首读者预留共享资格，再定向唤醒。预留者尚未运行时也阻止新到任务抢占；排队写者后的读者及 try_read 不能越过队列。等待不可中断，信号退出在取得与释放后处理，栈上的等待记录不被提前销毁。只有队列/引用/状态发布使用短关中断区，持锁跨设备等待允许其他任务运行。分配器回收深度及 lwext4 重入深度属于任务上下文。

外部IRQ的公共trap返回在S-mode仅允许空闲任务、且未持I/O锁时消费need_resched，防止中断在开中断与WFI之间已处理却继续入睡。这条限制不适用于既有timer调度：开启SIE的普通内核线程仍可在tick切换，持有I/O锁不等于禁止抢占。syscall进入时SIE关闭，但显式block/yield仍能切换；共享元数据必须有独立临界区和睡眠前引用，不能以单hart或持锁作为整段不可切换的证明。buddy/slab已补对应保护，见[物理页模块](physical-pages.md)。`test-scheduler-cases-riscv` 不借助 timer 验证 idle 返回调度，并验证读写 FIFO、后到者不抢锁及 1/8/32 等待者；原实现分别出现一次 idle 失败和两次交接顺序失败。成本锁持有时间包含资格已交接但获得者尚未运行的区间。

存储等待以 `interruptible=0` 登记：pending 信号与组退出不能拆除 DMA owner；设备完成或 reset 后原调用栈先释放资源，再在用户返回边界处理退出。指定的 cleanup task 排空已退出任务和 root-boot 收尾，阻塞时正常调度；idle/IRQ 不进入运行期可睡眠存储。无块设备的纯模块 fixture 可继续由 idle 回收不含存储的任务。清理结束检查任务锁/backend 状态均为空。

`make test-scheduler-cases-riscv test-scheduler-riscv test-io-sleep-riscv test-userland-riscv` 保护 FIFO 资格交接、设备等待/唤醒与真实任务组合行为。

`make test-diff-abi-riscv` 的 `tid.*` 使用同一 ELF 对照固定 Linux，覆盖私有 fork、共享页但独立 MM、vfork 退出/成功与失败 exec、坏地址/只读地址、线程 futex 等待。成功 exec 与退出共用旧 MM 的注销和清 TID 顺序；延迟资源清理不增加活跃使用者数。

## 系统统计与可等待内核任务

调度器独立登记所有已发布任务，逻辑退休时摘表；构造失败不发布。退休后仅因存储借用保留的页不再代表可见任务，未被wait/reap的zombie仍在表中。`kernel_scheduler_system_statistics()` 返回真实任务数（含尚未回收的 zombie，16 位饱和）和 16 位小数的负载。每 501 个 100 Hz tick 按固定 Linux 的 1884/2014/2037（11 位小数）更新 1/5/15 分钟指数平均。READY/RUNNING 与不可中断 BLOCKED 计入活动量；空闲 worker/cleanup 的工作队列等待标为 interruptible，不凭空形成空闲负载，实际存储与压力等待仍计入。

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

等待/就绪发布和对象内部资格使用 raw 互斥；普通对象与 CPU0 后端的执行范围见 [RV 两核模块](riscv-smp.md)。四种职责必须分开：

| 机制 | 保护与限制 | 当前状态 |
|---|---|---|
| 本地IRQ控制 | 保存/恢复本CPU中断；单CPU短元数据发布，允许显式切换 | arch IRQ与KERNEL_IRQ_SCOPE已有 |
| 抢占/迁移控制 | CPU本地指针借用和current稳定，不提供共享对象互斥 | cpu.h的嵌套深度；timer延后切换，既有安全点消费请求 |
| 跨核raw短锁 | 共享状态和内存序；不得持锁阻塞、I/O或调用会等待的分配路径 | raw_lock.h的32位acquire/release原子字、CPU guard链与rank/key序 |
| 可睡眠对象锁 | 任务guard、rank/key、资格交接和对象owner跨睡眠存活 | 内部raw30→调度raw40；业务数据仍由guard或原单CPU纪律负责 |

内核tp仍指向任务，架构前缀之后的CPU关联供当前CPU查询；current、need_resched、idle归属/恢复状态、退出丢弃上下文、运行结算及timer期限由CPU记录持有；RR剩余片跟随任务。
启动任务与idle显式绑定，RV采用固件hart ID并在高地址重定位后重绑，LA读取CPUID CSR。
bootstrap I/O上下文与任务I/O上下文分开，任务guard和回收深度不迁入CPU。raw guard必须按LIFO由同CPU释放，
递归、逆序、错误owner和计数损坏fatal；block/yield/退出及可睡眠锁获取拒绝禁止抢占上下文。
`make test-sync-host`验证真实四线程争用、IRQ恢复、显式preempt在raw内的合法嵌套及fatal反例。
延期tick同时保存OTHER轮转原因，安全点消费；普通唤醒不自动变成同级轮转，切换后不把旧原因交给新任务。
RV scheduler cases分别验证idle和两个同级OTHER任务的延期请求，恢复后peer必须进展并归还全部owner。
`test-sync-riscv/test-sync-loongarch`在512M/1G运行真实原子/IRQ正例，以及raw内block/yield/退出/睡眠锁的fatal。
runner从Make接收实际构建目录，COST入口不能借用普通产物；host保护目录覆盖和缺失输入不回退。
这些原语门禁不替代调度/索引组合以及共享MM/设备业务的客户机多核验收。

`kernel_rwlock`的栈waiter由阻塞调用持有；授予资格先于wake，新任务不得抢走已授予
资格。持有guard期间锁对象不能移动/销毁；现有等待不可中断。未来取消/超时要明确
撤销注册与未消费资格的owner，不能先释放调用栈再摘队。raw锁只可保护短队列修改，
释放后再park；不能把当前整个IRQ-off acquire机械包进raw spinlock。

登记、通知、阻塞和切换完成由本模块的短调度域保护；条件及task/callback/context的
业务owner仍由调用者保持。对象 mutex、raw 与实际引用分别覆盖条件、发布和存储寿命，
等待原语不能代替这些业务保护。当前销毁入口与借用规则见本文开头。

主要等待入口的保护与 owner 如下：

| 调用位置 | 切换/工作 | 睡眠期间的owner |
|---|---|---|
| kernel/sched/sync.c acquire | 对象raw内准备token，锁外park | 调用栈waiter、任务io_context及锁对象 |
| kernel/sched/park.c prepare/park | 仲裁登记与BLOCKED发布，锁外switch | task元数据/栈；queue资源由调用者保持 |
| kernel/physical_page.c allocate_order | EMPTY后回收/pressure_wait | 不持buddy半成品；回调固定cache group，深度在任务context |
| fs/page_cache.c lookup/get/readahead/writeback/pressure | loading、后台work、代次进展等待 | entry users、cache/runtime或group pin，停止先join |
| drivers/virtio/block.c | 等descriptor/span/request完成与reset | request token及业务buffer；DMA停止未确认不释放 |
| net/ethernet.c worker；net/socket.c协议入口 | 协议mutex内开放IRQ执行；短IRQ区登记/复查，归还协议资格后park/finish | root借设备，worker持网络；TX/RX loan直到归还/停DMA |
| mm/mm.c文件fault | 源I/O可显式睡眠，恢复后重查 | 来源操作pin、VMA代次和候选页，失败准确回收 |

等待交接须覆盖wake发生于检查前、登记后、释放保护后/park前；timeout和cancel
与grant竞争；destroy与callback遍历；拒绝raw锁下阻塞；每种交错不能漏wake、双授予、
重复运行或使用已销毁队列。原单CPU测试只作为退化配置，不能替代真实多CPU内存序。
固定依据为本地references/linux/Documentation/locking/locktypes.rst，commit
f4cdf7ca9a1fdcca413157df19753f388a5a224e；lwIP raw串行边界见references/lwip/doc/doxygen/main_page.h（pitfalls/multithreading），
commit77dcd25a72509eb83f72b033d219b1d40cd8eb95。实现依赖只在[路线图](../goals.md#p6b-锁等待与中断规则)维护。

受控匿名及tmpfs映射 MM 的创建/切换与最终释放见[两核模块](riscv-smp.md#私有匿名-mm-验证)。实际地址空间切换在 rank40 外执行 `kernel_mm_activate()`；热路径静态门禁允许这项登记/本地失效入口，但继续拒绝 MM 分配、引用修改、pin/fault、文件和 exec 生命周期操作进入 switch 路径。
