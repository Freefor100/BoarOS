# 官方双盘交付与受影响原始程序结果

2026-10-18，在固定官方容器和原 QEMU 设备参数下，两架构并行完成 glibc、musl 各 32 个 basic 原程序和 24 个选定 LTP 原程序。basic 四组均为 **100/102**；`fallocate06`、`fs_fill` 与两项 ftest 均完成其实际文件检查，全部磁盘检查正常。下文给出这组结果、逐例耗时、原因及地址长度修复后的短复测。

资源统计与 CPU 时钟的后续定向结果见[下文](#资源统计与-cpu-时钟)。

当前评测按预算跳过 `fs_fill`；下文保留已完成的原程序结果。
`brk`、`epoll_ctl04`、命令和判分差异集中见[评测输入与测例问题](oscomp-test-issues.md)。

官方交付布局、离线工具和启动方法统一见[评测模块](../modules/oscomp-compat.md#官方双盘启动熵与格式化工具)，通用能力后续目录见 main 同源的[能力规划](../goals.md)。

## 改好什么、还剩什么

- open11 的 28 项、fcntl13 的 4 项、sigaction01 的 4 项、gettimeofday01 的 3 项、mmap06 的 8 项和 mmap20 的 1 项均输出 TPASS，实际覆盖目录 O_CREAT 错误、设备描述符锁参数错误、SA_RESETHAND 保留字段、timezone 非法指针、mmap 权限/类型与 SHARED_VALIDATE 扩展标志分类。
- getdents02 的有效变体均验证 EBADF/EINVAL/ENOTDIR/已删除目录 ENOENT。RV-musl 编入 raw getdents64、libc getdents 和 libc getdents64 三个有效变体，其他配置只有两个；未提供的旧 syscall/wrapper 由原程序记 TCONF。因此 RV-musl 多 4 分来自原 ELF 提供的检查变体。
- clock_nanosleep01 四组均 12 PASS。libc wrapper 可能自行解引用 req/rem，原测试主动跳过两个非法地址变体，记 2 TCONF；raw syscall 的对应非法地址检查实际通过。
- preadv01/02、pwritev01/02 分别为 3/8/3/7 PASS，覆盖内容、偏移不变及参数错误。preadv03/pwritev03 已完成 ext4 格式化、挂载、BLKSSZGET=512，但在 O_RDWR|O_CREAT|O_DIRECT 的 open 上得到 EOPNOTSUPP，setup 报 TBROK；尚未执行其 O_DIRECT 向量 I/O 内容验证。
- connect01 的 7 项全部 TPASS，socket01 的 IPv4 类型/协议错误和 RAW protocol=0 分类通过。socket01 剩余项是合法 socket(PF_UNIX,SOCK_DGRAM,0)，Linux 会建立 UNIX datagram，当前 BoarOS 普通 socket 创建仍只接入 INET/INET6；需要接入普通 AF_UNIX socket 的创建与通信生命周期。
- sendto01 首次定向运行为 9 PASS、1 FAIL。失败项是已连接 UDP、1024 字节缓冲区、有效 sockaddr 指针、tolen=-1。地址导入用 uint64_t length 且缺少负值/128字节上限检查，仍按 IPv4 地址长度读取并发送。原程序将任意正返回值归一为0，因此日志中的 returned 0 是测试归一化后的值。main `e679c96` 已修正此地址长度导入，后面的四组短复测均10项通过。
- mmap12 的 MAP_POPULATE mmap 已成功，随后打开 /proc/self/pagemap 得 ENOENT，无法完成驻留页检查。mmap14 的 MAP_LOCKED 则明确返回 EOPNOTSUPP。接受 best-effort MAP_POPULATE 不等于已经提供预填页观测或页锁定功能。
- epoll_ctl04 期待第六层添加返回 EINVAL，本轮返回 ELOOP，是旧 LTP 20240524 与保留的 Linux 嵌套检查 errno 预期差异；本轮仍按原输出记录 TFAIL，未修改程序或 judge。
- fallocate06 四组均 8 PASS、1 TCONF：非对齐分配和 PUNCH_HOLE|KEEP_SIZE 清零范围验证成功，ext4 不支持的 COW 子例跳过。fs_fill 四组均 2 PASS，3 个 writer 线程完成两种访问模式下的填满、ENOSPC、删文件和回收。max_runtime=300 是预算，不是强制执行300秒。ftest01/ftest05 的多进程文件检查均输出 TPASS。
- basic mount/umount 四组实际返回成功，获得原judge完整分数。两次启动都记录 BOAROS-SEED READY（erased=4096、credited_bits=256、scratch=/dev/vdb3）。随机、FAT写入与MBR损坏检测的专门回归见下节。basic 其余31个程序满分；已有 brk 问题仍为1/3，不重复归因。

## 环境与计分含义

源码为4f373b8c11c86196d54e3e6f43e9b4e8903884ca。容器为 zhouzhouyi/os-contest@sha256:85dec949df7cef41fd03d30c6ad69f952204540e18d2c62bced9d2e262fef12d。实际两架构 QEMU 都是10.0.2、1 CPU、1 GiB，LTP报告版本20240524。运行产物根目录为 `build/official-delivery-implementation/`，完整命令和 kernel/init/QEMU 哈希见 `probe-run.json`，该批默认构建身份见 `initial-clean-build-identity.json`，最终默认交付身份见 `clean-build-identity.json`。probe 内核嵌入所选程序与计时用 PID1 配置；它与同批 `4f373b8` 默认内核的通用源码相同，二进制因启动配置而不同。最终交付另包含后续 socket 和 FAT 修复，其身份单独记录。

LTP只更换外层枚举为24个名字，原ELF、参数与内部循环保持不变；basic原run-all子程序执行增加监督计时。每例外部预算300秒。elapsed_ms包含建立子进程、运行和监督回收，也是长例格式化/挂载等准备开销所在；两架构并行运行，不可据此声称架构性能优劣。

使用 references/oscomp-autotest/kernel/judge 中未修改的脚本直接解析各组完整日志。LTP原shell成功时也输出 FAIL LTP CASE 名字 : 0，judge以此结束当前例，按Summary passed计分。sigaction01、connect01、sendto01、mmap14、ftest01、ftest05采用旧API，没有Summary，原judge一律0/0；功能输出单列，未改judge给它们补分。LTP all包括skipped/broken，不是全部可执行断言数。

下表聚合对应首次运行的 `4f373b8`；地址长度修复后的结果单列在文末。P/F/B/C 分别表示通过、失败、准备失败、条件跳过。

| 配置 | basic分数 | LTP分数 | LTP原输出P/F/B/C | LTP退出0/完成数 |
|---|---:|---:|---:|---:|
| rv/glibc | 100/102 | 103/113 | 126/5/2/5 | 17/24 |
| rv/musl | 100/102 | 107/116 | 130/5/2/4 | 17/24 |
| la/glibc | 100/102 | 103/113 | 126/5/2/5 | 17/24 |
| la/musl | 100/102 | 103/113 | 126/5/2/5 | 17/24 |

## 首次定向运行的 LTP 逐例表

每组判分沿用原 judge；通过、失败、准备失败和条件跳过来自原程序逐条输出。下表时间单位为秒。原退出状态：mmap12、mmap14、socket01、sendto01、epoll_ctl04 为1，preadv03/pwritev03 为2，其余17例为0，四种配置相同。地址长度修复后的 sendto01 另列于短复测。

| 原程序 | 每组判分与结果 | RV glibc | RV musl | LA glibc | LA musl |
|---|---|---:|---:|---:|---:|
| open11 | 28/28；28通过 | 0.185 | 0.147 | 0.198 | 0.108 |
| getdents02 | 8/10（8通过、2跳过）；RV musl12/13（12通过、1跳过） | 0.107 | 0.085 | 0.105 | 0.063 |
| fcntl13 | 4/4；4通过 | 0.062 | 0.042 | 0.063 | 0.041 |
| sigaction01 | 0/0；4通过 | 0.044 | 0.021 | 0.043 | 0.022 |
| gettimeofday01 | 3/3；3通过 | 0.062 | 0.041 | 0.042 | 0.042 |
| clock_nanosleep01 | 12/14；12通过、2条件跳过 | 1.571 | 1.562 | 1.585 | 1.563 |
| mmap06 | 8/8；8通过 | 0.062 | 0.062 | 0.062 | 0.064 |
| mmap12 | 0/1；1失败 | 0.083 | 0.041 | 0.084 | 0.041 |
| mmap14 | 0/0；1失败 | 0.041 | 0.022 | 0.041 | 0.041 |
| mmap20 | 1/1；1通过 | 0.082 | 0.062 | 0.064 | 0.063 |
| socket01 | 8/9；8通过、1失败 | 0.062 | 0.041 | 0.084 | 0.041 |
| connect01 | 0/0；7通过 | 0.042 | 0.042 | 0.044 | 0.044 |
| sendto01 | 0/0；9通过、1失败 | 0.041 | 0.045 | 0.041 | 0.042 |
| preadv01 | 3/3；3通过 | 0.064 | 0.042 | 0.066 | 0.063 |
| preadv02 | 8/8；8通过 | 0.062 | 0.065 | 0.061 | 0.065 |
| preadv03 | 0/1；1准备失败 | 8.298 | 9.358 | 7.202 | 6.597 |
| pwritev01 | 3/3；3通过 | 0.063 | 0.062 | 0.087 | 0.067 |
| pwritev02 | 7/7；7通过 | 0.062 | 0.064 | 0.064 | 0.066 |
| pwritev03 | 0/1；1准备失败 | 8.459 | 8.622 | 6.647 | 6.784 |
| epoll_ctl04 | 0/1；1失败 | 0.085 | 0.042 | 0.084 | 0.062 |
| fallocate06 | 8/9；8通过、1条件跳过 | 86.936 | 91.374 | 57.645 | 63.600 |
| fs_fill | 2/2；2通过 | 292.416 | 285.623 | 233.871 | 210.226 |
| ftest01 | 0/0；2通过 | 20.680 | 18.642 | 17.986 | 18.855 |
| ftest05 | 0/0；1通过 | 20.308 | 18.914 | 18.062 | 18.904 |

## Basic逐例表

四个配置每例分数相同，进程均退出0；四列时间单位为毫秒。mkdir_是原可执行文件名，原judge名称为test_mkdir。

| 原程序 | 四组各自分数 | RV glibc | RV musl | LA glibc | LA musl |
|---|---:|---:|---:|---:|---:|
| brk | 1/3 | 42 | 22 | 42 | 21 |
| chdir | 3/3 | 27 | 21 | 21 | 21 |
| clone | 4/4 | 21 | 22 | 22 | 22 |
| close | 2/2 | 22 | 21 | 21 | 21 |
| dup2 | 2/2 | 22 | 21 | 21 | 21 |
| dup | 2/2 | 22 | 42 | 21 | 45 |
| execve | 3/3 | 42 | 42 | 21 | 42 |
| exit | 2/2 | 21 | 21 | 22 | 21 |
| fork | 3/3 | 21 | 21 | 21 | 21 |
| fstat | 3/3 | 22 | 42 | 21 | 21 |
| getcwd | 2/2 | 43 | 21 | 21 | 21 |
| getdents | 5/5 | 22 | 21 | 21 | 21 |
| getpid | 3/3 | 22 | 22 | 21 | 21 |
| getppid | 2/2 | 21 | 22 | 22 | 21 |
| gettimeofday | 3/3 | 45 | 47 | 45 | 50 |
| mkdir_ | 3/3 | 21 | 21 | 21 | 21 |
| mmap | 3/3 | 21 | 21 | 22 | 22 |
| mount | 5/5 | 203 | 223 | 183 | 182 |
| munmap | 4/4 | 21 | 21 | 22 | 42 |
| openat | 4/4 | 21 | 22 | 21 | 21 |
| open | 3/3 | 21 | 22 | 21 | 21 |
| pipe | 4/4 | 21 | 21 | 21 | 22 |
| read | 3/3 | 21 | 21 | 42 | 22 |
| sleep | 2/2 | 1015 | 1017 | 1016 | 1015 |
| times | 6/6 | 21 | 21 | 22 | 21 |
| umount | 5/5 | 203 | 224 | 182 | 183 |
| uname | 2/2 | 22 | 21 | 21 | 21 |
| unlink | 2/2 | 21 | 21 | 21 | 21 |
| wait | 4/4 | 22 | 22 | 22 | 22 |
| waitpid | 4/4 | 22 | 21 | 21 | 21 |
| write | 2/2 | 21 | 21 | 21 | 21 |
| yield | 4/4 | 21 | 22 | 21 | 22 |

## 耗时与内核行为

四种配置均无单项超时。`fs_fill` 总耗时 **210.226–292.416 秒**，用于三名 writer 反复填满 2 GiB 文件系统、遇到 ENOSPC 后删除并重试；两条 PASS 分别对应向量顺序写和随机文件写。RV/glibc 距 300 秒单项预算约 7.6 秒，在宿主争用较高时有触及预算的实际可能。`fallocate06` 为 **57.645–91.374 秒**，主要工作包括填满磁盘，再验证预留范围仍可写、非对齐范围打洞后读零以及范围外内容保留。

这两项此前的 ENOSYS/ENOMEM 准备失败现已越过：真实 scratch 和 mke2fs 提供可格式化空间，通用块 ABI 让原工具访问设备，空间预留、写入接纳与回收/journal 配额修复使填盘过程完成。两项当前消耗的是实际文件系统负载。后续对照已把准备、向量填盘和4KiB随机文件写入分开，定位并修复extent路径的重复checksum计算；固定工作量、原程序结果及得分预算分析见[fs_fill成本分析](oscomp-ltp-analysis.md#fs_fill-的工作量内核成本与优化)。

每个程序的 START/END 由监督器使用单调时钟计时，正常退出、错误和超时均记录。程序内部 TPASS/TFAIL 是断言结果，表内时间属于整个程序，包含准备、执行与清理。前一批满盘验证中的子结果到达时间见[完整评测分析](oscomp-ltp-analysis.md#块设备与空间管理批次定向验证)。

## 磁盘与专门回归

两架构均打印 BOAROS-EVAL COMPLETE，PID1 正常结束且 heap-live=0。RV 外部 QEMU 总时长 **885.706 秒**，LA **681.116 秒**，两台并行运行；设备最终 errors/timeouts 均为 0。随后使用同源 e2fsck 1.47.2 对根盘、工具分区和 scratch 做六次 `-fn` 检查，并对两块 VFAT 做两次 `fsck.fat -vn`，八次返回值全部为 0。日志位于 `build/official-delivery-implementation/disk-checks/`。

通用回归另检查实际接口边界：

- 分区：MBR 边界与重叠、整盘/分区 claim、兄弟分区互不占用、只读、非对齐访问及引用释放。
- FAT32：四种页/簇组合的空间、身份和持久化检查，以及真实 EIO 的错误传播；两架构用户态程序再覆盖长短文件名、打开后删除、替换 rename、truncate 清零、preadv/pwritev、共享 mmap/私有 COW、活动映射卸载及只读重挂载，正常卸载后 FAT 和根盘 fsck 均正常。
- ABI：111 条固定 Linux 严格对照记录在 RV 512 MiB、LA 512 MiB/1 GiB 均一致。包含定位向量 I/O、错误检查顺序、socket 地址长度完整导入及嵌套 epoll 的五层成功/第六层 ELOOP/自引用 EINVAL/成环 ELOOP。
- 随机：按 bit 计数、字段/坏指针检查顺序和等待唤醒；实际官方设备启动未提供 RNG，种子擦除、同步后注入 256 bit，运行盘槽在关机后全零。

## 地址长度修复后的原程序短复测

使用兼容提交 `75d98e9`（含 main `e679c96` 地址长度修复及 `1faf9de` FAT 元数据修复），只运行原 `sendto01`、`connect01`，同一官方容器与原 QEMU 参数、1 CPU/1 GiB、两架构并行。所有8次程序执行均退出0，无失败或超时。

| 配置 | sendto01 | connect01 |
|---|---|---|
| RV / glibc | 10通过；105 ms | 7通过；50 ms |
| RV / musl | 10通过；65 ms | 7通过；42 ms |
| LA / glibc | 10通过；113 ms | 7通过；44 ms |
| LA / musl | 10通过；64 ms | 7通过；42 ms |

原失败项现返回 EINVAL。修复位于 `kernel/syscall/socket.c`：bind/connect/sendto 的地址长度按 Linux 的32位有符号参数解释，负值和大于128先返回 EINVAL，合法范围一次性导入完整地址，再解析协议；sendmsg 保留其独立的长度归一化和地址复制、iovec、协议检查顺序。相同错误机制的调用均已迁移，回归还检查拒绝后的 socket 状态及实际报文。

这两个旧 API 程序均不打印 Summary，原 judge 仍各记0/0。功能结果为全通过，判分行为保持原样。RV QEMU总时长1.858秒，LA1.503秒；两边种子槽均已擦除、heap-live=0，复测根盘的两次 e2fsck 均返回0。短复测的源码选择、实际内核/QEMU身份和原输出集中在 `short-probe/`，逐例结果为 `short-probe/results.json`。


固定原程序源码核对：[socket01](https://github.com/linux-test-project/ltp/blob/20240524/testcases/kernel/syscalls/socket/socket01.c)、[sendto01](https://github.com/linux-test-project/ltp/blob/20240524/testcases/kernel/syscalls/sendto/sendto01.c)、[mmap12](https://github.com/linux-test-project/ltp/blob/20240524/testcases/kernel/syscalls/mmap/mmap12.c)、[mmap14](https://github.com/linux-test-project/ltp/blob/20240524/testcases/kernel/syscalls/mmap/mmap14.c)、[preadv03](https://github.com/linux-test-project/ltp/blob/20240524/testcases/kernel/syscalls/preadv/preadv03.c)、[fs_fill](https://github.com/linux-test-project/ltp/blob/20240524/testcases/kernel/fs/fs_fill/fs_fill.c)，均固定LTP 20240524。Linux对照为仓库固定references/linux/net/socket.c的move_addr_to_kernel/__copy_msghdr，以及references/linux/net/unix/af_unix.c的unix_create。

完整本轮日志SHA-256：

- rv: `54ace6cf4dfe77dc7266cdd8cda18c99d9e7fae1cb4e8479cdc9af1585e521a1`
- la: `9c221101ce702c95f6495520424401b1d515a0d5c2d888cd8d640ec9805afab5`

## 资源统计与 CPU 时钟

main 的 `7f64307`、`df9a3d4`、`31dd8ed` 实现执行区间记账、getrusage 和 CPU 时钟，兼容提交 `1b2b3a8` 单向集成。机制见[任务资源记账](../modules/kernel-accounting.md)。固定官方容器内 `make all` 与85项适配检查通过；两架构使用原 QEMU 10.0.2 命令、1 CPU、1 GiB、官方双盘入口和独立运行盘，并行执行下列六个原始 LTP 20240524 程序。glibc/musl 在各架构内顺序运行，原参数和300秒单程序监督保持。默认启动配置与定向枚举配置分别构建，交付物恢复默认配置。

四种配置均为 **59/77 原始分，59 TPASS、18 TCONF，零 TFAIL/TBROK/超时**；其中 getrusage03 因前置条件跳过，其余五项退出0。该分数是这六项的原 judge 通过断言数。下表单位为秒，包含程序准备、执行与监督回收。

| 原程序 | 每组原始分与结果 | RV glibc | RV musl | LA glibc | LA musl |
|---|---|---:|---:|---:|---:|
| getrusage01 | 2/2；2通过 | 0.128 | 0.083 | 0.129 | 0.066 |
| getrusage02 | 3/4；3通过、1条件跳过 | 0.084 | 0.064 | 0.062 | 0.045 |
| getrusage03 | 0/1；0通过、1条件跳过 | 0.208 | 0.164 | 0.126 | 0.104 |
| clock_gettime01 | 16/16；16通过 | 0.256 | 0.237 | 0.216 | 0.211 |
| clock_gettime02 | 10/10；10通过 | 0.063 | 0.064 | 0.042 | 0.041 |
| clock_getres01 | 28/44；28通过、16条件跳过 | 0.086 | 0.063 | 0.063 | 0.066 |

- getrusage01 验证 SELF/CHILDREN 入口。getrusage02 的 raw 错误检查通过；libc 非法输出地址变体由原测例跳过一次。
- getrusage03 在申请 CAP_IPC_LOCK 前调用 capget(90)，当前入口返回 ENOSYS，LTP 以 TCONF 结束，RSS 主体尚未进入。这是凭据查询入口缺口。该原程序没有验证本轮 RSS 实现；RSS 的实际检查由下面的 raw 差分完成。其辅助程序仅由父程序按原参数调用。
- clock_gettime01/02 覆盖所支持时钟、非法参数与坏地址，均通过。clock_getres01 的16次 TCONF 来自四个测试变体各遇到 MONOTONIC_RAW、BOOTTIME、REALTIME_ALARM、BOOTTIME_ALARM 四个未提供的时钟；PROCESS/THREAD CPU 时钟检查实际通过。
- 两架构均正常结束，用户对象释放完成。两个根盘、两盘的工具/ext4 scratch/FAT 分区共8次磁盘检查全部返回0，启动种子槽已擦除。

### 短复现与开销

资源与 CPU 时钟54项、waitid58项、相关ABI111项，在RV64/LA64及512MiB/1GiB均与固定Linux匹配。RV的真实用户SMP在1/2CPU、两种内存配置通过54项；目标查询被暂停后强制回收、非主线程exec的12次启动均通过，检查实际task借用结束和正常磁盘一致性。

修复前，exec会丢失换出MM的历史RSS峰值；短复现先失败，保存旧MM峰值后两架构通过。两核exec还暴露了读盘调用链的8KiB内核栈耗尽：块层给每次读取都预留32项分区转换数组，连整盘单页缺页也付出1.25KiB栈开销。整盘现在直接借用原向量，分区按本次请求大小转换；同一短复现通过，栈最小剩余约1.25KiB。

后续CPU记账已改为CPU本地执行片段，并优化了counter访问。新的同批三版本对照、原版SQLite负载与观测到的LA事务耗时上升，统一见[CPU记账成本](cpu-accounting-cost.md)。本节六项原程序的分数仍对应前面标明的`1b2b3a8`运行；后续优化的资源/时钟、真实两核和程序成本记录由该报告维护。

本批运行原始文件保存在 `build/cpu-usage/official-probe/`：`probe-run.json`记录QEMU命令和kernel/init/盘身份，`container-identity.json`记录固定容器工具，`results.json`、各组judge输出和串口记录包含24次程序的结果与耗时，`disk-checks.json`与对应fsck日志记录8次磁盘检查。主线差分与并发日志在同目录上级；配对计时为 `matched-benchmark/results.json`。复用缓存继续保留，完成检查的运行盘删除。

## CPU 间隔定时器与信号等待

main 的 `6dc2737` 增加 ITIMER_VIRTUAL/PROF，`4105d5b` 整理无事件的信号返回路径，兼容提交 `63c261f` 单向集成。VIRTUAL 消耗整个线程组的用户 CPU 时间，PROF 消耗用户加内核 CPU 时间；睡眠、子进程与中断时间分别处理，线程退出前的累计用量继续保留。计时器由进程持有，fork 后关闭，exec 时保留；状态与实际 CPU 执行者的关系见[时间模块](../modules/kernel-time.md#进程-cpu-间隔定时器)。

### 原测例的结果、得分和耗时

固定官方容器内断网构建，`make all` 与85项适配检查通过。实际运行使用该容器的 QEMU 10.0.2、原双盘启动参数、1 CPU/1 GiB和独立运行盘，未增加 RNG 设备。两架构并行，每架构内依次运行两种 libc 的以下五个原程序。定向 PID1 只更改外层程序枚举；原 ELF、libc、参数和程序内部时限均保持，监督器仍为300秒。未设置 `LTP_TIMEOUT_MUL`，两项信号等待沿用原30秒内部时限。

下表的时间单位为秒，包含准备、执行和回收。分数为原 judge 的通过断言数。

| 原程序 | 每组原始分与结果 | RV glibc | RV musl | LA glibc | LA musl |
|---|---|---:|---:|---:|---:|
| getitimer01 | 30/30；30通过 | 0.144 | 0.082 | 0.133 | 0.088 |
| setitimer01 | 0/6；3个准备失败、3个失败 | 0.090 | 0.062 | 0.085 | 0.044 |
| setitimer02 | 3/3；3通过 | 0.062 | 0.042 | 0.064 | 0.042 |
| sigtimedwait01 | glibc 11/11；musl 0/1、1个运行超时 TBROK | 1.390 | 30.062 | 1.366 | 30.049 |
| sigwaitinfo01 | glibc 9/9；musl 0/1、1个运行超时 TBROK | 0.368 | 30.039 | 0.369 | 30.053 |

每架构 glibc 合计 **53/59**，musl 合计 **33/41**。这是所选五项的原始分；前文完整运行的总分继续对应其原版本。getitimer01、setitimer02及glibc信号等待退出0，setitimer01退出3，musl两项等待退出2；外层监督均未到300秒。

`getitimer01` 从旧结果20/32变为30/30。新增的实际通过数为10；分母减少2，是因为清理时的两次 `TST_EXP_PASS_SILENT(setitimer(...))` 成功后不打印通过，旧版返回 ENOSYS 时则各打印一次失败。两次清理仍执行。`setitimer02` 从1通过、1条件跳过变为3/3：三种计时器的坏输出指针现在均走到 EFAULT 检查。

### 两类剩余失败的具体原因

**setitimer01 被 RLIMIT_CORE 前置条件挡住。**每个子进程先调用 `tst_no_corefile(0)`，其 `getrlimit(4)` 在内核的资源限制分支返回 EOPNOTSUPP(95)，于是 TBROK 并退出2。父进程原本等待定时器信号终止子进程，看到退出2后再报 TFAIL；三种计时器共3个TBROK、3个TFAIL。该程序尚未执行设置与触发定时器的主体。main 的[能力目录](../goals.md)记录了 RLIMIT_CORE 缺口；本轮的真实信号触发由下面的通用短例验证。

**musl 两项等待超时来自库与测例预期的差异。**首项使用空等待集合，测例期待被 SIGUSR1 打断后返回 EINTR；原 musl 在内核返回 EINTR 后自行重试，继续等待。固定 Linux 使用同一原 ELF 和原 libc，两架构同样各30秒后超时，glibc则分别11/11、9/9。BoarOS 直接 syscall 的 EINTR 行为与 Linux 一致，包括 SA_RESTART。原输入与计分继续保留；库机器码、版本和具体调用解释见[测例问题](oscomp-test-issues.md#musl-信号等待库重试-eintr测例等待它返回-eintr)。

### 短回归、成本和磁盘检查

通用定时器36项、信号等待31项，在RV/LA的512 MiB和1 GiB均与固定Linux匹配。RV另用真实用户态SMP完成1/2 CPU、两种内存配置，检查两核实际执行、线程组用量合计、设置者/主线程退出、非主线程exec、周期信号合并及默认动作。新增1µs周期睡眠短例曾在开发版失败：进程已经睡眠，过期阈值仍随墙钟检查推进；现在只在该组确有正在执行的成员时处理CPU计时器，睡眠期间保留阈值，修复后与Linux一致。宿主调度并发、handoff、用户SMP以及实际RV counter顺序检查也通过。

未开启计时器的成本使用宿主同一P核类型、固定CPU0/1，所有计时串行，五轮轮换新旧版本和字节相同的旧版自对照。getpid耗时RV为−0.39%、LA为+0.68%，处于本组自对照幅度内；小块读写仍有RV +1.56%、LA +3.88%，RV yield +1.61%。具体负载、绝对值和剩余开销见[成本分析](cpu-accounting-cost.md#未启用-cpu-间隔定时器的成本)。这些配对计时与上面并行功能运行的耗时分别记录。

正式原程序运行RV为68.391秒、LA为68.021秒，均正常关机、heap-live=0，启动种子槽全零。根盘、工具分区、VFAT和scratch共8次离线磁盘检查全部返回0。默认交付配置在定向运行后重新构建。原始枚举与源码提交、实际kernel/init/QEMU身份、逐程序输出和判分、8次fsck结果集中在 `build/cpu-timer-signal/after-original/`；通用最终回归为同目录上级的 `itimer-*-final-fast`、`signal-wait-*-final-fast`，Linux原libc对照为 `musl-linux/`。中间版本与正式计时分开保存，完成检查的运行盘删除。

轮前第一次RV定向启动曾停在首个程序START后，随后相同内核的两次新盘重放及本轮正式运行均完成。该一次性停顿尚未复现和归因，未计作正常结果，也未据后续通过宣称修复。
