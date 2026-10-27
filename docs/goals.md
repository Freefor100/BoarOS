# 开发路线与未完成工作

本文只维护长期方向、当前缺口和依赖，不保存逐轮执行计划、已完成复选框或提交日志。
当前能力见 [README](../README.md)，接口与验证入口见[模块导航](README.md)，测量和验收证据见各 learning。
固定资料身份由 `references/sources.tsv` 管理；评审意见需经源码与验证核实后才成为事实。

## 近期方向

架构主线是SMP正确性；受控RV映射域已包含匿名、私有tmpfs及在发起CPU冷装入的ext4。真实RV用户可在1～4核执行，按业务入口交接仍需CPU0的操作，229项原程序及有限压力/生命周期组合已有本地两核与单核退化验证；ext4 冷读已具备 inode/mount 读资格、共享缓存短锁及块完成保护；已打开ext4文件的空间修改和同步准备也在发起CPU执行；目录与路径元数据也已在发起 CPU 执行，共享路径和 FS 上下文由跨核保护维护。实际启动多个 CPU 的可写 journal ext4 挂载使用逐目录资格，不同目录和文件的私有修改可同时准备，rename 仍由挂载拓扑互斥协调；四核短例覆盖线程/进程、共享 MM、信号/CPU 定时器和 ext4 并发；socket 收发的预约、用户复制与等待已在发起 CPU 执行，lwIP 与控制操作保留 CPU0；普通用户管道、poll/pselect和epoll等待也在发起CPU执行，eventfd和MONOTONIC/REALTIME timerfd已接入调用者读写、就绪查询和等待，signalfd读取当前调用线程的待决队列；futex的WAIT/WAKE、比较重排与复合唤醒在发起CPU执行，等待键与用户字由同一次MM准备取得；PI mutex 的锁定、解锁、超时、链式优先级继承、robust退出、PI条件重排及128项futex_waitv已接入，LA在QEMU virt/LA464上已接入真实1/2核用户执行，覆盖独立每核启动、IPI、timer、远程TLB确认、整数/FPU/SIMD状态、共享MM和代表性文件/等待消费者；默认仍单核，四核与实板另列后续。
维持现有调度策略、全局运行队列和串行 lwIP 核心，先证明并发正确，再依据锁等待与 CPU 分布决定并行优化。
默认生产仍为单 CPU；普通与受控对象共用保护与引用规则，普通文件缓存读写、用户复制与匿名缺页在发起 CPU 执行。ext4 冷读在发起 CPU 解析映射并提交块请求；其他磁盘后端冷读与后端释放复用既有 CPU0 worker，范围写回按窗口续批，fsync捕获固定持久化目标后由原调用者等待，完成者在共享短保护下处理错误观察并直接通知结果。系统分配器通知和干净页回收可由远端调用，脏数据仍由 worker 写回。网络端点队列使用跨核短锁，普通 socket 收发在调用 CPU 执行；lwIP、控制操作与最后销毁仍在 CPU0。epoll 远端通知有独立状态保护，控制和最终销毁保持CPU0，等待在发起CPU。

| 主线 | 尚缺能力 | 完成口径 |
|---|---|---|
| 共享 MM 与子系统 | 同MM的VMA编辑、文件/ELF装页仍使用独占资格，共享MM混合映射/锁页循环的等待交接与多核负加速；非ext4后端、设备/DMA与退出的未覆盖消费者组合 | 复制与撤映射、降权、截断、I/O/close/退出的强制交错正确；确认停止使用后才回收 |
| 组合与 LA 扩展 | RV四核更广应用及尚未覆盖的故障/消费者组合；LA四核、更多消费者及实板 | 架构结果独立验收，保留单核对照；正确性成立后再衡量并行收益 |

LA双核的固定总量计算、futex往返和独立锁已有加速；普通信号、共享锁及四文件定位读写加最终同步仍有负加速。具体耗时、CPU时间和延迟由兼容分支的CPU成本文档维护；双核执行范围见[LA模块](modules/loongarch-boot.md#双核执行)。

往返信号的空slab拆合已通过队列记录复用改善；四核独立PI锁的调度与交接成本下降，共享PI锁仍有多核负加速。最新UP控制波动、文件查询单次超线及PI扩展性见[定向成本分析](learning/cpu-accounting-cost.md#定向优化队列存储与-pi-交接)。 waitv/PI重排接入后的UP signalfd +4.17%、四核独立普通/PI锁约+10.7%/+10.2%继续保留，固定批次与控制波动见[本轮成本](learning/cpu-accounting-cost.md#futex多地址等待与pi重排成本)。

### P6b 锁、等待与中断规则

CPU 本地/current、抢占控制、buddy/slab raw、代次等待与可睡眠锁内部保护已存在。
压力挂钩使用实际组引用，可由远端发布通知；干净回收按正向锁序等待缓存元数据，归还物理引用后处理 node/entry 释放；脏数据由 worker 写回，写入接纳边界推进有界回收。网络协议与控制、epoll 控制、非 ext4 命名空间、挂载连接管理及设备业务仍有 CPU0 边界。可睡眠锁释放内部raw后才显式调度。
接口契约和调用边界见[调度模块](modules/kernel-scheduler.md#smp前置的同步与等待契约2026-10-09)。

任务索引、身份发布、查找借用和退休归还已在统一短调度域保护。
其他业务对象的引用与远程退出关系随共享MM和子系统并发推进；宿主并发不能代替客户机多核验收。
代码步骤和单次安排留在执行会话。

### P6c TLB 完成与页回收

受控匿名、私有tmpfs及ext4映射的lookup+pin、fault/COW重查、截断/快照、登记借用和远程确认已闭合；扩展SysV、系统缓存和一般业务时保留同样的寿命与权限要求。
PTE 撤销/降权后须覆盖活动 CPU 及并发切换者，确认旧翻译不能再用，才释放页、页表与映射引用；
未完成的失效由明确 owner 延后回收，不能持远核所需的锁等待确认。
文件 fault、COW、truncate/writeback、进程退出、FD/OFD 和设备停止同时服从此生命周期边界。
当前接口限制见 [MM](modules/kernel-mm.md#匿名-mm-的并发与回收)与[uaccess](modules/kernel-uaccess.md#并发与性能边界)。

## 当前应用阻塞与能力边界

下列是按真实消费者选择的缺口目录，不是全部同时推进的排期。
已有机制的具体子语义由对应模块说明，不重新把整个模块列为未实现。

| 类别 | 未完成部分与依赖 | 当前契约 |
|---|---|---|
| 信号、线程与事件 | futex2 的NUMA/MPOL等扩展；扩展 clone；timerfd的BOOTTIME/ALARM与运行时调钟取消 | [信号](modules/kernel-signal.md)、[调度](modules/kernel-scheduler.md)、[文件接口](modules/kernel-files.md)；事件对象按实际需求接 OFD |
| 用户内存 | mremap 的 DONTUNMAP、零长度共享复制、多VMA批量及特殊映射搬移；madvise 的 FREE/REMOVE、dump策略、回收/预装等扩展，VDSO 的 DONTNEED 重装；/proc/<pid>/pagemap | [MM](modules/kernel-mm.md)；普通匿名/文件映射已支持缩小、扩大及地址搬移；madvise五种基础操作及四种fork策略、SHM_LOCK/UNLOCK及MEMLOCK计费已接入，沿共同pin与TLB回收规则 |
| 进程查询与 IPC | pidfd 的扩展 ioctl、clone 创建 pidfd、kcmp 的 IO／EPOLL_TFD、IPC namespace、运行时 IPC sysctl；preadv2/pwritev2 的扩展标志 | getrusage、kcmp 的 FILE/VM/FILES/FS/SIGHAND/SYSVSEM、pidfd_open/send_signal/getfd、waitid(P_ALL/P_PID/P_PGID/P_PIDFD)、wait4、times、SysV 共享内存的 INFO/STAT/ANY 查询、信号量和消息队列及其 /proc/sysvipc 查询、preadv/pwritev 已存在；逐接口补齐语义与资源归还 |
| 存储介质与设备扩展 | GPT、扩展分区、FAT12/16、exFAT；块设备 mmap/discard/direct I/O；loop 分区扫描、CHANGE_FD 与 direct I/O | [块设备](modules/riscv-virtio-block.md)、[FAT32/VFAT](modules/fat32.md)；MBR 四主分区与常用文件后备 loop 已接入 |
| 凭据与限制 | 用户命名空间、ACL及匿名socketfs专属属性、登录认证与用户数据库管理；NICE 的优先级接口与公平调度权重、POSIX mqueue 及 MSGQUEUE 计费、core 文件写出 | [进程凭据](modules/kernel-scheduler.md)、[执行](modules/kernel-exec.md)、[内存](modules/kernel-mm.md)、[文件接口](modules/kernel-files.md)；16项资源限额均可查询、设置和继承；NPROC按真实UID计线程，RTPRIO约束调度授权，RTTIME限制连续RT执行，RSS/LOCKS沿Linux保留记录；AS/DATA、MEMLOCK锁页、CPU、NOFILE/STACK/CORE/FSIZE保持各自范围；线程 UID/GID/补充组、capability、DAC、set-ID/filecap 执行、tmpfs/ext4用户xattr及按真实 UID 的 SIGPENDING 计账已接入 |
| 网络与系统接口 | 外部 IPv6 与运行时网络配置；POSIX定时器的BOOTTIME/ALARM/TAI与运行时调钟、pipe 容量、按需 prctl | [网络](modules/kernel-network.md)、[时钟](modules/kernel-time.md)；UNIX STREAM/DGRAM/SEQPACKET、FD传递与消息凭据已提供；REAL/VIRTUAL/PROF间隔定时器及四类时钟的POSIX定时器已提供，DNS/TLS另核对真实随机/时间/证书依赖 |
| 架构与用户环境 | RV V；LA 纯 legacy PCI、更广原程序、客体原生开发及全断电恢复矩阵 | [LA](modules/loongarch-boot.md)、[程序环境](modules/program-environment.md)；不从共同清单或 SQLite 普通重启推定通过 |

### P1h 虚拟文件系统与多挂载

基础 proc/tmpfs/devpts/ext4/vfat 挂载、覆盖、只读与普通忙卸载已存在；这里保留扩展缺口。

| 扩展 | 尚缺语义 |
|---|---|
| proc 线程信息 | task 子目录中的 fd、exe、cwd 等资源入口及其他按需字段；task 枚举、线程 stat/status 与 thread-self 已提供，边界见[procfs模块](modules/procfs.md) |
| bind/rbind、move、传播与 mount namespace | 挂载边/实例/路径/OFD/MM 的引用及摘树关系，跨任务可见性与退出回收 |
| remount 与其他属性 | 根挂载只读重配置；atime/同步等属性的实际执行点，重配置时已打开对象与脏数据的失败交接 |
| umount2 扩展 | MNT_DETACH/FORCE/EXPIRE/UMOUNT_NOFOLLOW 及错误优先级；force 不允许提前释放用户或 DMA 在用对象 |

当前新 mount 接受 `MS_RDONLY/MS_NOSUID/MS_NODEV/MS_NOEXEC/MS_SILENT`，umount2 仅接受 flags=0，绝对根卸载返回 EINVAL。
NOEXEC在最终挂载的执行打开、access及文件映射上执行；NODEV限制新的设备路径打开和块设备挂载源，已打开对象保留原权利。
基础与扩展边界见 [VFS](modules/vfs-ext4.md)及[挂载学习记录](learning/memory-backed-mounts.md#基础挂载与扩展操作的边界)。

## 已知成本与未关闭现场

重复查询、扫描、复制和不合理的等待归属可从实际调用链判断；吞吐、开销占比和尾延迟由相关负载比较。
不据微实验或计数直接承诺应用加速，也不先加大网络窗口、设备队列或更换调度算法。

| 项目 | 已有事实与剩余问题 | 证据 |
|---|---|---|
| allocator 与批量 MM 成本 | 森林13.6%–22.4%、raw互斥6.0%–7.3%、等待交接的默认匿名新增2.5%–4.0%分别测量；后台提交/退出交错与批量MM的IRQ尾延迟仍有成本 | [森林](learning/cost-baseline.md#buddy-森林匹配时间2026-10-09)、[raw 对照](learning/cost-baseline.md#allocator-短锁的匹配时间)、[等待交接](learning/cost-baseline.md#等待交接的匹配时间) |
| 普通文件逐页管理与stdio成本 | 同规格原版iozone仍有RV／LA吞吐差距；小记录stdio的描述符导入、多目的区用户借用及库内复制分别产生开销。物理引用合批候选未通过单核性能检查，当前保留逐页引用实现 | [读取机制](modules/vfs-ext4.md#缓存版本内的批量读)；对照结果由兼容分支现有数据路径报告维护 |
| 共享调度域短窗口 | 4KiB复制唤醒与32等待者OFD的正式成本仍未决，归因已停止；保留数据，不追加诊断，不作为RV两核规划前置项 | [匹配时间](learning/cost-baseline.md#共享调度域的匹配时间) |
| 文件后端执行与剩余串行范围 | 范围写回已按窗口续批，fsync由调用者等待固定目标；ext4 冷读和 mmap 冷页在调用 CPU 装入；已打开文件的空间修改与 journal 接纳在发起 CPU；ext4 目录、路径元数据与普通最终关闭在发起 CPU；多执行者 journal ext4 的不同目录/文件可并行准备，接纳和拓扑变更保留各自必要的互斥；mount 管理、日志和后台写回继续在 CPU0，当前固定版本的匹配性能另行核对 | [VFS](modules/vfs-ext4.md#后台写回与水位回收)、[RV执行位置](modules/riscv-smp.md#普通文件io的执行位置) |
| GDT timestamp-touch 候选 | 隔离候选消除了干净 GDT 的 undo/restore 复制，但完整读取仍有负向观察，未进入 main | [时间戳研究](learning/file-timestamps.md) |
| 网络端点与原生构建成本 | 端点队列与预约由跨核短锁保护，收发的用户复制和等待在调用 CPU；协议请求交给既有 CPU0 执行者，普通用户管道与poll/epoll等待在调用CPU，控制与关联清理仍有CPU0边界。默认预算、同步/排空及Lua工程的剩余成本按对应负载检查 | [网络](learning/network-ownership.md)、[预算](learning/data-path-budget-experiments.md)、[工具链](learning/offline-toolchain-probe.md) |
| 未归因现场 | 历史Virtqueue/页释放fatal/偶发取消、RV块统计case47及固定Linux LA终端等待，保留各自首个阶段与缺失证据；同类修复和重跑不关闭原事件 | [事故边界](learning/cost-baseline.md#旧版内存释放与-virtqueue-告警2026-10-02)、[程序证据](learning/user-program-inventory.md)、[块诊断](modules/riscv-virtio-block.md) |

这些条目不是新执行计划；已有修复的过程与反例留在对应模块/learning，不重复列入待做。

## 后续项目边界

实板 VF2/2K1000LA 需取得固件、DTB 和设备输入后独立验收 RAM、IRQ、DMA/cache、根盘与网络；
ASID 复用/性能在能观察标签 TLB 的硬件上核对。并行构建与每核队列/allocator/cache 优化依赖多核正确性和争用证据。
LA 原生开发、C++/Rust、离线交付与更广应用由明确消费者触发，不作为 SMP 的无限前置清单。
NUMA、swap/透明大页、完整 namespace/cgroup、seccomp/eBPF、Linux 模块 ABI、io_uring 暂无主线排期。

## 验收与分支职责

每项交付按最窄反例、真实用户程序、相关架构回归核对返回语义与 owner；
OOM/设备错误与非法所有权 fatal 分开，预算终止不证明资源回收。
固定输入、计数、匹配时间和重建命令归 learning；当前接口与聚焦命令归 modules，逐轮计划留在会话。

通用修复先落 `main`，再单向集成至 `oscomp-compat`；比赛配置、原盘适配、监督与分数由兼容分支维护。
完整 Harness 的剩余失败和未到达项不能算通过，也不由共同229项清单推导。
托管 CI 状态见[CI 模块](modules/continuous-integration.md)，配置或本地成功不代替实际远程执行。
发布、push、比赛提交与实板操作依照 [AGENTS.md](../AGENTS.md)由维护者决定。
