# OSComp 测例与环境问题

我按程序启动、测例准备、接口调用和原判分四个边界定位异常。先确认程序走到了哪一步，
再检查该步实际使用的可执行文件、运行库、参数和返回值。必要时在 Linux 上运行同一
可执行文件和运行库，区分发布输入、内核行为与判分问题。

初赛盘为 pre-20250615，其中 Linux 测试项目（Linux Test Project，LTP）版本为 20240524。
原盘位置、启动和监督方式见[评测环境适配](../modules/oscomp-compat.md)。
逐次执行的串口、耗时和原判分保存在 `build/`。

## 测例启动与准备

| 项目 | 如何判断原因 | 处理 |
|---|---|---|
| `acl1` | 原发布文件仅包含可选访问控制列表（Access Control List，ACL）依赖不可用时的提示分支。固定 LTP 源码在未定义 `HAVE_LIBACL` 时构建此分支，程序输出 `The acl library was missing upon compilation.`，退出 32。 | 保留原文件。要执行完整测试，需用 libacl 重新构建，并提供文件、目录及指定的数字用户和组。原盘直接无参执行也无法满足这些前提。完整测试打印 `ACL TESTS RESULTS`，判读应结合实际输出。见[固定源码](https://github.com/linux-test-project/ltp/blob/20240524/testcases/network/nfsv4/acl/acl1.c)。 |
| `io_setup01` | 初赛盘中 RISC-V 和 LoongArch 的 glibc、musl 四份发布文件均编入了未定义 `HAVE_LIBAIO` 时的提示分支。测例构建时需检测到 `libaio.h` 并能链接 `libaio` 的 `io_setup`。发布文件启动后报告 `TCONF` 并退出 32，未发起 `io_setup` 系统调用。 | 在对应架构和 C 库的构建环境中准备头文件及库，重新构建固定版本的测例。原生异步输入输出入口现已接入通用内核；发布程序仍需重新构建才能进入该入口。见[原测例源码](https://github.com/linux-test-project/ltp/blob/20240524/testcases/kernel/syscalls/io_setup/io_setup01.c)及[构建检测](https://github.com/linux-test-project/ltp/blob/20240524/m4/ltp-eventfd.m4)。 |
| `setxattr01` 在 LoongArch glibc 上提示缺少 `mkfs.ext4` | 同环境直接启动 `mkfs.ext4` 成功，但 `system("exit 0")` 也返回 127。跟踪 glibc 的 `posix_spawn` 可见其 `clone3` 请求包含 `CLONE_CLEAR_SIGHAND`，旧内核返回 `EOPNOTSUPP`（95），该库仅在收到 `EINVAL` 或 `ENOSYS` 时回退。 | 通用内核已实现子进程信号动作重置语义，原测例无需修改。 |
| 使用 `getpwnam("nobody")` 的测例 | `access01`、`mkdir02`、`readlink01`、`pidfd_send_signal02`、`pidfd_getfd02`、`mlock02`、`mlock202`、`shmctl02`、`mount03`、`umount03`、`mq_open01`、`mq_unlink01` 依赖原盘账户数据库。缺少 `/etc/passwd` 时，准备阶段在目标操作前结束。 | 保留原盘。验证数字身份相关权限时，使用明确创建测试账户的独立运行环境。 |
| glibc 的 `float_*` | 线程取消或退出时，glibc 需要 `libgcc_s.so.1` 展开线程栈。缺少这一库时可在日志中看到加载失败。 | 归为该运行库配置缺失。 |
| LoongArch 原盘的动态程序 | 加载器路径取自程序的 `PT_INTERP` 段。glibc 的 `/lib64/ld-linux-loongarch-lp64d.so.1` 和 musl 的 `/lib64/ld-musl-loongarch-lp64d.so.1` 须指向原盘对应的加载器。固定 Linux 下缺同名链接也会在执行阶段失败。 | 启动脚本按发布盘清单创建实际路径链接。 |
| LoongArch musl 调度包装 | 原库的四个调度函数直接返回 `ENOSYS`。捕获系统调用可核对这些包装没有进入内核。 | 由[动态共享对象适配](../modules/oscomp-compat.md#调度适配)接入原生系统调用。 |
| libc-test 的脚本解释器 | 发布的 `run-static.sh` 和 `run-dynamic.sh` 无解释器声明行。LoongArch 原 musl BusyBox 直接执行时也会在 Linux 上失败。 | 临时外层脚本改用原盘对应的 BusyBox `sh` 显式调用两行，保留内层脚本和参数。 |
| `eventfd01/02` | 测例在依赖准备阶段读取 Linux 内核配置，缺少 `CONFIG_EVENTFD` 的配置信息时以 `TBROK` 停止。 | 判读时标记准备失败，实际事件描述符接口由进入系统调用的程序验证。 |
| `readahead01` | 测例通过 `tst_fd_next()` 构造多类文件描述符。构造器因接口或事件源不可用而跳过时，对应文件描述符尚未传给 `readahead`。 | 先按[固定构造器](https://github.com/linux-test-project/ltp/blob/20240524/lib/tst_fd.c)和当前日志区分准备、跳过及实际断言，再核对当前内核能力。 |

### final-2026 的根盘和构建输出管道

发布的 ext4 根盘曾在日志重放时被拒绝。顺着挂载错误追到 `jbd_extract_block_tag`，
发现旧实现额外要求描述符中可选的通用唯一标识符（Universally Unique Identifier，UUID）
等于日志超级块中的值。发布盘的可选字段为全零。Linux 恢复路径根据标志跳过该字段，
按日志校验和验证记录。通用实现已按该格式处理，发布盘副本可由内核直接重放。

BuildStorm 的 Rust 工具链曾在准备阶段中止。沿 `Child::wait_with_output`
追到 `ioctl(pipefd, FIONBIO, &one)`，当时普通管道返回 `ENOTTY`（25），库随后
因未能启用非阻塞读取而退出。通用文件描述符分派已接入 `FIONBIO`，该调用更新共享打开
文件描述（Open File Description，OFD）的非阻塞标志。构建产物与工具链版本检查应分别判读。

### 启动时设备未完成 FLUSH

两架构平台日志中的 `block queue fault reason=timeout` 来自旧块设备驱动。
等待对象是请求类型 4，即要求设备将先前写入推进至持久存储的 `FLUSH`。故障时设备状态
仍为初始化值，完成环未前进，驱动在 30 秒时复位设备，随后根文件系统挂载失败。
日志证明了请求未交付完成。宿主同步延迟和设备推进停滞均缺少足以区分的宿主侧记录。

现行驱动继续等待设备结果，按周期报告仍未完成。分析新日志时，先对照请求状态、完成环和
根盘挂载记录，若能访问宿主，再核对后备文件的同步完成和虚拟设备进展。驱动策略见
[VirtIO 块设备](../modules/riscv-virtio-block.md)。

## 实际调用与错误归属

- **`basic/brk`。** 发布的可执行文件把系统调用返回的 64 位堆顶截断为低 32 位再符号扩展。
  核对返回寄存器与下一次请求地址即可确认两次扩展请求实际使用了截断值。原文件和判分保留。
- **`epoll_ctl04`。** 固定 LTP 只接受 `EINVAL`。较新的 Linux 会先检查向上嵌套深度，
  第六层返回 `ELOOP`，BoarOS 使用同一错误顺序。LTP 上游已[同时接受两种错误](https://github.com/linux-test-project/ltp/commit/e84f0689cf7a8a77478a0e70aa62560f66c3bceb)。
- **LoongArch musl `mremap06`。** 发布文件以 4 KiB 为拆分步长，在 16 KiB 页系统上
  传入未对齐的源和目标地址。同一文件在 16 KiB 页 Linux 上也在第一次搬移报告 `TBROK`。
- **`sigtimedwait01`、`sigwaitinfo01`。** 测例要求信号打断空集合上的等待并返回
  `EINTR`。原 musl 的包装会重试；空集合始终无法取得信号，因此等待可能持续到内部监督到期。
- **原 musl `sendmsg01`。** 对 RISC-V 原库的反汇编显示，包装先从用户的
  `msg_control` 复制数据，再进入系统调用。非法指针可在用户态触发 `SIGSEGV`，
  同一原文件和库在 Linux 上表现一致。
- **`cve-2016-7117`。** 原日志把 `EINVAL` 记在 `recvmmsg` 上。逐项追踪显示，
  描述符并发关闭时接收调用实际返回 `EBADF`。紧接着的
  `clock_gettime(CLOCK_MONOTONIC_RAW)` 在旧内核上返回 `EINVAL`，覆盖了线程的
  `errno`。通用时钟查询已补齐，判读同类日志时应分别观察两个系统调用结果。
- **RISC-V 原静态 glibc 时钟断言。** 若时钟调用已返回成功，继续追踪 `errno` 的
  首次写入。已定位的一种情况是 `malloc` 初始化在进入 `main` 前调用
  `getrandom` 得到 `EAGAIN`，之后成功的时钟调用保留了这个旧值。旧盘启动先完成
  随机源初始化。

### 间歇性的 RISC-V musl 反向 TCP 失败

原 `iperf3` 常驻服务端依次处理六项。`REVERSE_TCP` 紧接 `REVERSE_UDP`，
失败现场在输出传输统计前报告控制连接重置，另一次平台运行该项成功。成功配置也会输出
个别不支持的套接字选项警告。复查时应保持同一服务端和前序测试，观察控制连接何时关闭；
目前只确认了失败所在阶段。

## 监督与判分

旧 LTP 入口直接遍历可执行目录，其中也包含需要控制器、管道、终止信号或配套服务的辅助
程序。例如 `netstress` 无参数默认作为服务端等待连接，`doio` 需要 `iogen` 提供
结构化输入。适配器依据[跳过表](../../tests/oscomp/ltp-skips.tsv)过滤缺前提的直接调用，
配套控制器仍能启动它们。兼容分支现行跳过表排除了 `fs_fill`，独立执行时还需隔离
[填盘后写回失败造成的挂载残留](#fs_fill-的空间预留缺陷)。

逐例监督从每个测试启动计时，原评测框架的整机预算从虚拟机启动计时。平台下载的日志
可能有大小截断。要判断某项正在运行、已经结束或从未进入，需结合完整串口、脚本结束标记
和虚拟机生命周期。下载文本的最后一行只确定可见范围。

- **LTP 标记。** 旧程序可打印 `TPASS` 而没有 `Summary:`，原判分只汇总后者。
  `FAIL LTP CASE … : 0` 是脚本的统一结束行，末尾 0 表示程序退出成功。
  `mremap05` 有一条内容不符分支也会打印 `TPASS`，内容应另行验证。
- **BusyBox `kill 10`。** 判分清单要求此标签，发布脚本实际执行
  `sh -c 'sleep 5' & ./busybox kill $!` 并按整行输出。缺标签与信号发送结果应分别判读。
- **IOzone。** 原判分使用多进程方法中的 `Max`，即最快子进程速率；自动模式的各方法
  不进入同一计分公式。前者受提前停止影响，分析整组吞吐时需检查逐进程计入字节和共同
  计时区间。发布文件对 `-i 11 -i 12` 报不支持并回退普通初写和重写，这两项仍可能
  进入判分。带 `-e` 的诊断应分别核对普通读取的 `fsync` 与标准输入输出库读取的
  `fflush` 是否落在方法计时内。定位数据和设备等待的方法见
  [文件系统说明](../modules/vfs-ext4.md#区分提前停止读取和同步)。
- **final-2026。** CAgent 的项目时间奖励合计可为 199.1 分，BuildStorm 构建失败时
  原脚本仍可给工具链检查分。发布说明要求八核，BuildStorm 判分脚本的 LoongArch
  核数告警采用 12 核判断。报告按原判分程序实际行为和发布配置分别说明。

## 初赛单项审计与 7200 秒整机预算

本轮使用兼容分支 `3a46215d` 和初赛盘原程序、原判分。每组以一个虚拟处理器和 1 GiB 内存执行，按架构与 C 标准库统计。为观察慢项的真实进展，本地审计关闭了逐项监督。遇到已挂载设备影响后续格式化、或者压力程序迟迟未结束时，在干净运行副本中从余项继续。下表是合并各段后的本地覆盖和原始小计，可用于决定检查顺序，不能直接当作一次平台运行的耗时或得分。平台按原始分数加总，整机预算为 7200 秒。兼容分支默认单项监督为 120 秒，两者独立。

| 配置 | 有结束记录的程序 | 其中有得分 | 原始分数 | 有通过断言但无分数 |
|---|---:|---:|---:|---:|
| LoongArch glibc | 2804 | 851 | 6935 | 174 |
| LoongArch musl | 2785 | 841 | 6886 | 168 |
| RISC-V glibc | 2803 | 859 | 6999 | 173 |
| RISC-V musl | 2784 | 848 | 6954 | 165 |
| 合计 | 11176 | 3399 | 27774 | 680 |

另有 144 个程序实例因原跳过表或本轮隔离故障而未计入完成数量。逐项记录见本地 `build/ltp-audit-20261121/cases.tsv`。其中 `observed-scores.tsv` 的最高观察分和判分端观察到的断言合计均来自本地输出。条件编译、同一程序的多种后端及未到达的分支都会影响这些值，不能据此给每个可执行文件指定固定满分。判分会计入 `Summary:` 中的 `passed`，所以有通过断言、同时有失败或准备中止的程序仍可能取得部分分数。

### 四种配置都通过断言、均得 0 分的慢项

下列 19 个独立程序逐个核对了四份串口记录。每个配置的退出状态均为 0，均有 `TPASS`，均未输出 `Summary:`，并且未出现 `TFAIL` 或 `TBROK`。表中通过断言条数为**每个配置中的条数**，耗时是四种配置分别测得的宿主墙钟范围。部分程序有条件跳过的分支，例如 `rename11` 的链接数上限检查。

| 程序 | 确认执行的工作 | 通过断言 | 单次秒数 |
|---|---|---:|---:|
| `rename11` | 查验符号链接循环和只读文件系统的改名错误，后者需要测试盘准备；链接数上限分支跳过 | 2 | 102.4–184.7 |
| `renameat01` | 相对和绝对路径的改名，及坏目录描述符、非目录、只读文件系统等错误 | 7 | 99.8–182.6 |
| `inode02` | 多进程创建、打开、写入和读取文件，检查文件及索引节点的使用 | 1 | 29.4–34.8 |
| `sendmsg02` | 启动并发发送者，按旧框架默认运行 15 秒后检查子进程结果 | 1 | 15.1 |
| `kill02` | 检查组内进程收到信号、组外进程不收到信号 | 2 | 10.1 |
| `ftest01` | 父子进程的读取、写入、截断和同步 | 2 | 7.7–11.9 |
| `ftest07` | 向量读取与写入，包含截断和同步 | 1 | 7.9–9.3 |
| `open12` | `O_APPEND`、`O_NOATIME`、`O_CLOEXEC`、`O_LARGEFILE` 四项 | 4 | 6.7–9.7 |
| `umount2_01` | 独立测试盘上的普通卸载 | 1 | 5.8–10.4 |
| `ftest05` | 并发文件读写、截断和同步 | 1 | 7.1–7.8 |
| `mknodat02` | 目录描述符相对创建设备节点与错误条件，包括只读文件系统 | 9 | 5.8–8.8 |
| `lchown03` | 符号链接循环及只读文件系统错误 | 2 | 5.8–8.4 |
| `ftest03` | 向量读写与文件长度检查，包含截断和同步 | 1 | 6.4–7.6 |
| `fcntl14_64` | 记录锁操作及跨进程的锁结果，使用 64 位接口构建变体 | 96 | 5.7–5.9 |
| `fcntl14` | 记录锁操作及跨进程的锁结果 | 96 | 5.7–5.8 |
| `nptl01` | 本地 POSIX 线程库（Native POSIX Thread Library，NPTL）线程压力用例完成 | 1 | 2.7–8.5 |
| `rename14` | 多进程改名压力用例完成 | 1 | 5.1 |
| `ftest08` | 向量读写、文件重开与同步 | 1 | 3.5–4.7 |
| `ftest04` | 向量读写、文件大小和同步检查 | 1 | 3.4–4.1 |

三个同为四组零分但跨库结果不同的程序需单独看。`epoll-ltp` 在 LoongArch 两种库和 RISC-V glibc 上各有 33 条通过断言且无摘要。RISC-V musl 有 32 条通过断言和 1 条失败，失败项要求负数参数的 `epoll_create` 返回错误。`semop05` 的 musl 变体约 20 秒后完成且无摘要，glibc 变体在线程退出时报告缺少 `libgcc_s.so.1`。`profil01` 的 glibc 变体约 5 秒后验证采样数据且无摘要，musl 变体输出 `TCONF`。这些差异应随库分类。

### 其他慢零分项的实际阻塞点

| 分类 | 测例与证据 |
|---|---|
| 账户准备 | `chdir01`、`creat09`、`mknod07`、`mount03`、`mount04`、`openat04`、`prctl06`、`rename12`、`swapon02`、`swapoff02`、`umount03`、`utime02/03/05` 均在 `getpwnam("nobody")` 中止。多个程序在查账户前已经格式化或挂载，因此几秒到十余秒主要花在准备。 |
| 设备枚举与进程文件 | `sync01`、`syncfs01`、`fsync04`、`fdatasync03`、`sync_file_range02` 在 `opendir("/sys/block/")` 停止。`readahead02` 需要 `/proc/self/io`，`msync04` 需要 `/proc/self/pagemap`。这些是实际缺失的内核观察接口。日志中的失败位置在各自目标操作之前。 |
| 文件系统能力 | `execveat03`、`inotify07/08` 跳过叠加文件系统 OverlayFS。`fanotify01/03/05/06/09/10/13/14/15/16/17/18/19` 在通知接口或其准备检查处跳过，`fanotify13` 另需 OverlayFS，`fanotify23` 先在挂载 ext2 时得到 `ENODEV`。`swapoff01` 在准备交换文件时调用 `FIEMAP` 查询文件区间的物理映射，返回 `ENOTTY`。`swapon03` 报告不支持交换空间。 |
| 现有接口的选项缺口 | `preadv203` 和 `preadv203_64` 需要非阻塞缓存读取标志 `RWF_NOWAIT`。`statx10` 缺直接输入输出（Direct I/O）对齐报告 `STATX_DIOALIGN`，`statx12` 缺挂载根属性 `STATX_ATTR_MOUNT_ROOT`。`fsconfig01` 和 `fspick01` 都因 `sync` 参数返回 `EINVAL`，详见下节。 |
| 原程序自己的超时 | `send02`、`statx11` 四组均约 30 秒并自行报告 `Test killed! (timeout?)`。本轮关闭了外层逐项监督，单凭这行无法判断它们在等哪个事件。 |
| 运行库和脚本 | `float_bessel`、`float_exp_log`、`float_iperb`、`float_power`、`float_trigo` 的 glibc 路径存在缺少 `libgcc_s.so.1` 的退出故障，musl 的退出与输出另计。`lchown03_16` 所需的旧 16 位标识符接口不属于两种原生 64 位接口。`memcg_test_4.sh` 使用相对路径调用 `./memcg_test_4`，当前工作目录找不到该程序。`run_sched_cliserv.sh` 的脚本启动结果跨库不同，均未提供可计分摘要。 |
| 压力程序与纯退出 | `hackbench`、`ebizzy`、`mmap-corruption01`、`mmap2` 结束时均无可计分断言。前两项分别施加进程通信／调度和内存分配负载。`mmstress` 有非零退出，四组均无摘要，现有日志不足以统一归因。 |

### 耗时长但有分数的压力程序

| 程序 | 本地墙钟与实际得分 | 如何用于验收 |
|---|---|---|
| `dio_sparse` | 四组约 1143–1462 秒，各 1 分 | 直接输入输出下的稀疏文件压力。以 120 秒逐项监督运行时，预算模型不能计入该 1 分。 |
| `dio_append` | RISC-V 两组 967、1016 秒，各 1 分；LoongArch 两组准备时报告空间不足，0 分 | 追加压力和盘容量是两个条件。需要另外验收时固定测试盘容量，检查完成的迭代和实际文件内容。 |
| `mmap1` | 四组约 180 秒，各 1 分 | 映射与并发压力，运行时间由原负载决定。 |
| `inotify09` | 三组约 150 秒，各 1 分；RISC-V musl 约 180 秒后程序超时，0 分 | 保留库差异，后者需另查事件与等待。 |
| `starvation` | 四组约 215–231 秒，各 2 分 | 同时验证调度进展和完成条件，单项限时会切掉原工作量。 |
| `cve-2016-7117` | 三组约 60 秒，各 1 分；RISC-V musl 约 90 秒后程序超时，0 分 | 与前文已经解决的时钟错误覆盖问题分开处理。 |
| `mallocstress` | 四组约 7–35 秒，各 1 分 | 记录分配压力和实际结果，区分主机上快慢配置。 |
| `shm_test` | 无可计分摘要。独立观察约 54 秒只完成前 30 轮，随后停止 | 默认 30 线程、每线程 1000 轮，每访问一个字节调用 `sched_yield` 主动让出处理器。程序无内部时限。已经看到进度，未证实死锁。 |
| `fs_fill` | 审计中 LoongArch glibc 约 37 秒、1 分，第二阶段破损；其他配置因隔离而未验收 | 是真实持久化缺陷，另见下节。应在独立可写盘验证，避免牵连后续程序。 |

按本地 LoongArch 顺序和既有前置组耗时模拟，现行 120 秒监督及现行 `fs_fill` 排除条件下，两组约在 6418 秒结束。再过滤本地单组至少 3 秒且通过断言但无摘要的实例，投影约 5609 秒，保持模型内原分；进一步过滤所有慢零分实例约 4307 秒。以上用的是本地分段数据，平台宿主可能更慢。适配仅应跳过已复核的配置，压力和正确性验证仍可用独立短例或专用盘执行。此次只记录候选，原发布程序和当前跳过表继续使用。

### 内核功能补齐清单

| 优先验证的能力 | 本轮最直接的证据及分数边界 |
|---|---|
| 文件句柄与挂载属性 | `name_to_handle_at01/02` 和 `open_by_handle_at01/02` 在获取文件句柄阶段遇到 `ENOSYS`，后续按句柄打开不能继续。`fsconfig01` 对 `FSCONFIG_SET_PATH` 的 `sync` 键返回 `EINVAL`，`fspick01` 对字符串形式的 `sync` 同样失败，后者已经取得挂载重配置上下文。`fallocate04` 已经取得 1 分，随后因 `SEEK_HOLE` 不支持提前停止，剩余断言尚未执行。`lseek11` 也已直接确认普通文件的 `SEEK_DATA/SEEK_HOLE` 缺口。 |
| 改名、调度和时钟选项 | `renameat201/202` 需要 `RENAME_EXCHANGE/RENAME_WHITEOUT`，`sched_getattr01` 和 `sched_setattr01` 需要截止期调度 `SCHED_DEADLINE`。`rtc01/02` 缺实时时钟（Real-Time Clock，RTC）的闹钟、更新中断或设时操作，`leapsec01` 缺闰秒插入。上述原程序在四种配置中均未得分。 |
| 文件通知和文件系统观察 | `fanotify` 的初始化与监视接口缺失。原盘 23 个同名程序还可能依赖 OverlayFS 或其他文件系统，补齐入口后仍需逐项核对。设备枚举、进程读写统计、页表映射信息和文件区间映射接口见上一节。`wqueue01–09` 还需要 `O_NOTIFICATION_PIPE` 的内核事件通知管道。当前有匿名与共享内存大页，`hugemmap*` 等因缺大页文件系统 `hugetlbfs` 无法准备。 |
| 进程记账、密钥和内存操作 | `acct01/02` 缺进程记账，`keyctl01` 及相关程序缺内核密钥管理，`userfaultfd01` 缺用户态缺页处理入口，`remap_file_pages01/02` 缺对应的旧映射操作。各接口族使用独立探针复核，再量化可恢复分数。`ioctl_ns03` 缺命名空间所有者标识查询，`ioctl_ns06` 缺用户命名空间。 |
| 网络套接字选项与协议 | `accept02` 在设置 IPv4 组播成员选项 `MCAST_JOIN_GROUP` 时返回 `ENOPROTOOPT`，还未执行接纳断言。`setsockopt04` 缺强制发送缓冲区选项 `SO_SNDBUFFORCE`，`setsockopt02` 缺 `AF_PACKET`，`sendto02` 需要流控制传输协议（Stream Control Transmission Protocol，SCTP）。`accept03` 已有 17 分，文件通知、用户态缺页处理及性能事件等描述符构造分支跳过。 |
| 更大范围的内存、设备与异步接口 | 交换空间、非均匀内存访问（Non-Uniform Memory Access，NUMA）的内存策略和迁移、内存保护密钥、`io_uring`、模块装载、文件系统配额分别核对。配额原程序还缺 `quotacheck` 等工具，`io_uring01/02` 与部分模块测例先检查 Linux 配置；此类零分记录本身不足以证明已执行对应 syscall。 |
| 已接入但原程序无法核对的接口 | 新版本已有原生异步输入输出（Asynchronous I/O，AIO）、`sync_file_range`、`adjtimex`、`clock_adjtime` 及 `membarrier`。发布盘的 `io_setup01` 在构建时缺 libaio，运行未调用内核。当前审计中 `sync_file_range01`、`clock_adjtime01`、`membarrier01` 已分别在四种配置得到 5、9、12 分。`perf_event_open01` 在配置检查阶段停止，现有内核接口也只覆盖有限事件。 |

按架构提供的旧入口要另外处理。例如原 RISC-V `futex_waitv01/02/03` 使用的是发布程序构建时的无效系统调用号 `-1`，而 LoongArch 版本已执行断言。`getrlimit03` 在 LoongArch 期待该架构的 Linux 原生接口未提供的旧 `getrlimit` 入口，RISC-V 对应原程序有 16 分。类似的 16 位用户标识符、旧 `socketcall` 和 `readdir` 测例，先核对架构应用二进制接口（Application Binary Interface，ABI）及发布程序的头文件。

### 挂载相关的三类故障

**`df01.sh` 的工具选项及挂载残留。** RISC-V glibc 原脚本先格式化并挂载测试盘，再用 `stat -f ... --printf=%b` 等命令读取总块数、空闲块数和块大小，以检查 `df` 输出。发布盘 BusyBox 的 `stat` 提供 `-c` 格式选项，遇到 GNU `--printf` 报错。命令替换得到空字符串，脚本在第 85 行算术表达式退出，未运行框架的卸载清理。下一程序 `execveat03` 对同一分区执行格式化时报告设备仍在挂载。LoongArch 原程序遇到的是更早的 Shell 超时参数准备错误，未走到同一条格式命令。短例只将原脚本的 `stat --printf` 参数映射到 `stat -c`，保留原脚本和 BusyBox。原程序恢复 7 项通过，剩下 5 项分别在 BusyBox 的 `df -t/-v/-H/--version/-x` 失败。框架仍在释放设备时打印警告，但同一虚拟机里的下一个 `umount2_02` 已能格式化原测试分区并通过 7 项，确认这次未遗留挂载。完整保留原脚本时，应在兼容环境同时提供 GNU Coreutils 的 `stat` 和 `df`，并将测例放到私有挂载命名空间，或在失败后显式卸载测试分区，确认无残留再复用。GNU 两个程序的完整对照和 LoongArch 的超时参数仍需分别验收。

**`umount03` 的账户准备失败。** LoongArch glibc 原程序约 10.7 秒退出，状态为 2、得 0 分。它先以特权用户格式化并挂载测试盘，随后在 `getpwnam("nobody")` 得到 `ENOENT`（找不到账户），所以未切换有效用户标识符，也未执行预期返回 `EPERM` 的卸载断言。在独立运行副本加入 `nobody:x:65534:65534:nobody:/:/bin/false`，保留原程序再次执行，LoongArch glibc 按预期得到 `EPERM`，原摘要为 1 个通过。今后若决定为评测统一提供该账户，应在准备可写副本时检查同名账户是否已经存在，再补缺失项。该程序只读取账户条目并调用 `seteuid`，无需为它另配登录程序。

**`umount2_02` 的两步卸载。** 旧内核拒绝 `MNT_EXPIRE`，两个 glibc 配置只通过 2 项 `UMOUNT_NOFOLLOW` 断言。原测例继续执行后会留下叠放的挂载。通用内核接入过期标记后，LoongArch 和 RISC-V 的 glibc 原程序均通过 7 项，退出状态均为 0。短例在同一 RISC-V 可执行文件中对照 BoarOS 与 Linux，额外验证了忙引用、普通访问清除标记及绑定视图之间的隔离。

### `fs_fill` 的空间预留缺陷

原程序先让三个写入线程用 `writev` 写至 `ENOSPC`，再用不同大小的文件反复填盘。第二阶段遇到空间不足时，`lib/tst_fill_fs.c` 先调用 `fsync` 提交成功接纳的写入，然后缩小请求并继续。LoongArch glibc 在干净测试盘上单独复现了 `fsync` 返回 `ENOSPC`，随后的 `umount` 返回 `EIO`。部分运行还在用户态报出堆损坏信息，这条日志出现在首个同步错误之后，来源需要单独验证。

诊断将首个同步错误定位到 ext4 延迟分配的区段树扩展。写入接纳为文件数据预留块。回写分配完当前数据块后，区段树需要额外申请一个元数据块。诊断记录到超级块还有 27839 个空闲块，但扣除已有预留及当前事务剩余额度后，可用块数为 0。区段树的申请返回 `ENOSPC`，错误经 `ext4_write_spans` 回到页缓存写回及 `fsync`。卸载跟踪确认，页缓存清理前重试写回，写回失败返回缓存输入输出错误，ext4 将该状态转换为 `EIO`。失败盘的只读检查跳过了日志重放，现场不足以据此判断掉电恢复或已接纳数据的完整性。修复需补足数据接纳与区段树元数据的共同预留，并在写回成功、事务取消、截断和打洞时归还各自额度。直接忽略同步错误会使已接纳的数据无法得到持久化保证。修复后用独立小盘、并发填盘及磁盘检查验证。
