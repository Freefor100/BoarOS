# BoarOS

<p align="center">
  <img src="assets/boaros_header.png" alt="BoarOS 吉祥物与字标" width="100%">
</p>

BoarOS 是 C／少量汇编内核，目标是运行未经 BoarOS 特改的 Linux 用户程序。
Linux 是用户可观察语义的参考；内部实现独立维护对象所有权、失败回滚和资源回收。

## 当前能力

生产平台为单 CPU QEMU virt：**RV64、Sv39／4 KiB 页**和**LA64、LA464、16 KiB／三级页表**。
两侧共用 MM、调度、VFS、设备和用户程序策略，架构实现在构建期绑定。

| 范围 | 已验证能力 | 主要边界／模块 |
|---|---|---|
| 内存与 CPU | buddy/slab、连续页与引用、VMA、按需页、共享匿名/文件映射、COW、独立栈窗口与真实 guard；RV F/D、LA FPU/LSX/LASX | 无 SMP、远程 TLB、RV V；固定 QEMU 不提供 LA LBT；[物理页](docs/modules/physical-pages.md)、[MM](docs/modules/kernel-mm.md)、[LA](docs/modules/loongarch-boot.md) |
| ELF 与运行时 | 静态/动态 ELF、PIE、shebang、解释器、musl DSO/TLS；RV glibc 2.44、LA glibc 2.42 的五种形态及线程/取消子集 | 无 execveat，更广 glibc 应用仍须逐个验收；[ELF](docs/modules/user-elf.md)、[exec](docs/modules/kernel-exec.md) |
| 进程与同步 | fork/vfork/pthread、线程组/会话/进程组、exec/wait/退出回收；OTHER/FIFO/RR、deadline；基本 futex、robust-list、标准信号和同步 fault | 单 CPU；无 PI futex、共享文件 futex、实时信号队列、sigaltstack；[调度](docs/modules/kernel-scheduler.md)、[信号](docs/modules/kernel-signal.md) |
| 文件与事件 | fd/OFD、dup、阻塞 pin、向量/定位 I/O、pipe/FIFO、poll/select/epoll、传统/OFD 记录锁、活 inode/路径、link/rename | 无完整凭据/权限、EXCHANGE/WHITEOUT；[文件接口](docs/modules/kernel-files.md) |
| 文件系统与挂载 | ext4、procfs、tmpfs、devpts、POSIX/SysV 共享内存；用户态 mount 与普通忙卸载、独立第二 ext4 盘 | 无 bind/remount/move/传播、扩展卸载 flags 或 mount namespace；[VFS](docs/modules/vfs-ext4.md)、[tmpfs](docs/modules/tmpfs.md) |
| 存储 | 共用 VirtIO transport/split queue、批量/乱序 I/O、flush/reset、页缓存、后台写回/压力回收、ordered journal、durable commit/checkpoint、orphan/replay | RV legacy/modern MMIO；LA modern PCI及transitional设备的modern接口，纯legacy PCI未接；[框架](docs/modules/virtio-framework.md)、[存储](docs/modules/vfs-ext4.md) |
| 网络 | IPv4/IPv6 loopback、TCP/UDP、AF_UNIX socketpair；VirtIO-net、静态 IPv4/ARP、有界分片、宿主双向 TCP/HTTP、RX loan 与 TX SG/复制回退 | 无命名 AF_UNIX、外部 IPv6、DHCP/DNS/TLS；[网络](docs/modules/kernel-network.md) |
| 终端与系统服务 | IRQ UART、TTY/termios2、作业控制、Unix98 PTY/devpts、原 BusyBox ash/stty/script；日志、可信 RNG、RTC、时钟/睡眠、NOFILE/STACK | 固定 root；无其他行规程、完整 modem 控制或全部资源限制；[TTY](docs/modules/kernel-tty.md)、[日志](docs/modules/kernel-log.md) |

2026-10-09 本地验收：RV/LA 在 512 MiB、1 GiB 下，各自完成 **229 项原程序**的同 ELF Linux 对照；
两侧各 **1366 条 ABI 差分**、glibc 五形态、栈/guard，以及相关存储、设备、网络、终端组合通过。
具体输入与适用范围见[程序基线](docs/learning/user-program-inventory.md)和
[allocator 组合验收](docs/learning/memory-management.md#阶段a组合收口2026-10-09)。
这些结果限定于上述平台和案例；LA 客体原生开发、全断电恢复矩阵、实板与多核仍需独立验收。

主线 [CI](docs/modules/continuous-integration.md)覆盖共用 host、RV 回归、LA 核心/平台和双侧运行时，
原程序/设备组合及存储恢复分层运行。本轮结果来自本地执行，未重新运行托管 CI。

## 构建与验证

工具和固定环境见[工具链](docs/toolchain.md)、[参考资料](references/README.md)。默认 `make all` 生成 `kernel-rv`。

```sh
make all
make kernel-la
make test-riscv
make test-loongarch
make test-userland-riscv
make test-glibc-riscv test-glibc-loongarch
make test-diff-abi-riscv test-diff-abi-loongarch
make test-allocator-cost-host
make test-sync-host test-allocator-concurrency-host
make test-wait-host test-sleep-lock-host test-wait-riscv test-wait-loongarch
make test-stack-usage test-stack-usage-la
```

完整分层入口见[CI 模块](docs/modules/continuous-integration.md)，聚焦命令见[模块导航](docs/README.md)。
通用根盘 fixture 使用 `INIT_CONFIG=config/init.json`。`make run-riscv` 不附根盘，停留 timer-idle；
`make debug-riscv` 以 `-S -s` 等待 GDB。LA 启动参数见[LA 模块](docs/modules/loongarch-boot.md)。

`build/` 仅长期保留可复用构建缓存。`python3 -B tests/prune-build.py` 预览、`make prune-build` 清理已核对的运行目录、
日志和镜像；需要保留现场时使用 `PRUNE_BUILD_KEEP`。`make clean` 会同时删除可复用内核构建产物。

## 开发方向与文档

当前主线转向 **SMP 同步基础与生命周期正确性**，再进入 RV 两核、共享 MM/TLB 和 LA 多核；
具体未完成能力与依赖只维护在[开发路线](docs/goals.md)，逐轮执行计划留在会话中。
CPU 本地/current、嵌套抢占控制与 raw 短锁已接入 buddy/slab 元数据；等待代次、借用游标、
新栈交接与可睡眠锁内部资格已建立。压力回收、共享MM及其他业务对象仍需跨核保护。
等待交接的[固定P核匹配时间](docs/learning/cost-baseline.md#等待交接的匹配时间)单列默认业务、
IRQ/锁/唤醒和观测开销；匿名生命周期额外约2.5%–4.0%，部分组合窗口仍有成本与漂移，未取得普遍性能提升。

buddy 已消除小页操作随无关大块页数线性检查/重写的问题，并接入只读查询快路径与完整审计。
4/16 KiB 工作量及组合门禁通过；森林改造的默认匿名生命周期回退 **13.6%–22.4%**，
后续 raw 互斥相对其独立基线另增加 **6.0%–7.3%**，未取得典型吞吐提升。
机制见[物理页模块](docs/modules/physical-pages.md)，时间、IRQ 和观测开销分别见[森林结果](docs/learning/cost-baseline.md#buddy-森林匹配时间2026-10-09)与[短锁结果](docs/learning/cost-baseline.md#allocator-短锁的匹配时间)。

文档分工：README 概括能力和常用入口；`docs/modules/` 维护当前契约与聚焦验证；
`docs/learning/` 解释机制、取舍和验收证据；`docs/goals.md` 维护未完成工作。
完成项从路线图撤下，历史问题的具体归因未确认时仍保留证据边界。

开发分支为 `main` 与 `oscomp-compat`。通用修复先落 main，再单向集成到兼容分支；
比赛启动、辅助脚本、运行时适配、官方容器结果和子项分数由兼容分支维护。
main 默认构建不发布或预加载 LA 原盘调度适配 DSO。

- [文档导航](docs/README.md)：模块、学习材料和当前基线。
- [工程原则](docs/design.md)、[贡献说明](CONTRIBUTING.md)：设计、验证和提交规则。
- [固定资料](references/README.md)、[第三方代码](docs/third-party.md)：版本、来源与许可。
