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
| `io_setup01` | 初赛盘中 RISC-V 和 LoongArch 的 glibc、musl 四份发布文件均编入了未定义 `HAVE_LIBAIO` 时的提示分支。测例构建时需检测到 `libaio.h` 并能链接 `libaio` 的 `io_setup`。发布文件启动后报告 `TCONF` 并退出 32，未发起 `io_setup` 系统调用。 | 在对应架构和 C 库的构建环境中准备头文件及库，重新构建固定版本的测例。通用内核已接入原生异步输入输出（Asynchronous I/O，AIO）。当前初赛盘只跳过这个程序。发布方更换目标头文件和库后重新构建，程序才能验证该入口。见[原测例源码](https://github.com/linux-test-project/ltp/blob/20240524/testcases/kernel/syscalls/io_setup/io_setup01.c)及[构建检测](https://github.com/linux-test-project/ltp/blob/20240524/m4/ltp-eventfd.m4)。 |
| `setxattr01` 在 LoongArch glibc 上提示缺少 `mkfs.ext4` | 同环境直接启动 `mkfs.ext4` 成功，但 `system("exit 0")` 也返回 127。跟踪 glibc 的 `posix_spawn` 可见其 `clone3` 请求包含 `CLONE_CLEAR_SIGHAND`，旧内核返回 `EOPNOTSUPP`（95），该库仅在收到 `EINVAL` 或 `ENOSYS` 时回退。 | 通用内核已实现子进程信号动作重置语义，原测例无需修改。 |
| 使用 `getpwnam("nobody")` 的测例 | `access01`、`mkdir02`、`readlink01`、`pidfd_send_signal02`、`pidfd_getfd02`、`mlock02`、`mlock202`、`shmctl02`、`mount03`、`umount03`、`mq_open01`、`mq_unlink01` 依赖原盘账户数据库。缺少 `/etc/passwd` 时，准备阶段在目标操作前结束。 | 保留原盘。验证数字身份相关权限时，使用明确创建测试账户的独立运行环境。 |
| glibc 的 `float_*` | 线程取消或退出时，glibc 需要 `libgcc_s.so.1` 展开线程栈。缺少这一库时可在日志中看到加载失败。 | 此发布输入的五个浮点程序按确切名称跳过。两架构的 musl 变体也均无可计分摘要。 |
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
  `EINTR`。BoarOS 和固定 Linux 的调用轨迹显示，`rt_sigtimedwait` 已返回 `EINTR`
  且信号处理函数已执行，原 musl 随后重新调用 `rt_sigtimedwait`。两架构的原 musl 库都包含此重试。
  空集合无法取得匹配信号，原程序等到内部监督到期。检查内核的中断与消费语义时，
  应直接调用 `rt_sigtimedwait`，并分别测试匹配信号和空集合。
- **`mmap04`。** 固定 RISC-V Linux 为 `PROT_WRITE` 的映射添加 `VM_READ`，
  所以 `/proc/<pid>/maps` 显示 `rw-`，原测例要求 `-w-`。LoongArch 保留请求的逻辑权限，
  只写映射显示 `-w-`，处理器的有效访问规则仍允许读。两架构应分别比较映射显示和实际访问。
- **RISC-V musl 的 `mount07`。** 启用 `MS_NOSYMFOLLOW` 后，原库的 `realpath()`
  返回 `ELOOP`。同一发布程序和库在固定 Linux 上也返回这个错误。直接打开链接报
  `ELOOP`，`readlink()` 可读链接目标，`statfs()` 报告挂载属性，这三项均通过。
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
配套控制器仍能启动它们。兼容分支现行跳过表排除了 `fs_fill`，填盘与卸载使用独立副本验证，见
[填盘及耗尽空间的验证](#fs_fill-的空间预留修复)。

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

[正式跳过表](../../tests/oscomp/ltp-skips.tsv)按程序名应用于两架构、两种 C 标准库。
评测平台按通过断言的原始分数加总，虚拟机的整轮预算为 7200 秒。单项监督在启动测例时单独计时。
遇到截断的串口日志时，使用组结束标记、单项完成记录和原判分区分零分与未运行。
各次执行的逐项结果保存在本地 `build/`。

- **控制器与输入。** 正式脚本逐项执行目录中的程序。跳过表排除无参数单独执行时缺控制器、
  结构化输入或配套服务的辅助程序。其配套测试仍可按原方式启动这些程序。
- **有通过结果但无可计分摘要。** 已核实的 19 个程序在四个配置下均通过断言且退出为零，
  但无原判分所需的摘要。测例名称和源文件见跳过表。`epoll-ltp` 使用旧式汇总，
  正式流程仍运行它。其摘要格式和断言分别判读。
- **发布程序缺依赖。** 跳过 `io_setup01` 和五个浮点程序 `float_bessel`、
  `float_exp_log`、`float_iperb`、`float_power`、`float_trigo`。对应构建条件和运行库见上表。
- **耗时长且得分低。** `dio_append`、`dio_sparse`、`inotify09`、`mmap1`、
  `mmap3`、`starvation`、`cve-2016-7117` 至少有一组耗时达到 60 秒，
  四组的最高观察分均不超过 2 分。`fs_fill` 使用独立盘检查写满、同步与卸载。
  `shm_test` 的默认压力场景无内部上限、无计分摘要，可使用独立短例验证共享内存。
- **准备阶段失败。** 文件通知测例先核对格式化程序、请求的文件系统类型、挂载和监视后端。
  只有到达事件断言的结果才能用于判断通知本身。配额测例也可能缺 `quotacheck`。
  `io_uring01/02` 和部分模块测例
  先检查 Linux 内核配置，判分为零时需核对是否执行了系统调用。
- **原生入口和发布头文件。** 原 RISC-V `futex_waitv01/02/03` 的头文件将调用号设为
  无效值 `-1`。LoongArch 的 Linux 原生接口没有 `getrlimit03` 所要求的旧入口。
  旧的 `socketcall`、`readdir` 和 16 位用户标识符测例也需要查实际架构接口和构建头文件。

### 现行测例判读

- `fanotify03` 检查监视进程能否允许或拒绝目标进程的打开、读取和执行。
  `fanotify10` 同时检查事件过滤和 `/proc/self/fdinfo/<fd>` 的忽略标记记录。
  旧式忽略掩码不能携带目录及子项过滤语义，对应三种配置在固定 Linux 也跳过。
  `fanotify16` 检查目录句柄、名称、目标句柄及改名前后记录。通知语义见
  [文件模块](../modules/kernel-files.md#fanotify-文件访问通知)。
- `fanotify23` 明确格式化并挂载 ext2，回收缓存后检查可回收标记是否撤下，并核对普通标记仍然有效。
  `accept03`、`readahead01`、`splice07` 则将 `fsopen("ext2", 0)` 返回的配置描述符
  作为输入检查错误语义。其他描述符类型的创建如果在准备阶段返回 `ENOSYS`，相应组合会跳过，
  不计为这些系统调用的断言失败。

- `fanotify13` 按变体格式化并挂载测试盘。ext4 基础变体检查索引节点、挂载、文件系统
  三种标记在打开和关闭后交付的事件，并把文件系统标识和文件句柄与 `statfs`、
  `name_to_handle_at` 对照。两架构、两种 C 标准库的原程序在 ext4 上分别通过 45 项断言。
  基础变体通过 15 项，下层与上层监视各通过 10 项，监视叠加视图中的下层与上层对象各通过
  5 项。与固定 Linux 一致，监视下层或上层时各有两项因底层挂载标记不可见而按配置跳过。
  直接监视叠加视图时各有四项因当前配置只提供文件标识编码而按配置跳过。通知实现见
  [文件模块](../modules/kernel-files.md#fanotify-文件访问通知)。
- `clock_getres01`、`timer_delete01`、`timer_settime01/02` 的闹钟分支依赖运行环境提供
  可用的实时时钟（Real-Time Clock，RTC）。比较结果时，应区分启动累计时钟、国际原子时
  （International Atomic Time，TAI），以及只有正确探测实时时钟才启用的闹钟查询和
  POSIX 定时器。定时器文件描述符的闹钟权限检查另有直接消费者覆盖。
- `fallocate05` 先预分配，再写满测试盘，最后验证写入预分配区、耗尽空间后的继续分配、
  打洞及打洞后的写入。超时样本在 `tst_fill_fs` 中仍持续写入，后续断言尚未执行。
  顺利完成填盘的配置通过四项断言。定位此类超时应先记录格式化、填盘完成量及测试自己的限时，
  再看空间不足后的操作是否实际运行。

- `fallocate04` 可以验证预分配、打洞和 `FALLOC_FL_ZERO_RANGE`。其后的
  `FALLOC_FL_COLLAPSE_RANGE` 要求移除文件区间并前移后续数据，当前返回
  `EOPNOTSUPP`，原程序按配置跳过。
- `statx10/11` 分别查询普通文件和块设备的直接输入输出（Direct I/O）对齐要求。
  `statx12` 检查的是挂载视图的根目录属性。发布程序的对齐字段见下文。
- `readahead02` 依赖进程读写统计、缓存控制及设备预读参数。`msync04` 从
  `/proc/self/pagemap` 得到页帧编号，通过 `/proc/kpageflags` 观察写入共享映射后的
  脏页及同步后的状态。`msync03` 的锁页断言可以通过，发布程序无可计分摘要。
- `accept03`、`readahead01` 和 `splice07` 依赖多类描述符构造器。软件中央处理器
  （Central Processing Unit，CPU）时钟计数器现可通过原 `perf_event_open` 构造，
  恢复了前两项各一条断言和 `splice07` 的 41 条输入／输出组合断言。剩余跳过来自
  `userfaultfd`、`io_uring_setup`、`memfd_secret` 或构造器给 `fsopen` 传入的
  文件系统类型。构造失败的分支尚未进入目标系统调用。

#### 文件句柄测例的标准输入假设

`open_by_handle_at02` 的 `stale-dfd` 使用文件描述符 0 选择挂载，期待它位于测试文件所在卷之外。
初赛启动脚本从根盘打开 `/dev/console` 作为标准输入，测例在同一 ext4 根盘的 `/tmp` 创建文件。
因此描述符 0 仍选中包含目标文件的实例，原句柄能打开文件。原程序在此环境中通过另外六项。
固定 Linux 启用了设备临时文件系统（device temporary filesystem，devtmpfs）的自动挂载，
`/dev/console` 位于另一实例。在本地给同一发布程序传入进程文件系统（process filesystem，
procfs）中的普通文件作为标准输入后，BoarOS 在该项也返回 `ESTALE`，原程序通过七项。
判读该项时应核对描述符 0 实际选择的挂载。正式启动脚本和发布程序保留原来的输入方式。

### 挂载相关的三类故障

初赛发布根盘带有 `/musl/busybox` 和 `/glibc/busybox`。BoarOS 内核挂载根文件系统后，
按兼容分支生成的启动配置，加载 `/musl/busybox` 并以 `sh -c` 运行启动脚本，作为用户态的
第一个进程。BusyBox 将多条命令合在一个可执行文件里，调用名为 `sh` 时解释脚本，调用名为
`stat` 或 `df` 时执行对应的工具。启动脚本用 `busybox --install -s /bin` 创建这些命令的
符号链接。通常执行 `df01.sh` 时，内核处理文件第一行的 `#!/bin/sh` 并启动解释器。LoongArch 的兼容入口显式选择 Dash。解释器按 `PATH` 查找并运行 `stat`、`df`。命令自己的代码解析选项、查询文件系统并格式化输出，底层
查询与挂载通过系统调用交给内核。发布盘的 `stat`、`df` 与脚本所使用的 GNU Coreutils
命令具有不同的选项集合；版本更新也需要核对实际应用程序及其构建配置。

**`df01.sh` 的工具选项及挂载残留。** RISC-V 原脚本挂载测试分区后，用 GNU 形式的
`stat -f ... --printf=%b` 等命令读取总块数和空闲块数，并用这些值核对 `df` 的输出。原盘
BusyBox 的 `stat` 只支持 `-c`，命令替换得到空字符串，脚本在算术表达式处退出并遗留挂载。
只替换 `stat` 时，BusyBox `df` 还缺少 `-t/-v/-H/--version/-x`。LoongArch 的原 Shell
在进入断言前还无法正确传递外层局部变量 `sec`，导致测例的超时准备失败。正式流程从只读
工具分区提供 GNU `stat`、`df`，只对这一测例更改命令搜索路径，LoongArch 由 Dash 解释原
脚本。原脚本先卸载测试分区。它随后对外部提供的分区发出释放请求，兼容入口按设备号检查挂载
表后结束这次租用，实际回环设备仍由上游工具执行解除绑定。

**`umount03` 的账户准备失败。** LoongArch glibc 原程序约 10.7 秒退出，状态为 2、得 0 分。它先以特权用户格式化并挂载测试盘，随后用 C 库的 `getpwnam("nobody")` 查找约定的低权限账户。该用户态查询从根盘的账户数据库取得账户名对应的数字用户和组标识，通常由 `/etc/passwd` 提供。两个架构的初赛原始根盘均缺少 `/etc/passwd`。测例查找返回 `ENOENT`（找不到账户），所以未调用 `seteuid` 切换有效用户标识，也未执行卸载权限断言。内核在 `seteuid` 和卸载时使用数字身份及进程的权限状态。在独立根盘副本加入 `nobody:x:65534:65534:nobody:/:/bin/false` 后，原程序按预期得到 `EPERM`（权限不足），原摘要为 1 个通过。正式流程由兼容分支的用户态启动脚本在可写根盘只补缺失项。原始发布镜像、反复格式化的 `/dev/vdb3` 和内核启动代码都无需写入该账户。

#### 初赛挂载位置

`tests/oscomp/init.sh` 负责初赛用户态启动，由 `tests/oscomp/prepare.py` 加入针对架构的
设备节点、加载器路径和工具分区挂载命令后生成启动配置。内核先挂载根盘，再执行其中的
`/musl/busybox sh -c`。启动脚本挂载 `/proc`、`/sys` 和 `/dev/shm`，工具分区
`/dev/vdb1` 只读挂到 `/boaros-tools`。测例使用的 `/dev/vdb3` 由原 LTP 程序在各自
准备阶段格式化和挂载。启动配置只在运行 LTP 时向可写根盘补缺失的账户。见
[初赛启动与工具分区](../modules/oscomp-compat.md#原盘bootstrap与监督)。

**`umount2_02` 的两步卸载。** 旧内核拒绝 `MNT_EXPIRE`，两个 glibc 配置只通过 2 项 `UMOUNT_NOFOLLOW` 断言。原测例继续执行后会留下叠放的挂载。通用内核接入过期标记后，LoongArch 和 RISC-V 的 glibc 原程序均通过 7 项，退出状态均为 0。短例在同一 RISC-V 可执行文件中对照 BoarOS 与 Linux，额外验证了忙引用、普通访问清除标记及绑定视图之间的隔离。

### 两个使用内部超时的测例

- **`send02`。** 四组都停在首个传输控制协议（Transmission Control Protocol，TCP）子项，未进入用户数据报协议（User Datagram Protocol，UDP）子项。原程序每轮先发 16 字节，再用 `MSG_MORE` 暂留 16 字节，最后用 1 字节结束本批发送。它用非阻塞接收反复检查最后 17 字节。RISC-V 相同可执行文件的短对照中，BoarOS 在 10.007 秒完成 80 轮，9.943 秒花在等最后 17 字节；Linux 在 253 毫秒完成 1000 轮。通过 `TCP_INFO` 核对，BoarOS 最后一次发送返回时，前一段 16 字节仍待确认。Nagle 算法继续暂缓后续短报文，到对端延迟确认或原缓冲期限触发才推进。关闭 Nagle 的对照消除了这段等待，随后仍出现一次提前收到 `MSG_MORE` 数据，不能作为兼容适配。通用内核现在对首个有序数据段及时安排确认，后续数据保留延迟确认。控制确认可以独立发送，输入和定时器不会提前释放应用指定的短尾。同一套接字的数据报发送也支持 `MSG_MORE`，最后一次无标志发送提交完整数据报。
- **`statx11`。** 超时在准备阶段。`setup` 先查 `/sys/block/vdb3/queue/logical_block_size`，找不到便逐字符截短设备名，预期能查到整盘 `vdb`。Linux 通常经 `/sys/block/vdb/queue/` 提供整盘队列的属性，分区目录没有独立的 `queue`。发布程序没有空名称终止条件，耗尽字符后还有越界写入，四组均未执行 `statx`。

初赛兼容脚本此前只挂载进程文件系统（process filesystem，procfs），现在也把系统文件系统
（system filesystem，sysfs）挂到 `/sys`。初赛仍由用户态以 `mknod` 创建设备节点，Linux
也可通过设备临时文件系统（device temporary filesystem，devtmpfs）提供它们。设备节点
`/dev/vdb3` 用于打开分区，整盘队列属性由 `/sys/block/vdb/queue/` 提供，两处属于不同接口。

使用官方容器中的 QEMU 及初赛原程序验证时，两架构、两种库的 `df01.sh` 均由原摘要报告
12 项通过、0 警告，并可正常复用测试分区。`send02` 均通过 TCP 和三种 UDP 发送场景，
`umount03` 均到达卸载权限断言并通过。

#### `statx11` 的发布程序缺少对齐断言

BoarOS 从实际块设备导出 `logical_block_size` 和 `dma_alignment`，并在 `statx` 中通过
`STATX_DIOALIGN` 报告直接输入输出（direct I/O）的对齐要求。初赛发布程序中，LoongArch glibc
通过两个对齐断言。其余三组在 `statx` 成功且返回掩码含 `STATX_DIOALIGN` 后报告配置跳过
（`TCONF`）。提示字符串写死了 glibc，两种 C 标准库的构建都可能显示它。

固定的 [LTP 源码](https://github.com/oscomp/testsuits-for-oskernel/blob/4b40ee92e8b594fb20e3957ebc2f79ad43aca538/ltp-full-20240524/testcases/kernel/syscalls/statx/statx11.c) 用 `HAVE_STRUCT_STATX_STX_DIO_MEM_ALIGN` 控制是否编译两个断言。
`configure.ac` 在启用 `_GNU_SOURCE` 后经目标工具链的 `<sys/stat.h>` 探测该成员。发布盘程序的
调试信息保留了编译时实际使用的 `struct statx`，用 `readelf --debug-dump=info` 和
`readelf --debug-dump=rawline` 可以核对成员和声明来源。

| 发布程序 | `struct statx` 的声明来源及对齐字段 |
| --- | --- |
| RISC-V glibc | 目标工具链的 `<linux/stat.h>`。已有 `stx_mnt_id`，两个对齐字段所在位置仍是保留成员 `__spare2`。 |
| RISC-V musl | LTP 的 `include/lapi/stat.h` 提供兼容定义，表示配置阶段未在 `<sys/stat.h>` 找到完整定义。两个对齐字段所在位置纳入 `__spare2[14]`。 |
| LoongArch musl | 目标工具链的 `<sys/stat.h>` 提供旧版定义，两个对齐字段所在位置纳入 `__pad1[14]`。 |
| LoongArch glibc | 目标工具链的 `<linux/stat.h>` 声明了 `stx_dio_mem_align` 和 `stx_dio_offset_align`。 |

这三组发布程序在交叉编译时就已选入配置跳过分支。初赛的
[LTP 构建脚本](https://github.com/oscomp/testsuits-for-oskernel/blob/4b40ee92e8b594fb20e3957ebc2f79ad43aca538/ltp-full-20240524/build-oscomp.sh)
按架构和库分别执行 `configure`，随后[制盘配方](https://github.com/oscomp/testsuits-for-oskernel/blob/4b40ee92e8b594fb20e3957ebc2f79ad43aca538/Makefile)
将编译好的程序复制进 ext4 镜像。发布方如需恢复断言，应让这三组构建使用可经 `<sys/stat.h>`
见到两个字段的目标头文件，再重新配置、编译和制盘。

本地用原 `statx11.c`、带对齐字段的头文件重新配置并编译这三组。在独立运行盘上查询
`/dev/vdb3`，三组均通过原来的两项断言，失败和跳过均为零。`stx_dio_mem_align = 512`
字节，对应整盘的 `dma_alignment = 511` 掩码。`stx_dio_offset_align = 512` 字节，与整盘的
`logical_block_size = 512` 一致。重编程序仅用于本地验证，正式入口仍运行原发布程序。

### `fs_fill` 的空间预留修复

原程序先让三个写入线程用 `writev` 写至 `ENOSPC`，再用不同大小的文件反复填盘。第二阶段遇到空间不足时，`lib/tst_fill_fs.c` 先调用 `fsync` 提交成功接纳的写入，然后缩小请求并继续。LoongArch glibc 在干净测试盘上单独复现了 `fsync` 返回 `ENOSPC`，随后的 `umount` 返回 `EIO`。部分运行还在用户态报出堆损坏信息，这条日志出现在首个同步错误之后，来源需要单独验证。

诊断将首个同步错误定位到 ext4 延迟分配的区段树扩展。写入接纳为文件数据预留块。回写分配完当前数据块后，区段树需要额外申请一个元数据块。诊断记录到超级块还有 27839 个空闲块，但扣除已有预留及当前事务剩余额度后，可用块数为 0。区段树的申请返回 `ENOSPC`，错误经 `ext4_write_spans` 回到页缓存写回及 `fsync`。卸载跟踪确认，页缓存清理前重试写回，写回失败返回缓存输入输出错误，ext4 将该状态转换为 `EIO`。失败盘的只读检查跳过了日志重放，现场不足以据此判断掉电恢复或已接纳数据的完整性。通用内核已在接纳空洞写入时同时预留数据与区段树元数据。写回按类型扣减，事务回滚恢复原有额度，成功后退还未使用的部分。额度紧张时先推进同一文件系统的写回再重查，避免因临时的元数据预留而过早终止追加。独立的 LoongArch glibc 原程序完成两轮填盘，退出状态为 0，摘要 2 项通过，正常卸载；宿主短例在可用额度为零时完成扩树和同步，覆盖事务取消后重试、打洞、截断和卸载。
