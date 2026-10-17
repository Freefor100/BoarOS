# 开发路线与未完成工作

本文只维护长期方向、当前缺口和依赖，不保存逐轮执行计划、已完成复选框或提交日志。
当前能力见 [README](../README.md)，接口与验证入口见[模块导航](README.md)，测量和验收证据见各 learning。
固定资料身份由 `references/sources.tsv` 管理；评审意见需经源码与验证核实后才成为事实。

## 近期方向

架构主线是SMP正确性；受控RV映射域已包含匿名、私有tmpfs及CPU0 I/O交接的ext4。真实RV用户两核按业务入口交接仍需CPU0的操作，229项原程序及有限压力/生命周期组合已有本地两核与单核退化验证；下一步明确inode、mount和journal的实际共享状态，缩小文件后端串行范围，再推进端点跨CPU所有权及LA多核。
维持现有调度策略、全局运行队列和串行 lwIP 核心，先证明并发正确，再依据锁等待与 CPU 分布决定并行优化。
默认生产仍为单 CPU；普通与受控对象共用保护与引用规则，普通文件缓存读写、用户复制与匿名缺页在发起 CPU 执行。冷读与后端释放复用既有 CPU0 worker，范围写回按窗口续批，fsync捕获固定持久化目标后由原调用者等待，CPU0完成事件处理错误观察并直接通知结果。系统分配器通知和干净页回收可由远端调用，脏数据仍由 worker 写回。网络已将端点队列的短IRQ保护与开中断的lwIP核心互斥分开；端点仍依赖CPU0，不能据此视为支持跨CPU消费。

| 主线 | 尚缺能力 | 完成口径 |
|---|---|---|
| 共享 MM 与子系统 | SysV/其他外部后备的并行内核修改；文件系统后端、socket/epoll/FIFO、命名空间及 DMA 业务与退出同步 | 复制与撤映射、降权、截断、I/O/close/退出的强制交错正确；确认停止使用后才回收 |
| 组合与 LA 多核 | RV 4核及尚未覆盖的故障/消费者组合；LA CPU 启动/IPI/TLB/状态保存 | 架构结果独立验收，保留单核对照；正确性成立后再衡量并行收益 |

### P6b 锁、等待与中断规则

CPU 本地/current、抢占控制、buddy/slab raw、代次等待与可睡眠锁内部保护已存在。
压力挂钩使用实际组引用，可由远端发布通知；同步干净回收只尝试取得缓存保护，忙时跳过，脏数据交给worker。网络端点、命名空间及设备业务仍有CPU0边界。可睡眠锁释放内部raw后才显式调度，不能机械替换IRQ域。
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
| 信号、线程与事件 | sigaltstack、实时信号队列/计费；CMP_REQUEUE/WAKE_OP、PI futex；扩展 clone；eventfd/timerfd/signalfd | [信号](modules/kernel-signal.md)、[调度](modules/kernel-scheduler.md)、[文件接口](modules/kernel-files.md)；事件对象按实际需求接 OFD |
| 用户内存 | mremap、按操作区分的 madvise、mlock；文件映射截断/退出错误的进一步交错验证 | [MM](modules/kernel-mm.md)；地址迁移依赖对象和并发边界稳定 |
| 凭据与限制 | 可变 UID/GID/补充组及完整权限；合成对象权限、AS/DATA/FSIZE/CPU 等限额 | [文件接口](modules/kernel-files.md)、[syscall](modules/kernel-syscall.md)；现有固定 root 和 NOFILE/STACK 不重复立项 |
| 网络与系统接口 | 命名 AF_UNIX/控制消息、原生 accept4、外部 IPv6 与运行时网络配置；CPU-time clock、VIRTUAL/PROF timer、pipe 容量、按需 prctl | [网络](modules/kernel-network.md)、[时钟](modules/kernel-time.md)；DNS/TLS 另核对真实随机/时间/证书依赖 |
| 架构与用户环境 | RV V；LA 纯 legacy PCI、更广原程序、客体原生开发及全断电恢复矩阵 | [LA](modules/loongarch-boot.md)、[程序环境](modules/program-environment.md)；不从共同清单或 SQLite 普通重启推定通过 |

### P1h 虚拟文件系统与多挂载

基础 proc/tmpfs/devpts/ext4 挂载、覆盖、只读与普通忙卸载已存在；这里保留扩展缺口。

| 扩展 | 尚缺语义 |
|---|---|
| bind/rbind、move、传播与 mount namespace | 挂载边/实例/路径/OFD/MM 的引用及摘树关系，跨任务可见性与退出回收 |
| remount 与其他属性 | 根挂载只读重配置；NOEXEC/NODEV/NOSUID、atime/同步等属性的实际执行点，已打开对象与脏数据的失败交接 |
| umount2 扩展 | MNT_DETACH/FORCE/EXPIRE/UMOUNT_NOFOLLOW 及错误优先级；force 不允许提前释放用户或 DMA 在用对象 |

当前 mount 仅接受 `MS_RDONLY/MS_SILENT`，umount2 仅接受 flags=0，绝对根卸载返回 EINVAL。
基础与扩展边界见 [VFS](modules/vfs-ext4.md)及[挂载学习记录](learning/memory-backed-mounts.md#基础挂载与扩展操作的边界)。

## 已知成本与未关闭现场

重复查询、扫描、复制和不合理的等待归属可从实际调用链判断；吞吐、开销占比和尾延迟由相关负载比较。
不据微实验或计数直接承诺应用加速，也不先加大网络窗口、设备队列或更换调度算法。

| 项目 | 已有事实与剩余问题 | 证据 |
|---|---|---|
| allocator 与批量 MM 成本 | 森林13.6%–22.4%、raw互斥6.0%–7.3%、等待交接的默认匿名新增2.5%–4.0%分别测量；后台提交/退出交错与批量MM的IRQ尾延迟仍有成本 | [森林](learning/cost-baseline.md#buddy-森林匹配时间2026-10-09)、[raw 对照](learning/cost-baseline.md#allocator-短锁的匹配时间)、[等待交接](learning/cost-baseline.md#等待交接的匹配时间) |
| 共享调度域短窗口 | 4KiB复制唤醒与32等待者OFD的正式成本仍未决，归因已停止；保留数据，不追加诊断，不作为RV两核规划前置项 | [匹配时间](learning/cost-baseline.md#共享调度域的匹配时间) |
| 文件后端串行范围 | 范围写回已按窗口续批，fsync由调用者等待固定目标；inode数据处理、mount元数据与journal接纳仍在CPU0，继续按实际共享状态拆分执行范围 | [VFS](modules/vfs-ext4.md#后台写回与水位回收)、[RV执行位置](modules/riscv-smp.md#普通文件io的执行位置) |
| GDT timestamp-touch 候选 | 隔离候选消除了干净 GDT 的 undo/restore 复制，但完整读取仍有负向观察，未进入 main | [时间戳研究](learning/file-timestamps.md) |
| 网络端点与原生构建成本 | 端点队列已离开lwIP全局互斥，但短IRQ保护仍依赖CPU0；本地消费需要实际端点锁与引用，lwIP可继续单一owner。默认预算、同步/排空及Lua工程的剩余成本按对应负载检查 | [网络](learning/network-ownership.md)、[预算](learning/data-path-budget-experiments.md)、[工具链](learning/offline-toolchain-probe.md) |
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

## 兼容分支的剩余边界

原评测与官方容器的具体状态由[兼容模块](modules/oscomp-compat.md)和
[评测证据](learning/oscomp-compat-baseline.md)维护，不在这里复制已完成适配或逐次执行记录。

- 最近完整官方调用联合整数分2130；两侧LTP-glibc触及总预算，LTP-musl未到达。
  剩余组须在同一次调用内执行，预算终止的资源收口仍未验证，不能拼接成绩。
- 原basic四份结果90/102：共同缺VFAT及真实`/dev/vda2`环境；原brk ELF截断64位返回值。
  已确认保留当前地址布局和原输入，独立修正诊断92/102不替代默认结果。
- 原程序的`/proc/cpuinfo`、Bash/Perl、MMC和network namespace等依赖按真实消费者归属；
  通用能力先在main实现，环境与辅助脚本留在本分支。历史零采样及其他未定归因保留原证据。
