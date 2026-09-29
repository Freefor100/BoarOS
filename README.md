# BoarOS

<p align="center">
  <img src="assets/boaros_header.png" alt="BoarOS 吉祥物与字标" width="100%">
</p>

BoarOS 是从零搭建、面向 OS Comp 能力建设的 C / 少量汇编内核，目标是运行未经 BoarOS 特改的 Linux 用户程序。Linux 是用户可观察契约和机制参考：在声明支持的范围内，返回值、对象身份、共享关系、错误、并发与生命周期必须正确；内部结构不必复制 Linux。

## 当前能力

当前生产路径为 **RV64、QEMU virt、单 hart、Sv39 / 4 KiB 页**。下表是已验证子集，具体接口与限制见模块文档。

| 范围 | 已有能力 | 主要边界 |
|---|---|---|
| 启动与内存 | OpenSBI、DTB、高半区/direct map、buddy/slab、连续页和引用回收 | 无 SMP；任务栈有 canary/高水位，没有未映射 guard page |
| 虚拟内存 | VMA、按需匿名页、共享匿名与共享文件映射、文件私有 COW、共享文件首次写追踪、`msync`、跨 MM 截断撤映射 | 无 `mremap`、按操作区分的 `madvise`、共享文件 futex、匿名共享页 swap 回收或 SMP 页表同步 |
| ELF / exec | shebang、按需 ELF、PIE、`PT_INTERP`、初始栈/auxv、musl DSO/TLS、固定 glibc 2.44 启动/TLS/pthread 子集、失败保持旧映像 | 无 `execveat`；glibc 应用覆盖尚有限 |
| 进程与等待 | 统一 TID/TGID/PGID/SID 身份对象、会话/进程组、fork/vfork、child-TID 生命周期差分、pthread clone、线程组退出、非组长 exec、wait/zombie/reparent、时钟与睡眠 | 合法 clone 组合仍有限；无 TTY 作业控制；单 hart 关中断不等于跨核同步 |
| 调度 | OTHER tick 轮转、FIFO/RR 1–99 优先级、CPU0 affinity、RESET_ON_FORK、可配置全局实时预算及 proc 查询 | 默认 1 秒 / 950 毫秒；无 nice 权重、PI、SMP 或硬实时保证 |
| 随机数 | ChaCha20 fast-key-erasure、BLAKE2s 混种、legacy/modern VirtIO RNG、`getrandom` 与 random/urandom 字符节点 | QEMU 宿主是信任边界；DTB/用户写入不计可信熵，缺设备时保持未就绪 |
| futex / 信号 | WAIT/WAKE/REQUEUE、超时/重启、跨 MM 共享匿名 futex、同 MM 非 PI robust-list 退出清理、标准信号、用户 handler、`rt_sigtimedwait` | 无共享文件 futex、PI futex、实时信号队列和 `sigaltstack`；单 hart 验证范围 |
| 文件与事件 | fd/OFD 分离、dup/CLOEXEC、共享 offset、阻塞 pin、部分/向量/定位 I/O、pipe、poll/select/epoll；传统与 OFD 记录锁；socket OFD 与读写/就绪；mknodat 字符节点按设备号接入 null、zero、console | 无 devfs、完整 TTY；设备 mmap 未支持 |
| 路径与 ext4 | 共享活目录项、cwd/dirfd、普通/NOREPLACE rename、可写/只读根盘、符号链接、目录枚举、稀疏文件、显式纳秒时间、真实文件系统统计、打开后删除、私有映射截断；共享挂载树可用户态挂载/卸载 proc、tmpfs 和第二 ext4 盘，通用 linkat 硬链接，含 meminfo、uptime、self、exe/cwd/root/fd、挂载信息与首批进程 stat/status 字段 | 无 EXCHANGE/WHITEOUT 或完整权限；缺少 /dev/console 节点时的初始标准 fd 没有路径链接，meminfo 已提供真实缓存/共享/脏页/可用量，完整进程字段尚未完成 |
| 内存文件 | 统一稀疏内存后备对象、tmpfs 页/inode 配额、硬链接、共享/私有映射，musl POSIX 共享内存、SysV 共享内存和 tmpfs 工作目录的离线 GCC | 无 swap、SysV 信号量/消息队列、共享文件 futex；tmpfs 不持久化 |
| 缓存与存储 | read/write/private fault 共用文件页、inode 脏范围与定向写回、OFD 错误观察、`fsync/fdatasync/O_SYNC/O_DSYNC`；VirtIO legacy/modern 多设备独立 IRQ/队列、每实例页缓存/worker 与 flush 屏障 | ordered journal/replay、持久 orphan；恢复承诺限于已验证块模型，已接入阈值驱动后台写回与 2%/4% 空闲水位回收，无周期清脏 |
| 身份与资源 | 单用户 root 的 UID/GID 查询；线程组共享并执行 NOFILE/STACK，fork 继承、exec 保留 | 无凭据变更/完整权限；fd 硬容量 1024、栈硬容量 8 MiB；其他有效 limit 返回 `ENOTSUP` |
| 平台与网络 | RISC-V QEMU 真实根盘可配置 PID 1（默认 `/init`） 与 musl 用户态；单 hart IPv4 UDP/TCP loopback，固定 lwIP 2.2.1 raw API，AF_UNIX socketpair | 无命名 AF_UNIX 端点、真实网卡链路、LoongArch、实板或多核验证 |

单 hart 存储等待已由运行期 IRQ 唤醒：两个不同文件冷读可同时在途，等待期间计算与无关缓存命中继续执行；OFD、inode、后端事务与退出清理各自保留 owner。八槽乱序完成、flush 屏障和一秒超时 reset 在 legacy/modern、writeback/writethrough 四种组合验收，见[可睡眠存储](docs/learning/sleepable-storage.md)。

文件层已有部分读写、OFD 生命周期、稀疏文件与映射截断的语义深度；显式时间设置和真实挂载统计已接入；共享匿名映射已迁移统一稀疏内存后备对象，与共享文件页均可跨 MM 读写，完整 TTY 仍有缺口。ext4 恢复已覆盖 512 字节原子写、未 flush 写丢失或重排的故障模型；实板持久性仍待独立验证。固定 glibc 2.44 的五种 ELF 形态与 TLS/pthread/信号组合已双侧验证，完整 glibc 应用兼容尚未证明。

内存统计按文件页、共享匿名/tmpfs 后备页和各盘块缓冲真实 owner 计量；`sysinfo` 返回真实任务数与 1/5/15 分钟负载。原镜像 BusyBox `free` 已显示有效容量，LTP 越过缺失 `Cached` 的阻塞。已新增由真实 timer 快照支持的 coarse clock，并通过窄差分；原静态/动态 glibc `utime` 各 30 次复跑通过，诊断环境边界见[文件时间](docs/learning/file-timestamps.md)。LTP cgroup 辅助程序等待已独立定位，见[路线与验收](docs/goals.md)。

固定 SQLite 3.53.4 的原生 Unix VFS 已在单 hart 上运行静态/动态 CLI、多进程 DELETE 回滚日志和普通多进程 WAL，并验证第二盘 WAL 的独立重启读回；WAL 工作负载用同一 ELF 在固定 Linux 与 BoarOS 验证 writer 竞争、未提交进程退出及第二次启动后的完整性。DELETE 与 WAL 的 EXTRA/FULL 恢复各有 NBD 断电/故障矩阵；实板持久性未验证。

客体内固定 Alpine v3.22 RV64 GCC 14.2.0-r6 已在同一离线镜像上完成预处理、编译、汇编、静态链接和运行；固定 Linux 与 BoarOS 的五阶段状态、产物哈希和输出一致。同一编译流程也通过 tmpfs 工作目录；产物复制到根盘供比对，不代表 tmpfs 持久。范围是固定的小型 C 负载，其他项目和 Rust 尚未验收。

固定 BusyBox/libc-test 最近一次全量清单为 228 项、227 项双侧通过，原 BusyBox 包装器内部 53/55 成功，dmesg 缺 klogctl、hwclock 缺 RTC 字符接口；身份、日期及逐项边界见[程序清单](docs/learning/user-program-inventory.md)。通用 ABI 差分 1000 条匹配，包含 tmpfs、硬链接与共享映射时间语义；成本门禁见[单核规模回归](docs/learning/single-hart-scale.md)，不以 QEMU 墙钟倍数宣称性能。

## 构建与验证

需要 RISC-V bare-metal GCC/binutils、GNU Make 和 QEMU；支持 `riscv64-unknown-elf-` 与 `riscv64-elf-` 前缀。真实用户态和 Linux 差分的额外工具见[工具链](docs/toolchain.md)及[差分模块](docs/modules/differential-abi.md)。

```sh
make all                       # kernel-rv
make test-riscv                 # 通用模块、架构与真实根启动
make test-userland-riscv        # 静态 musl、动态 pthread / TLS
make test-glibc-riscv           # 固定 glibc 2.44 静态/动态/PIE、TLS、pthread
make test-diff-abi-riscv        # 同一 ELF 对照固定 Linux
make test-io-sleep-riscv        # 暂扣响应验证并发、计算/缓存进展、flush 与 reset
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
make test-root-multi-block-riscv # 真实双盘、tmpfs 嵌套、忙引用与重启
make test-multi-disk-io-riscv # 暂扣一盘 I/O 与故障隔离
make test-sqlite-second-disk-riscv # 第二 ext4 盘 WAL 与重启
make test-offline-c-tmpfs-riscv # tmpfs 工作目录的同 ELF 离线 GCC
make test-busybox-tmpfs-riscv # 固定 BusyBox 在 tmpfs 上执行文件操作
make test-stack-usage
make test-lwext4-host
make test-lwext4-recovery-host # 日志与 orphan 的断电/故障矩阵
make test-lwext4-rename-host   # 改名、硬链接与最后链接回收的故障矩阵
make test-lwext4-metadata-host # 时间设置、空间计数与几何/失败验证
make inventory-userland-riscv  # 能力清单，不是必过门禁
make test-references
```

聚焦测试只在对应[模块文档](docs/README.md)维护。`make run-riscv` 不附根盘，启动后停留 timer-idle，需人工退出；`make debug-riscv` 以 `-S -s` 等待 GDB。完整比赛 Harness 当前因缺少 `kernel-la` 等能力阻塞，不算已通过。

`build/` 是可重建的本地产物目录，不是验证档案。仅长期保留内核/用户程序编译结果、工具链、当前配置的 Linux 构建缓存等可跨轮复用的产物；一次性运行目录、磁盘镜像、日志和旧构建缓存应在核对结果后清理。`python3 tests/prune-build.py` 预览，`make prune-build` 执行清理；`make clean` 连可复用的内核构建产物也删除。需要临时保留案例镜像以调试时，可给清单入口传 `--keep-pass-images`，调试结束后仍应清理。

## 近期工作与文档

[开发路线](docs/goals.md)统一记录本轮任务、分支交接和后续依赖。通用兼容性在 `main`，比赛环境与运行入口在 `oscomp-rv-compat`；后者单向合入已验收主线。只跑 RV 的原 judge 评分不等于双架构比赛交付，也不能把逐组诊断分数拼成正式总分。

已按统一 VFS 对象路线分阶段拆分 ext4 后端、实现挂载路径和首批真实 procfs。真实内存快照、RV64 sysinfo、后台写回及 proc fd 复用压力已接入；统一内存后备对象、tmpfs、硬链接和真实第二 ext4 磁盘已接入，验收见[多挂载证据](docs/learning/memory-backed-mounts.md)。会话/进程组、可信随机数、真实调度、coarse clock、AF_UNIX socketpair 与 SysV 共享内存已完成本轮验收；同期量化文件写入、同步、扫描和锁等待成本，再依据证据选择优化。随后分别闭环日志/RTC 用户接口，iperf、netperf、iozone 继续独立定位。SMP、LoongArch、实板和更大工具链按新基线另行排期。

- [文档导航](docs/README.md)：模块契约与可复用学习材料。
- [工程原则](docs/design.md)与[贡献说明](CONTRIBUTING.md)：技术取舍、验证与提交边界。
- [固定资料](references/README.md)与[第三方代码](docs/third-party.md)：版本、来源及许可。
