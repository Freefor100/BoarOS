# BoarOS

<p align="center">
  <img src="assets/boaros_header.png" alt="BoarOS 吉祥物与字标" width="100%">
</p>

BoarOS 是 C／少量汇编内核，目标是运行未经 BoarOS 特改的 Linux 用户程序。
Linux 是用户可观察语义的参考；内部实现独立维护对象所有权、失败回滚和资源回收。

当前分支为 **oscomp-compat**：从 main 单向集成通用修复，保留双架构原盘启动、LTP 监督、
判分和 LA 调度运行时适配。入口及适配范围见[兼容模块](docs/modules/oscomp-compat.md)；
下面的共同能力与验收证据来自主线，不能据此推定原评测全部通过。

## 当前能力

默认生产平台为单 CPU QEMU virt：**RV64、Sv39／4 KiB 页**和**LA64、LA464、16 KiB／三级页表**。
两侧共用 MM、调度、VFS、设备和用户程序策略，架构实现在构建期绑定。
独立 RV 两核调度核心及受控私有/共享匿名、独立 tmpfs/磁盘文件 MM 的 pin/COW/TLB 回收闭环已验证。磁盘冷缺页通过CPU0 worker交接，写回按页排空写入借用；该受控配置的普通用户程序、实际磁盘I/O与现有业务仍在CPU0。
同域共享FD表和匿名pipe已接入受控两核，验证查找/pin、dup/close、等待、迁移及最后端点回收；预先打开的同域tmpfs普通文件可就地I/O，ext4普通文件的整次操作交给既有CPU0 worker，已验证共享偏移、关闭身份及持久化。socket/epoll/FIFO、命名空间和一般设备业务跨核仍未开放。

独立 `kernel-rv-user-smp` 配置开放真实用户态两核执行：syscall、同步fault和待服务通知迁移实际任务到CPU0，保留调用栈与身份；CPU1纯本地timer/IPI返回不进入业务服务。普通MM仅开放远端执行和失效后退休，内核业务仍在CPU0。当前同ELF Linux SMP对照已覆盖独立进程、pthread/TLS/寄存器、嵌套信号、撤权限/截断、取消、非leader exec、组停止/继续及实时服务资格，详见[用户两核边界](docs/modules/riscv-smp.md#真实用户态与cpu0业务交接)。默认单核和原受控配置保持。
2026-10-14以固定Linux SMP同ELF复核229项原程序：2/1 CPU与512 MiB/1 GiB四个profile各229项通过；共享映射首缺页、pipe/futex等待及取消、组退出、低内存回收、SQLite DELETE/WAL与独立重启共60次组合启动通过。当前边界仍是CPU0执行内核业务，不代表任意内核路径都可并发。

| 范围 | 已验证能力 | 主要边界／模块 |
|---|---|---|
| 内存与 CPU | buddy/slab、连续页与引用、VMA、按需页、共享匿名/文件映射、COW、独立栈窗口与真实 guard；RV F/D、LA FPU/LSX/LASX；独立 RV 受控两核配置 | 一般文件/设备及外部后备的并行内核业务尚未开放，RV V；固定 QEMU 不提供 LA LBT；[物理页](docs/modules/physical-pages.md)、[MM](docs/modules/kernel-mm.md)、[两核边界](docs/modules/riscv-smp.md)、[LA](docs/modules/loongarch-boot.md) |
| ELF 与运行时 | 静态/动态 ELF、PIE、shebang、解释器、musl DSO/TLS；RV glibc 2.44、LA glibc 2.42 的五种形态及线程/取消子集 | 无 execveat，更广 glibc 应用仍须逐个验收；[ELF](docs/modules/user-elf.md)、[exec](docs/modules/kernel-exec.md) |
| 进程与同步 | fork/vfork/pthread、线程组/会话/进程组、exec/wait/退出回收；OTHER/FIFO/RR、deadline；基本 futex、robust-list、标准信号和同步 fault | 默认用户任务限CPU0，独立RV用户SMP的内核服务仍在CPU0；无 PI futex、共享文件 futex、实时信号队列、sigaltstack；[调度](docs/modules/kernel-scheduler.md)、[信号](docs/modules/kernel-signal.md) |
| 文件与事件 | fd/OFD、dup、阻塞 pin、向量/定位 I/O、pipe/FIFO、poll/select/epoll、传统/OFD 记录锁、活 inode/路径、link/rename；受控RV共享FD/匿名pipe两核 | 一般文件/OFD与socket/epoll/FIFO业务仍限CPU0；无完整凭据/权限、EXCHANGE/WHITEOUT；[文件接口](docs/modules/kernel-files.md) |
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

工具和固定环境见[工具链](docs/toolchain.md)、[参考资料](references/README.md)。本分支默认 `make all` 同时生成顶层 `kernel-rv`、`kernel-la`，分别使用对应架构的评测启动配置。
显式 `INIT_CONFIG=...` 统一覆盖两侧；通用回归使用 `INIT_CONFIG=config/init.json`。

```sh
make all
make test-oscomp-host
make test-oscomp-compat      # 本地双架构诊断
make test-oscomp-official    # 固定官方容器与原 Harness
make all INIT_CONFIG=config/init.json
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
通用根盘 fixture 使用 `INIT_CONFIG=config/init.json`，验证后用 `make all` 恢复兼容默认。`make run-riscv` 不附根盘，停留 timer-idle；
`make debug-riscv` 以 `-S -s` 等待 GDB。LA 启动参数见[LA 模块](docs/modules/loongarch-boot.md)。

`build/` 仅长期保留可复用构建缓存。`python3 -B tests/prune-build.py` 预览、`make prune-build` 清理已核对的运行目录、
日志和镜像；需要保留现场时使用 `PRUNE_BUILD_KEEP`。`make clean` 会同时删除可复用内核构建产物。

## 开发方向与文档

当前主线推进 **一般文件/页缓存/设备的并行内核业务及未开放后备域**，再按组合证据扩展RV核数和LA多核；
具体未完成能力与依赖只维护在[开发路线](docs/goals.md)，逐轮执行计划留在会话中。
CPU 本地/current、运行结算、idle/timer归属与嵌套抢占控制已建立，raw 短锁已接入 buddy/slab 元数据；任务选择、身份发布及存储借用在统一短调度域保护，资源清理留在锁外；等待代次、借用游标、
新栈交接与可睡眠锁内部资格已建立。受控磁盘映射保留CPU0 I/O及最终释放；系统压力回收、未开放外部后备及一般文件/其他业务对象仍需跨核保护。
默认生产继续单CPU；新增[RV受控两核配置](docs/modules/riscv-smp.md)使用独立构建入口，
2026-10-11在512MiB/1GiB完成MTTCG两核抢占、IPI/WFI交接、双向迁移、执行资格、
RFENCE后栈回收、私有allocator及CPU0原静态/pthread组合；同一内核的一CPU退化也通过。
用户任务及现有业务仍限CPU0，
受控任务可绑定匿名和私有arena的tmpfs/ext4文件映射MM，完成片段pin、跨MM共享/私有COW、文件关闭代次与登记借用、活动集合及确认失效后回收。CPU0保留文件修改、磁盘I/O和最终释放；未开放后备与一般业务对象的并行内核操作仍需独立验收。普通用户两核使用独立USER_SMP配置，在真实调用任务上交接CPU0内核服务。
共享FD表、匿名pipe、预开普通文件及真实用户跨核接入已存在；229项原程序四个USER_SMP profile及有限压力/生命周期组合已在本地验收，托管CI尚需实际运行确认；一般并行内核业务仍是后续方向，既有所有权边界继续适用。
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
本分支默认构建并由 bootstrap 发布 LA 调度适配 DSO，musl 动态程序调用真实调度 syscall；
main 默认构建不发布或预加载它。原盘程序、libc 和 judge 保持原身份。
最近完整官方调用的联合整数分为 **2130**；LTP-glibc 被总预算终止、LTP-musl 未到达，
正常 PID 1 和资源收口未验证。逐组分数、历史运行及适配身份见
[评测证据](docs/learning/oscomp-compat-baseline.md#默认make-all接入la-linux调度运行时)。
这次主线集成不生成新的正式成绩。

- [文档导航](docs/README.md)：模块、学习材料和当前基线。
- [工程原则](docs/design.md)、[贡献说明](CONTRIBUTING.md)：设计、验证和提交规则。
- [固定资料](references/README.md)、[第三方代码](docs/third-party.md)：版本、来源与许可。
