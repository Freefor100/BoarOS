# OSComp 测例与环境问题

我按程序启动、测例准备、接口调用和原判分四个边界定位异常。先确认程序走到了哪一步，
再检查该步实际使用的可执行文件、运行库、参数和返回值。必要时在 Linux 上运行同一
可执行文件和运行库，区分发布输入、内核行为与判分问题。

旧盘为 pre-20250615，其中 Linux 测试项目（Linux Test Project，LTP）版本为 20240524。
固定输入见 [旧盘清单](../../tests/oscomp/inputs.json) 和
[final-2026 清单](../../tests/oscomp/inputs-final-2026.json)。
现行启动和监督方式见[评测环境适配](../modules/oscomp-compat.md)。逐次执行的串口、
耗时、原判分和输入身份保存在 `build/`。

## 测例启动与准备

| 项目 | 如何判断原因 | 处理 |
|---|---|---|
| `acl1` | 原发布文件仅包含可选访问控制列表（Access Control List，ACL）依赖不可用时的提示分支。固定 LTP 源码在未定义 `HAVE_LIBACL` 时构建此分支，程序输出 `The acl library was missing upon compilation.`，退出 32。 | 保留原文件。要执行完整测试，需用 libacl 重新构建，并提供文件、目录及指定的数字用户和组。原盘直接无参执行也无法满足这些前提。完整测试打印 `ACL TESTS RESULTS`，判读应结合实际输出。见[固定源码](https://github.com/linux-test-project/ltp/blob/20240524/testcases/network/nfsv4/acl/acl1.c)。 |
| `setxattr01` 在 LoongArch glibc 上提示缺少 `mkfs.ext4` | 同环境直接启动 `mkfs.ext4` 成功，但 `system("exit 0")` 也返回 127。跟踪 glibc 的 `posix_spawn` 可见其 `clone3` 请求包含 `CLONE_CLEAR_SIGHAND`，旧内核返回 `EOPNOTSUPP`（95），该库仅在收到 `EINVAL` 或 `ENOSYS` 时回退。 | 通用内核已实现子进程信号动作重置语义，原测例无需修改。 |
| 使用 `getpwnam("nobody")` 的测例 | `access01`、`mkdir02`、`readlink01`、`pidfd_send_signal02`、`pidfd_getfd02`、`mlock02`、`mlock202`、`shmctl02`、`mount03`、`mq_open01`、`mq_unlink01` 依赖原盘账户数据库。缺少 `/etc/passwd` 时，准备阶段在目标操作前结束。 | 保留原盘。验证数字身份相关权限时，使用明确创建测试账户的独立运行环境。 |
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
配套控制器仍能启动它们。`fs_fill` 按整轮评测预算排除，该负载执行实际填盘、空间耗尽和
清理。

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
