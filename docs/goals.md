# 开发路线与验收

本文是唯一开发路线入口，保留 P/N/L 编号便于引用。`[x]` 只表示具体交付已验收，
不代表整个子系统或 Linux 兼容完成。固定依据由 `references/sources.tsv` 管理；
Linux 为 `references/linux` 的 `f4cdf7ca9a1fdcca413157df19753f388a5a224e`。
历史评审是调查输入，不自动成为设计批准。

此前于 `main@959da70` 完成仅文档审计；下面独立 Review 小节记录新一轮实际修复与验证。
下列“已交付”引用该阶段证据，“未实现”据当前源码，“待定位”不等于已证实内核缺陷。

## 独立 Review 的组合边界（2026-09-30）

核实起点为 `main@d2a548d`，Review ZIP SHA-256 为 `c462c3a48c6ed22ea83118c4117a9d113f25e455192a9224016aeb8166bb0c7f`；八个关键文件与该起点一致。旧 1031 条 ABI、VMA/scale 基线缺少本次组合边界，不能反证本次缺陷。修复后新增回归先证伪旧实现，最终 1091 条固定 RV64 Linux/BoarOS 记录一致。完整 RV64、musl、glibc 五种形态、四组合 io-sleep、scale 与栈检查通过；栈静态分析覆盖 1674 函数，最大 2368 字节。SQLite WAL/重启双侧及当前内核的 126 切点、26 写失败、16 flush 失败恢复矩阵通过，固定输入见[恢复记录](learning/record-lock-sqlite-recovery.md)。独立全改动审查未发现 Critical/Important 缺陷。

完整比赛 Harness 保持阻塞、未执行：本地 `references/oscomp-autotest/kernel/run.py`（commit `d1bb3a3c4b27274e196a2648518525c1a304e339`）同时启动 `kernel-rv` 与 `kernel-la`，当前 `make all` 只有前者且后者缺失。不能以本轮 RV64 测试替代双架构验收；本轮没有 push、发布或阶段转换。

2026-09-30 独立 Review 的组合边界修复：

- [x] R1（`7d89d2f`）：SHM 附加 registry OOM 回滚不消费段表 owner；scale 聚焦故障后重试及最终页回收通过。
- [x] R2（`e5769ee`）：显式 attachment 与通用 VMA 片段 open/close；覆盖分裂、起始页撤销、RMID、匿名/文件/SHM 固定替换、fork/退出与 metadata/page OOM，保持 prepare/commit 失败原子性。
- [x] R3（`f30f521`）：file-source 操作 pin 覆盖 msync 的 inode 等待与错误游标；三任务最后撤映射、终止请求、同步/关闭失败及最终清理通过四种 io-sleep 配置。
- [x] R4（`5008e09`）：AF_UNIX DGRAM 整包复制/提交、零消息和整包预算；差分、scale OOM/fault/容量复用与真实 pthread 整包等待/取消通过。
- [x] R5（`d8052ee`，完成断言修正 `c76bd0e`）：线程同步故障记录与统一返回交付、si_code/si_addr、阻塞/忽略强制默认；真实 U-mode 修复/上下文返回、坏帧和默认组退出及 5 条固定 Linux 故障差分通过。
- [x] R6（`dad0005`）：通用 `maximum_permissions` 替代文件专用布尔量；只读附件 WRITE 升级在 PTE 修改前返回 EACCES，split/fork 保留上限，8 条新增固定 Linux 差分通过。
- [x] R7（`dd1467c`）：rank 15 inode 写操作门闩覆盖整次 write/writev/pwrite/append/同步收尾与截断；四种 io-sleep 配置、同 inode 缺页缓冲、实际 U-mode 向量追加和 partial-write 通过。SQLite WAL/重启双侧及完整 WAL 恢复矩阵通过。
- [x] R8（`b104889`）：已有 key 的零大小查找与创建下限分开；已有/缺失 key、零/合法/超原段大小、CREAT/EXCL 的 20 条新增固定 Linux 差分通过。

## 已交付：进程身份、可信随机数与真实调度

从 `main@b5593ca` 演进，统一 TID/TGID/PGID/SID 身份对象、会话/进程组、
coarse clock、VirtIO RNG 与随机接口，以及 OTHER/FIFO/RR、CPU0 affinity 和
全局实时带宽已交付。默认周期 1 秒、预算 950 毫秒是本项目选定参数。
该阶段完整差分 **1000 条匹配**；228 项清单 **227 pass、原 BusyBox 包装器 1 项失败**，
逐 ID 无回退。固定 BusyBox、libc daemon、原 iperf/cyclictest 的实际调用链、
原静态/动态 utime 各 30 次双侧复跑，以及普通/实时双盘进展均有证据。
RISC-V、userland/glibc、规模、睡眠 I/O、栈、离线 GCC ext4/tmpfs、ext4 恢复与
SQLite DELETE/WAL 完整矩阵通过；不同验证快照的边界明确分列。

证据与重建命令见[完整差分](modules/differential-abi.md)、
[程序清单](learning/user-program-inventory.md)、[消费者](learning/session-consumers.md)、
[调度](learning/kernel-scheduling.md)及[恢复矩阵](learning/record-lock-sqlite-recovery.md)。
本阶段未扩展 TTY、完整凭据、PI futex、PID namespace、nice 权重或 SMP，
不承诺硬实时；iperf 网络连接、mlock、chown 和 cgroup 缺口保留独立归属。
固定依据为本页 Linux 和 QEMU v11.1.0；实际运行 QEMU 为 11.1.1。

## 前置交付与历史证据入口

历史数字用于识别阶段，不替代上方最新基线；逐次输入哈希、失败原因和重建命令归 learning。

| 已交付基础 | 证据入口 |
|---|---|
| child-TID、shebang、PID 1 配置、设备节点 | [线程与 futex](learning/threads-and-futex.md)、[ELF](learning/elf-loading.md)、[根启动](modules/riscv-root-boot.md) |
| 通用 VFS、挂载路径、proc 代次/OFD、fd 复用压力 | [VFS](modules/vfs-ext4.md)、[procfs](modules/procfs.md) |
| 真实内存/sysinfo、阈值写回、水位回收、可睡眠 I/O | [内存](learning/memory-management.md)、[存储](learning/sleepable-storage.md) |
| 统一内存后备、tmpfs、硬链接、真实第二盘 | [多挂载验收](learning/memory-backed-mounts.md)、[tmpfs](modules/tmpfs.md)；该阶段为 783 条 ABI |
| 进程/随机/调度与完整存储矩阵阶段 | [消费者](learning/session-consumers.md)、[程序清单](learning/user-program-inventory.md)、[恢复](learning/record-lock-sqlite-recovery.md)；该阶段为 1000 条 ABI |

正常开发沿 main；评测分支只单向接收已验收主线，其 uname/镜像适配和正式成绩
不合回通用实现。旧评测结果不是本轮成绩，分支交接、push、发布及正式评分由维护者决定。
仅在有新机制/证据时更新 README、模块与 learning，依赖/阻塞/优先级只在本页维护。
阶段收口记录可重建输入与结论后预览并执行 make prune-build；原始参考镜像保留，
运行副本、日志及临时 plan/spec 不入库。

### 直接观测的缺口与定位队列

依据为固定 autotest `d1bb3a3` 原镜像诊断及最新进程阶段独立复跑。下表明确区分已解除、仍阻塞和待定位；未到达不等于已测失败。

| 观测 | 结论边界与归属 |
|---|---|
| libctest 包装器存在却 not found | P1h 真实 self/exe 已解除 BusyBox ENOEXEC 回退的前置阻塞；内部失败需按 libc 与 syscall 分别复现，不能再归为内核缺 shebang |
| LTP 缺 meminfo，最终反复读 /proc/5/stat | 真实 Cached/MemAvailable 已交付；独立原 abort01 越过内存查询，首次停在 chown ENOSYS。历史反复查询不能据此推断等待死锁 |
| LTP 到 `cgroup_fj_proc` 后无输出 | 独立 Linux/BoarOS 均在无参数 helper 的 sigsuspend 等待，3 秒后 SIGKILL/wait 正常；原 runner 无参数枚举辅助程序会耗尽预算。function 的参数和控制器缺口另列；本轮复跑已越过 setpgid，见程序清单 learning |
| BusyBox df/ps/free 的实际内容 | 目录枚举修正后 ps 实际列出进程；最新清单的 df 列出 /dev/shm 的 tmpfs，但仍跳过来源名 rootfs 的根盘；free 已通过并由真实 sysinfo/meminfo 提供非零容量、缓存与可用量；df 的来源策略仍单列，不以退出 0 算内容正确 |
| dmesg klogctl 未实现 | P5c 日志接口，必须有真实内容与权限边界 |
| hwclock 无 RTC 字符接口 | 最新原包装器无法打开 /dev/misc/rtc；内核已有 RTC 时钟来源不等于已提供用户 RTC 设备/ioctl，归 P5c |
| 历史 kill 10 未计分 | 原目标/脚本环境需单列；最新 55 子项包装器的启动子进程再 kill 已通过，不能继续当作该清单失败 |
| cyclictest 原 affinity/调度阻塞 | 本轮原 ELF 已真实调用调度接口并完成 C:10；mlock 仍 ENOSYS，尚不声明完整延迟或锁页能力 |
| hackbench 创建 fdpair 失败 | N2 已交付 AF_UNIX / socketpair (199)，支持 STREAM/DGRAM、背压与关闭语义，原版 hackbench 运行通过且关机 heap-live=0 |
| iperf 原随机/daemon 阻塞 | 原 iperf 客户端已越过随机接口与 affinity，当前停在 loopback connect ECONNRESET；另有独立 libc daemon 探针通过，未宣称原 iperf daemon 模式完成 |
| iozone 吞吐 shmget ENOSYS | P4f SysV IPC 已交付 shmget/shmat/shmdt/shmctl，支持 IPC_PRIVATE 与命名 key、RMID 延迟销毁和 fork/exit 继承与清理，原阻塞解除 |
| iozone 自动模式能完成，慢写和日期异常 | P0/P7 分别复现时间 ABI 与写入成本；不是永久卡死证据 |
| lmbench /tmp/hello 启动错误仍有计时 | 原镜像 hello 脚本写死 `/code/lmbench_src/bin/build/lmbench_all`，根盘缺该目录；评测分支按 libc 链接镜像自带二进制，不能把原环境缺口归为通用 exec 失败 |
| glibc libctest static utime 一轮通过、一轮失败 | 原镜像 libc 的 time() 使用 CLOCK_REALTIME_COARSE，BoarOS 返回 EINVAL 后旧 libc 仍读取结果，导致比较不稳定；独立 realtime/UTIME_NOW/fstat 跨秒正确。本轮已补齐 coarse clock，原静态/动态 ELF 双侧各 30 次通过；作为已关闭缺口保留原因，见时间戳 learning |
| musl LTP、Lua、netperf 未到达 | 本轮没有独立能力结论，历史诊断与新基线分开 |

待定位项先交付独立复现与结论，再决定机制修改；不按分数猜测根因。

## 当前局限与证据边界

| 方向 | 已有基础 | 尚缺能力或尚未证明的结论 |
|---|---|---|
| IPC / 共享内存 | 共享匿名、统一后备对象、tmpfs/POSIX shm、匿名共享 futex、AF_UNIX/socketpair、SysV 共享内存 | SysV 信号量与消息队列尚无入口；AF_UNIX 命名端点/SCM_RIGHTS、共享文件 futex、PI 仍缺 |
| 文件与存储 | 页缓存、阈值写回、真实同步、日志恢复、多盘独立 owner | 范围写回顺序扫描与逐页提交；fdatasync 与 fsync 共用保守路径；无周期清脏、事务合并、预读或负目录项缓存 |
| 并发与调度 | 单 hart IRQ 等待、每盘八槽、读共享/写独占、FIFO/RR 及预算 | 单盘后端写事务串行；同 OFD 位置、命名空间和冲突 inode 互斥；八槽不代表每个应用都可产生八个并发请求；无 SMP/PI/硬实时 |
| 内存与信号 | demand paging、COW、fork、线程与标准信号 | mremap、按操作区分的 madvise、mlock、sigaltstack、实时信号队列未交付 |
| 系统环境与安全 | sysinfo/proc、会话、可信虚拟熵源、固定 root 查询 | klogctl、RTC 字符接口、完整凭据/权限、TTY、内核栈 guard、实板熵源仍缺；canary 不等于 guard |
| 网络与应用 | IPv4 loopback、原 socket entry、原 cyclictest C:10、固定离线 GCC | iperf 连接差异未归因，netperf 尚无独立结论；无真实网卡；更大 C/Rust 构建、完整 libc/LTP 未证明 |

`busybox.official` 是唯一未通过的顶层清单案例，但内部为 **53/55**：dmesg 与 hwclock
失败。Linux 同脚本 55/55；shell exit 0 不代表断言通过。df 根盘展示另有内容缺口。
这不缩减上表范围，也不表示当前 1091 条 ABI 覆盖所有 Linux 接口。

## 后续开发顺序与进入条件

保持用户态兼容性优先；以下是建议排期与验收边界，尚未批准任何新子系统架构。
AF_UNIX/socketpair 与 SysV 共享内存已在本轮分别交付并闭环验证。存储成本诊断、
系统环境小闭环与真实消费者定位继续推进。

| 阶段 | 工作与最小交付 | 进入下一步的证据 |
|---|---|---|
| 已完成：IPC 复现与路线 | 固定 hackbench、iozone 原 ELF/参数并确认调用链；AF_UNIX 复用 socket endpoint/OFD，SysV 段表复用 memory_object，路线已选定 | 原失败、所需 syscall/flags 与后续阻塞记录在 N2/P4f；不把未到达调用标失败 |
| 已交付：IPC 与组合边界 | N2 的 AF_UNIX/socketpair 与 P4f 的 SysV shm 各自提交；本轮 R1/R2/R4/R6/R8 继续修复 owner、片段、权限和整包契约 | 生命周期、fork/exec/退出、fault/OOM、并发关闭/删除、ID 复用差分通过；原程序后续能力与本轮回归分别记证据 |
| 同期诊断：存储成本 | 对 iozone 慢写和小写同步建立计数分解，先分离缓存接收、写回、journal/flush、扫描和锁等待 | 固定负载/容量/缓存/同步语义；因果证据达到下节门槛后才选优化，不以 QEMU 墙钟倍数定瓶颈 |
| 随后：系统环境小闭环 | P5c 分别设计真实日志读取、RTC 字符节点；单独核实 df 根盘来源展示 | 原 BusyBox 55 子项及实际内容；dmesg 来自真实日志，hwclock 来自设备读数；不得改包装器或用空成功 |
| 持续：真实消费者诊断 | iperf/netperf 使用受控监听端、网络配置和固定参数；iozone 日期、其余 libc/LTP 单独复现 | 区分启动环境、无监听、ABI、协议和预算；cyclictest 的 mlock、LTP 的 chown/cgroup 各自归属，不能由一次 ENOSYS 推出完整子系统范围 |
| 后续兼容性 | P2c 信号栈/实时队列，P4d 文件 futex、P4e mremap/madvise 与锁页需求，凭据/TTY 按消费者立项 | 先固定真实触发与所有权，特别是取消、exec、截断和限额；不因已有基础而勾选完成 |
| 新基线之后 | SMP、LoongArch、实板、更大 C/Rust 工程各立里程碑 | SMP 先跨核 owner/TLB，LoongArch 先真实 U-mode，复杂工程先单核正确性；已有单核 I/O 不能冒充 SMP 安全 |

### 存储成本调查与优化门槛

当前源码入口：`fs/files/io.c`、`fs/page_cache.c`、`fs/vfs.c`、`fs/lwext4_port.c`、
`arch/riscv/virtio_mmio_block.c`；机制/锁边界见[VFS/ext4](modules/vfs-ext4.md)。
固定 Linux `mm/filemap.c`、`fs/fs-writeback.c`、`fs/sync.c` 仅作语义与机制对照，
不能据 Linux 架构或 BoarOS 静态锁数量直接推定耗时比例。

- [ ] 建立普通 write、O_SYNC/O_DSYNC、每次/每批 fsync/fdatasync、共享映射+msync 对照；包含小写/页写、顺序/随机、覆盖/扩展、同 inode/不同 inode、单盘/双盘、冷/热缓存与压力状态。
- [ ] 分别计数用户复制和页解析、缓存命中/扫描/快照、实际数据/metadata 写入、事务/flush、设备请求字节与在途深度、队列等待和各层锁持有/等待；计时区分客体运行与 I/O 等待，不把宿主暂停计作内核计算。
- [ ] 以已通过的 1 MiB 写入 256 分块/256 页解析、16→64 MiB resident 探测增长 3.95 倍作为既有成本边界；新负载另测，不外推已有数字。
- [ ] 根据测量选择一次可归因改动，保留无收益结果；周期清脏会改变策略，不能以“优化”名义默默加入已明确排除的功能。
- [ ] 涉及写回/事务/队列时重跑错误 owner、退出卸载、双盘隔离与 SQLite DELETE/WAL 完整恢复矩阵；按新事件轨迹枚举，不削弱 flush 契约换成绩。

### 既有 IPC 路线与待确认的存储候选

| 问题 | 候选比较与当前路线 |
|---|---|
| SysV shm 对象 | 已采用专用 key/id 表与 segment owner，复用 memory_object，并在 R2 增加稳定 attachment。匿名内存文件路线可复用文件接口，但增加隐藏挂载、名字与 fd 生命周期隔离成本，未采用。 |
| AF_UNIX/socketpair | 已采用 socket endpoint 内的缓冲/等待原语；R4 单独维护整包 owner 与预算。独立本地传输后端便于以后名字空间/SCM_RIGHTS，但增加 endpoint 交接接口，未采用。 |
| 存储瓶颈 | ① 若扫描占比高，增加脏页/范围索引，成本是索引内存与失效一致性；② 若小请求/屏障放大占比高，批量数据提交或事务合并，收益需覆盖错误归属与恢复复杂度；③ 若锁等待主导，缩小路径/inode/实例临界区，需新增 pin/版本复查而非单纯删锁。按数据选，不能预先三项全做。 |

SysV/UNIX 固定语义入口分别为本页 Linux commit 的 `ipc/shm.c`、`ipc/util.c` 与
`net/unix/af_unix.c`。既定路线内的小修继续验证；新的结构性存储优化仍须先测量，再由维护者确认路线。
水位/预算不承诺任意分配成功或硬实时；push、发布、评测分支交接与正式评分仍由维护者决定。

## 已完成能力的证据入口

P0a 文档职责、P1a root 查询、P1b–g 持久存储/路径/时间/统计已经落地；
P0b 时序复跑已完成，但历史取消异常根因仍未关闭，见[程序清单](learning/user-program-inventory.md)。
P2a robust-list 与 P4a/d 共享匿名/futex 见[线程证据](learning/threads-and-futex.md)；
P3/P4 记录锁、共享文件、DELETE/WAL 见[恢复证据](learning/record-lock-sqlite-recovery.md)
和[内存管理](learning/memory-management.md)。完整历史身份保留在 learning，
下列旧 build 路径仅表示历史运行位置，不能作为唯一证据。

### 单核规模能力

UDP 大包 read/readv 的内部 256 字节截断已修复；普通文件/TCP 请求使用有任务 owner 的页级缓冲。驻留文件页新增动态地址哈希，首次写/写回 rearm 使用单页改权及地址级本地失效。成本门禁为 `make test-scale-riscv`，复现与成本定义见[单核规模回归](learning/single-hart-scale.md)。

PR 增加规模测试、SQLite DELETE/WAL 和离线 GCC；完整恢复矩阵每周一北京时间 02:00 或手动触发。glibc 的固定本机工具/runtime 哈希仍是本地严格收口门禁；可移植固定输入供应尚待单独交付。单 hart 可睡眠 I/O 已接入 IRQ、完成/超时/reset、DMA 停止与 VFS/页缓存 owner 协议，交付证据见[可睡眠存储](learning/sleepable-storage.md)。本轮不推进共享文件 futex、更多 socket ABI、批量事务或 SMP。

### 主要依赖

```text
P0 证据 ─┬─ P1 路径/文件/设备 ─┬─ P3 同步/锁/SQLite 回滚日志
         │                     └─ N socket/loopback/设备网络
         ├─ P2 robust/信号/线程/资源
         └─ L LoongArch 最小入口 → 双架构与实板

P4a 共享匿名对象已完成，提供稳定后备身份
P1 文件身份 + P3 写回/错误协议 → P4b/c 共享文件页与 msync
P4 文件共享页 + P3 锁/同步 → SQLite 普通多进程 WAL
P4d 共享 futex 扩展独立推进，不作 WAL 统一前置条件
P1–P4 按真实依赖 → P5 glibc/单核编译完整交付（试跑可提前）
P2/P4 单 hart 契约 + IPI/IRQ 入口 → P6 SMP 正确性
P5 + P6 → P7 多核编译与性能；P7 + N + L → P8 平台交付
```

箭头表示主要验收依赖，不禁止先做块 flush、试跑 SQLite/编译器或启动第二架构。网络不依赖多核编译；匿名共享不必等文件持久化；2 hart 启动不等于 SMP 已完成。

## Review P-A–P-E：成本核实与测量门槛

这些是源码成本结构，尚无本轮延迟/吞吐收益结论。deadline 结构、选择性唤醒和驻留范围索引重构继续等待测量与路线确认。已有 scale 的页解析/PTE 访问计数不能代替以下各层总成本。

| 项目 | 当前核实 | 待测工作负载和验收门槛 |
|---|---|---|
| P-A deadline | `kernel/sched/wait.c::kernel_scheduler_expire_deadlines` 每 tick 遍历全部 blocked，包括无 deadline 的等待者 | 固定少量到期任务，增加无期限 blocked；计扫描节点、最大 IRQ 占用、到期偏差。保持超时/取消一致后，才比较独立定时结构。 |
| P-B 改权 | `arch/riscv/mm.c::kernel_mm_mprotect` 仍遍历全部 file_residents；R6 权限上限检查已限定到重叠 VMA，数组编辑/合并成本仍在 | 固定单页目标，增加无关 VMA/驻留页；把 prepare/commit、resident 与 PTE 访问都计入，禁止只报告目标页计数。先获得总探测数，再选择范围索引。 |
| P-C 唤醒 | `kernel/sched/sync.c::kernel_lock_release` 在可用时 wake_all，等待写者阻止新读者；新写门闩复用这一机制 | 独立 inode 与同 inode 混合读写，计有效唤醒、再次睡眠、切换、等待分位与最长饥饿。保持取消/超时与进展，不直接改 wake_one。 |
| P-D 小写 | 普通文件/TCP 非零请求仍分配整页 staging；AF_UNIX DGRAM 已改整包 heap owner，有界为 64 KiB | 1/3/63/64/65/4096 字节 × 缓存冷/热，计每调用物理/heap 分配、复制字节和峰值所有权。优化后须保留 fault/OOM/退出清理与消息原子性。 |
| P-E 延迟 | `arch/riscv/trap_entry.S` 至用户返回保持 SIE 关闭；缓存命中的大复制/扫描可能无睡眠点 | 固定大复制/改权与独立唤醒任务，计 irq-off 最大值、锁持有/等待、唤醒到运行延迟和切换。先明确重入约束，不随意开中断/yield，不承诺硬实时。 |

下列 C0–C6 已交付成本诊断和真实消费者状态；受控成本证据不能代替消费者兼容通过或正式评测。

## 本轮：成本测量优先（2026-09-30 验收）

维护者已选择先量化写入、扫描、唤醒和延迟，再决定优化。目标是得到能归因、可重建的
成本基线，保护 R1–R8 已验收的行为；最终完成 20 配置、60 串行独立启动和 339 观测窗口，见[最终报告与归档](learning/cost-baseline.md)。C0–C5 受控负载验收通过；C6 完整记录原消费者的完成、不可用、超时和启动拒绝，但实际旧 glibc I/O 测量及超时归因未完成，C6 尚未完全验收，未将这些阻塞算作兼容通过。尚无新的结构优化。
起点为本轮最终生产源码，内核 SHA-256
`0c0c6a77f54160c06f284f19133b6bb6516f7c6e2d8dcb5f0864390ce1361fbb`，
1091 条 ABI、四组合 io-sleep、scale 与 WAL 恢复证据见上文。

固定事实入口为 `references/linux` commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e`
的 `fs/read_write.c`、`mm/filemap.c`、`mm/mprotect.c`、`kernel/sched/`，及
`references/qemu` v11.1.0 commit `84f07211cc5b4fc6a371559bf8a5de4fb068e648` 的 VirtIO/NBD；
实际运行记录 QEMU 版本与哈希，不预设某台宿主的墙钟速度。

### 观测方式候选与边界

| 候选 | 正确性与演进 | 复杂度与观测成本 |
|---|---|---|
| ① 默认关闭的分层聚合统计，复用现有 MM/设备统计（推荐） | 在实际 syscall、MM、调度和存储边界计数；按 task/MM/inode/设备区分范围，切换时结算运行时间；不增加 Linux 用户 ABI | 热路径不分配、不打印、不等待；固定计数器/直方图占用可核对，必须验证默认关闭和观测开销；可逐层扩展 |
| ② 仅测试链接包装器及 NBD 外部计数 | 易复用 scale/io-sleep 故障探针，基本不改生产路径；可独立校验候选①的关键计数 | 看不到 static/inline 内部扫描和完整 trap 区间，独立生产 U-mode 的归因不完整；适合辅助检错 |
| ③ 有界事件环记录时间线 | 能还原复杂等待与嵌套关系，便于后续并发诊断 | 增加事件格式、丢失/溢出、采样和对象代次管理，扰动及存储成本更高；本阶段不建议先建通用追踪系统 |

维护者已确认并实施候选①配合候选②独立检错，接口与 owner/生命周期已验收；当前已确认的是测量优先的范围，
不是 deadline 索引、wake_one、驻留范围树或事务合并路线。关闭观测时不添加热路径工作；
开启时所有计数按真实操作递增，不能按测例 ID/固定输入切换语义。

输出同时记录请求字节、实际接受字节及各层复制/设备字节，错误和短写保留真实结果。
数据/metadata/journal 分类必须有实际后端来源依据；无法唯一分类的字节列为 unknown，
不按固定扇区号或测例输入猜测。峰值统计只记录本层实际 owner，避免把同一后备页的多个引用重复当容量。
层间嵌套时间不能直接相加当作总时间；运行、就绪等待、I/O 阻塞、主动暂扣响应分别标记。
计时使用客体单调时钟并记录分辨率；宿主暂停、QEMU 墙钟和调度 tick 不冒充指令执行成本。
按 hart 实际 SIE 跟踪连续 IRQ-off；SIE 保持关闭的上下文切换不截断，恢复中断后的睡眠不计入。采样段的前后盲区单列，不宣称硬件上界。

### C0：观测契约、固定输入与独立检错

入口为 `include/arch/riscv/mm.h::riscv_kernel_mm_get_statistics`、
`include/arch/riscv/virtio_mmio_block.h::riscv_virtio_mmio_block_get_statistics`、
`include/kernel/sync.h` 和 `kernel/sched/scheduling.c::scheduler_account_runtime`。
已新增 `tests/cost-riscv.py`、`tests/test-cost-report.py` 与 Makefile 的 `test-cost-riscv`
入口；C0 已交付这些入口。使用默认关闭的 `COST_DIAGNOSTICS=0`；开启时由 Makefile
给 C/assembly 同时传递 `BOAROS_COST_DIAGNOSTICS`，不能只给 C 加标志而漏掉 trap 入口。
诊断构建独立放 `build/cost/`，复用 `BUILD_DIR`、`KERNEL_RV` 和 `CFLAGS_EXTRA`，
配置进入缓存身份，避免覆盖普通构建或串用缓存。
runner 的 `--case`/Makefile 的 `COST_CASE` 为 contract/write/locking/mprotect/deadline/latency/consumer/all；
各任务只运行已交付的对应 case，未知或尚未实现 case 必须明确失败，最终 all 不得跳过缺项。

- [x] 已定义每项指标的单位、采样范围、owner、起止点、失败/取消记账和清零条件；运行时不重置仍在途的计数。
- [x] runner 在启动前快照 kernel/ELF/fixture；结果包含源码 tree、配置、工具与 QEMU 哈希、负载参数、冷/热准备方式和完整退出/资源基线。丢记录、负差值、溢出、错误 owner 或缺少结束标记必须失败。
- [x] 先写协议/计数红测：刻意破坏一个字段、缺一个阶段或交换 owner，报告不得通过。固定小负载由数据内容、完成字节和独立包装器验证计数，不锁定私有布局或偶然调用次序。
- [x] 验证诊断关闭后目标代码不引用新增观测入口；开启后不分配/睡眠/修改 errno，测量记录自身占用与开销，不能用计数器开销伪造被测路径成本。
- [x] `python3 -B tests/test-cost-report.py` 与 `make test-cost-riscv COST_CASE=contract`（三启动，各四窗口）通过；当前 Python discovery 不收集连字符文件，直接运行防止零测试假通过。任务新增 48 字节，元数据页断言与退出栈余量保持。最终聚合含预留 60947 字节，任务含 backend guard 增量总64字节，默认/观测栈检查通过。

### C1：写入与分配分解（P-D、存储成本）

修改范围为 `fs/files/io.c::write_request`/`buffered_write_request`、
`arch/riscv/uaccess.c`、`fs/page_cache.c::writeback_entry`/`kernel_page_cache_writeback_range`、
`fs/ext4_backend.c`、`fs/lwext4_port.c`、`arch/riscv/virtio_mmio_block.c`；
扩展 `tests/riscv/scale_main.c`，新增真实 U-mode `tests/workloads/cost/write.c`。

- [x] 先核对 0/1/3/63/64/65/4096 字节和对齐/错位 1 MiB：记录 staging/后备页/heap 分配、用户页解析、复制、缓存探测、脏范围、快照、数据/metadata/journal/flush、队列深度与等待；区分普通文件、TCP、AF_UNIX DGRAM。
- [x] 用普通 write、O_SYNC、O_DSYNC、每次/每 16 次/结束时 fsync 或 fdatasync，以及共享映射+msync 分组对照；分别改变顺序/随机、覆盖/扩展、冷/热和压力条件，不把全部因素一次混成笛卡尔积。
- [x] 每个基准配置使用三个独立启动副本；内容、offset、大小、短写、EFAULT/EIO/OOM 和资源基线先验收，再保存计数和客体时间分布。既有 1 MiB 256 分块/页解析门槛继续独立保护。
- [x] 窄验证 `make test-scale-riscv test-files-partial-write-riscv`，再 `make test-cost-riscv COST_CASE=write` 与 `make test-userland-riscv`；交付各层放大比和错误路径结果，不预先宣称小写分配或 flush 是主瓶颈。

C1 阶段证据：modern/writeback 的观测开/关各三个独立启动，36 窗口完整，
独立 usercopy/page-resolution/device-stat wrapper 通过，默认 scale、partial-write、musl/pthread 通过。
小写 staging 放大、同步频度快照和 guest 时间分布见[成本基线](learning/cost-baseline.md)。
压力/取消的四组合观测已随 C2 的 io-sleep fixture 补齐，尚不能由 C1 结果关闭存储成本总任务。

### C2：整次写门闩、锁等待与唤醒（P-C、R7 成本）

入口为 `kernel/sched/sync.c::acquire`/`kernel_lock_release`、`kernel/sched/wait.c`、
`fs/vfs.c` 和 `tests/riscv/io_sleep_main.c`，复用 C1 的实际 U-mode 写负载。

- [x] 分 rank 10/15/20/30/40 记录取得次数、等待/持有时间、阻塞次数、wake_all 唤醒数、重新阻塞、成功取得和切换；固定直方图需同时输出边界和样本数，不报虚假的精确分位数。
- [x] 对照同 OFD、独立 OFD 同 inode、不同 inode、单盘/双盘；等待者为 1/8/32，读写混合。覆盖第二块冷页 usercopy、同 inode 映射输入、O_SYNC、截断和取消，观察独立 inode 的进展。
- [x] 用既有暂扣/放行握手把受控 I/O 等待和缓存热写分开；核对 wake/重阻塞计数可检出无效唤醒，终止后 pin、锁和缓冲归零，最长观测等待不冒充无饥饿保证。
- [x] `make test-cost-riscv COST_CASE=locking` 与 `make test-io-sleep-riscv test-files-partial-write-riscv test-userland-riscv` 通过 legacy/modern × writeback/writethrough；交付门闩自身等待与后端串行成本的分解，本任务不修改公平策略。

### C3：单页改权的总扫描成本（P-B）

入口为 `arch/riscv/mm.c::kernel_mm_mprotect`、`mm/vma.c`、
`arch/riscv/sv39.c`、`tests/riscv/scale_main.c` 与 `tests/riscv/vma_cases.c`。

- [x] 固定一页改权目标，分别增加无关 VMA 16/64/256 和文件驻留页 0/16/64 MiB；记录查询、prepare/commit、VMA 编辑/合并、file_residents 扫描、PTE 与 TLB 失效。
- [x] 全范围失败、洞、只读 SHM/共享文件上限拒绝、split/fork、NONE 降权恢复、OOM 仍保持原契约；统计失败路径不得触发新的 PTE 修改或 owner 变化。
- [x] `make test-cost-riscv COST_CASE=mprotect` 与 `make test-vma-riscv test-scale-riscv test-diff-abi-riscv`；报告总访问次数随无关集合增长的关系，不能用每目标页三级 PTE 访问掩盖外围扫描。

### C4：deadline 遍历与到期偏差（P-A）

入口为 `kernel/sched/wait.c::kernel_scheduler_expire_deadlines`、队列校验、
`kernel/sched/core.c`、`tests/riscv/scheduler_cases.c` 和 `tests/riscv/io_sleep_main.c`。

- [x] 固定 4 个带 deadline 的任务，另建 0/32/128/256 个无期限 blocked；分别计队列校验和 deadline 循环访问、tick 整条路径耗时、实际到期唤醒与运行偏差。
- [x] 覆盖同期限、提前普通唤醒、信号/取消、退出与对象复用；握手确认任务已 blocked 后再开始采样，不用宿主 sleep 推断调度状态。
- [x] `make test-cost-riscv COST_CASE=deadline` 与 `make test-scheduler-cases-riscv test-io-sleep-riscv test-sched-policy-host test-sched-bandwidth-riscv`；得到固定 deadline 数下的增长曲线，仍保留遍历机制至证据支持结构选择。

### C5：连续关中断与唤醒到运行（P-E）

入口为 `arch/riscv/trap_entry.S`、`include/arch/riscv/context.h`、
`kernel/sched/scheduling.c`、`arch/riscv/trap.c` 与 `tests/workloads/cost/write.c`。
该任务依赖 C0 的观测契约及 C2–C4 的运行/等待区分。

- [x] 嵌套 save/restore、用户 trap、异常返回、调度睡眠/恢复、idle 分别核对观测起止；采样本身不递归取得锁、开启中断或分配，不漏掉 assembly 前后区间。
- [x] 固定热缓存 4 KiB/64 KiB/1 MiB 复制和单页/大范围改权，配独立唤醒任务；报告 IRQ-off 最大区间、锁持有、唤醒到运行分布与切换，不按 QEMU 墙钟设性能及格线。
- [x] `make test-cost-riscv COST_CASE=latency` 与 `make test-trap-riscv test-trap-return-riscv test-context-riscv test-user-riscv test-io-sleep-riscv test-sched-bandwidth-riscv test-stack-usage`；运行观测开/关对照。本任务不引入任意 yield 或 syscall 内开中断。

### C6：真实消费者、归因与收口（尚未完全验收）

复用 `tests/runtime-diagnostics.py`、固定原镜像的 iozone 以及 C0 runner；
最终证据归 `docs/learning/cost-baseline.md`/`cost-measurements.json`，规模/存储文档链接该结论，契约归对应模块，
能力/后续依赖分别更新 README 与本页，不另建永久计划或原始日志档案。

- [ ] 固定原 iozone ELF/依赖/镜像哈希、argv、工作目录、文件规模、缓存与同步条件；先核实实际调用链，再用 C1–C5 的受控负载解释其观测，未执行或超时保留原始状态。输入身份与逐命令记录已完成；原旧 glibc 实际 I/O 成本及 musl 超时归因尚未完成，不能把采样完成当作该项全部验收。
- [x] `make test-cost-riscv COST_CASE=consumer` 与最终 `make test-cost-riscv COST_CASE=all` 输出全部阶段、输入身份和观测开/关对照，三个独立启动副本均有完整结果。
- [x] 汇总每项主成本与未解释余量，报告重复分布、观测开销和无收益结果；只有因果对照支持时，提出 2–3 个对应瓶颈的候选，由维护者选择一次优化。
- [x] 测量代码收口运行 `make test-riscv test-userland-riscv test-glibc-riscv test-diff-abi-riscv test-scale-riscv test-io-sleep-riscv test-stack-usage`，以及 SQLite DELETE/WAL 正常与选定错误恢复。后续实际修改写回、事务或队列时再跑两种完整恢复矩阵与双盘隔离。
- [x] 每个 C 任务独立验证/提交，使用 `Co-authored-by: GPT-6.1 Sol <codex@openai.com>`；固定输入和结论入 Git 后 `python3 -B tests/prune-build.py`、`make prune-build`。完整 Harness 的 kernel-la 阻塞继续单列；最终独立审查无新增 P1/P2，核对后已清理50个临时路径，保留cost与固定Linux可复用缓存。

依赖为 C0 → C1/C3/C4，C1 → C2，C2/C3/C4 → C5，全部测量 → C6。下一轮的结构优化需从报告中三个候选重新确认。

- [ ] 兼容分支单向合入本轮已验收 main 后重跑原 glibc/musl iozone 与原 judge；`oscomp-rv-compat@3dcfe77` 已有 uname 4.15.0 适配，本轮未更新该分支，也未改 main uname。main 的原 glibc 启动拒绝不提供实际 I/O 成本。
- [ ] 原 iozone (11,12) 所选测试在固定 Linux 也不可用，不计作向量 ABI 验收；其余 timeout 不据短预算判定永久卡死。完整 Harness 的缺少 kernel-la 阻塞保留。
日志/RTC、SysV RMID 后再 attach 的既有兼容限制和 iperf/netperf 定位保留后续队列；
本阶段不扩展 SMP、LoongArch、deadline 索引、选择性唤醒、驻留范围索引或持久化策略。

## P0：固定证据与时序问题

**入口**：`tests/program-inventory/{inputs.json,run.py,suites.py,reports.py}`、`tests/diff-abi/`、`.github/workflows/ci.yml`、[程序清单](learning/user-program-inventory.md)。2026-09-23 基线的 228 项为 223 pass、2 个直接 entry 退出不符、3 个包装失败；2026-09-27 整合内核为 227 pass、1 个 BusyBox 包装失败。静态/动态和脚本重叠不重复计算缺陷。

### P0c 时序根因闭环

- [ ] 固定 kernel/ELF/loader/fixture/runner/QEMU 身份与次数，分别复跑静态、动态取消 entry 和原脚本；保存双侧 stdout/stderr、真实 wait status、超时和第一个失败。先区分 reference-not-pass、setup-error、脚本顺序依赖。
- [x] 在固定构建输入上重复运行静态/动态取消直接 entry 各 30 次：Linux 与 BoarOS 均 30/30 通过，原始逐次证据和身份在 `build/cancel-repeat-20260923/`；未复现不等于旧异常已定位，原脚本复跑和最小因果序列继续保留在上条待办。
- [ ] 取消异常缩成可独立运行的 shm_open、取消登记、阻塞、join/clear_tid 序列；确认取消点前后状态，不让错误诊断中的 write 再次隐藏原错误。用同步事件安排顺序，不靠随意 sleep 假定先后。
- [x] P1c 后以同步握手确认独立后台子进程进入目标阶段，再执行 kill/回收，固定重复 20 轮均成功；原 BusyBox 包装器中的 sleep+kill 子项也为 20/20，包装器其他缺口继续单列，未据此关闭历史取消异常。
- [ ] 若定位到支持范围内的错误，先加入能证伪旧实现的最小回归，再修所属模块；记录失败前和修复后的重复次数。一次转绿不能关闭未知根因。

### P0d 持续证据与清单维护

- [x] 本轮证据保留工作树内核、ELF/loader、fixture、runner、QEMU 与固定 Linux/BusyBox/libc-test 身份；旧基线只保留历史定位用途。
- [x] 原七类直接失败按设备/身份、cwd、时间设置、文件系统统计、robust、socket 聚类；设备、cwd、显式时间、统计、robust 与 socket 静态/动态直接 entry 已关闭。整合内核的 228 项清单为 227 pass、1 upstream-failure，余下 BusyBox 官方包装脚本；不按非零退出码猜 syscall。
- [x] 已有硬回归严格通过；能力清单如实保存缺口。全量严格验收复用 `--require-pass`，不重建状态系统或跳过失败；相关静态/动态 entry、原包装器与 228 项清单均已重跑。
- [x] 限定集合的 `--require-pass` 只严格判定本次 selection，未选项目保持历史状态或 `not-run`；未知 ID、所选失败/未完成、中断、参考侧失败与全量严格模式均有 runner 回归。

**验收**：每个新增任务都有可证伪输入、有效 Linux 参考和归属机制；历史原始失败保留，未运行项目不计通过。不要求 P0 达到 228 项全绿。

## P1：路径、设备与文件元数据

**入口**：[文件模块](modules/kernel-files.md)、[VFS/ext4](modules/vfs-ext4.md)；`include/kernel/{vfs.h,open_file.h,fs_context.h}`、`fs/{vfs.c,open_file.c,fs_context.c}`、`fs/files/{path.c,metadata.c,io.c,table.c,poll.c,epoll.c}`、`kernel/syscall/{file.c,dispatch.c}`。

### P1b 最小对象与操作边界（最小 mount/路径对象路线已完成）

- [x] 文件身份使用 mount+inode，路径对象另持父目录项；OFD 持共享 flags/offset/引用，fd 槽持访问与 CLOEXEC。open→pin/dup/fork→close→末引用回收均有聚焦回归。
- [x] ext4 适配器提供按目录 inode 查找、按 inode 打开和读链接；字符节点按 `rdev` 选择后端，lwext4 类型未泄露到 task/syscall。普通文件、pipe、epoll 保留统一分派下的各自语义。
- [x] root/cwd、普通文件、目录和字符节点均持真实引用；unlink/rmdir 后旧对象存活，同名重建获得新 inode，外部引用使卸载返回忙。rename 子集已由 P1g 交付。
- [x] 发布流程按路径→后端→OFD→fd 逐级取得 owner；分配/打开/fd 满与 close/dup/fork/阻塞 pin 有失败和回收检查。ext4 close 失败转交 mount cleanup node，非法释放继续 fatal。

**交付**：一个可复用的 ext4/设备纵向入口及所有权说明；拟新增 `fs/path_walk.c`、`fs/mount.c` 等仅在职责确定时拆分，不要求一次重做 Linux dcache/RCU。

### P1c 真实设备文件

- [x] 统一路径/OFD 后端可打开 `/dev/null`、`/dev/zero`、`/dev/console`；身份来自 ext4 字符节点与 `rdev`，未知设备号返回 `ENXIO`，初始标准 fd 复用 console 实现。
- [x] null/zero/console 的读写、非阻塞、信号打断、零长度、坏指针、跨页部分 fault、向量与定位 I/O 均由真实 U-mode 或固定 Linux 差分验证。
- [x] 设备 `st_rdev`、seek、未知 ioctl、poll/epoll、访问模式与 `O_TRUNC` 已核对；OFD pin、dup/fork/close/退出及分配失败回到资源基线。
- [x] 原静态/动态 `stat`、`syscall_sign_extend` 已通过；后台 sleep+kill 同步重复 20 轮成功。daemon 的 cwd 阻塞后由 P1d 解除，P1e/f 收口时静态/动态原始 entry 均已通过；完整 session/TTY 仍归 P2d。

### P1d cwd 与目录 fd

- [x] 实现目录 fd 起点及 `chdir/fchdir/getcwd`；无效 fd 与非目录 fd 分开，绝对路径忽略坏 dirfd，空路径/尾斜线/过长路径和缓冲不足按固定契约返回。
- [x] 按分量处理 `.`/`..`、相对和绝对 symlink、循环上限与 mount 边界；保留 `symlinkat/readlinkat/O_NOFOLLOW/lstat`。不能先把整个字符串折叠再跟随 symlink。
- [x] fork 拥有独立 fs context，CLONE_FS 共享同一上下文，exec 保留；切换 cwd 时先取得新引用再提交，失败保持旧 cwd，退出释放准确 owner。
- [x] 测试不同任务/线程的 cwd 可见性、相对 symlink+`..`、打开目录后 rename 再 openat、unlink 后仍持目录句柄/getcwd 的行为。rename 完成前先建立身份契约，相应组合用例在 P1g 一起关闭。

### P1e 显式时间设置

- [x] 在活 inode 上实现 `utimensat` 与 libc `futimens` 所需入口；复用已有 realtime、relatime 和纳秒编码，不建立旁路时间戳缓存。
- [x] 对照固定 Linux 的 NOW/OMIT、空 times、非法纳秒、两项 OMIT、路径/nofollow、fd/dirfd 和权限检查顺序；保留未修改字段，ctime 的变化也必须符合契约。
- [x] 覆盖只读挂载、坏指针/跨页导入、无效 fd、不存在路径及真实写失败；明确元数据已变更后 I/O 错误的归属，不伪造回滚或错误成功。
- [x] 受控时钟测试与真实 Linux 差分共同验证；重跑原 `utime` 静态/动态及 BusyBox touch，保持 create/read/write/truncate/unlink 时间回归。

上述勾选对应程序清单的固定输入；2026-09-29 原 RV 评测镜像的 glibc `utime`
另出现时间比较失败；双侧原 ELF 与原 libc 探针已定位为 CLOCK_REALTIME_COARSE 缺失导致旧 time() 使用未初始化值。诊断已闭环，能力补齐仍待后续；详见文件时间 learning。

### P1f 文件系统统计

- [x] 从挂载和真实 ext4 superblock/分配状态提供 `statfs/fstatfs`；核对目标架构结构、块大小、块/inode 总量与空闲量、名称限制和只读标志，不返回固定容量数字。
- [x] 以 mount 为统计 owner，fd 查询不依赖旧路径重查；定义与正在进行的分配/释放及失败 I/O 的一致性范围。
- [x] 对比文件分配/截断/删除前后计数，覆盖 sparse hole 不等于已分配块、只读、坏 fd/用户指针和卸载；重跑原 `statvfs` 两种 entry。

### P1g 链接、rename 与权限相关文件操作

- [ ] `linkat` 覆盖同 inode 身份、nlink、打开后删除、跨 mount EXDEV、目录限制与失败后原对象；符号链接跟随 flags 单独验证。
- [x] `renameat/renameat2` 支持普通/NOREPLACE；单事务文件/空目录覆盖、跨目录移动、祖先拒绝、同 inode、活目标及失败回滚，EXCHANGE/WHITEOUT 明确不支持。单根挂载的跨 mount 拒绝存在，真实多挂载验证归 P1h。
- [ ] `umask` 已按 fs context 的 fork 复制与 `CLONE_FS` 共享实现，真实 inode 的 `fchmod/fchmodat` 已覆盖；继续按消费者补 `faccessat` 的权限模型、合成 inode 的 chmod 与凭据依赖，与 P2e 保持一致，不能总返回允许。已有 open 未知 bits 拒绝策略另用差分核对，不能写成 Linux 通用要求。

### P1h 虚拟文件系统与多挂载

- [x] main 已补普通文件/字符节点 mknodat，复用 null/zero/console；块节点已供设备识别和 ext4 挂载使用；裸块 I/O、devfs 和 FIFO 后端仍未交付。见[节点创建](modules/kernel-files.md#节点创建)。

- [x] 通用后端、mount/path 生命周期、null/zero/console 设备后端与首批真实 procfs 已分阶段交付；内部覆盖挂载遮蔽、根和 `..`、忙卸载及引用回收，真实第二盘及 tmpfs 的组合验证见本阶段 learning。
- [x] tmpfs 与真实第二 ext4 挂载具备独立路径身份；内存文件有真实后备对象/目录生命周期、配额耗尽、截断及最后引用回收；不宣称持久化或共享文件 futex。
- [x] 最小 procfs 的 uptime、meminfo、self/exe、self/fd、进程状态及挂载信息直接读取内核对象；PID 代次、线程退出、组长存活边界和非组长 exec 经固定 Linux 差分。本轮另补 256 轮 fd 关闭复用压力；完整 Linux 字段仍未覆盖。
- [x] 统一内存 owner 快照提供 Cached/MemAvailable/Shmem/Buffers/Dirty/Writeback，RV64 sysinfo 接入真实任务数和负载；无 swap/slab 回收时才返回对应零值。原 BusyBox free 与 LTP 已复验。
- [x] 阈值驱动后台写回及低/高水位回收，专用快照页、64 槽批次、有限失败、join 退出与资源回收；不加入周期清脏，fsync 错误/flush 契约保持。见物理页/VFS 模块和内存 learning。
- [x] 多挂载覆盖路径跨越、根和 `..`、挂载点被引用、卸载忙、跨挂载文件操作和失败交接；设备/内存/磁盘错误保持所属 owner。真实双盘延迟/写/flush 失败隔离已独立验证。
- [ ] eventfd/timerfd 只在真实消费者提出需求后接统一 OFD，就绪、非阻塞、poll/epoll 和退出回收一起验收；signalfd 另依赖 P2c 队列。

**验证与退出**：先扩展 `tests/riscv/files_main.c` 等现有聚焦入口；当前入口为 `tests/userland/{namespace.h,metadata.h}` 与 `tests/diff-abi/{devices,namespace,metadata}.c`，均接入现有 runner。`make test-files-riscv test-vfs-riscv test-lwext4-host` → `test-userland-riscv`/`test-diff-abi-riscv`。设备、cwd、时间、统计分别解除对应真实程序阻塞，旧 ext4/pipe/epoll/COW/回收不退步；daemon 新出现的 setsid 依赖交给 P2d。

## P2：线程、futex、信号与资源

**依赖与入口**：可与 P1 独立推进；[调度模块](modules/kernel-scheduler.md)、[信号模块](modules/kernel-signal.md)、[时间模块](modules/kernel-time.md)；`kernel/sched/{process.c,futex.c,signal.c,wait.c,private.h}`、`kernel/syscall/{process.c,signal.c,time.c}`、`arch/riscv/signal.c`、`include/kernel/task.h`。不重写已有 pthread/普通 futex。

### P2a robust-list 退出协议（同步所有权路线已完成）

- [x] 增加 `set_robust_list/get_robust_list` 和每线程注册信息；RV64 长度、存活目标 TID、当前 root 凭据、输出故障顺序、坏地址注册与 fork/exec 生命周期有真实 U-mode 和固定 Linux 差分。
- [x] 同步清理发生在退出线程旧 MM 与原 TID 有效时；有界处理链、signed offset、pending、owner-died 和唤醒。普通退出、致命信号、exit_group 和 exec 清退成员共用退出调用链；存活 exec 线程使用换号前 TID。
- [x] 坏指针、循环链、非 owner、PI 标记、COW 与 OOM 的用户访问失败均有真实路径验证；2048 项上界和先读下一链接防止无限遍历及已释放页访问。robust 清理先于 clear_child_tid 和 MM 释放。
- [x] 未特改 musl 静态/动态 `pthread_robust_detach`、真实 pthread owner-died/consistent/不可恢复状态与原始 `SYS_exit` 探针均通过；PI 与跨 MM 共享语义不计入 P2a。

### P2b 等待、取消与 futex 扩展

- [ ] 写出比较、登记、睡眠、超时、signal、wake、requeue、clear_tid、组终止的状态转换与唯一队列 owner；保证 compare-and-block 不可分割，无漏唤醒/重复摘链。
- [x] `WAIT_BITSET/WAKE_BITSET`、掩码与绝对超时已由 glibc/差分验收。
- [ ] 按实际调用补 `CMP_REQUEUE/WAKE_OP`；每个 operation 单独核对参数宽度、bitset、比较失败、relative/absolute 和 CLOCK_REALTIME，未知/未支持操作不算完成。PI futex 后置。
- [ ] 保持无超时 WAIT 的 SA_RESTART、带超时 WAIT 的 EINTR 与无 handler restart 保持原 deadline；测试信号在登记前后到达、超时与 wake 交错、重启前用户字变化、哈希碰撞和同桶 requeue。
- [ ] 取消/exec/exit_group 让阻塞线程沿原栈释放 pin 的 OFD、等待节点及 MM 引用；不能直接删除仍执行的内核栈。共享匿名跨 MM key 已接入 P4d，单 hart 关中断仅是当前实现条件。

### P2c 替代栈与实时信号

- [ ] `sigaltstack` 覆盖配置/查询、边界和禁用、嵌套 handler、主栈耗尽后的执行、sigreturn、普通 fork 继承、CLONE_VM 且无 CLONE_VFORK 时禁用、exec 重置；错误用户栈产生规定 fault，不破坏内核栈。
- [ ] 实时信号用可拥有的队列项表示，与标准信号合并位分开；验证重复排队、顺序、目标线程/组、屏蔽/等待/handler 消费，以及退出线程的队列回收；成功 exec 保留存活线程/组仍有效的 pending，不把 handler 重置当成 pending 清空。
- [ ] 队列 OOM、用户复制失败和取消时明确是否已消费；在真实队列计费后实现 `RLIMIT_SIGPENDING`。signalfd 等待队列稳定且有 OFD 后端后再接。
- [ ] 保留已有 `rt_sigtimedwait`、stop/continue 和各 syscall 不同的 EINTR/SA_RESTART；对嵌套、超时、信号排队与线程退出运行固定重复回归。

替代栈与 exec 生命周期依据本页固定 Linux 的 `kernel/fork.c`、`fs/exec.c`，pending 保留同时与现有 [信号模块](modules/kernel-signal.md) 契约对齐。

### P2d 会话、进程组与 TTY

- [x] 统一身份对象承担 TID/TGID/PGID/SID 角色引用；已实现 setsid/setpgid/getpgid/getsid，105 条差分覆盖组长、父子/exec、zombie、组信号与等待、身份继续存活及孤儿组。见[线程证据](learning/threads-and-futex.md)。
- [x] 原始消费者验证 daemon 的 SID/PGID 实际变化；孤儿组按固定 Linux 的退出/收养触发 HUP/CONT，SA_SIGINFO 与 sigwait 均保留 SI_KERNEL。TTY 行为未扩展。见[消费者诊断](learning/session-consumers.md)。
- [ ] TTY 对象出现后接控制终端、前台组、作业控制与终端信号；无 TTY 阶段不能宣称交互 shell 作业控制完整。对象末引用与关闭唤醒分别验收。

### P2e clone、凭据与资源限制

- [x] child-TID 生命周期（main）：私有 fork 的首次 SETTID、按活跃 MM 使用者判断清零、vfork 成功/失败 exec 和线程唤醒。新增 8 条同 ELF 差分，完整 556/556；MM/exec/user 聚焦通过。基线 `fa38845` 的三处差异与交接检查见[线程证据](learning/threads-and-futex.md#child-tid-生命周期差分2026-09-28)。

- [ ] 逐项扩大合法 clone 组合，区分 flags 依赖错误与当前未支持组合；核对 MM/files/fs/disposition 各自复制/共享，TLS、TID 发布失败、组长先退、非组长 exec、收养和 wait 仍正确。
- [ ] 在 P1a 查询基础上设计显式凭据：真实/有效/保存 UID/GID、补充组及权限消费者；确认复制/共享/变更/exec 所有权后再支持 set 类接口。统一替换当前 root 假设，不出现查询身份与资源访问权限脱节。
- [ ] 保持 NOFILE/STACK：调低不关闭旧 fd、线程组共享、fork 继承、exec 保留、exec 后调高栈和非页对齐限制；用户输出 EFAULT 不回滚已生效的 prlimit 设置。
- [ ] 按需求增加 AS/DATA/FSIZE/CPU，分别说明记账 owner、生效点、越限 errno/信号和恢复；AS 是虚拟区间而非驻留页，VMA 合并/切分/mmap/brk/mremap 与 lazy allocation 保持一致。
- [ ] 先核对固定 Linux 对 RSS/LOCKS 的非执行语义、NPROC 的真实 UID 和特权例外，再决定支持范围；不机械地对每个历史枚举加限额，也不把有效但未支持资源成功空返回。

**验证与退出**：robust 已由 `tests/userland/pthread.c` 的原始退出/真实 mutex 消费者、`tests/riscv/uaccess_oom.S`、`tests/diff-abi/robust.c` 和静态/动态原 entry 验收；聚焦 `make test-scheduler-cases-riscv test-signal-riscv test-syscall-riscv`、`test-userland-riscv`、329 条差分、`test-riscv` 与栈检查均通过。拟新增 `tests/userland/signal_stress.c`、`tests/diff-abi/futex_signal.c` 仍属后续；旧取消异常继续 P0c，不以全部当前 pthread 通过冒充共享 futex 或 SMP 完成。

### P2f 单 hart 调度 ABI

- [x] OTHER/FIFO/RR、实时优先级、CPU0 affinity、RESET_ON_FORK 与真实查询；23 条策略差分，5 条 proc stat 差分。
- [x] 全局实时带宽默认 1 秒 / 950 毫秒，proc 参数原子更新并保留消费；36 条控制差分，真实 U-mode 验证节流、恢复、RR 余片与同时耗尽轮转。见[调度学习](learning/kernel-scheduling.md)。
- [ ] nice 权重、PI、SMP 和硬实时保证未交付；后续需求先独立比较方案。

## P3：同步持久化、文件锁与 SQLite

**依赖与入口**：文件/目录同步依赖稳定 VFS/OFD，块层可先做；`include/kernel/block.h`、`kernel/block.c`、`arch/riscv/virtio_mmio_block.c`、`fs/{vfs.c,page_cache.c}`、`fs/files/io.c`、`kernel/syscall/file.c`、`fs/lwext4_config/generated/ext4_config.h`。涉及 lwext4 变更时维护来源、许可证与本地 patch 记录。

### P3a 块设备 flush（已完成）

- [x] 块层已有缓存模式与 flush 回调；VirtIO legacy/modern 在 writeback/writethrough 四种组合通过真实请求测试。`make test-block-host` 验证能力拒绝、错误传播和可复用的易失缓存/稳定镜像故障模型；journal 与文件系统恢复证据另见 P3d。

- [x] 对照固定设备资料定义能力描述、缓存属性和 flush 请求：协商支持、提交、完成、I/O 错误、超时和只读边界；两种 VirtIO MMIO transport 都验证，不能把内存 fence 当介质同步。
- [x] 定义请求/描述符/bounce buffer 的 owner；超时/reset 后必须确认 DMA 不再访问，才能释放或复用请求内存。不可恢复设备错误由所属设备/mount 保留明确状态。
- [x] 无 flush 能力时依据设备真实缓存契约返回结果；不能忽略能力并承诺不可满足的持久性。测试正常完成、拒绝/失败、超时与 reset 回收。

### P3b fsync、fdatasync 与目录同步（已选逐 inode 路线）

- [x] 明确调用覆盖的文件数据、必要元数据、目录项和设备缓存；从 syscall/OFD 接到 ext4 提交与块 flush，顺序和成功承诺可解释。目录 fsync 单独验收。
- [x] 已选择并实现 inode 缓存页索引、脏范围/修改代次、定向写回及 OFD 错误观察；独立 open 与 dup、关闭后脏 owner、O_SYNC/O_DSYNC、真实 musl 和 260 条 Linux 差分验证。journal 事务依赖已接入 P3d。
- [x] 定义延迟错误属于哪个 mount/文件，以及后续哪些调用观察它；用户错误、真实写/flush 错误和内核不变量分开。关闭先摘 fd，不让历史 cleanup 改变当前有效 close 结果。
- [x] 同步链建立后才接 `O_SYNC/O_DSYNC`；覆盖普通/部分写、metadata 必要性、无效 fd、只读/设备及重复同步。新进程/重启看到已承诺内容只是正常路径证据，不能单独证明崩溃一致性。

### P3c 记录锁

- [x] 传统锁按共享 `kernel_files_record`，OFD 锁按打开文件对象；inode 增广 AVL 与 owner 索引支持六个命令、负长度/EOF、合并/拆分和跨类型冲突。独立区间模型核对随机操作、树不变量、稀疏/重叠规模及 OOM 不变性。
- [x] 固定 Linux 同 ELF 覆盖独立 open、dup/覆盖、fork、exec/CLOEXEC、任意相关 fd close、unlink、pthread 共享 owner、等待时 fd 关闭复用及组退出；末引用与清理 owner 仍由原文件层负责。
- [x] 阻塞、唤醒、信号打断/`SA_RESTART` 和有限传统锁死锁检测与固定 Linux 差分一致；等待请求 pin 与组强制退出的静态 musl 负载回到资源基线。OFD 不承诺死锁检测；所有结果限于单 hart。

### P3d ext4 journal 与恢复（已验收）

- [x] 已选择并启用 ordered journal/replay；事务 before-image、flush 顺序、checksum v2/v3 和 revoke、superblock 恢复、持久 orphan_file/传统链及分批 extent/间接块回收已接入生产。
- [x] 元数据事务提交与 checkpoint 分阶段 flush；关键日志错误 sticky，损坏或只读无法恢复时拒绝开放用户访问。已知日志提交前的资源不足可安全回滚，不能据此清除不确定 I/O 错误。
- [x] `make test-lwext4-recovery-host` 使用 512 字节原子写、易失缓存/稳定镜像，覆盖写与 flush 失败、丢失/重排、两次恢复和 e2fsck；完整 orphan 回收矩阵为 16 种组合、4,362 次断电执行。QEMU 正常退出和未验收实板不属于该恢复证据。
- [x] 真实 musl 已验收“临时文件→fsync→rename→两侧父目录 fsync”应用序列，后端 rename 另有断电/重排矩阵；记录锁与 SQLite DELETE 见 P3c/e，共享文件映射见 P4，IPv4 loopback 首切片见 N。

### P3e SQLite 回滚日志负载

- [x] 官方 SQLite 3.53.4 amalgamation 固定 URL/SHA-256，原生 Unix VFS 和线程/WAL 编译能力保留；启动核对版本、`THREADSAFE=1`、`DELETE/NORMAL/mmap_size=0` 与 EXTRA/FULL 实际 PRAGMA 值。
- [x] 静态/动态 CLI、建表、提交/回滚、多连接及独立进程 writer 冲突、未提交事务的进程直接退出与同次启动 hot journal 恢复、关闭重开和 `integrity_check` 在真实 U-mode 通过；同一恢复 ELF 的 setup/mutate/recover 也在固定 Linux 执行。
- [x] NBD 服务在宿主协议测试和 QEMU 接入通过；EXTRA/FULL 正常事务、FULL 写/flush 错误传播、热日志和已确认提交后的两次重启恢复通过，逐字节检查整事务与 ext4。小事务逐事件矩阵完成 441 次断电、100 次写失败、47 次 flush 失败；普通多进程 WAL 后续在 P4 另行验证，不以共享文件 futex 为统一前置。

**验证与退出**：`make test-record-lock-host test-record-lock-riscv test-diff-abi-riscv test-nbd-host test-sqlite-rollback-riscv test-sqlite-nbd-riscv test-sqlite-recovery-riscv test-sqlite-recovery-matrix-riscv` 分别覆盖模块、Linux 差分、正常运行与恢复；既有 files/userland/lwext4/栈/RISC-V 全套与 228 项清单已重跑，清单仍为 223/2/3 且失败 ID 不变。WAL 与实板持久性不计入本阶段。

## P4：共享后备对象、文件页与跨 MM futex

**依赖与入口**：[MM](modules/kernel-mm.md)、[VMA](modules/kernel-vma.md)、[VFS](modules/vfs-ext4.md)；`include/kernel/{mm.h,vma.h,file_mapping.h,memory_object.h}`、`mm/{vma.c,memory_object.c}`、`arch/riscv/mm.c`、`fs/{page_cache.c,vfs.c}`、`kernel/syscall/memory.c`、`kernel/sched/futex.c`。P4a 已迁移统一内存后备对象；文件页及共享 futex 复用稳定身份所需的接口按各自契约扩展，不预建通用插件框架。

### P4a 共享匿名对象（已完成）

- [x] mmap 时建立可引用身份，即使尚无驻留物理页；fork 共享后备对象，按对象+页索引发布页。已驻留页保留共享 PTE 权限，私有页继续 COW。
- [x] VMA 借用对象并保存连续 offset，MM registry 持对象引用；对象页槽与每个 PTE 分别持物理页引用。切分、合并、fixed replace、munmap 和 fork 后身份与 offset 连续，末引用回收页槽。
- [x] fork 前已驻留、fork 后子先缺页、fork 后父先缺页均用同步握手验收双向可见；固定 Linux 差分加入同一 ELF 的三页共享案例。
- [x] 覆盖 PROT_NONE/恢复、部分 unmap、替换、父/子先退出与 fault/PTE 分配 OOM；中途失败回滚新页槽，引用计数回到基线。

### P4b 共享文件页与权威数据源

- [x] 同挂载/inode/文件 offset 的独立 open/mmap 指向同一文件页；普通 read/write 与映射读写三向可见。MAP_SHARED 首次写故障标记 inode 缓存脏页，写回前经页别名反向索引重新保护 PTE，generation 防止写回中重写被误清。
- [x] 缓存页保留 clean/dirty/writeback 与 generation 状态；驻留共享 PTE 持物理引用并在缓存页中登记别名，回收不得驱逐仍映射的页。fd 关闭与路径删除不改变 inode 页身份。
- [x] `msync` flush EIO 后可重试；成功写回重新保护共享别名，之后再写能重新标脏，驱逐缓存并重读仍是新值。共享 fork metadata OOM 与固定替换分配失败保留原别名，结束时引用回到基线。
- [x] 测试专用 ext4 回调在旧值写入后、generation 比较前，通过另一个驻留共享 VA 写入新值；再次 `msync` 和驱逐重读证明新脏数据未被旧写回清除，两个别名及最终 OFD 回到基线。
- [ ] 第二个 hart 的真实并发写入、跨核 TLB 与多别名回收资格仍由 P6 验证；单 hart 的确定性重入不替代它。
- [x] 固定 Linux 同一 ELF 差分覆盖独立 open、多个 VA 别名、fork 后独立 MM、MAP_PRIVATE 隔离、共享页与普通读写可见性；SQLite WAL 子进程另经独立 open/mmap 使用共享索引。
- [x] 注入 `msync` flush 失败返回 EIO，真实 OFD/mount 保留可重试状态；持映射/临时 pin 的缓存页不按无引用页驱逐。
- [ ] 并发 miss/锁外 I/O 的跨核发布在 P6d 验收。

### P4c 截断与 msync

- [x] 已驻留页跨 fork 后在另一 MM 中截断仍撤映射，越 EOF 再访问产生 SIGBUS；`O_TRUNC`、非对齐尾页、先缩后扩与普通写跨 EOF 空隙由固定 Linux 差分验证。
- [ ] 私有修改及截断故障交错仍需补充聚焦验收，并保持已有 private COW/PROT_NONE 截断回归。
- [x] 关闭 fd 后映射仍活、unlink 后仍能访问，由持有 OFD 的 MM 来源维持 node 生命周期。
- [x] 固定替换分配失败仍保留共享文件 VMA、驻留 PTE、缓存别名和 OFD 来源；fork 的 metadata 分配失败不改变父映射，成功子 MM 释放后父写回及末引用回收通过。
- [ ] 退出清理时真实 I/O 错误与多别名回收交错仍需单独验证。
- [x] `msync(MS_SYNC)` 走真实 dirty→写回→同步与错误传播，不能只失效缓存；`MS_ASYNC`、`MS_INVALIDATE`、对齐、空区间、空洞和互斥 flags 经固定 Linux 同一 ELF 差分，注入 flush EIO 后可重试。

### P4d 共享 futex

- [x] private key 使用单调且不复用的 MM 身份号与虚拟地址；共享匿名 key 由稳定对象身份与连续字节偏移派生，不依赖物理页地址。非 private 操作解析用户映射并在坏页返回 EFAULT；共享文件 futex key 仍待实现。
- [x] WAIT 登记、WAKE 和 REQUEUE 持有共享匿名对象引用；unmap/最后映射消失与等待者恢复不会悬空或重用旧 key。fd 后备尚未接入。
- [ ] 跨 fork 的独立 MM 共享唤醒、不同对象同 VA 隔离、共享→私有 requeue、同对象不同偏移的同桶/跨桶迁移，以及最后映射撤销后的等待超时已有真实 U-mode 验证；基础唤醒、requeue 和坏地址另有固定 Linux 差分。共享文件映射现已提供不同 VA 别名的构造路径，但共享文件 futex key 和跨 MM 信号交错仍需聚焦验证。单 hart 先通过，跨核原子比较/登记属于 P6。

### P4e mremap、madvise 与 WAL

- [ ] 对象生命周期稳定后实现 mremap，覆盖移动/增长/收缩、旧/新地址失败原子性、共享身份和 AS/DATA 记账；不复制成意外私有对象。
- [ ] madvise 按真实消费者逐项增加；DONTNEED 区分 private/shared/file，已丢弃的私有内容不能再次读出，错误不能成功空返回。
- [x] SQLite 普通多进程 WAL 使用固定 3.53.4 原生 Unix VFS，在同一 ELF 的固定 Linux/BoarOS 双侧运行独立进程 writer 竞争、已提交/未提交事务、进程直接退出、重开与第二次启动恢复及 `integrity_check`；P3 回滚日志基线继续保留。
- [x] WAL 的独立 NBD 矩阵覆盖 EXTRA/FULL、hot、已确认提交、全部 42 个事务内块事件的 126 个断电组合和 42 个写/flush 失败位置；每次从稳定镜像恢复两次，并检查整事务与 ext4。实板持久性未验证。

**验证与退出**：P4a 已由 `test-vma-riscv`、真实 U-mode 的 `tests/userland/shared_mapping.h`、`tests/diff-abi/shared_mapping.c` 验证。P4b/P4c 的共享页别名、权限、fork、截断尾页、unlink、`msync` 参数及普通写扩展由 `make test-diff-abi-riscv` 双侧验证；聚焦错误交错由 `make test-files-partial-write-riscv` 验证。普通 WAL 与重启由 `make test-sqlite-wal-riscv`，存储恢复由 `make test-sqlite-wal-recovery-riscv test-sqlite-wal-recovery-matrix-riscv` 验证。P4d 基础路径由 `tests/userland/shared_futex.h`、`tests/diff-abi/futex_shared.c` 与 `tests/riscv/mm_cases.c` 验证；不同 VA 别名及交错矩阵仍按上一条跟踪。共享文件 futex 分开跟踪。

### P4f SysV IPC

- [x] 当前 shmget/shmat/shmdt/shmctl 均已接入；针对 IPC_PRIVATE 与命名 key、段大小对齐与 Linux 布局完成 194–197 系统调用接入。
- [x] 确认 key/id/代次、segment 与 attach 的 owner、IPC_RMID 后存活/末引用释放、fork 继承与 exec/退出分离，以及权限/限额/OOM/fault 回滚；统一后备对象作为物理页后备。本轮 R2 将 VMA 裸 segment 借用改为 attachment 引用，并在通用 VMA 编辑/销毁处维护 nattch。
- [x] 验证跨进程/不同地址、删除后已有映射、ID 反复复用、部分失败与资源基线；Linux 差分 ABI 新增 19 条全部一致（累计 1031 条），用户态多进程 fork/shmdt/IPC_RMID 与裸机 scale 测试全数通过，关机物理页完全回收。SysV semaphore/message queue 不自动纳入本阶段。

## P5：glibc、exec 与单核真实工具链

**入口**：[ELF](modules/user-elf.md)、[exec](modules/kernel-exec.md)、[程序环境](modules/program-environment.md)；`kernel/{elf64.c,elf64_source.c,exec.c,random.c}`、`arch/riscv/elf_image.c`、`kernel/syscall/`、`tests/program-inventory/inputs.json`。试跑可现在开始，完整应用依赖按真实调用落到 P1–P4。

### P5a glibc 独立矩阵

- [x] 固定 glibc 2.44 官方源码参考、宿主 RV64 loader/libc、相应 ELF、构建/运行环境与许可证/哈希；不修改二进制绕过内核缺失，不把 musl 结果套用为 glibc 结果。宿主二进制由逐文件 SHA 固定，尚未提供从官方源码逐位重建的工具链证明。
- [x] 基础矩阵双侧验证静态、动态、PIE、静态 PIE、额外 DSO、初始 TLS、dlopen TLS、pthread、信号及其组合；保留装载/main 之前、运行时和退出阶段的首个失败。更广 glibc 应用仍待试跑。
- [x] 已区分内核 PT_INTERP 装载和用户动态链接器/运行时职责；首次 pthread_join 失败经固定 Linux 差分定位到 futex bitset，而非动态重定位。

整合后 `make test-glibc-riscv test-diff-abi-riscv test-userland-riscv test-sqlite-wal-riscv test-riscv test-stack-usage` 通过，固定 Linux 差分为 408 条一致。228 项清单按 `python3 tests/program-inventory/run.py --reuse-builds --output build/glibc-futex-inventory` 全量重跑仍为 223/2/3，五个旧失败 ID 不变；内核 SHA-256 `ebc11763ddac2661df3af45cca96ca322abeba0623e5cb9919a9051392db4574`，suite identity SHA-256 `5419a733de0e8e5095f53876f261a5a17c0c9f49b6f445dc0953e6fcef48c9c9`。输入和重建命令见[程序清单](learning/user-program-inventory.md)。

### P5b shebang 与 exec 组合

- [x] main 已交付 shebang 的解释器路径、单个可选参数、argv/envp、嵌套与错误边界。固定 Linux 开启 BINFMT_SCRIPT 后先复现四组差异；含递归错误优先级和空参数边界的 15 条脚本记录纳入完整 583 条差分，exec、musl、glibc 和栈检查通过。契约见[exec](modules/kernel-exec.md)，根因见[ELF 学习](learning/elf-loading.md#shebang-与-shell-回退2026-09-28)。原镜像 BusyBox 已验证无 shebang 回退依赖自执行路径，真实 procfs 仍属 P1h 后续。
- [ ] 保留 PT_PHDR/auxv、段重叠/对齐、文件尾页+BSS、PIE/解释器布局回归；与线程组 exec、信号、CLOEXEC 和文本写互斥组合验证，不在内核代替动态链接器重定位。

### P5c 随机数与系统环境

- [x] getrandom 区分可信熵就绪/未就绪、flags、阻塞/信号及用户 fault；VirtIO RNG legacy/modern 从宿主安全随机后端取得至少 32 字节后置 ready。DTB/用户写入和早期 ASLR 降级不计可信熵；实板熵源未验证。见[随机数来源](learning/random-source.md)与[RNG 传输](modules/riscv-virtio-rng.md)。
- [ ] klogctl/日志读取需真实日志 owner、缓冲、覆盖/读游标、fault 和权限设计；用原 dmesg 核对实际日志，不以空成功消除报错。
- [ ] RTC 字符设备读取及所需 ioctl 复用平台时钟来源，明确设备号、节点、无设备与错误边界；原 hwclock 必须输出真实时间，写 RTC/告警能力另行定范围。
- [x] sysinfo 的 RV64 完整布局、内存/负载/任务数与 EFAULT 已交付。
- [ ] prctl 等只按真实调用链新增；版本和统计来自内核事实，未知能力返回规定错误，用户查询不能触发整机 fatal。

### P5d 离线编译闭环

- [x] 同一静态 musl 驱动和磁盘在固定 Linux/BoarOS 记录预处理、编译、汇编、链接、运行五阶段的独立结果，重放 journal 后检查逐阶段产物哈希与 ext4；无原生编译器时两侧明确停在 `preprocess:exec:2`。这是诊断基线，不是编译闭环。
- [x] 固定 Alpine v3.22 riscv64 GCC 14.2.0-r6 和 14 个依赖 APK，完成同一镜像中的预处理→编译→汇编→静态链接→运行；五阶段退出码、产物哈希与最终输出在固定 Linux/BoarOS 一致。现只覆盖固定小型 C 源码，其他项目须逐个验证。
- [ ] 单独补齐 pipe/匿名 inode 的 mode 存储、`fstat` 与 `fchmod` 契约：固定 Linux 对 `fchmod(pipefd)` 成功，当前 BoarOS 对无 VFS inode 的 fd 返回 `ENOTSUP`；不以成功存根替代元数据。
- [ ] 再固定 rustc/cargo、依赖锁定和离线最小项目，成功后扩大完整项目。先 `-j1` 建立正确性，不要求 SMP，也不把网络下载失败混入内核 ABI。
- [ ] 每个新失败最小化、对照固定 Linux，再修通用机制；保留可恢复的成功输入和失败样本，不能为构建脚本改写预期输出。

**验证与退出**：现有 `test-exec-riscv`、`test-elf-tail-riscv`、`test-userland-riscv`，新增 `test-glibc-riscv`、futex bitset 固定 Linux 差分和 `make test-offline-c-riscv` 的客体原生五阶段编译验收；`tests/userland/exec_scripts.c` 仍待后续能力建设。未特改动态 glibc 的基础矩阵和固定小型 C 程序的离线构建已有闭环；Rust 与更大项目尚未验收，不能把剩余运行失败合并成一个“动态链接未支持”。

整合后 `make test-diff-abi-riscv test-files-riscv test-userland-riscv test-lwext4-metadata-host test-references test-offline-c-riscv test-glibc-riscv test-sqlite-wal-riscv test-sqlite-wal-recovery-riscv test-riscv test-offline-c-baseline-riscv test-stack-usage` 通过；固定 Linux 差分为 430 条。228 项清单通过 `python3 tests/program-inventory/run.py --reuse-builds --output build/offline-gcc-inventory` 全量重跑仍为 223/2/3，五个旧失败 ID 不变。内核 SHA-256 为 `9f848c4b74aa8415c0869616abfccd456e26742e1959d717b7f59f57f50164c4`，清单身份 SHA-256 为 `f3540ed6f317d7ca605780c938e4dbd6b66f76af56836e128234018f7c9329a4`；固定 Linux Image SHA-256 为 `16a93ddb1d451898b93fff14de0cc076bcf1b10dad54c19a3e179a6cd81103b1`。

## P6：SMP、TLB 与中断/I/O 并发

**前置与入口**：共享对象和线程契约已有单 hart 回归，平台具备可验证的 IPI/timer/IRQ 入口。`kernel/sched/{core.c,wait.c,process.c,futex.c}`、`kernel/physical_page.c`、`mm/heap.c`、`fs/{page_cache.c,vfs.c}`、`arch/riscv/{context.c,trap.c,mm.c,sv39.c}` 及设备后端。锁/每核/远端 TLB 接口数量由真实消费者决定，路线仍待确认。

### P6a 每核运行状态

- [ ] current、idle、trap 栈、FPU owner 和 timer 状态按核分离；2 hart 能进入内核只算启动探针，必须继续证明任务正常进出 U-mode。
- [ ] ready 发布、取出、迁移和退出具有唯一 owner，同一线程不能在两核同时执行；增加可检测重复运行的探针，避免只靠应用输出“看似正确”。
- [ ] 保持单 hart 基线和错误诊断，逐个消费者替换当前 tp/全局状态假设；页分配/引用/堆元数据同步也必须纳入。

### P6b 锁、等待与中断规则

- [ ] 列出哪些锁可在 IRQ 中获取、哪些允许睡眠、锁顺序及关中断范围；本地 SIE 不保护另一核，不能只把 refcount 改成原子就宣称 SMP 安全。
- [ ] futex compare→登记→阻塞由同桶/等价同步协议保护；跨核 wake 与入队、timeout、signal、clear_tid 对撞不漏唤醒，不重复运行或摘队。
- [ ] 缺页/设备 I/O 可能睡眠，不能持 spinlock 调 lwext4 或等磁盘；退出/取消期间，阻塞原栈释放已 pin 对象，IRQ 路径不做复杂生命周期回收。

### P6c TLB 完成与页回收

- [ ] PTE 失效/降权→通知相关 CPU→确认旧翻译不能再用→释放物理页、页表和映射引用；完成含义、目标核集合与并发地址空间切换必须明确。
- [ ] 避免持 MM 锁等待也需要该锁的远核；确定锁外完成及延期回收 owner。未确认不能安全复用旧页，也不能把超时转换成普通用户成功。
- [ ] 一核读写、另一核 unmap/truncate/mprotect，跨核 COW 同页与页表释放反复验证；保留单 hart 本地 SFENCE 顺序和 PROT_NONE 所有权。

### P6d 缺页和页缓存并发发布

- [ ] 锁内定位 VMA 并 pin 对象/版本；锁外读取/分配；回锁重查 VMA、权限、EOF、对象和版本。另核已 fault 或 unmap/truncate 时丢弃过期候选，释放准确引用后重试/返回。
- [ ] 同文件页并发 miss 只发布一个权威页；错误状态、dirty/writeback 和被固定页不会被后到的候选覆盖。不可持 spinlock 跨后端 I/O。
- [ ] 交错 fault、COW、fixed replace、退出和 OOM，核对页/对象/缓存/任务资源回到基线；不将所有资源耗尽升级成整机 fatal，不掩盖非法释放等核心不变量。

### P6e 请求完成与外部中断

- [x] 单 hart 已实现 IRQ/等待队列、八槽乱序完成、队列满等待、flush 屏障及超时 reset；双盘生命周期与失败隔离已验收。这里的剩余工作是跨核保护，不是重做轮询迁移。
- [ ] 明确提交、完成、取消、超时、reset、DMA 停止后内存可释放的 owner 转移；组退出时 I/O 尚未完成、另核关闭/复用 fd 不会回收在用对象。
- [ ] 在既有多请求队列上验证跨核提交、IRQ 归属、乱序完成/队列满、失败唤醒与设备停止；将跨核正确性与吞吐优化分开提交。

### P6f 多核验收矩阵

- [ ] 2→4→8 hart 各自重复：同页 COW、同文件页 fault、读写与 unmap/truncate/mprotect、wake 与入队、I/O 中 exit_group、fd 关闭/复用、内存耗尽与退出回收。
- [ ] 每一核数记录固定输入、重复次数、失败种类、泄漏/重复释放和同任务双核运行检查；保留单 hart 对照定位通用 ABI 与跨核错误。
- [ ] 正确性收口后进入 P7 并行构建；不先替换为 EEVDF。只有饥饿/公平性或性能证据要求时，再比较调度策略。

### P6g 真实内核栈 guard（可单 hart 先做）

- [ ] 比较栈虚拟映射方案，明确未映射 guard 与 direct-map 别名的保护范围、栈页 owner 和映射失败回滚；不能把连续物理栈底 canary 当 guard page。
- [ ] 用受控越界验证 fault 可诊断，保留 canary、高水位、compiler stack usage 和实际调用链预算；合法退出切到可信栈后才回收执行栈，zombie 不持有它。

**验证与退出**：拟新增 `tests/userland/smp_memory.c`、`tests/userland/smp_wait.c`、`tests/riscv/ipi_tlb_main.c` 与对应多 hart runner；保留 `test-riscv`、`test-userland-riscv`、`test-stack-usage` 的单 hart 门禁。2/4/8 核各自有重复正确性和资源基线证据，不能由某一核数或 multi-hart boot 推定其余配置。

## N：socket 与网络支线

**依赖与入口**：P1 的 OFD/后端边界已用于单 hart IPv4 loopback。`net/socket.c` 持有 endpoint、数据队列和等待，`fs/files/socket.c` 持有 fd/OFD 提交，`kernel/syscall/socket.c` 导入 Linux ABI，现有 poll/epoll 使用 socket 就绪。固定输入与验收见[网络模块](modules/kernel-network.md)。

### N1 对象与协议栈选型（路线已确认，首个切片完成）

- [x] BoarOS 持有 fd/OFD、socket、请求 pin、队列和等待；协议回调由最后真实 OFD 引用销毁时解绑。首切片实现 IPv4 UDP/TCP loopback、阻塞/非阻塞与读写/就绪；shutdown 等余项继续 N2。
- [x] 已比较自写协议子集、成熟 C 栈与宿主转发，确认 BoarOS ABI owner + 固定 lwIP 2.2.1 raw API、NO_SYS 事件驱动。协议/pbuf 使用有界静态池，socket/OFD/请求使用 kernel_heap；来源、所有权和成本理由见[学习记录](learning/network-ownership.md)。

### N2 本地与 loopback 链路

- [x] AF_INET loopback UDP/TCP 首个真实消费者已通过；原 socket entry 调用由固定源码 `src/functional/socket.c` 与日志确认。
- [x] AF_UNIX/socketpair 先交付原 hackbench 所需类型与 flags；覆盖阻塞/非阻塞、EOF/半关闭、信号取消、fork/dup、对端退出、poll/epoll 和资源耗尽。命名端点及 SCM_RIGHTS 按各自真实需求扩展，不能因 socketpair 通过标记全部本地 socket 完成。
- [ ] bind/connect/listen/accept、send/recv、非阻塞 EAGAIN、半关闭、EOF、失败连接、poll/epoll 和信号打断逐项验收；失败连接不能假装建立 endpoint。
- [ ] sendmsg/recvmsg 与 SCM_RIGHTS 明确被传 fd 的 OFD 引用、用户复制失败和消息未接收/对端退出时回收；不能只传可被关闭复用的整数 fd。
- [x] 原静态/动态 socket 直接 entry 在固定 Linux 与 BoarOS 同一 ELF 双侧通过；PID 1 关机 `heap-live=0`。UDP 池耗尽、释放和重用、TCP segment 池耗尽后 `ERR_MEM`、TCP 200 秒协议定时回收由 host 测试保护；真实 pthread U-mode 覆盖零长度 UDP datagram、共享 OFD 双读、close/fd 复用、线程组强制退出、全局池压力下的 POLLOUT 抑制及释放后进展。整合内核全量 228 项为 227 pass、1 BusyBox 包装失败，不以此推出 AF_UNIX 或真实网卡完成。

### N3 网卡与真实服务

- [ ] VirtIO-net 实际数据路径、ARP/IP 与宿主双向报文，再验证多个连接、异常断连、半关闭、并发请求及停止回收的固定真实服务。
- [ ] VF2/LS2K 后端分别核对 DMA/cache、MDIO、checksum、IRQ、路由和链路故障；内存 loopback 不算实网卡完成。
- [ ] DNS/TLS 属于用户态时独立验证其随机、时间、文件/证书和网络依赖；TCP 通不等于 HTTPS 可用。

**交付证据**：固定客户端/服务输入、双方日志与实际外部报文，分别报告 loopback、QEMU 网卡、每块实板；所有权和失败场景与性能分开。

## L：LoongArch 与实板支线

**入口与资料**：`arch/riscv/`、架构头与 Makefile；后续拟新增 `arch/loongarch/` 和 `kernel-la`。先读 `references/README.md` 与清单中的 LoongArch 手册/文档、Linux、QEMU、VF2/2K1000LA 资料，记录具体 commit/tag/文档版本或 SHA-256。

### L0 架构依赖盘点

- [ ] 列出通用 MM/调度对 RV 头、satp、SFENCE.VMA、trap frame、页大小和寄存器布局的直接依赖，按实际消费者提取架构操作；不复制 `arch/riscv/mm.c` 中通用 VMA/文件页策略。
- [ ] 保留架构 MMU/context/trap 与平台 DTB/MMIO/DMA 的区分，新增接口由第二实现验证，不预建空泛 HAL。

### L1 最小启动与用户态

- [ ] 串口→trap→timer→物理页→TLB/页表→高地址映射→一个真实 U-mode exit，每步有独立启动/故障/回收证据。
- [ ] 延续已选 LA64 16 KiB/三级页表配置，页大小是架构构建期事实；不把目标配置描述为硬件唯一能力。只生成 `kernel-la` 不算用户态通过。

### L2 ABI 与映像

- [ ] ELF 段对齐、BSS 尾页、auxv、用户栈、stat/signal 结构及 clone 寄存器逐项核对；不能只换汇编入口却保留 RV ABI 编码。
- [ ] 同一用户源码分别编译 RV/LA ELF，每架构内部用同一 ELF 对照 Linux 与 BoarOS；共享测试语义，隔离寄存器/页表差异，不拿 RV ELF 验证 LA。

### L3 扩大真实用户空间

- [ ] 静态 musl→动态 musl/DSO/TLS→fork/COW/信号→共享映射→glibc→真实应用，逐层保留错误与资源回收结果。
- [ ] 维持 RV/LA 同口径功能矩阵，缺能力记录阻塞，不让新平台回退到固定输出或修改过的用户程序。

### L4 两块实板

- [ ] 每块板分别验证固件交接/DTB→RAM 保留区→timer→块设备→根盘读写→shell→IRQ→网络→SMP；QEMU、VF2、2K1000LA 结果分列。
- [ ] 记录 DMA 地址/缓存一致性、设备限速/超时、烧录校验、固件/DTB/镜像哈希与可回退启动配置；先确认镜像和网段，不把环境错误先归因 ABI。
- [ ] 每板最小交付是可重复启动和真实根文件系统读写，之后单独标记网络、多核、复杂应用及性能；未拿到设备或固件输入时明确阻塞。

## P7：多核真实编译与性能

**依赖**：P5/P6；实板性能另需 L4。代码优化落到被测模块，先保留基准和成本解释。

### P7a 可比的负载与测量

- [ ] 固定源码、工具链、镜像副本初态、核数、内存、日志、计时起止、冷/热缓存；明确数据写到 RAM、介质缓存还是已经同步。
- [ ] 真实 C/Rust `-j1/-j2/-j4/-j8` 全部构建成功且产物运行正确，再报告多次中位数/范围和失败。Linux/BoarOS 保持相同口径，不跨 ISA/机器/持久性模型直接比快慢。
- [ ] 观察加速曲线中的串行部分和竞争，不要求所有负载核数翻倍性能翻倍；QEMU 的功能证据与板上吞吐分开。

### P7b 测量驱动的优化候选

- [ ] 收集 fault/major fault、重复读取、缓存命中、全 LRU node 失效扫描、inode 查找、写回流量、块请求大小/队列深度、堆锁、调度/唤醒、TLB 同步与栈高水位。
- [ ] 有证据后比较 node 缓存页索引、inode 哈希、批量 I/O/预读、锁外 I/O、每核缓存、deadline 结构、ASID/批量 shootdown 与调度公平性；精确失效不代表查找成本已优化，也不能未经测量宣称它是主瓶颈。
- [ ] 每次只改变可归因机制，写清内存/复制/锁/延迟成本与回退；保留负收益。性能改动仍须满足 ABI、owner、并发与资源耗尽回归。

## P8：系统交付与现场复现

### P8a 离线交付包

- [ ] 新目录或新机器按固定输入离线构建，校验产物/镜像身份，不依赖开发目录残留；保留依赖许可证与版本。
- [ ] 提供内核 commit、编译器/依赖、磁盘/DTB/固件哈希、平台启动命令、已支持/未支持范围、失败复现、回归日志与恢复/回退方式。
- [ ] 验证目录 rootfs 重新制盘、整镜像校验、串口抓日志、双网卡网段/路由和最小应用探针。真实设备操作遵循人的发布/烧录授权。

### P8b 组合与现场演练

- [x] main 提供只读 PID 1 path/argv/envp 构建配置，默认 `/init`，独立生成依赖；真实用户栈与交替重建见[根启动](modules/riscv-root-boot.md#pid-1-构建配置)。比赛配置留在评测分支。

- [ ] 从未知组合脚本/服务逐层定位设备→根盘→用户态→应用的首个失败，形成最小复现修通用机制；不能识别脚本名称输出答案。
- [ ] 默认引导、交互模式和评测 runner 分开，由同一内核能力支撑；保留一个已验证可回退版本。
- [ ] 数据库恢复、网络服务、多核编译、两个 QEMU 架构和每块板分别给验收结果，不能合成“全面支持 Linux”。他人能据列出的输入和命令复现才收口。

## 实施前需要确认的路线

以下是可行候选与调查方向，不是批准记录。实施任务时继续补全固定资料、接口、失败路径与 owner 的证据，再由维护者确认。明显违反契约的“fsync 空返回”“只共享已有 PTE”“用本地关中断代替跨核同步”不列作候选。

| 取舍 / 当前证据 | 可行候选与正确性、演进、复杂度、成本 |
|---|---|
| P1b VFS：原 fs context 借用单根 mount、cwd 字符串；OFD 已独立 | 已选择并完成②：最小 mount+路径对象/后端操作。路径持有 mount、inode 与父链引用，ext4 字符节点按 `rdev` 分派；共享可改名目录项、普通/NOREPLACE rename、linkat、tmpfs 与独立第二盘已完成；新后端仍需独立设计。 |
| P4a 共享匿名 | 已选择并交付②：专用对象按索引惰性发布页，保持稀疏分配并提供稳定身份；急切分配会改变 lazy/OOM 成本。后续已迁移到统一 memory_object，匿名跨 MM futex 与共享文件页已交付；共享文件 futex 仍待设计。 |
| P3b 持久化 | 用户已选择并交付逐 inode dirty/error、定向写回和真实 flush；共享事务可提交关联元数据，不主动全量写回无关文件。 |
| P3d 恢复 | journal/replay 与持久 orphan 已启用并验收；故障模型、限制和复现命令见 VFS 模块。 |
| P6 SMP：当前 SIE 串行化，缺远端 TLB 确认 | ① 进程态对象先用粗粒度可睡眠锁、IRQ/队列另设短锁，验证较少但并行有限；② MM/OFD/cache/队列对象锁直接演进，锁顺序/取消成本更高。先盘点消费者和睡眠边界再选，临时启动大锁有退出条件。 |
| P6g 栈 guard：连续物理栈、canary/高水位 | ① 独立虚拟栈区映射已有页，便于未映射 guard，但需页表与回收接口；② 调整内核现有映射形成受保护栈区域，初始接口可能更少，但别名/大页拆分与 direct-map 消费者成本须实测。先验证真实越界保护范围再选。 |
| N1 协议栈与分配 owner | 已确认 BoarOS 持有 fd/OFD、ABI、等待与缓冲队列，固定官方 lwIP 2.2.1 raw API/NO_SYS；协议和 pbuf 静态有界池，socket/OFD/请求由 kernel_heap 持有。先验收单 hart IPv4 loopback，网卡与 AF_UNIX 继续 N2/N3。 |

单 hart 已交付 OTHER/FIFO/RR 与全局实时带宽；后续调度改动以公平性/负载和实际消费者证据比较策略。第二架构按连续小里程碑推进；不等待 RV “全部完成”，也不复制整套通用内核。

## 每个任务的统一验收与现有命令

| 维度 | 提交前应有的证据 |
|---|---|
| 触发与契约 | 独立最小输入、声明支持范围、固定参考路径与版本；正常返回、errno、部分成功与 fault 可观察 |
| 失败前后 | 同输入的真实失败与修复结果，Linux 参考有效；未改上游答案、未宽泛归一化错误 |
| 所有权与安全 | 对象由谁持有、睡眠期间谁 pin、发布/失败/退出谁释放；真实 I/O 错误 owner 与分配器 fatal 不变量分开 |
| 并发 | 单 hart 临界区与跨 hart 同步分列；等待登记、可见性、发布/回收顺序和竞争探针 |
| 资源与错误 | OOM/超时/真实 I/O 返回所属层状态；合法释放完成即返回，非法释放/引用损坏 fatal，不能建立无 owner 重试链 |
| 性能 | 记录成本边界；无数据时称候选瓶颈。风格建议不混入 ABI/安全/并发结论 |
| 验证与文档 | 最窄→真实 U-mode→相关架构/全量；已知缺能力写阻塞。检查 README、modules、learning；TODO 更新具体完成条目 |

现有入口如下，子任务按依赖选择；上文拟新增文件不代表已经有同名 Make 目标：

```sh
make test-riscv
make test-userland-riscv
make test-diff-abi-riscv
make test-lwext4-host
make test-stack-usage
make test-program-inventory-host test-diff-abi-host
make inventory-userland-riscv
```

`inventory-userland-riscv` 默认成功只说明清单生成成功。全量 228 项仍有明确缺口，严格模式失败不是自动产生的新回归；`--case` 与 `--require-pass` 只严格判定本次选择集合，未选项目保留历史结果或 `not-run`，选择集合写入状态供恢复报告解释。完整 Harness 缺 `kernel-la` 或其他能力时保留阻塞原因。

## 范围与交付边界

暂不进主线：NUMA、swap、透明大页、完整 namespace/cgroup、seccomp/eBPF/ftrace、Linux 内核模块 ABI、复杂可加载框架和 io_uring。它们可另立任务，当前失败无需先完成这些能力。不因未来可能有用引入第三方框架或 Agent 编排平台。

`final-2025` 沿用只是规划假设；比赛事实只据本地固定规则/Harness，不能预测未来赛题。许可证和正式发布策略仍待决定。正常开发沿当前分支；提交围绕可说明/可验证的问题，AI 贡献按 CONTRIBUTING 添加 trailer；不提交会话材料、JSONL、秘密或运行镜像，发布权限遵循 [AGENTS.md](../AGENTS.md)。
