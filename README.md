# BoarOS

<p align="center">
  <img src="assets/boaros_header.png" alt="BoarOS 吉祥物与字标" width="100%">
</p>

BoarOS 是 C／少量汇编内核，目标是运行未经 BoarOS 特改的 Linux 用户程序。
Linux 是用户可观察语义的参考；内部实现独立维护对象所有权、失败回滚和资源回收。

## 当前能力

默认生产平台为单 CPU QEMU virt：**RV64、Sv39／4 KiB 页**和**LA64、LA464、16 KiB／三级页表**。
两侧共用 MM、调度、VFS、设备和用户程序策略，架构实现在构建期绑定。
RV 两核调度支持受控内核任务和独立 `kernel-rv-user-smp` 的真实用户执行。普通与受控 MM、页缓存、FD/OFD 共用保护与引用规则；匿名缺页、驻留用户复制及普通文件缓存读写在发起 CPU 完成。冷读只把缺失批次交给既有 CPU0 worker，保留 128 KiB SG；回写、同步的元数据准备和后端最终释放仍由 CPU0 执行，持久化等待由原调用者执行。命名空间、进程生命周期、设备和网络在实际业务入口串行，不再迁移整次通用 syscall。执行边界与验证见[RV 两核](docs/modules/riscv-smp.md#真实用户态与cpu0业务交接)。

2026-10-14 的原程序记录包含 2/1 CPU、512 MiB/1 GiB 四个 USER_SMP profile，各 229 项通过；这是当时实现的结果。本轮普通对象并发、调用 CPU 执行位置及 SQLite 独立重启结果记录在对应[模块说明](docs/modules/riscv-smp.md#用户态原程序与压力组合)。

| 范围 | 已验证能力 | 主要边界／模块 |
|---|---|---|
| 内存与 CPU | buddy/slab、连续页与引用、VMA、按需页、共享匿名/文件映射、COW、独立栈窗口与真实 guard；RV F/D、LA FPU/LSX/LASX；独立 RV 受控两核配置 | 设备、网络及命名空间的并行内核业务尚未开放，RV V；固定 QEMU 不提供 LA LBT；[物理页](docs/modules/physical-pages.md)、[MM](docs/modules/kernel-mm.md)、[两核边界](docs/modules/riscv-smp.md)、[LA](docs/modules/loongarch-boot.md) |
| ELF 与运行时 | 静态/动态 ELF、PIE、shebang、解释器、musl DSO/TLS；RV glibc 2.44、LA glibc 2.42 的五种形态及线程/取消子集 | 无 execveat，更广 glibc 应用仍须逐个验收；[ELF](docs/modules/user-elf.md)、[exec](docs/modules/kernel-exec.md) |
| 进程与同步 | fork/vfork/pthread、线程组/会话/进程组、exec/wait/退出回收；OTHER/FIFO/RR、deadline；private/共享匿名/共享文件/SysV futex、robust-list、标准信号和同步 fault | 默认用户任务限CPU0，独立 RV 用户 SMP 仅在需要串行的业务范围进入 CPU0；无 PI futex、实时信号队列、sigaltstack；[调度](docs/modules/kernel-scheduler.md)、[信号](docs/modules/kernel-signal.md) |
| 文件与事件 | fd/OFD、dup、阻塞 pin、向量/定位 I/O、pipe/FIFO、poll/select/epoll、传统/OFD 记录锁、活 inode/路径、link/rename；受控RV共享FD/匿名pipe两核 | 普通文件数据访问可就地执行，socket/epoll/FIFO 业务仍在 CPU0；无完整凭据/权限、EXCHANGE/WHITEOUT；[文件接口](docs/modules/kernel-files.md) |
| 文件系统与挂载 | ext4、procfs、tmpfs、devpts、POSIX/SysV 共享内存；用户态 mount 与普通忙卸载、独立第二 ext4 盘 | 无 bind/remount/move/传播、扩展卸载 flags 或 mount namespace；[VFS](docs/modules/vfs-ext4.md)、[tmpfs](docs/modules/tmpfs.md) |
| 存储 | 共用 VirtIO transport/split queue、批量/乱序 I/O、flush/reset、页缓存、后台写回/压力回收、ordered journal、durable commit/checkpoint、orphan/replay | RV legacy/modern MMIO；LA modern PCI及transitional设备的modern接口，纯legacy PCI未接；[框架](docs/modules/virtio-framework.md)、[存储](docs/modules/vfs-ext4.md) |
| 网络 | IPv4/IPv6 loopback、TCP/UDP、AF_UNIX socketpair；VirtIO-net、静态 IPv4/ARP、有界分片、宿主双向 TCP/HTTP、RX loan 与 TX SG/复制回退 | 无命名 AF_UNIX、外部 IPv6、DHCP/DNS/TLS；[网络](docs/modules/kernel-network.md) |
| 终端与系统服务 | IRQ UART、TTY/termios2、作业控制、Unix98 PTY/devpts、原 BusyBox ash/stty/script；日志、可信 RNG、RTC、时钟/睡眠、NOFILE/STACK | 固定 root；无其他行规程、完整 modem 控制或全部资源限制；[TTY](docs/modules/kernel-tty.md)、[日志](docs/modules/kernel-log.md) |

2026-10-14 本地默认单核组合复核：RV/LA在512MiB、1GiB下的BoarOS各完成 **229 项原程序**；RV 1GiB和LA双RAM的同ELF联合对照全部通过。RV 512MiB固定Linux原BusyBox脚本的`du`动态`/proc`遍历失败，该组保留`reference-not-pass`，BoarOS该组成功；此前LA参考失败也保留原证据，详见[程序基线](docs/learning/user-program-inventory.md)。
两侧各 **1400 条 ABI 差分**、glibc 五形态、栈/guard，以及相关存储、设备、网络、终端组合通过。
具体输入与适用范围见[程序基线](docs/learning/user-program-inventory.md)和
[调度组合核对](docs/learning/threads-and-futex.md#调度域身份发布与退休存储)。
这些结果限定于上述平台和案例；LA客体原生开发、更多断电/组合矩阵、实板、超出229项的用户SMP应用与一般文件及外部后备的并行内核业务仍需独立验收。

主线 [CI](docs/modules/continuous-integration.md)以同一模板覆盖共用 host、双侧 core/ABI、platform 和 glibc/SQLite，
另有独立RV受控两核与真实用户两核结果组；
原程序/设备组合及存储恢复分层运行。定时组合显式准备原程序、原网络运行时及TAP能力；
上述验收来自本地执行，不等于托管CI结果。

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
make test-sched-concurrency-host test-sched-lifecycle-riscv test-sched-lifecycle-loongarch
make test-smp-host test-smp-riscv
make test-smp-userland-riscv
make test-user-smp-programs-riscv
make test-user-smp-composition-host test-user-smp-composition-riscv
make test-mm-pin-host test-mm-concurrency-host
make test-mm-smp-riscv test-mm-smp-userland-riscv
make test-memory-object-concurrency-host test-mm-shared-concurrency-host
make test-mm-shared-smp-riscv test-mm-shared-smp-userland-riscv
make test-file-mapping-concurrency-host test-mm-file-concurrency-host
make test-mm-file-smp-riscv test-mm-file-smp-userland-riscv
make test-page-cache-concurrency-host test-mm-disk-concurrency-host
make test-mm-disk-smp-riscv test-mm-disk-smp-userland-riscv
make test-files-concurrency-host test-pipe-concurrency-host
make test-files-smp-riscv test-files-smp-userland-riscv
make test-stack-usage test-stack-usage-la
```

完整分层入口见[CI 模块](docs/modules/continuous-integration.md)，聚焦命令见[模块导航](docs/README.md)。
通用根盘 fixture 使用 `INIT_CONFIG=config/init.json`。`make run-riscv` 不附根盘，停留 timer-idle；
`make debug-riscv` 以 `-S -s` 等待 GDB。LA 启动参数见[LA 模块](docs/modules/loongarch-boot.md)。

`build/` 仅长期保留可复用构建缓存。`python3 -B tests/prune-build.py` 预览、`make prune-build` 清理已核对的运行目录、
日志和镜像；需要保留现场时使用 `PRUNE_BUILD_KEEP`。`make clean` 会同时删除可复用内核构建产物。

## 开发方向与文档

当前主线继续推进设备、网络和命名空间的并行内核业务，再按实际需要扩展 RV 核数与 LA 多核；具体未完成能力与依赖维护在[开发路线](docs/goals.md)。普通文件缓存、匿名缺页、用户 pin 与系统压力通知已经使用共同对象规则，不再保留 IRQ-only 缓存、MM/FD 并发启用开关和 resident-only read 支路。需要串行的 ext4 后端、设备及进程生命周期仍在实际 CPU0 操作边界执行，默认生产与 LA 继续单 CPU。
共享调度域的[成本核对](docs/learning/cost-baseline.md#共享调度域的匹配时间)保留既有开销和短窗口未决结果；
短窗口归因已停止，不再作为两核规划的前置门槛。既有3%测量协议保留，未决不记为通过。
FS-Off候选仍在实验分支，未进入main；宿主同步测试与单核验收不代表整个内核已具备SMP安全性。
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
