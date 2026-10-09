# 开发路线与未完成工作

本文只维护长期方向、当前缺口和依赖，不保存逐轮执行计划、已完成复选框或提交日志。
当前能力见 [README](../README.md)，接口与验证入口见[模块导航](README.md)，测量和验收证据见各 learning。
固定资料身份由 `references/sources.tsv` 管理；评审意见需经源码与验证核实后才成为事实。

## 近期方向

架构主线是 SMP 正确性。当前重点为同步基础和对象生命周期；RV 两核、共享用户地址空间及 LA 多核依次依赖它。
维持现有调度策略、全局运行队列和串行 lwIP 核心，先证明并发正确，再依据锁等待与 CPU 分布决定并行优化。

| 主线 | 尚缺能力 | 完成口径 |
|---|---|---|
| 同步基础 | 可睡眠锁内部保护、等待登记/阻塞/唤醒/取消/销毁交接，调度共享状态与对象引用同步 | 不丢 wake、不重复授予；等待和引用 owner 在并发退出时仍存活 |
| RV 两核 | 独立 secondary 入口/栈，BSS 一次初始化，per-CPU trap/timer/FPU、IPI、远程 wake | 同一任务不双核运行；idle 可被唤醒；正常用户态切换与可信栈回收 |
| 共享 MM 与子系统 | uaccess pin/权限、COW 发布、活动 CPU、远程 TLB 确认，OFD/cache/DMA 与退出同步 | 复制与撤映射、降权、截断、I/O/close/退出的强制交错正确；确认停止使用后才回收 |
| 组合与 LA 多核 | RV 2/4 核低内存、故障、真实程序；LA CPU 启动/IPI/TLB/状态保存 | 架构结果独立验收，保留单核对照；正确性成立后再衡量并行收益 |

### P6b 锁、等待与中断规则

CPU 本地/current、抢占控制与 buddy/slab raw 锁已存在；压力回调仍限定单 CPU。
已有可睡眠锁会在 IRQ 关闭时显式调度，不能机械替换成 raw 锁。
接口契约和调用边界见[调度模块](modules/kernel-scheduler.md#smp前置的同步与等待契约2026-10-09)。

- [ ] 闭合条件检查、登记、释放保护与 park 的交接；wake、timeout、cancel、destroy 争用同一注册 owner。
- [ ] 将可睡眠锁内部元数据接入跨核保护，保持 rank/key、先授予资格再唤醒，禁止持 raw 锁等待 I/O。
- [ ] 保护调度共享状态与其他对象引用；同一任务的发布、运行和退出必须有唯一 owner。

这些是一个同步基础任务的核心不变量；具体实现方案、代码步骤和单次验收安排在执行会话中确定。

### P6c TLB 完成与页回收

共享 MM 跨核执行依赖同一保护下的 lookup+pin、fault 睡眠后重查、片段权限许可点和精确复制前缀。
PTE 撤销/降权后须覆盖活动 CPU 及并发切换者，确认旧翻译不能再用，才释放页、页表与映射引用；
未完成的失效由明确 owner 延后回收，不能持远核所需的锁等待确认。
文件 fault、COW、truncate/writeback、进程退出、FD/OFD 和设备停止同时服从此生命周期边界。
当前接口限制见 [MM](modules/kernel-mm.md#smp前置的用户复制与回收契约2026-10-09)与[uaccess](modules/kernel-uaccess.md#并发与性能边界)。

## 当前应用阻塞与能力边界

下列是按真实消费者选择的缺口目录，不是全部同时推进的排期。
已有机制的具体子语义由对应模块说明，不重新把整个模块列为未实现。

| 类别 | 未完成部分与依赖 | 当前契约 |
|---|---|---|
| 信号、线程与事件 | sigaltstack、实时信号队列/计费；CMP_REQUEUE/WAKE_OP、共享文件/PI futex；扩展 clone；eventfd/timerfd/signalfd | [信号](modules/kernel-signal.md)、[调度](modules/kernel-scheduler.md)、[文件接口](modules/kernel-files.md)；事件对象按实际需求接 OFD |
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

性能项须由独立工作负载、成本计数和匹配时间支持，每次只选择一个可证伪的机制。
不据微实验或计数直接承诺应用加速，也不先加大网络窗口、设备队列或更换调度算法。

| 项目 | 已有事实与剩余问题 | 证据 |
|---|---|---|
| allocator 与批量 MM 成本 | 森林的典型回退13.6%–22.4%与后续 raw 互斥新增6.0%–7.3%分别测量；批量 MM 的 IRQ 尾延迟未改善 | [森林](learning/cost-baseline.md#buddy-森林匹配时间2026-10-09)、[raw 对照](learning/cost-baseline.md#allocator-短锁的匹配时间) |
| 文件驻留记录范围选择 | 点查询已有哈希，mprotect/部分撤映射仍可能扫描整个驻留链；只改善范围选择/删除，不同时树化全部 MM | [规模边界](learning/single-hart-scale.md)、[MM](modules/kernel-mm.md) |
| GDT timestamp-touch 候选 | 隔离候选消除了干净 GDT 的 undo/restore 复制，但完整读取仍有负向观察，未进入 main | [时间戳研究](learning/file-timestamps.md) |
| 串行协议、存储与原生构建成本 | 网络批次仍有大 IRQ 区间；默认预算吞吐、同步/排空及 Lua 工程还有已测成本，按目标阶段归因 | [网络](learning/network-ownership.md)、[预算](learning/data-path-budget-experiments.md)、[工具链](learning/offline-toolchain-probe.md) |
| 历史 Virtqueue／页释放 fatal／偶发取消 | 同类 allocator 竞态已修复；原事件缺队列/owner现场，具体归因仍未确认。复发时保留首次状态，不用 clean run 反推关闭 | [事故边界](learning/cost-baseline.md#旧版内存释放与-virtqueue-告警2026-10-02)、[程序证据](learning/user-program-inventory.md) |

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
