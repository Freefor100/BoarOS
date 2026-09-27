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
| 虚拟内存 | VMA、按需匿名页、共享匿名与共享文件映射、文件私有 COW、共享文件首次写追踪、`msync`、跨 MM 截断撤映射 | 无 `mremap`、共享文件 futex、匿名共享页 swap 回收或 SMP 页表同步 |
| ELF / exec | 按需 ELF、PIE、`PT_INTERP`、初始栈/auxv、musl DSO/TLS、失败保持旧映像 | glibc 未独立验证；无 shebang、`getrandom` |
| 进程与等待 | fork/vfork、pthread clone、线程组退出、非组长 exec、wait/zombie/reparent、FIFO 抢占、时钟与睡眠 | 合法 clone 组合仍有限；无完整会话/TTY；单 hart 关中断不等于跨核同步 |
| futex / 信号 | WAIT/WAKE/REQUEUE、超时/重启、跨 MM 共享匿名 futex、同 MM 非 PI robust-list 退出清理、标准信号、用户 handler、`rt_sigtimedwait` | 无共享文件 futex、PI futex、实时信号队列和 `sigaltstack`；单 hart 验证范围 |
| 文件与事件 | fd/OFD 分离、dup/CLOEXEC、共享 offset、阻塞 pin、部分/向量/定位 I/O、pipe、poll/select/epoll；传统与 OFD 记录锁；ext4 节点按设备号接入 null、zero、console | 无 devfs、完整 TTY 或 socket 后端；设备 mmap 未支持 |
| 路径与 ext4 | 共享活目录项、cwd/dirfd、普通/NOREPLACE rename、可写/只读根盘、符号链接、目录枚举、稀疏文件、显式纳秒时间、真实文件系统统计、打开后删除、私有映射截断 | 无硬链接、EXCHANGE/WHITEOUT、多挂载或完整权限 |
| 缓存与存储 | read/write/private fault 共用文件页、inode 脏范围与定向写回、OFD 错误观察、`fsync/fdatasync/O_SYNC/O_DSYNC`；VirtIO legacy/modern flush | ordered journal/replay、持久 orphan；恢复承诺限于已验证块模型，无后台写回线程 |
| 身份与资源 | 单用户 root 的 UID/GID 查询；线程组共享并执行 NOFILE/STACK，fork 继承、exec 保留 | 无凭据变更/完整权限；fd 硬容量 1024、栈硬容量 8 MiB；其他有效 limit 返回 `ENOTSUP` |
| 平台与网络 | RISC-V QEMU 真实根盘 `/init` 与 musl 用户态 | 无 socket 传输链、外部中断、LoongArch、实板或多核验证 |

文件层已有部分读写、OFD 生命周期、稀疏文件与映射截断的语义深度；显式时间设置和真实挂载统计已接入；共享匿名映射与共享文件页可跨 MM 读写，完整 TTY 仍有缺口。ext4 恢复已覆盖 512 字节原子写、未 flush 写丢失或重排的故障模型；实板持久性仍待独立验证。动态 musl 通过不代表完整 glibc 兼容。

固定 SQLite 3.53.4 的原生 Unix VFS 已在单 hart 上运行静态/动态 CLI、多进程 DELETE 回滚日志和普通多进程 WAL；WAL 工作负载用同一 ELF 在固定 Linux 与 BoarOS 验证 writer 竞争、未提交进程退出及第二次启动后的完整性。回滚日志的 EXTRA/FULL 故障恢复有独立 NBD 验收；WAL 尚无存储断电矩阵，实板持久性未验证。

2026-09-27 最近一次固定 BusyBox/libc-test 全量验证（输入与身份见[程序清单](docs/learning/user-program-inventory.md)）：228 个顶层案例全部完成，223 项双侧一致、2 个直接 entry 退出不符、3 个包装脚本断言失败；失败仍是原 socket 静态/动态和原包装器五项，无旧通过项回退。BusyBox 原脚本为 50/55 success，另有独立 pwd/cd/mv/touch 组合双侧通过。包装脚本与 entry 有重叠，清单完成不等于全部兼容。

## 构建与验证

需要 RISC-V bare-metal GCC/binutils、GNU Make 和 QEMU；支持 `riscv64-unknown-elf-` 与 `riscv64-elf-` 前缀。真实用户态和 Linux 差分的额外工具见[工具链](docs/toolchain.md)及[差分模块](docs/modules/differential-abi.md)。

```sh
make all                       # kernel-rv
make test-riscv                 # 通用模块、架构与真实根启动
make test-userland-riscv        # 静态 musl、动态 pthread / TLS
make test-diff-abi-riscv        # 同一 ELF 对照固定 Linux
make test-record-lock-host      # 区间树随机模型、所有权与分配失败
make test-record-lock-riscv     # 同 ELF 的 Linux/BoarOS 线程、fork、fd 复用、退出
make test-nbd-host              # NBD 协议、易失/稳定镜像与断电策略
make test-sqlite-rollback-riscv # 静态/动态 CLI、多进程回滚日志
make test-sqlite-nbd-riscv      # QEMU 通过 Unix NBD 跑同一负载
make test-sqlite-recovery-riscv # 固定 Linux/BoarOS 与 NBD 热日志恢复
make test-sqlite-recovery-matrix-riscv # 小事务逐事件故障矩阵
make test-sqlite-wal-riscv      # 固定 Linux/BoarOS 双侧多进程 WAL 与重启
make test-stack-usage
make test-lwext4-host
make test-lwext4-recovery-host # 日志与 orphan 的断电/故障矩阵
make test-lwext4-rename-host   # 原子改名、覆盖与目录移动的故障矩阵
make test-lwext4-metadata-host # 时间设置、空间计数与几何/失败验证
make inventory-userland-riscv  # 能力清单，不是必过门禁
make test-references
```

聚焦测试只在对应[模块文档](docs/README.md)维护。`make run-riscv` 不附根盘，启动后停留 timer-idle，需人工退出；`make debug-riscv` 以 `-S -s` 等待 GDB。完整比赛 Harness 当前因缺少 `kernel-la` 等能力阻塞，不算已通过。

`build/` 是可重建的本地产物目录，不是验证档案。仅长期保留内核/用户程序编译结果、工具链、当前配置的 Linux 构建缓存等可跨轮复用的产物；一次性运行目录、磁盘镜像、日志和旧构建缓存应在核对结果后清理。`python3 tests/prune-build.py` 预览，`make prune-build` 执行清理；`make clean` 连可复用的内核构建产物也删除。需要临时保留案例镜像以调试时，可给清单入口传 `--keep-pass-images`，调试结束后仍应清理。

## 近期工作与文档

[TODO 与阶段依赖](docs/goals.md)集中维护下一步、阻塞与验收：块同步、逐 inode 写回、journal/replay、cwd/dirfd/rename、时间与统计、同 MM 非 PI robust-list、共享匿名对象及其跨 MM futex 已落地；记录锁、SQLite 回滚日志、共享文件映射、`msync` 和普通多进程 WAL 各有验证入口。历史取消异常在固定输入重复运行中未复现，根因仍未确定。共享文件页的错误注入与 WAL 断电恢复矩阵、不同 VA 的共享 futex、`mremap`/`madvise` 仍按各自依赖推进；glibc、LoongArch 和网络另行推进。SMP 先验证所有权、唤醒和 TLB 回收，再谈调度策略与性能。

- [文档导航](docs/README.md)：模块契约与可复用学习材料。
- [工程原则](docs/design.md)与[贡献说明](CONTRIBUTING.md)：技术取舍、验证与提交边界。
- [固定资料](references/README.md)与[第三方代码](docs/third-party.md)：版本、来源及许可。
