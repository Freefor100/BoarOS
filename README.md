# BoarOS

<p align="center">
  <img src="assets/boaros_header.png" alt="BoarOS 吉祥物与字标" width="100%">
</p>

BoarOS 是从零搭建、面向 OS Comp 能力建设的 C / 少量汇编内核，目标是运行未经 BoarOS 特改的 Linux 用户程序。Linux 是用户可观察契约和机制参考：在声明支持的范围内，返回值、对象身份、共享关系、错误、并发与生命周期必须正确；内部结构不必复制 Linux。

## 当前能力

当前生产路径为 **RV64、QEMU virt、单 hart、Sv39 / 4 KiB 页**。下表是已验证子集，具体接口与限制见模块文档。

| 范围 | 已有能力 | 主要边界 |
|---|---|---|
| 启动与内存 | OpenSBI、DTB、高半区/direct map、buddy/slab、连续页和引用回收、独立栈窗未映射 guard 页 | 无 SMP；guard 只覆盖窗口 VA 的 SP 式越界，direct-map 别名仍在；idle/boot 栈无 guard |
| 虚拟内存 | VMA、按需匿名页、共享匿名与共享文件映射、文件私有 COW、共享文件首次写追踪、`msync`、跨 MM 截断撤映射 | 无 `mremap`、按操作区分的 `madvise`、共享文件 futex、匿名共享页 swap 回收或 SMP 页表同步 |
| ELF / exec | shebang、按需 ELF、PIE、`PT_INTERP`、初始栈/auxv、musl DSO/TLS、固定 glibc 2.44 启动/TLS/pthread及取消子集、失败保持旧映像 | 无 `execveat`；glibc 应用覆盖尚有限 |
| 进程与等待 | 统一 TID/TGID/PGID/SID 身份对象、会话/进程组、fork/vfork、child-TID 生命周期差分、pthread clone、线程组退出、非组长 exec、wait/zombie/reparent、时钟与睡眠、进程组 ITIMER_REAL/SIGALRM | 合法 clone 组合仍有限；串口及 Unix98 PTY 控制终端；单 hart 关中断不等于跨核同步 |
| 调度 | OTHER tick 轮转、FIFO/RR 1–99 优先级、CPU0 affinity、RESET_ON_FORK、可配置全局实时预算及 proc 查询、到期有序索引与最早 deadline 参与 timer 重装 | 默认 1 秒 / 950 毫秒；无 nice 权重、PI、SMP 或硬实时保证 |
| 随机数 | ChaCha20 fast-key-erasure、BLAKE2s 混种、legacy/modern VirtIO RNG、`getrandom` 与 random/urandom 字符节点 | QEMU 宿主是信任边界；DTB/用户写入不计可信熵，缺设备时保持未就绪 |
| futex / 信号 | WAIT/WAKE/REQUEUE、超时/重启、跨 MM 共享匿名 futex、同 MM 非 PI robust-list 退出清理、标准信号、用户 handler、同步 SEGV/BUS/ILL/TRAP 故障信息与恢复、`rt_sigtimedwait` | 无共享文件 futex、PI futex、实时信号队列和 `sigaltstack`；单 hart 验证范围 |
| 文件与事件 | fd/OFD 分离、dup/CLOEXEC、共享 offset、阻塞 pin、部分/向量/定位 I/O、匿名pipe及ext4/tmpfs命名FIFO、poll/select/epoll；传统与 OFD 记录锁；socket OFD 与读写/就绪；mknodat 字符节点按设备号接入 null、zero、console、RTC | 独立 devpts、PTY 锁定/peer 与 packet、36/44 字节 termios；无 devfs，设备 mmap 未支持 |
| 路径与 ext4 | 共享活目录项、cwd/dirfd、普通/NOREPLACE rename、可写/只读根盘、符号链接、目录枚举、稀疏文件、显式纳秒时间、真实文件系统统计、打开后删除、私有映射截断；共享挂载树可用户态挂载/卸载 proc、tmpfs 和第二 ext4 盘，通用 linkat 硬链接，含 meminfo、uptime、self、exe/cwd/root/fd、挂载信息与首批进程 stat/status 字段 | 无 EXCHANGE/WHITEOUT 或完整权限；缺少 /dev/console 节点时的初始标准 fd 没有路径链接，meminfo 已提供真实缓存/共享/脏页/可用量，完整进程字段尚未完成 |
| 内存文件 | 统一稀疏内存后备对象、tmpfs 页/inode 配额、硬链接、共享/私有映射，musl POSIX 共享内存、SysV 共享内存和 tmpfs 工作目录的离线 GCC | 无 swap、SysV 信号量/消息队列、共享文件 futex；tmpfs 不持久化 |
| 缓存与存储 | read/write/private fault 共用文件页、inode 脏范围与定向写回、OFD 错误观察、`fsync/fdatasync/O_SYNC/O_DSYNC`；VirtIO legacy/modern 多设备独立 IRQ/队列、每实例页缓存/worker、八 span 批量读写发布与 flush 屏障 | ordered journal/replay、durable commit 与后续 checkpoint、持久 orphan；恢复承诺限于已验证块模型，已接入阈值驱动后台写回与 2%/4% 空闲水位回收，无周期清脏 |
| 终端 | DTB ns16550 IRQ＋worker，ttyS0/console/tty、canonical/raw、termios/termios2、VMIN/VTIME、控制终端和前后台作业；Unix98 PTY/devpts、packet、真实 libc PTY API及原 BusyBox ash/stty/script/replay | 其他行规程、break 生成和完整 modem 控制未交付；固定 root、单 hart |
| 内核日志 | 从启动保存16KiB真实内核日志、完整klogctl 0–10、消费式阻塞读、清空及console级别控制 | 当前不可变root权限模型；用户console输出与日志分离，无/dev/kmsg接口 |
| 身份与资源 | 单用户 root 的 UID/GID 查询；线程组共享并执行 NOFILE/STACK，fork 继承、exec 保留 | 真实ext4/tmpfs/匿名pipe所有权可变，进程仍固定root；无凭据变更/完整权限；fd 硬容量 1024、栈硬容量 8 MiB；其他有效 limit 返回 `ENOTSUP` |
| 平台与网络 | RISC-V QEMU 真实根盘可配置 PID 1（默认 `/init`） 与 musl 用户态；单 hart IPv4/IPv6 UDP/TCP loopback、双栈监听、连接选项、半关闭与向量消息，固定 lwIP 2.2.1 raw API，AF_UNIX socketpair；legacy/modern VirtIO-net、静态 IPv4/ARP、有界分片重组与隔离宿主双向 TCP/HTTP，custom pbuf RX、TX indirect+SG 零拷贝（保留复制回退）、无 NIC 时协议/OFD 定时器仍由内核 worker 推进 | 无命名 AF_UNIX 端点、外部 IPv6、公网/DHCP/DNS/TLS、完整 LA 用户环境、实板或多核验证 |

活 inode 的再次打开先取得现有节点资格，避免临时后端打开与关闭；创建权限通过已有句柄设置。弱路径 registry 仍不保存常驻目录项缓存。

单 hart 存储等待已由运行期 IRQ 唤醒：两个不同文件冷读可同时在途，等待期间计算与无关缓存命中继续执行；OFD、inode、后端事务与退出清理各自保留 owner。八槽乱序完成、flush 屏障和超时 reset 在 legacy/modern、writeback/writethrough 四种组合验收，见[可睡眠存储](docs/learning/sleepable-storage.md)。

双盘暂扣与故障隔离测试已修复单字节控制终端握手，并接入 CI 配置；本机原矩阵和 FIFO/RR 组合通过，托管 CI 状态另行核对。

网络 worker 已在协议推进前归还 TX 完成槽，并在睡眠前复查新容量与收包；已验证两种 VirtIO 传输下的实际 TAP 程序；综合改动的匹配实验既有收益也有默认预算吞吐回退，见[预算结果](docs/learning/data-path-budget-experiments.md#正式匹配结果2026-10-06)。TCP 流发送已用接纳 reservation 约束 payload 复制，预先无容量时不解析用户页；socket poll 已收紧为局部快照；短 syscall 与统一 worker 按独立协议预算推进，资源归还只服务等待集合。验证边界见[网络记录](docs/learning/network-ownership.md#纯就绪与有界协议服务2026-10-05)。

文件追加增长已与截断分离：对齐增长不遍历缓存页，非对齐增长只处理旧 EOF 尾页；1–64 MiB 的实际页缓存规模门禁保护这一成本界；另有三启动匹配吞吐测量，64 MiB 小请求追加不再随文件增长急剧降速，缓存完成与显式同步分别报告。冷页完整覆盖省去页缓存旧内容读取，范围写回按哈希/脏页链选择较小集合，快照只复制脏区间；恢复与测量边界见[VFS 模块](docs/modules/vfs-ext4.md#当前成本边界)。

epoll 以完整用户事件交付作为 ET/ONESHOT 提交点，复制 fault 保留未交付项；扫描与重入通知独立，取消和 close 保持对象寿命。验证边界见[事件交付](docs/learning/epoll-delivery.md)。

PID 1 退出后先结束并回收剩余用户进程，再停止内核服务和卸载根盘；后台 daemon
持有的 cwd、文件或 MM 不再使关机提前遇到 EBUSY。正常用户退出与 PID 1 的完成
状态分别保留，见[调度生命周期](docs/modules/kernel-scheduler.md#退出与-exec)。

文件层已有部分读写、OFD 生命周期、稀疏文件与映射截断的语义深度；显式时间设置和真实挂载统计已接入；共享匿名映射已迁移统一稀疏内存后备对象，与共享文件页均可跨 MM 读写，串口与 Unix98 PTY 已具备真实行规程、控制终端和有界传输；其他行规程与完整 modem 控制仍有缺口。ext4 恢复已覆盖 512 字节原子写、未 flush 写丢失或重排的故障模型；实板持久性仍待独立验证。固定 glibc 2.44 的五种 ELF 形态与 TLS/pthread/取消清理/信号组合已双侧验证，完整 glibc 应用兼容尚未证明。

内存统计按文件页、共享匿名/tmpfs 后备页和各盘块缓冲真实 owner 计量；`sysinfo` 返回真实任务数与 1/5/15 分钟负载。原镜像 BusyBox `free` 已显示有效容量，LTP 越过缺失 `Cached` 的阻塞。已新增由真实 timer 快照支持的 coarse clock，并通过窄差分；原静态/动态 glibc `utime` 各 30 次复跑通过，诊断环境边界见[文件时间](docs/learning/file-timestamps.md)。LTP cgroup 辅助程序等待已独立定位，见[路线与验收](docs/goals.md)。

固定 SQLite 3.53.4 的原生 Unix VFS 已在单 hart 上运行静态/动态 CLI、多进程 DELETE 回滚日志和普通多进程 WAL，并验证第二盘 WAL 的独立重启读回；WAL 工作负载用同一 ELF 在固定 Linux 与 BoarOS 验证 writer 竞争、未提交进程退出及第二次启动后的完整性。DELETE 与 WAL 的 EXTRA/FULL 恢复各有 NBD 断电/故障矩阵；实板持久性未验证。

客体内固定 Alpine v3.22 RV64 GCC 14.2.0-r6 已在同一离线镜像上完成预处理、编译、汇编、静态链接和运行；固定 Linux 与 BoarOS 的五阶段状态、产物哈希和输出一致。同一编译流程也通过 tmpfs 工作目录；产物复制到根盘供比对，不代表 tmpfs 持久。另已完成原 GNU make4.4.1 默认FIFO jobserver的Lua5.4.3工程构建、增量、错误恢复和产物运行；其他项目与Rust尚未验收。

固定BusyBox/libc-test最近完整清单仍为228项、227项双侧通过的历史结果；此前环境补全验收原BusyBox包装器，55/55子项成功，dmesg/RTC及df根盘内容另做真实核对。当前通用ABI差分1366条匹配，终端另有同ELF的107条差分记录；完整清单和本轮选择集合分别见[程序清单](docs/learning/user-program-inventory.md)。成本门禁见[单核规模回归](docs/learning/single-hart-scale.md)。

顺序预读与连续写回提供有界实验候选，生产默认仍为预读关闭、写回一页。
机制门禁和吞吐测量分别记录；TCP 27 组、存储 20 组已完成匹配筛选和组合扩展，共 1,218 次发布启动与 184 次诊断。用户依据结果批准网络默认改为 8 MSS/池 4 倍/协议堆 2 倍；存储仍为 RA0/WB1。历史结果中的默认标签指调整前的 8/1/1，见[结果、每连接完成时间和输入身份](docs/learning/data-path-budget-experiments.md#正式匹配结果2026-10-06)。

LoongArch L0–L1 首阶段已交付：QEMU virt/LA464 单核、LA64、16 KiB/三级页表，
独立内存 ELF 经共用 MM/exec/任务/syscall 路径进入用户态，并通过 timer 抢占、
故障/回收及 512 MiB/1 GiB 同 ELF Linux 对照。第二阶段的现代 PCI→共用
VirtIO 块核心→ext4 根盘→LP64S 静态 musl 已通过两种 RAM 的新验收，包含
未修改的完整 BusyBox 中 cp/cmp/grep/cat/echo/uname/dd 七个 applet、真实文件映射、
fork/exec/wait、错误/创建 OOM 与资源基线；真实 PCI 写故障保留失败 I/O owner 并明确停止。
`kernel-la` 有盘时启动可配置 PID 1，无盘时运行首阶段内存 ELF 契约，见
[LA 模块](docs/modules/loongarch-boot.md)。整数信号 handler/sigreturn、同步故障恢复、
mask/嵌套和等待重启已通过同 ELF 的双侧两种 RAM 验证；静态 musl pthread/TLS、
errno 隔离、timer 寄存器保持、同步/取消、futex/非 PI robust、线程组 exec/退出及
创建 OOM/回收也已验证。原 BusyBox ash 的非交互 trap/wait 有双侧证据。
原版LP64D musl的动态PIE/非PIE、解释器、DT_NEEDED/RPATH、初始与dlopen DSO TLS已通过双侧两种RAM；
标量FR/FCC/FCSR、浮点信号/exec/clone也已验证，整数内核与原LP64S用户程序继续可用。
LA内核栈已接入PGDH共享窗口及真实16KiB guard，含NX、撤映射、OOM回滚与可信异常栈验收。
LSX/LASX状态、信号、clone/exec和关闭扩展的HWCAP已双侧验收。固定原版glibc2.42的五形态、
初始/dlopen TLS、pthread取消和信号已在双侧两种RAM验收；RV仍固定2.44，版本差异保留。
共用VirtIO net与Ethernet已接LA现代PCI，固定Linux/BoarOS两种RAM的真实TAP、
原BusyBox HTTP和共享块/RNG/net IRQ、正常及构造/reset失败回收通过。完整网络ABI
仍有AF_UNIX sendfile发送者计费差异待修复；TTY/RTC及更广原程序矩阵继续推进。
DTB随机种子仅支持早期材料和AT_RANDOM，不计可信熵。真实PCI RNG的正常、缺失、
延迟、在途停止已双侧两种RAM验收，BoarOS构造失败与回收也已在两种RAM验收。LBT、完整终端/网络及
更广原程序、实板和SMP仍须另行验收，LA与RV尚不等价。
共用VirtIO transport/split queue已接入RV MMIO与LA PCI block，保留batch/flush/
超时及DMA业务owner；RNG已迁入，net仍在本轮计划内，见[框架契约](docs/modules/virtio-framework.md)。
LA EXEC 页的数据读权限已按固定 Linux 的冷/驻留状态核对；页表有效权限与请求
VMA 权限分别保留，真实读取、uaccess、fork和撤权均有双侧两种 RAM 验证。

## 构建与验证

需要 RISC-V bare-metal GCC/binutils、GNU Make 和 QEMU；支持 `riscv64-unknown-elf-` 与 `riscv64-elf-` 前缀。真实用户态和 Linux 差分的额外工具见[工具链](docs/toolchain.md)及[差分模块](docs/modules/differential-abi.md)。

```sh
make all                       # kernel-rv
make test-riscv                 # 通用模块、架构与真实根启动
make test-userland-riscv        # 静态 musl、动态 pthread / TLS
make test-glibc-riscv           # 固定 glibc 2.44 静态/动态/PIE、TLS、pthread与取消
make test-diff-abi-riscv        # 同一 ELF 对照固定 Linux
make test-io-sleep-riscv        # 暂扣响应验证并发、计算/缓存进展、flush 与 reset
make test-cost-riscv COST_CASE=contract # 默认关闭的诊断窗口，三个启动副本
make test-scale-riscv           # I/O 分块、用户页解析、驻留查找与单页改权成本
make test-lwip-host             # loopback、UDP 池耗尽/重用、TCP 定时回收
make test-random-host           # 密码向量、就绪与设备契约
make test-rng-riscv             # 两种 VirtIO 传输、延迟/取消与退出回收
make test-sched-policy-host     # 策略、队列模型与调度 ABI
make test-sched-bandwidth-riscv # 实时预算、RR 余片、普通任务进展
make test-multi-disk-rt-riscv   # 实时负载下双盘与清理进展
make test-record-lock-host      # 区间树随机模型、所有权与分配失败
make test-record-lock-riscv     # 同 ELF 的 Linux/BoarOS 线程、fork、fd 复用、退出
make test-nbd-host              # NBD 协议、易失/稳定镜像与断电策略
make test-sqlite-rollback-riscv # 静态/动态 CLI、多进程回滚日志
make test-sqlite-nbd-riscv      # QEMU 通过 Unix NBD 跑同一负载
make test-sqlite-recovery-riscv # 固定 Linux/BoarOS 与 NBD 热日志恢复
make test-sqlite-recovery-matrix-riscv # 小事务逐事件故障矩阵
make test-sqlite-wal-riscv      # 固定 Linux/BoarOS 双侧多进程 WAL 与重启
make test-sqlite-wal-recovery-riscv # 固定 Linux/BoarOS 的 WAL 正常与错误恢复
make test-sqlite-wal-recovery-matrix-riscv # WAL 逐事件断电/写/flush 故障矩阵
make test-offline-c-baseline-riscv # 双侧定位缺少客体原生编译器的第一失败
make test-offline-c-riscv # 固定 Alpine 原生 GCC，双侧五阶段离线编译与运行
make test-root-loongarch       # 同 LA 静态 musl/原 BusyBox ELF 对照 Linux，含错误与 OOM
make test-root-io-loongarch    # 真实 PCI 写故障保留 owner；独立于正常回收验收
make test-root-multi-block-riscv # 真实双盘、tmpfs 嵌套、忙引用与重启
make test-multi-disk-io-riscv # 暂扣一盘 I/O 与故障隔离
make test-sqlite-second-disk-riscv # 第二 ext4 盘 WAL 与重启
make test-offline-c-tmpfs-riscv # tmpfs 工作目录的同 ELF 离线 GCC
make test-busybox-tmpfs-riscv # 固定 BusyBox 在 tmpfs 上执行文件操作
make test-stack-usage
make test-lwext4-host
make test-environment-riscv      # 实际日志/RTC/OFD生命周期
make test-lwext4-cache-host # 命中先于回收、引用与失败owner
make test-lwext4-recovery-host # 日志与 orphan 的断电/故障矩阵
make test-lwext4-rename-host   # 改名、硬链接与最后链接回收的故障矩阵
make test-lwext4-metadata-host # 时间设置、空间计数与几何/失败验证
make inventory-userland-riscv  # 能力清单，不是必过门禁
make test-references
```

聚焦测试只在对应[模块文档](docs/README.md)维护。`make run-riscv` 不附根盘，启动后停留 timer-idle，需人工退出；`make debug-riscv` 以 `-S -s` 等待 GDB。完整比赛 Harness 当前仍因 LA 完整用户环境等能力阻塞，不算已通过。

`build/` 是可重建的本地产物目录，不是验证档案。仅长期保留内核/用户程序编译结果、工具链、当前配置的 Linux 构建缓存等可跨轮复用的产物；一次性运行目录、磁盘镜像、日志和旧构建缓存应在核对结果后清理。`python3 tests/prune-build.py` 预览，`make prune-build` 执行清理；`make clean` 连可复用的内核构建产物也删除。需要临时保留案例镜像以调试时，可给清单入口传 `--keep-pass-images`，调试结束后仍应清理。

## 近期工作与文档

[开发路线](docs/goals.md)统一记录本轮任务、分支交接和后续依赖。通用兼容性在 `main`，比赛环境与运行入口在 `oscomp-rv-compat`；后者单向合入已验收主线。只跑 RV 的原 judge 评分不等于双架构比赛交付，也不能把逐组诊断分数拼成正式总分。

统一VFS对象、活目录项、多挂载、共享后备、SysV shm、日志/RTC、串口TTY和真实网络
应用已交付，具体边界见[开发路线](docs/goals.md)与模块。R1–R8组合边界的修复覆盖
SHM附件/权限、msync来源pin、整包DGRAM、同步故障与整次写门闩；文件系统仍保留
journal、必要屏障、durable同步、orphan及恢复契约。

C0–C6提供默认关闭的成本观测；组提交、版本量封口、commit/checkpoint分离和批量
I/O已经接入。机制、历史性能口径和unknown见[成本分析](docs/learning/cost-baseline.md)，
不以内部计数下降替代真实程序效率。当前能力与下一项优化由有效应用证据选择。

评测兼容分支单向接纳main；main保留自身uname，旧glibc结果属于兼容配置。
完整Harness仍缺LA完整用户环境，单侧诊断和逐组补跑不能宣称完整交付。逐次成绩和运行
输出留在忽略的build；SMP、LA后续用户环境、实板及更大应用另行规划。

- [文档导航](docs/README.md)：模块契约与可复用学习材料。
- [工程原则](docs/design.md)与[贡献说明](CONTRIBUTING.md)：技术取舍、验证与提交边界。
- [固定资料](references/README.md)与[第三方代码](docs/third-party.md)：版本、来源及许可。

异步日志与组提交已启用并验收：操作私有 undo、挂载点 running group、不可变提交版本与 joinable worker 保持 ordered/log/commit/checkpoint 屏障；完整 lwext4、SQLite DELETE/WAL 恢复和双盘隔离通过。关闭观测的原版 iozone 三次启动，musl/glibc 五项写入共十格中位吞吐提升 25.29–54.00 倍，自动模式降至 16.450/18.372 秒，均达到该轮目标。原 1GiB iozone 专项得 24.8500/25.1791；它不是完整 Harness。重读 Max 下降36%–38%，Parent 提高12%–13%，不能由最快子进程推导整体读退化。热写同步、前台版本准备及未分类读请求仍有成本，见[机制与性能验收](docs/learning/cost-baseline.md#异步日志与组提交验收2026-10-01)。

S6–S8 存储流水线已落地：有界资源复用、封口与容量等待分离、durable commit 与 checkpoint 分离、八 span 批量 I/O 及热读共享 relatime 查询。最终恢复与系统回归通过，但 S9 收益目标未完成：匹配 S5 的十格 Parent 写吞吐为0.98–1.88倍，自动模式程序加同步收尾下降约22%–23%，未达到当时的性能预期；固定4MiB热读回退低于2%，musl 四进程普通读 Parent 回退17.05%。原1GiB iozone 专项为25.0271/25.3164，不代表完整 Harness。分配调用下降约91%，提交仍510组，说明前台与小事务固定成本仍须处理；完整分析、未关闭项及后续方向见[本轮验收](docs/learning/cost-baseline.md#s9-存储流水线验收2026-10-01)与[路线](docs/goals.md)。

块缓存已修正先回收再查询的命中破坏：生产目标8块不变，八块热工作集宿主预热后800次访问的额外设备读从800降为0。匹配旧/新三个关闭观测启动，自动程序加durable中位改善7.76%/6.99%，四进程普通读Parent改善14.08%/1.20%；glibc四进程整条命令增加0.73%，如实保留。定点读请求下降约68%，日志组/屏障仍510/1547，热读固定工作量0设备请求但存在前台成本；原停止规则不能用来证明调度饥饿，完整分布、资源与观测扰动见[纠错分析](docs/learning/cost-baseline.md#缓存查询顺序纠错2026-10-01)。日志/RTC/根盘真实内容及BusyBox55/55此前已验收，当前ABI累计1179条；完整228项本轮未重跑。后续主线统一见[开发路线](docs/goals.md)，固定吞吐倍数不作为开发准入条件。

原 iperf 3.13、netperf 2.7.0 的两种 libc 共22个受控子项完成实际传输、结果交换和退出，单连接16MiB、五连接各8MiB及UDP一万次请求响应另有内容核对。最终两种libc的代表测量统一在兼容分支，旧glibc结果不代表main版本身份支持。原连续iperf脚本仍有listener重建竞态，不能将受控完成写成原脚本全部通过；netperf原脚本两侧5/5。此前N2关闭观测三次启动的TCP接收吞吐中位为musl单/五连接242/352.6 Mbit/s、glibc261/346.7 Mbit/s；UDP_RR为6443/6612事务每秒。限制、丢包与成本解释见[网络应用结果](docs/learning/network-ownership.md#原版网络应用交付2026-10-02)。该段是N2 loopback测量；本轮真实网卡结果见下。

2026-10-02纠错轮已交付未连接TCP/零长度recv、UNIX数据报半关闭、接收扩容通知和活动reservation的终止事件等待；流发送复用同请求暂存尾部。journal取消64次操作软封口，保留版本量/首脏期限、同步和恢复协议。关闭观测三次启动，自动iozone程序加durable中位musl11.541→5.129秒、glibc12.108→5.359秒；四进程所选两组改善约8%–11%，最终卸载余量约0.04秒。单TCP仅249→250、268→274Mbit/s，非阻塞跨调用复制放大仍在，不能称为主要网络瓶颈已解决。完整lwext4/SQLite恢复与双盘、1179 ABI、相关系统回归已验收。后续调查确定性复现了timer切换造成的buddy/slab元数据竞态，并已加短临界区；该竞态可导致合法释放fatal，但原始iozone fatal缺少owner快照，无法确认那一次的具体触发链。Virtqueue告警依然未找到那次运行的具体队列破坏原因；新增确定性反例说明同一页双发会污染DMA owner，重跑未复现QEMU告警。结果、边界和重建见[本轮存储](docs/learning/cost-baseline.md#版本量封口与socket纠错对照2026-10-02)与[网络](docs/learning/network-ownership.md#本轮应用结果与剩余复制2026-10-02)。这段保留上一轮的机制与测量，不作为当前网卡能力的状态。

2026-10-02已交付N3：DTB发现的VirtIO-net legacy/modern、静态eth0、ARP及有界IPv4重组，IRQ收割、worker每批八帧。RX直接引用DMA至最后pbuf释放，最多32借用并有界回退；TX仍复制。原BusyBox wget/httpd在main/musl与兼容glibc的两种transport完成双向16MiB GET、16MiB CGI上传和4KiB文本POST。完整RV64、真实libc、1196 ABI、scale和栈，以及22项loopback已验收。

modern关闭观测三次启动，固定内容的单/五TCP双向总量效率中位297/285Mbit/s，匹配Linux1171/1107；完整程序12.151秒，退出后到根卸载关机另约2.035秒。两项计时不能混称为纯网络或checkpoint耗时。仍有复制、协议和应用固定成本，不设置倍数门槛；buddy/slab timer交错已修复；旧QEMU Virtqueue告警未确定具体来源。公网、DNS/TLS和外部IPv6未交付。后续主线与近期队列统一见[开发路线](docs/goals.md)。

接口与owner见[网卡模块](docs/modules/riscv-virtio-net.md)，内容、分布和剩余成本见[真实网卡记录](docs/learning/network-ownership.md#真实-virtio-net-与宿主应用交付2026-10-02)。下一应用由真实需求选择，现有路线只保留一项待选应用与证据触发的性能候选。

2026-10-03本轮已先补匿名管道共享元数据：fchmod/fstat与两端、dup/fork/proc重开一致。
命名FIFO与原GCC/make的Lua5.4.3工程已交付构建、增量、失败恢复、默认-j2和产物运行，构建成本已收口，-j1/-j2中位135.542/133.225秒；tmpfs表明工作目录存储不足以解释整体差距，清理元数据与内存CPU候选见[工具链记录](docs/learning/offline-toolchain-probe.md)，具体进度归[开发路线](docs/goals.md)。

2026-10-03串口TTY已交付：原ash/stty在modern/legacy完成设置恢复、行编辑、管线与重定向、
前台cat/sleep/计算程序Ctrl-C，以及停止、jobs/bg/fg、后台TTIN/TTOU和退出回收。
控制终端、64字节readv continuation、fault前缀、取消和旧OFD hangup由同ELF的
27＋80条Linux差分及真实资源清理保护，见[TTY契约](docs/modules/kernel-tty.md)与
[应用机制](docs/learning/session-consumers.md#原串口ashstty与控制终端2026-10-03)。

CPU线已有默认关闭的有限诊断。选定heap清零/搬迁与页内usercopy不足以解释原工程
成本；保守宽字候选改善对齐微实验，但没有可验证的工程收益，**未启用到main**。
生产保留字节实现与诊断，见
[实际工作与候选判断](docs/learning/offline-toolchain-probe.md#内存操作的有限归因与未上线候选2026-10-03)。
文件所有权已接入fchown/fchownat：修改真实inode的UID/GID、ctime和特权位，
ext4在同一事务删除文件capability属性；进程身份仍固定root。线程退出直接选择ready
任务，清理继续由原worker拥有。Unix98 PTY、devpts、packet 和真实 libc PTY API 已交付；
原 BusyBox script/replay 可录制原 ash/stty、控制作业并读取完整关闭尾部，显式同步后的
录制文件可以重启读取。资源与应用语义见[TTY契约](docs/modules/kernel-tty.md)，
原程序的退出状态、resize和持久化边界见[消费者机制](docs/learning/session-consumers.md#pty-的身份传输与应用边界)。
O_PATH已持有独立路径资格，支持目录相对与空路径身份操作，不获得数据或设备打开资格。
路径truncate已接入共同截断与capability清理，负长度先于路径访问拒绝。
空路径stat接受NULL并避免完整路径缓冲；glibc的fstat经newfstatat进入时也复用fd资格。
全局sync与挂载范围syncfs已接入稳定引用快照、数据交接及日志durable等待；
syncfs独立观察挂载错误，dup/fork共享OFD游标。它们不将checkpoint强行并入每次同步。
原生accept4仍缺；原LTP准备依赖、旧libc包装、
镜像环境与目标接口失败分开，不能由遍历结束宣称完整兼容。
性能候选包括元数据路径、短睡眠deadline和协议背压；按真实工作量选择一项，不因理论
先进或微实验更快自动上线，见[证据读法](docs/learning/user-program-inventory.md#性能结果必须对应实际工作)。
