# 开发路线与未完成工作

本文只维护长期方向、当前缺口和依赖，不保存逐轮执行计划、已完成复选框或提交日志。
当前能力见 [README](../README.md)，接口与验证入口见[模块导航](README.md)，测量和验收证据见各 learning。
固定资料身份由 `references/sources.tsv` 管理；评审意见需经源码与验证核实后才成为事实。

## 近期方向

RV、LA 已支持实际 1～8 核用户执行，默认交付仍为单核。每个 READY 任务只归属一个 CPU 队列；跨核调度、等待、共享 MM 失效、信号与各对象生命周期使用共同实现。

主要数据路径已在调用核执行：ext4 读写及目录准备、用户复制、匿名缺页、futex、IPC 收发、普通管道和事件等待。各子系统的执行位置及共享状态统一见[共同调度与业务状态](modules/kernel-scheduler.md#smp-的共同调度与业务状态)。多核可调用与独立负载得到加速分别验收；已建立的 TCP 连接由各归属核推进；ext4 的日志提交和 checkpoint 按在线核数分配。设备中断、UDP、挂载控制和页缓存后台写回仍保留 CPU0 边界。
| 主线 | 尚缺能力 | 完成口径 |
|---|---|---|
| 共享 MM 与子系统 | 非固定私有匿名映射、匿名撤销和锁页属性已支持范围协调；固定地址替换、mprotect、文件/ELF装页仍有独占范围。共享MM锁页循环的等待交接仍有多核负加速，非ext4后端、设备/DMA与退出仍有未覆盖的消费者组合 | 复制与撤映射、降权、截断、I/O/close/退出的强制交错正确；确认停止使用后才回收 |
| 组合与 LA 扩展 | RV／LA八核的更广应用及尚未覆盖的故障/消费者组合、实板 | 架构结果独立验收，保留单核对照；正确性成立后再衡量并行收益 |

普通任务的实际入队、领取、旧栈归还及无期限等待的阻塞提交由各核协调；有期限的等待、信号状态及实时资格仍有共同控制。共享 MM 的数组编辑/PTE 发布、缓存元数据与 LRU、共享 IP 和协议内存池仍有串行范围，尚有负加速和同步尾延迟问题。各负载的耗时、CPU、吞吐及同内核扩展性由兼容分支的[CPU 成本报告](learning/cpu-accounting-cost.md)维护。

### P6b 锁、等待与中断规则

CPU 本地/current、抢占控制、buddy/slab raw、代次等待与可睡眠锁内部保护已存在。
压力挂钩使用实际组引用，可由远端发布通知；干净回收按正向锁序等待缓存元数据，归还物理引用后处理 node/entry 释放；脏数据由 worker 写回，写入接纳边界推进有界回收。UDP 与监听和建连控制、epoll 控制、部分文件后端、挂载连接管理及设备业务仍有 CPU0 边界。可睡眠锁释放内部raw后才显式调度。
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
| 信号、线程与事件 | futex2 的NUMA/MPOL等扩展；clone3 的指定 PID、cgroup、非挂载命名空间及其他扩展标志；timerfd的BOOTTIME/ALARM与运行时调钟取消 | [信号](modules/kernel-signal.md)、[调度](modules/kernel-scheduler.md)、[文件接口](modules/kernel-files.md)；clone3与CLONE_PIDFD覆盖现有进程资源共享组合 |
| 用户内存 | mremap 的多VMA批量及特殊映射搬移；madvise 的 COLD/PAGEOUT 等回收扩展；/proc/<pid>/pagemap | [MM](modules/kernel-mm.md)；普通匿名/文件映射支持缩小、扩大、搬移和DONTUNMAP，零旧长度可复制共享映射；madvise基础建议、POPULATE预装、dump标记、匿名FREE、四种fork策略与共享后备REMOVE、SHM_LOCK/UNLOCK及MEMLOCK计费已接入，沿共同pin与TLB回收规则；vDSO ELF 通过 AT_SYSINFO_EHDR 提供时间查询与信号返回 |
| 进程查询与 IPC | pidfd 的扩展 ioctl、kcmp 的 EPOLL_TFD、IPC namespace、运行时 IPC sysctl | execve/execveat及按FD执行、chroot；getrusage、kcmp 的 FILE/VM/FILES/FS/SIGHAND/IO/SYSVSEM、pidfd_open/send_signal/getfd、clone 创建 pidfd、waitid(P_ALL/P_PID/P_PGID/P_PIDFD)、wait4、times、SysV 共享内存的 INFO/STAT/ANY 查询、信号量和消息队列及其 /proc/sysvipc 查询；逐接口补齐语义与资源归还 |
| 文件接口扩展 | openat2 的纯缓存查找、memfd 的大页后备、copy_file_range跨实例及其他扩展标志；RWF_NOWAIT／ATOMIC／DONTCACHE底层能力 | [openat2](modules/kernel-files.md#openat-与路径边界)支持受限路径解析，[memfd_create](modules/kernel-files.md#memfd_create-与文件封印)支持匿名内存文件与封印；flock、close_range及UNSHARE/CLOEXEC、fchmodat2、epoll_pwait2、同实例文件复制、sendmmsg/recvmmsg、splice/tee/vmsplice、inotify、preadv2/pwritev2的常用按次选项、六种fadvise建议及显式readahead已提供；RV用户指令同步复用共享MM的指令代次与SBI确认 |
| 存储介质与设备扩展 | GPT、扩展分区、FAT12/16、exFAT；块设备 mmap/discard/direct I/O；loop 分区扫描、CHANGE_FD 与 direct I/O | [块设备](modules/riscv-virtio-block.md)、[FAT32/VFAT](modules/fat32.md)；MBR 四主分区与常用文件后备 loop 已接入 |
| 凭据与限制 | 用户命名空间、socketfs所有权与时间属性扩展、登录认证与用户数据库管理；NICE 的优先级接口与公平调度权重、core 文件写出 | [进程凭据](modules/kernel-scheduler.md)、[执行](modules/kernel-exec.md)、[内存](modules/kernel-mm.md)、[文件接口](modules/kernel-files.md)；16项资源限额均可查询、设置和继承；NPROC按真实UID计线程，RTPRIO约束调度授权，RTTIME限制连续RT执行，RSS/LOCKS沿Linux保留记录；AS/DATA、MEMLOCK锁页、CPU、NOFILE/STACK/CORE/FSIZE保持各自范围；线程 UID/GID/补充组、capability、DAC、set-ID/filecap 执行、tmpfs/ext4用户xattr及POSIX ACL，以及按真实 UID 的 SIGPENDING、POSIX mqueue 容量计账已接入 |
| 网络与系统接口 | 外部 IPv6、IPv4 多地址与更丰富的路由；POSIX定时器的BOOTTIME/ALARM/TAI与运行时调钟、按需 prctl | [网络](modules/kernel-network.md)、[时钟](modules/kernel-time.md)；IPv4 地址、掩码、MTU 和单条默认网关可运行时设置；UNIX STREAM/DGRAM/SEQPACKET、FD传递与消息凭据已提供；匿名管道/FIFO支持容量查询、调整和创建 UID 额度；POSIX mqueue 与 libc 线程通知见[消息队列](modules/posix-mqueue.md)，NETLINK当前提供通知接收，通用路由处理另行实现；REAL/VIRTUAL/PROF间隔定时器及四类时钟的POSIX定时器已提供，DNS/TLS另核对真实随机/时间/证书依赖 |
| BPF 网络与跟踪 | JIT、XDP/TC、AF_PACKET、seccomp、硬件 PMU 及 kprobe/uprobe/fentry 挂载 | [BPF](modules/kernel-bpf.md)；共用解释器、socket 过滤、map/BTF/bpffs、raw_syscalls tracepoint/perf/link 和 ringbuf 已接入；兼容范围围绕原版 bpftool/libbpf/bpftrace |
| 架构与用户环境 | RV V；LA 纯 legacy PCI、更广原程序、客体原生开发及全断电恢复矩阵 | [LA](modules/loongarch-boot.md)、[程序环境](modules/program-environment.md)；不从共同清单或 SQLite 普通重启推定通过 |

### P1h 虚拟文件系统与多挂载

基础 proc/tmpfs/devpts/mqueue/ext4/vfat 挂载、覆盖、目录/文件 bind、rbind、MS_MOVE、chroot、RO/RW 重配置、普通忙卸载与 MNT_DETACH/UMOUNT_NOFOLLOW 已存在。挂载命名空间通过CLONE_NEWNS、unshare、setns与/proc/<pid>/ns/mnt使用，复制保留传播关系。

| 扩展 | 尚缺语义 |
|---|---|
| proc 查询扩展 | mounts/mountinfo的FD打开时根目录捕获语义、其他按需线程字段；task 枚举、线程 stat/status、fd/fdinfo、exe/cwd/root、maps 与 thread-self 已提供，边界见[procfs模块](modules/procfs.md) |
| 挂载接口扩展 | fsopen/fsconfig/fsmount/fspick、open_tree的无VFS路径匿名FD视图及其他新mount API；pivot_root、open_tree、move_mount、mount_setattr与既有传播、命名空间共用连接和引用规则 |
| 挂载属性扩展 | 同步、lazytime、NOSYMFOLLOW、ID映射及其他尚未支持的属性与重配置选项；四种atime策略及O_NOATIME已执行 |
| umount2 扩展 | MNT_FORCE/EXPIRE 及错误优先级；force 不允许提前释放用户或 DMA 在用对象 |

当前新 mount 与基础 `MS_REMOUNT` 接受 `MS_RDONLY/MS_NOSUID/MS_NODEV/MS_NOEXEC/MS_SILENT`及atime策略，`MS_BIND` 与 `MS_BIND|MS_REC` 复用实例，`MS_REMOUNT|MS_BIND` 更新单个视图属性，`MS_MOVE` 移动既有挂载连接；umount2 接受 flags=0、`MNT_DETACH`、`UMOUNT_NOFOLLOW`及后两者组合；进程根所属挂载的普通卸载尝试转为 RO；目标仍须是实际挂载根，chroot子目录本身的卸载返回 `EINVAL`，无 parent 的全局根对 `MNT_DETACH` 返回 `EINVAL`。重配置的写入者、映射、同步和失败所有权见[VFS 模块](modules/vfs-ext4.md)。
NOEXEC在最终挂载的执行打开、access及文件映射上执行；NODEV限制新的设备路径打开和块设备挂载源，已打开对象保留原权利。
基础与扩展边界见 [VFS](modules/vfs-ext4.md)。

## 当前性能边界

下列是当前实现仍存在的共享范围。历史单轮百分比和旧候选矩阵留在 Git 历史；不能把它们当成当前版本的开销。

| 部分 | 仍需处理的实际工作 | 当前机制 |
|---|---|---|
| 分配器与 MM | 物理页管理、共享 VMA/PTE 编辑及远程失效协调仍有共同保护；先按实际负载分开锁等待、页面分配和确认成本 | [物理页](modules/physical-pages.md)、[MM](modules/kernel-mm.md) |
| 调度与等待 | 普通任务由每核队列维护，有期限等待、信号状态和实时/PI 仍有共同控制；独立对象和共享对象的交接成本应分别比较 | [调度](modules/kernel-scheduler.md) |
| 文件 I/O | 逐页用户借用、稳定版本、共享缓存/LRU、后台写回和日志持久目标各有成本；读取、写入接纳与最终同步分别报告 | [VFS/ext4](modules/vfs-ext4.md)、[数据路径工具](learning/data-path-budget-experiments.md) |
| 网络 | TCP 按连接归属核推进；端点状态、协议池以及 UDP/监听/建连控制仍有共享范围 | [网络](modules/kernel-network.md) |

重复工作可由源码判断，时间占比和应用收益需要相同 ELF、输入与完成条件的比较。现用测量方法见[CPU 测量](learning/cpu-accounting-cost.md)和[数据路径工具](learning/data-path-budget-experiments.md)。

## 后续项目边界

实板 VF2/2K1000LA 需取得固件、DTB 和设备输入后独立验收 RAM、IRQ、DMA/cache、根盘与网络；
ASID 复用/性能在能观察标签 TLB 的硬件上核对。并行构建与每核队列/allocator/cache 优化依赖多核正确性和争用证据。
LA 原生开发、C++/Rust、离线交付与更广应用由明确消费者触发，不作为 SMP 的无限前置清单。
NUMA、swap/透明大页、完整 namespace/cgroup、seccomp、BPF 的 JIT/XDP/TC 与探针挂载、Linux 模块 ABI、io_uring 暂无主线排期。

## 验收与分支职责

每项交付按最窄反例、真实用户程序、相关架构回归核对返回语义与 owner；
OOM/设备错误与非法所有权 fatal 分开，预算终止不证明资源回收。
固定输入、计数、匹配时间和重建命令归 learning；当前接口与聚焦命令归 modules，逐轮计划留在会话。

通用修复先落 `main`，再单向集成至 `oscomp-compat`；比赛配置、原盘适配、监督与分数由兼容分支维护。
完整 Harness 的剩余失败和未到达项不能算通过，也不由共同229项清单推导。
发布、push、比赛提交与实板操作依照 [AGENTS.md](../AGENTS.md)由维护者决定。
