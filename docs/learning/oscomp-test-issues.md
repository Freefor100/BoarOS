# OSComp 评测测例、运行库与环境问题归因手册

在运行大规模操作系统兼容性测试套件时，一条失败日志背后可能藏着四种截然不同的根因：**发布镜像与启动环境缺失**、**测试程序或 C 运行库的编译/封装差异**、**内核系统调用或文件系统真实缺陷**，以及**判分脚本与整机时间预算口径**。

定位每一例异常时，应沿着**程序启动 → 测例准备 → 实际系统调用 → 判分汇总**四个边界逐层核对实际执行的二进制文件、动态库、传入参数与内核返回值，必要时在参考 Linux 上运行同一份可执行文件与同一套 C 标准库对照确认归属。本文系统整理初赛镜像（`pre-20250615`，内含 LTP `20240524`）与决赛公开镜像（`final-2026`）中已查清的典型问题、根因分析与处理方案。运行环境与脚本适配架构见 [OSComp 双架构评测环境适配](../modules/oscomp-compat.md)。

---

## 一、发布镜像构建依赖与启动环境问题

下表归纳发生在测例真正进入目标系统调用之前的启动、依赖与环境准备问题：

| 测例 / 组件 | 根因分析 | 处理与判读方式 |
| :--- | :--- | :--- |
| **`acl1`** | 原发布二进制在交叉编译时未检测到 `HAVE_LIBACL`，直接编入了无 ACL 依赖的提示分支，启动后输出 `The acl library was missing upon compilation.` 并以状态码 `32` 退出；且上游无参直接调用也缺少目标文件、目录与测试用户参数（见 [上游源码](https://github.com/linux-test-project/ltp/blob/20240524/testcases/network/nfsv4/acl/acl1.c)）。 | 保留原发布文件；完整验证需链接 `libacl` 重编并传入完整测试参数，结合 `ACL TESTS RESULTS` 输出判读。 |
| **`io_setup01`** | 初赛盘中双架构、双 C 库的四份发布文件均因交叉编译环境缺少 `libaio.h` / `libaio` 未定义 `HAVE_LIBAIO`，启动即报告 `TCONF` 并退出 `32`，全程未触发 `io_setup` 系统调用（见 [测例源码](https://github.com/linux-test-project/ltp/blob/20240524/testcases/kernel/syscalls/io_setup/io_setup01.c) 与 [构建检测](https://github.com/linux-test-project/ltp/blob/20240524/m4/ltp-eventfd.m4)）。 | 通用内核已支持原生异步 I/O（Asynchronous I/O，AIO）；初赛跳过表跳过此发布文件，验证内核 AIO 需在具备 `libaio` 头文件与库的环境中重编。 |
| **`setxattr01`（LoongArch glibc）** | 现象为误报缺少 `mkfs.ext4`，连 `system("exit 0")` 也返回 `127`。追踪发现 glibc 的 `posix_spawn()` 发起 `clone3` 时携带 `CLONE_CLEAR_SIGHAND`（要求子进程重置所有已捕获信号为默认动作）；旧内核返回 `EOPNOTSUPP`（95），而 glibc 仅在收到 `EINVAL` 或 `ENOSYS` 时回退传统 `clone`。 | 通用内核已实现 `CLONE_CLEAR_SIGHAND` 语义，原发布测例无需改动即可通过。 |
| **依赖 `getpwnam("nobody")` 的测例与 `umount03`** | `access01`、`mkdir02`、`readlink01`、`pidfd_send_signal02`、`pidfd_getfd02`、`mlock02`、`mlock202`、`shmctl02`、`mount03`、`umount03`、`mq_open01`、`mq_unlink01` 均通过 C 库查询 `/etc/passwd` 获取 `nobody` 账户的数字 UID/GID。两架构原始初赛根盘均缺少 `/etc/passwd`，导致如 `umount03`（LoongArch glibc 约 10.7 秒退出、状态码 2、0 分）在 `getpwnam` 即报 `ENOENT` 退出，根本未执行 `seteuid` 和卸载权限断言。 | 兼容分支启动脚本在进入 LTP 组前，仅向**可写根盘副本**补充缺失的 `nobody:x:65534:65534:nobody:/:/bin/false`，不改动原盘镜像、`/dev/vdb3` 或内核代码。补齐后双架构双库的 `umount03` 均正确到达卸载权限断言并收到 `EPERM` 通过。 |
| **glibc `float_*` 五项浮点测例** | `float_bessel`、`float_exp_log`、`float_iperb`、`float_power`、`float_trigo` 在线程取消或退出时，glibc 依赖 `libgcc_s.so.1` 执行栈展开，发布镜像缺失该库导致动态加载失败；且双架构 `musl` 变体也均无可计分摘要。 | 按确切程序名列入跳过表。 |
| **LoongArch 动态加载器路径** | 动态可执行文件的解释器路径写死在 `PT_INTERP` 段（如 `/lib64/ld-linux-loongarch-lp64d.so.1` 与 `/lib64/ld-musl-loongarch-lp64d.so.1`）；缺同名路径链接在参考 Linux 上同样无法启动。 | 启动脚本按 `tests/oscomp/inputs.json` 清单创建指向原盘真实加载器的符号链接。 |
| **LoongArch `musl` 调度函数桩** | 发布盘的 LoongArch `musl` 将四个 `sched_*` 接口直接实现为返回 `ENOSYS`，根本不进入内核。 | 由工具分区 [linux-sched.so 调度适配](../modules/oscomp-compat.md) 经 `LD_PRELOAD` 桥接至原生系统调用。 |
| **`libc-test` 缺少 shebang** | 发布盘 `run-static.sh` 与 `run-dynamic.sh` 首行无 `#!/bin/sh`，LoongArch 原 `musl` BusyBox 在参考 Linux 上亦拒绝直接执行。 | `libctest-hook.sh` 仅在临时外层脚本副本中改用原 C 库 BusyBox `sh` 显式解释两行，保留内层脚本与参数。 |
| **`eventfd01/02`** | 准备阶段读取 Linux 内核配置，缺少 `CONFIG_EVENTFD` 配置项即以 `TBROK` 终止。 | 判读时归为准备阶段依赖缺失，实际 `eventfd` 语义由直接调用系统调用的测例覆盖。 |
| **`acct02`** | 发布程序读取 Linux 配置以选择记账记录格式。BoarOS 固定输出 v3，正式启动流程没有提供 Linux 配置文件。 | 本地用原程序和实际支持的 `CONFIG_BSD_PROCESS_ACCT`、`CONFIG_BSD_PROCESS_ACCT_V3` 配置验证记录内容，两架构两种 C 库均通过。正式盘的配置读取失败属于准备阶段。 |
| **`keyctl02`** | 原程序在准备阶段临时提高 `/proc/sys/kernel/keys/root_maxkeys`，再读取 `CONFIG_PREEMPT_RT` 和 `CONFIG_PREEMPT_RT_FULL`，据此决定是否在并发读密钥和撤销密钥的循环中为实时内核的线程清理留出额外时间。缺少配置文件时，原程序会报 `Cannot parse kernel .config`。 | 初赛逐项执行脚本仅为 `keyctl02` 写入这两项的未启用状态，并通过 `KCONFIG_PATH` 传给原程序。密钥上限仍按内核实际计费，程序、参数和判分规则沿用发布版。 |
| **`accept03` / `readahead01` / `splice07`** | 三者通过 [tst_fd.c](https://github.com/linux-test-project/ltp/blob/20240524/lib/tst_fd.c) 的 `tst_fd_next()` 构造多类描述符。当前可构造 `perf_event_open` 的软件 CPU 时钟计数器、`fsopen("ext2", 0)` 的配置描述符和 `userfaultfd` 描述符。仍有 `io_uring_setup` 和 `memfd_secret` 两类描述符在构造时以 `ENOSYS` 跳过。独立的 `userfaultfd01` 实际登记私有匿名映射，读取缺页事件并填入页面。 | 按日志区分构造阶段的配置跳过与实际系统调用断言；保留发布程序及原判分规则。 |
| **`open_by_handle_at02` 标准输入挂载假设** | `stale-dfd` 子项用标准输入（FD 0）选定一个挂载点，期望它与测试文件所在卷属于不同文件系统实例。初赛启动脚本将根盘上的 `/dev/console` 作为 FD 0，测例又在同一 ext4 根盘的 `/tmp` 建文件，因此 FD 0 与目标文件落在同一挂载实例中，原句柄得以成功打开（其余 6 项均通过）。参考 Linux 因自动挂载设备临时文件系统（devtmpfs）使 `/dev/console` 位于独立挂载；本地将 `procfs` 中的普通文件重定向给 FD 0 后，BoarOS 同样返回 `ESTALE` 并通过全部 7 项。 | 保留正式启动脚本与原程序不变，判读该项时核对 FD 0 所属挂载实例即可。 |

---

## 二、挂载、文件系统与存储边界问题

### 2.1 `df01.sh` 工具选项缺失与挂载残留

初赛启动脚本执行 `busybox --install -s /bin` 后，`/bin/stat` 与 `/bin/df` 均指向发布盘内置的 BusyBox，其选项集与脚本依赖的 GNU Coreutils 存在三处不兼容：

1. RISC-V 原脚本挂载测试分区后调用 GNU 风格的 `stat -f ... --printf=%b` 读取块数以校验 `df` 输出；BusyBox `stat` 仅支持 `-c`，命令替换返回空字符串，脚本在算术求值处异常退出并把测试分区遗留在挂载状态。
2. 仅替换 `stat` 后，BusyBox `df` 仍缺少 `-t`、`-v`、`-H`、`--version` 与 `-x` 选项。
3. LoongArch 原 Shell 在进入断言前无法正确向内层传递外层局部变量 `sec`，导致测例超时准备失败。

**解决方案**：工具分区提供离线构建的 GNU Coreutils 9.5 `stat`/`df` 与 LoongArch `Dash 0.5.12`，仅在运行 `df01.sh` 时调整 `PATH` 并在 LoongArch 上用 Dash 解释原脚本；原脚本完成卸载后发出外部测试分区释放请求时，`ltp-device.sh` 按设备号核对挂载表确认已卸载后结束租用（真实回环设备仍交上游 `tst_device` 解绑）。经官方容器验证，双架构、双 C 库的 `df01.sh` 均通过全部 12 项断言、0 警告，且可干净复用 `/dev/vdb3`。

### 2.2 `umount2_02` 两步过期卸载（`MNT_EXPIRE`）

旧内核拒绝 `MNT_EXPIRE` 标志，导致两个 `glibc` 配置仅通过 2 项 `UMOUNT_NOFOLLOW` 断言并留下叠放挂载。通用内核完整实现过期标记后（首次带 `MNT_EXPIRE` 置位过期标记返回 `-EAGAIN`，期间若无访问则第二次调用完成卸载，普通访问清除过期标记，绑定视图间相互隔离），LoongArch 与 RISC-V 的 `glibc` 原程序均通过全部 7 项断言并正常退出。

### 2.3 `statx11` 准备阶段死循环与发布程序缺失对齐断言

`statx11` 曾暴露出**准备阶段超时**与**三组跳过对齐断言**两个相互独立的问题：

1. **准备阶段缺 `sysfs` 导致越界死循环**：`setup` 先读取 `/sys/block/vdb3/queue/logical_block_size`；读不到时逐字符截短设备名以回退查询整盘 `/sys/block/vdb/queue/`（Linux 仅在整盘目录提供 `queue` 属性，分区目录无独立 `queue`）。由于发布程序循环缺少空串终止条件且存在越界写，当 `/sys` 未挂载时四组全部卡死超时、未进入 `statx`。初赛启动脚本现已将系统文件系统（sysfs）挂载至 `/sys` 并导出块设备队列属性（`/dev/vdb3` 节点用于打开分区，`/sys/block/vdb/queue/` 提供整盘队列参数，两者分属不同接口）。
2. **三组发布二进制未编译对齐断言**：BoarOS 已从真实块设备导出 `logical_block_size = 512` 与 `dma_alignment = 511`，并在 `statx()` 中通过 `STATX_DIOALIGN` 返回 `stx_dio_mem_align = 512`、`stx_dio_offset_align = 512`。在原发布盘中，仅 LoongArch glibc 通过了这两项断言，其余三组在 `statx` 返回含 `STATX_DIOALIGN` 掩码后报告 `TCONF` 跳过（提示文案写死了 glibc，实际 musl 也会打印该文案）。

通过 `readelf --debug-dump=info` 与 `rawline` 检查原发布盘二进制内保留的调试信息，确认 [LTP 源码](https://github.com/oscomp/testsuits-for-oskernel/blob/4b40ee92e8b594fb20e3957ebc2f79ad43aca538/ltp-full-20240524/testcases/kernel/syscalls/statx/statx11.c) 由编译期宏 `HAVE_STRUCT_STATX_STX_DIO_MEM_ALIGN` 决定是否编入断言：

| 发布程序 | 交叉编译时 `struct statx` 声明来源 | 对齐字段编译结果 |
| :--- | :--- | :--- |
| **RISC-V glibc** | 工具链 `<linux/stat.h>` | 已有 `stx_mnt_id`，但两对齐字段仍处于保留成员 `__spare2`，选入 `TCONF` 分支 |
| **RISC-V musl** | LTP 兼容头 `include/lapi/stat.h` | 配置阶段未在 `<sys/stat.h>` 找到完整定义，两字段落入 `__spare2[14]`，选入 `TCONF` 分支 |
| **LoongArch musl** | 工具链 `<sys/stat.h>` 旧版定义 | 两字段落入 `__pad1[14]`，选入 `TCONF` 分支 |
| **LoongArch glibc** | 工具链 `<linux/stat.h>` | 完整声明 `stx_dio_mem_align` 与 `stx_dio_offset_align`，正常编入并**通过两项断言** |

本地使用包含这两个字段的目标头文件重编三组原 `statx11.c` 在独立运行盘上验证时，三组均通过两项断言、0 失败 0 跳过；正式评测继续原样运行发布盘二进制。

### 2.4 `fs_fill` 满盘写回 `ENOSPC` 与 ext4 区段树元数据预留

`fs_fill` 先用三个写线程以 `writev` 将测试盘写至 `ENOSPC`，再分批用不同大小文件反复填盘；遇到空间不足时，`lib/tst_fill_fs.c` 先执行 `fsync()` 提交已接纳的数据再缩小请求继续。此前在 LoongArch glibc 上复现过 `fsync()` 返回 `ENOSPC`、随后 `umount()` 返回 `EIO` 的故障。

- **根因定位**：问题出在 ext4 延迟分配（Delayed Allocation）的区段树（Extent Tree）扩展路径。写入接纳阶段仅预留了文件数据块，没有为可能触发的区段树分裂预留元数据块；待后台写回分配完数据块、区段树需要新申请 1 个元数据块时，虽然超级块仍显示有 `27,839` 个空闲块，但扣除已登记的延迟预留与当前事务额度后可用余量为 `0`，导致区段树分配返回 `ENOSPC`。错误沿 `ext4_write_spans` 回传给页缓存写回与 `fsync()`，卸载阶段清理页缓存重试写回再次失败并转换为 `EIO`。
- **通用内核修复**：接纳空洞写入时**同步预留数据块与区段树元数据块额度**；写回按类型扣减，事务回滚恢复原额度，提交成功后归还多余的元数据预留；额度紧张时先推进同一文件系统的脏页写回再重查，避免临时元数据预留过早拒绝追加。修复后独立运行 LoongArch glibc 原 `fs_fill` 完整跑通两轮填盘，退出状态 `0`，通过 2 项断言并正常卸载（在正式初赛整轮中，因填盘耗时高仍按时间预算列入跳过表）。

### 2.5 其他文件与通知测例判读说明

- **`fanotify03` / `10` / `13` / `16` / `23`**：`fanotify03` 验证打开/读/执行权限拦截；`fanotify10` 同时验证事件过滤与 `/proc/self/fdinfo/<fd>` 中的忽略标记（旧式忽略掩码不支持目录/子项过滤语义，对应 3 种配置与参考 Linux 一致跳过）；`fanotify16` 验证目录句柄、名称、目标句柄与改名事件；`fanotify23` 格式化挂载 ext2 并在缓存回收后核对可回收标记自动撤除而普通标记保留；`fanotify13` 在 ext4 基础与叠加（OverlayFS）变体下，四组原程序各通过 45 项断言（基础 15 项 + 下层/上层各 10 项 + 叠加视图下层/上层各 5 项，其余跳过项与参考 Linux 行为一致）。实现详见 [文件资源层](../modules/kernel-files.md)。
- **`fallocate04` / `fallocate05`**：`fallocate04` 验证预分配、打洞与 `FALLOC_FL_ZERO_RANGE`（后续 `FALLOC_FL_COLLAPSE_RANGE` 返回 `EOPNOTSUPP` 按配置跳过）；`fallocate05` 需先填满测试盘再测预分配区写入与打洞，顺利填满的配置通过 4 项断言，若发生超时应核查 `tst_fill_fs` 写入进度与盘容量耗时。
- **`readahead02` 与 `msync03` / `04`**：`readahead02` 依赖进程 I/O 统计、缓存清理与块设备预读参数；`msync04` 经 `/proc/self/pagemap` 与 `/proc/kpageflags` 验证共享映射脏页同步；`msync03` 锁页断言通过但发布程序无计分摘要。
- **早期块设备启动 `FLUSH` 超时诊断**：旧版块设备驱动在面对类型 4（`FLUSH`）持久刷盘请求 30 秒未见设备完成环推进时会强制复位设备，引发根文件系统挂载失败（日志见 `block queue fault reason=timeout`）。现行驱动改为持续等待设备结果并周期性打印诊断状态；遇到同类日志时需对照请求状态、完成环进展与宿主后备文件同步延迟（详见 [VirtIO 块设备](../modules/riscv-virtio-block.md)）。

---

## 三、`final-2026` 公开盘与工具链兼容问题

在适配 `final-2026` Debian 公开镜像（CAgent 与 BuildStorm）期间定位并修复了两处通用内核问题：

1. **ext4 日志描述符零 UUID（`jbd_extract_block_tag`）**：公开根盘挂载时因日志重放被拒而失败。追查 `jbd_extract_block_tag()` 发现旧实现额外强制要求日志描述符块中可选的通用唯一标识符（Universally Unique Identifier，UUID）必须等于日志超级块中的值，而公开镜像的可选 UUID 字段为全零。参考 Linux 恢复路径按标志位跳过该可选字段并依靠日志校验和验证记录完整性。通用内核按此修正后，公开盘副本可直接由内核完成日志重放与挂载。
2. **Rust 构建输出管道的 `FIONBIO` 控制（BuildStorm）**：BuildStorm 准备阶段 Rust 工具链在 `Child::wait_with_output()` 中意外中止。系统调用追踪显示其向管道描述符发起了 `ioctl(pipefd, FIONBIO, &one)` 请求启用非阻塞读，旧内核对普通管道返回 `ENOTTY`（25）导致工具链退出。通用文件描述符分派层现已为支持非阻塞的描述符统一接入 `FIONBIO`，直接更新共享打开文件描述（Open File Description，OFD）的非阻塞标志。

---

## 四、系统调用 ABI、C 运行库与架构特性差异

| 测例 / 场景 | 现象与根因 | 归属与处理 |
| :--- | :--- | :--- |
| **`basic/brk`** | 发布程序将 `brk` 系统调用返回的 64 位堆顶地址截断为低 32 位后再符号扩展传回下一次调用。核对寄存器返回值与下次入参即可确认。 | 发布测例自身 32 位截断缺陷，保留原文件与原判分。 |
| **`epoll_ctl04`** | 固定版 LTP 仅期望第 6 层嵌套返回 `EINVAL`，而较新 Linux 先检查向上嵌套深度并在第 6 层返回 `ELOOP`（BoarOS 遵循同一顺序，LTP 上游后续已[修改为同时接受两种错误](https://github.com/linux-test-project/ltp/commit/e84f0689cf7a8a77478a0e70aa62560f66c3bceb)）。 | 属新旧 Linux 错误优先级差异，内核保持现代 Linux 语义。 |
| **LoongArch musl `mremap06`** | 发布二进制写死 4 KiB 拆分步长，在 16 KiB 页大小的 LoongArch 系统上向 `mremap` 传入未对齐地址；同一二进制在 16 KiB 页参考 Linux 上同样首次搬移即报 `TBROK`。 | 属发布程序硬编码 4 KiB 页大小问题。 |
| **`sigtimedwait01` / `sigwaitinfo01`** | 测例要求信号打断空信号集等待并返回 `EINTR`。调用轨迹证实 BoarOS 与参考 Linux 均已返回 `EINTR` 且执行了信号处理函数，但双架构发布盘 `musl` 包装收到 `EINTR` 后自动重试 `rt_sigtimedwait`，空集合永远无法匹配新信号直至触发内部超时。 | 属发布盘 `musl` 封装重试行为；验证内核中断语义应直接调用原始 `rt_sigtimedwait`。 |
| **`mmap04`** | RISC-V Linux 会为 `PROT_WRITE` 映射自动附加 `VM_READ` 使 `/proc/<pid>/maps` 显示 `rw-`，而原测例断言 `-w-`；LoongArch 保留逻辑权限显示 `-w-`（硬件有效访问仍允许读）。 | 架构特定权限显示差异，需区分映射展示与实际硬件访问权限。 |
| **RISC-V musl `mount07`** | 挂载启用 `MS_NOSYMFOLLOW` 后，直接打开符号链接返回 `ELOOP`、`readlink()` 与 `statfs()` 均通过，但原 `musl` 的 `realpath()` 返回 `ELOOP`——同一发布程序在参考 Linux 上表现完全相同。 | 属 `musl` `realpath()` 实现与参考 Linux 一致的行为。 |
| **RISC-V musl `sendmsg01`** | 反汇编确认原 `musl` 包装在进入系统调用前先在用户态复制 `msg_control`，传入非法指针直接在用户态触发 `SIGSEGV`，与参考 Linux 一致。 | 属 C 库用户态前置拷贝行为。 |
| **`cve-2016-7117`** | 并发关闭描述符时 `recvmmsg` 实际已正确返回 `EBADF`，但旧日志把 `EINVAL` 误记在其上——原因是紧随其后的 `clock_gettime(CLOCK_MONOTONIC_RAW)` 在旧内核返回 `EINVAL` 覆盖了线程 `errno`。 | 通用内核已补齐 `CLOCK_MONOTONIC_RAW`；判读日志时需拆分相邻系统调用的 `errno`。 |
| **RISC-V 静态 glibc 时钟断言** | 某些静态 glibc 程序在时钟调用返回 `0` 时断言 `errno == 0` 失败：原因是进入 `main` 前 `malloc` 初始化调用 `getrandom` 收到 `EAGAIN`，随后成功的时钟调用按 POSIX 规则不清空旧 `errno`。 | 初赛启动流程通过一次性种子提前完成内核随机源就绪，消除前置 `EAGAIN`。 |
| **`send02`（TCP `MSG_MORE` 交互）** | 此前四组均卡在首个 TCP 子项未进入 UDP。测例每轮先发 16 B，再带 `MSG_MORE` 暂留 16 B，最后发 1 B 结束本批，并用非阻塞接收轮询末尾 17 B。通过 `TCP_INFO` 查明：最后 1 B 发出时首段 16 B 仍待确认，Nagle 算法暂缓短包直到对端延迟确认或超时（80 轮耗时 10.007 秒，参考 Linux 1,000 轮仅 253 毫秒）；简单关闭 Nagle 又会导致提前放出 `MSG_MORE` 数据。 | 通用内核现对首个有序数据段及时安排确认、后续段保留延迟确认，且独立发送控制确认不提前破坏 `MSG_MORE` 边界，同时支持 UDP 套接字 `MSG_MORE`。双架构双库四组均通过全部 TCP 与三种 UDP 场景。 |
| **`clock_getres01` / `timer_*` 闹钟分支** | 闹钟时钟（`CLOCK_REALTIME_ALARM` / `CLOCK_BOOTTIME_ALARM`）依赖运行环境提供可用实时时钟（Real-Time Clock，RTC）。 | 判读时区分单调/启动时钟、国际原子时（TAI）与需要探测到 RTC 才启用的闹钟接口。 |
| **RISC-V musl `iperf3` `REVERSE_TCP` 间歇重置** | 常驻 `iperf3` 服务端连续跑完六项时，紧接 `REVERSE_UDP` 后的 `REVERSE_TCP` 偶发在打印统计前出现控制连接重置（另一轮次同配置通过）。 | 复现排查需保持同一常驻服务端及前序 `REVERSE_UDP`，追踪控制连接关闭时机。 |

---

## 五、LTP 跳过表策略与判分脚本口径

### 5.1 整轮时间预算与 `ltp-skips.tsv` 分类依据

初赛官方本地框架单台虚拟机预算为 **3,600 秒**，线上双架构整轮评测预算为 **7,200 秒**，单项监督器在每个测例启动时独立计时（默认 120 秒）。评测平台按通过断言的原始分数加总。遇到截断的平台串口日志时，需结合组结束标记、单项 `BOAROS-CASE END` 记录与原判分，准确区分“未到达”与“跑完得零分”。

共享跳过表 [tests/oscomp/ltp-skips.tsv](../../tests/oscomp/ltp-skips.tsv) 按程序名统一作用于双架构、双 C 库，分为四类明确依据：

1. **缺少控制器或结构化输入的辅助程序**：初赛脚本直接平铺遍历 `testcases/bin` 目录无参调用每个可执行文件，把原本必须由控制器发 `SIGUSR1`/停止信号、由 `iogen` 提供标准输入流或配套客户端交互的辅助程序（如 `netstress`、`doio`、`pthserv`、`fs_racer_*`、`cgroup_regression_*`、`cpuhotplug_do_*`、`tst_hexdump` 等）也当作独立测例启动。跳过表仅过滤无参直接调用，其配套主测试仍能正常拉起这些辅助程序。
2. **全部通过但无上游计分摘要（0 原始分）**：
   - 已核实的 **19 个程序**（`fcntl14`、`fcntl14_64`、`ftest01/03/04/05/07/08`、`inode02`、`kill02`、`lchown03`、`mknodat02`、`nptl01`、`open12`、`rename11`、`rename14`、`renameat01`、`sendmsg02`、`umount2_01`）在全部四个配置下均断言通过且退出码为 `0`，但使用旧式输出格式、不含上游判分脚本解析的 `Summary:` 块；
   - `epoll-ltp`、`semop05`（`musl` 通过路径固定睡眠 20 秒且仅打单条旧式通过行）与 `hackbench`（仅报调度基准耗时）在四组中同样恒为 `0` 分。
3. **发布程序缺失编译或运行库依赖**：`io_setup01`（缺 `libaio`）与 5 个 `float_*` 程序（缺 `libgcc_s.so.1` 且 `musl` 无摘要）。
4. **整轮时间预算内的超长低分/无界压力测试**：
   - `dio_append`、`dio_sparse`、`inotify09`、`mmap1`、`mmap3`、`starvation`、`cve-2016-7117` 至少有一组耗时 ≥ 60 秒，而四组最高原始得分仅 1–2 分；
   - `copy_file_range01`（全矩阵每组 2 分）、`fork14`、`mallocstress`、`mmapstress01`（大压力每组最高 1 分）以及 `fs_fill`（独立副本验证填盘、同步与卸载）和无内部上限且无摘要的 `shm_test` 均按预算策略跳过。

> 对于其余未跳过但报准备失败或零分的测例，判读时还需核对架构与构建前提：如 `io_uring01/02`、模块测例先查内核配置；配额测例依赖 `quotacheck`；原 RISC-V `futex_waitv01/02/03` 编译头文件将系统调用号错设为 `-1`；LoongArch Linux 原生 ABI 本身不存在 `getrlimit03` 所测旧入口以及传统 `socketcall`、`readdir`、16 位 UID 等历史系统调用。

### 5.2 上游判分脚本的关键细节

- **LTP 判分与标记**：上游判分只汇总 `Summary:` 段落，仅打印 `TPASS` 的旧测例不计分；脚本末尾统一打印的 `FAIL LTP CASE <name> : 0` 仅表示“单项结束行”，末尾 `0` 实际代表退出成功；`mremap05` 有一条数据内容不符分支也会误印 `TPASS`，核验内存搬移正确性需结合内容断言。
- **BusyBox `kill 10`**：判分清单按固定标签 `kill 10` 匹配，但发布脚本实际运行 `sh -c 'sleep 5' & ./busybox kill $!` 并按整行输出，需把“信号发送成功”与“判分标签格式不匹配”分开判读。
- **IOzone 计分与提前停止**：原判分取多进程测试中的 `Max`（最快子进程速率），且自动模式各子项不进入同一计分公式；受提前停止（early stop）影响，评估真实整组吞吐须看每个子进程实际完成字节与共同计时区间。此外，发布程序对 `-i 11 -i 12` 报不支持并回退为普通初写与重写（仍可能进入判分），带 `-e` 诊断时应区分普通读取的 `fsync` 与标准 I/O 库的 `fflush` 是否落在方法计时内（详见 [VFS 与 ext4 文件系统](../modules/vfs-ext4.md)）。
- **`final-2026` 判分口径**：CAgent 含时间奖励的总分上限可达 `199.1` 分；BuildStorm 即便最终编译失败，上游脚本仍会为已通过的工具链检查项给分；另外发布说明要求 8 核运行，但 BuildStorm 判分脚本对 LoongArch 的核数告警阈值写死了 12 核，解读告警时需对照实际 8 核发布配置。
