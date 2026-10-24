# OSComp 评测输入、调用与测例问题

本文集中记录固定发布盘中的程序问题、调用方式、版本预期和判分差异，以及当前处理。
输入版本、下载地址与校验集中在 [inputs.json](../../tests/oscomp/inputs.json)；当前输入为
pre-20250615，盘内 LTP 报告版本为 20240524。运行和交付方式见
[兼容模块](../modules/oscomp-compat.md)，实际得分见[官方交付结果](oscomp-official-delivery.md)。

## 文档入口

以下五份正文只在 `oscomp-compat` 维护：

| 文档 | 阅读用途 |
|---|---|
| [双架构原评测运行与评分](../modules/oscomp-compat.md) | 构建、官方双盘、启动、工具、运行时适配和监督入口 |
| [输入、失败与证据](oscomp-compat-baseline.md) | 原盘、运行库、Linux 对照及历次完整运行的具体归因 |
| [完整评测的得分、耗时与 LTP 能力分析](oscomp-ltp-analysis.md) | 完整组成本、LTP 细项、存储修复、lmbench 和预算取舍 |
| [官方双架构交付与原程序结果](oscomp-official-delivery.md) | 固定官方容器、原 QEMU 参数下的定向得分、耗时与磁盘检查 |
| 本文 | 测例、发布程序、调用和判分问题的集中索引及处理依据 |

兼容分支另有四份数据附件：`oscomp-dual-official-results.tsv`、
`oscomp-rv-five-results.tsv`、`oscomp-libctest-original-failures.tsv` 和
`data-path-controlled-delay.tsv`。正文负责解释结论，附件保留数值。

## 发布程序与版本预期

### basic/brk：发布 ELF 截断了返回地址

raw `brk(0)` 返回当前堆顶地址。原 RV ELF 在 syscall 后执行 `sext.w a0,a0`，
LA ELF 执行 `slli.w a0,a0,0`，都只保留低 32 位再符号扩展。随后程序拿这个截断地址
加 64 请求扩展，已经偏离真实堆顶，两个增长断言失败。因此原四配置均为 **1/3**。

固定源码的包装器声明与发布 ELF 的这段指令不同，归因以实际运行字节为准。
此前独立修正诊断已验证恢复完整返回宽度后为 3/3；默认评测保留原 ELF 和原分数。
两架构原指令、地址观测和输入哈希见
[basic 与运行时差异](oscomp-compat-baseline.md#basic的90分与原musl调度接口差异)。

### epoll_ctl04：旧测例只接受 EINVAL，新嵌套检查先返回 ELOOP

测例先建立五层 epoll，再添加第六层。LTP 20240524 只接受 `EINVAL=22`。
Linux 的 `f2e467a48287` 提前检查向上的嵌套深度，从 6.17-rc 起该输入先得到
`ELOOP=40`。LTP 上游随后提交 `e84f0689`，同时接受 EINVAL、ELOOP；该修正于
2025-08-14 合入。

当前 BoarOS 与固定 Linux 对照均为：

| 输入 | 可见结果 |
|---|---|
| 五层嵌套 | 成功 |
| 增加第六层 | ELOOP |
| 自己监听自己 | EINVAL |
| 多个 epoll 构成环 | ELOOP |

默认继续执行原 `epoll_ctl04`，保留其 TFAIL 和 **0/1** 判分；内核保留 ELOOP。
共同 ABI 回归在 [selected.c](../../tests/diff-abi/selected.c) 的 `epoll_nesting()`。
依据：[旧测例](https://github.com/linux-test-project/ltp/blob/20240524/testcases/kernel/syscalls/epoll_ctl/epoll_ctl04.c)、
[LTP 修正](https://github.com/linux-test-project/ltp/commit/e84f0689cf7a8a77478a0e70aa62560f66c3bceb)、
[Linux 修改](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/commit/?id=f2e467a48287c868818085aa35389a224d226732)。

### LA 原 musl mremap06：4 KiB 拆分粒度与 16 KiB 页不匹配

原ELF把 `MMAP_GRANULARITY` 编译为4096，第一次请求搬移 `base+4096` 到 `base+12288`。当前LA使用16384字节页，两地址均未页对齐。相同原ELF、原libc在固定16KiB Linux上也在首次mremap报TBROK；实际调用参数与反汇编一致。按两端参数检查，该请求属于EINVAL；原程序未打印errno，调查未捕获raw返回寄存器。

对应LTP头在有 `HAVE_SYS_SHM_H` 时选 `SHMLBA`，否则用 `getpagesize()`；固定musl 1.2.5通用头的SHMLBA为4096。原ELF的常量与此前者一致，官方构建config.h未取得，因此归因以产物与实测参数为准。原libc的mremap正确传递第五参数，getpagesize从运行时数据取值，本次没有证明它返回错误。

保留原输入、对齐语义和原judge的0/1；其余三个组合为3/3。具体得分和耗时见[mremap结果](oscomp-ltp-analysis.md#mremap-常用范围与线程-proc-查询)，原ELF、同输入Linux输出和参数见`build/mremap-proc-20261023/la-original-mremap06.md`。[固定头文件](https://github.com/linux-test-project/ltp/blob/20240524/include/lapi/mmap.h)

### mremap05：内容错误分支仍打印 TPASS

固定20240524源码的 `test_mremap()` 在成功返回目标地址后检查首字节；内容不符时仍调用 `tst_resm(TPASS, ... "value failed")`。因此其7条TPASS和退出0并不独立证明搬移内容正确。当前保留原ELF和judge，内容、源/目标子范围及共享/COW行为由[共同ABI短例](../../tests/diff-abi/mremap.c)实际检查。[固定源码](https://github.com/linux-test-project/ltp/blob/20240524/testcases/kernel/syscalls/mremap/mremap05.c)

### musl 信号等待：库重试 EINTR，测例等待它返回 EINTR

`sigtimedwait01`、`sigwaitinfo01`的[首项](https://github.com/linux-test-project/ltp/blob/20240524/libs/libltpsigwait/sigwait.c#L12)使用空等待集合，要求SIGUSR1打断调用并返回EINTR。原musl库收到syscall的`-EINTR`后会重新调用；空集合没有可成功取走的匹配信号，于是反复等待，最后由LTP的30秒watchdog结束。

原RV库标注musl1.2.0、LA库为1.2.5，两份原库反汇编均确认该重试。固定Linux使用同一原ELF和原libc，两个架构均得到：glibc分别11/11、9/9通过，musl各30秒后以0通过、1个TBROK（运行超时）结束。BoarOS的直接syscall对照覆盖空集合与SA_RESTART，返回EINTR的行为与Linux相同。

当前沿用原输入和原judge计分。调整这一差异应落在测试调用方式或显式libc接口适配；原结果、运行身份与短回归见[CPU定时器与信号等待](oscomp-official-delivery.md#cpu-间隔定时器与信号等待)。

## 原脚本调用与判分差异

| 项目 | 具体问题与影响 | 当前处理 |
|---|---|---|
| 六项 socket LTP 的临时复核入口 | 10月24日10:36的临时包装只生成了选择性 init，随后直接复制默认交付内核，漏掉用该 init 重建内核的步骤。保留的 `probe-rv/init.json`、`probe-la/init.json` 均只执行六项 LTP；实际内核仍内嵌默认组脚本，因此先进入 basic、busybox、cyclictest、iozone。此前关于过滤锚点失效的归因有误。 | 在 LTP 前停止两个客体；该尝试没有 LTP 结果、得分或超时记录，临时运行盘已删除。此前10:21按选择性 init 重建后取得的原程序结果有效。选择性运行须把生成的 init 传给内核构建，并核对实际启动内核身份；只生成 JSON 不改变已编译内核。记录见 `build/unix-named-20261024/original-refinement/aborted.json`。 |
| iozone 向量 I/O | 原命令 `./iozone -t 4 -i 11 -i 12 -r 1k -s 1m` 请求 pwritev/preadv，发布二进制却报 `Selected test not available on the version`，随后输出默认 initial writers/rewriters。 | 保留原命令；分析按实际输出判断。该次运行只证明默认写入路径完成，向量能力由独立 ABI 和原 preadv/pwritev 测例验证。 |
| libc-test 脚本 | `run-static.sh`、`run-dynamic.sh` 没有 shebang；原 LA musl BusyBox 在固定 Linux 也拒绝直接执行，后面的程序尚未开始。 | 临时外层脚本用该 libc 自己的 BusyBox `sh` 显式解释这两行；内层列表和参数原样执行。 |
| BusyBox 的 `kill 10` | judge 的固定清单要求此标签，发布 `busybox_cmd.txt` 实际执行 `sh -c 'sleep 5' & ./busybox kill $!`，成功标签按整行输出。judge 另补的 `kill 10` 因无对应标签得零。 | 保留原脚本和 judge；**54/55** 的差额按标签错位解释，后台 PID 的实际 kill 单列。 |
| LTP 旧 API 输出 | `ftest01/05`、`inode02`、`hackbench`、`mremap01～05`、`getgroups01`、`sigaltstack01`、`signalfd01/signalfd4_01/signalfd4_02`、`sendmsg01`等会输出TPASS，部分正常退出，但没有现代 `Summary:`；原 judge 只从 Summary 累计 passed，记 **0/0**。 | 结果中同时列功能输出和原判分。`getgroups01` 四配置均输出4条TPASS、退出0，原judge各记0/0；两架构ftest的实际内容与磁盘一致性已通过。`sendmsg01` 的glibc正常完成和musl中止均记0/0、0分，逐配置输出及耗时见[控制消息原程序结果](oscomp-ltp-analysis.md#unix-控制消息与-inet-peek-原程序结果)。 |
| LTP 的 `FAIL LTP CASE … : 0` | 原脚本对每个返回码统一打印这行，名字中的 FAIL 也出现在成功退出时。 | 判读末尾状态及 Summary；保留原结束行作为 judge 的当前测例边界。 |

iozone 的完整命令核对见[原脚本对照](oscomp-compat-baseline.md#2026-10-17-cpu0-存储分批与同步等待集成)。
其余脚本适配机制和重建入口见[兼容模块](../modules/oscomp-compat.md#原盘bootstrap与监督)。

## 为什么有些项目会一直等

原 `ltp_testcode.sh` 遍历 `ltp/testcases/bin/*`，对每个普通文件直接执行 `"$file"`，
没有使用 LTP 的 runtest 分组来建立控制器、管道、参数和配套服务。目录中也存放测试
辅助程序；这些程序的调用方负责提供输入或结束它们。

| 代表项目 | 正常调用所需条件 | 当前处理 |
|---|---|---|
| `cgroup_fj_proc`、`cpuctl_fj_cpu-hog`、`cpuset_*`、`memcg_test_2/4`、`pids_task2` | 父控制器通过信号、FIFO 或终止动作驱动；部分 worker 本来就持续施压。 | 共享跳过表过滤原脚本的直接无参数调用，控制器入口继续执行。 |
| `cpuhotplug_do_*_loop`、`fork_exec_loop`、`fs_racer_*.sh` | 热插拔、freezer 或 racer 控制器负责运行时间和结束。 | 同上。 |
| `doio` | `iogen` 提供结构化标准输入。 | 过滤单独执行。 |
| `shell_pipe01.sh`、`tst_hexdump`、`timed_forkbomb` | 分别需要上游管道、输入及 EOF、控制器启动令牌。 | 过滤单独执行。 |
| `pthserv`、`ftp-download-stress02-rmt.sh` | 对应客户端／服务端、持续时间与结束控制。 | 过滤单独执行。 |
| `netstress` | 默认作为 server，`-l` 才选择 client；`server_run()` 持续循环 accept，需要客户端产生负载、控制程序结束服务端。历史无参数运行等待约 300 秒，得零。 | 共享跳过表过滤独立无参数调用，原因是 `controller-client-and-termination-required`；配对网络测试继续由其控制程序运行。 |

逐名来源和原因以 [ltp-skips.tsv](../../tests/oscomp/ltp-skips.tsv) 为准。
netstress 的默认模式见[固定源码](https://github.com/linux-test-project/ltp/blob/20240524/testcases/network/netstress/netstress.c)。
跳过表在执行前匹配名字；300 秒监督则结束已经启动的程序，两者作用时点不同。

## 运行库和原盘依赖

| 项目 | 已确认归因 | 当前状态／具体依据 |
|---|---|---|
| glibc `float_*` 的线程退出／取消 | 完整旧日志已有 `libgcc_s.so.1` 缺失；glibc 需要它展开线程栈，缺库会 abort。 | 属于原盘依赖。musl 的 SIGSEGV 单独保留归因；见[慢项分类](oscomp-ltp-analysis.md#慢项与超时分类)。 |
| LA musl 的四个 `sched_*` 函数 | 原 libc 直接返回 ENOSYS，尚未进入内核；对应上游 musl 的接口语义取舍。 | 默认 LA musl 通过调度 DSO 调用真实 Linux syscall，原 cyclictest 已获得样本；见[运行时接入](oscomp-compat-baseline.md#默认make-all接入la-linux调度运行时)。 |
| 原 musl `sendmsg01` 的非法控制指针 | RV 原 libc 的 `sendmsg` 包装器先复制 msghdr，再从 `msg_control` 复制控制区到栈，随后才调用 syscall。反汇编中 `0x435bc` 读取控制指针、`0x435d0` 调用 `memcpy`，到 `0x43640` 才装入 syscall 号211并于 `0x43644` 调用 `__syscall_cp`；因此非法控制指针可在进入 sendmsg 的 ecall 前触发用户态 SIGSEGV。 | RV／LA 原musl在该分项均收到SIGSEGV并中止，具体计数和耗时见[原程序结果](oscomp-ltp-analysis.md#unix-控制消息与-inet-peek-原程序结果)。当前指令级证据来自RV原库，保存在 `build/unix-scm-20261024/original-linux-rv-musl-sendmsg01/musl-sendmsg.asm`；固定Linux以同一原RV ELF和原libc复现14条TPASS，随后在第15项收到SIGSEGV、2条TBROK、退出2；运行盘检查为0。记录为 `pristine-serial.log`、`pristine-result.json`。LA本轮有同类原程序输出，未另作原LA库的指令级归因。保留原ELF、libc和原judge。 |
| RV 原静态 glibc 的 clock 测试 | malloc 初始化遇 getrandom/EAGAIN 后把 errno 留到 main；后续 clock 调用成功，原断言仍看到旧 errno。 | 当前启动种子在运行测例前完成随机源初始化；指令级对照见[errno 来源](oscomp-compat-baseline.md#rv静态clock_gettime的errno来源)。 |
| `mkdir02/readlink01/access01` 的用户数据库 | 固定输入根盘缺少 `/etc/passwd`，`getpwnam("nobody")` 返回ENOENT，准备阶段结束。 | 原`mkdir02/readlink01`未进入GID/SGID继承、root/nobody读取主体；10月24日`access01`四配置也在`access01.c:275`报TBROK，原judge各为0/1。真实UID/GID切换、capset与DAC现已实现，数字身份的同ELF短例和musl/glibc消费者已通过；原盘仍保持原账户输入。原程序得分与耗时见[凭据与权限结果](oscomp-ltp-analysis.md#可变凭据与权限原程序结果)。 |
| `eventfd01/02` 的Kconfig输入 | 原ELF请求`CONFIG_EVENTFD`，LTP在解析Linux `.config`时TBROK，退出2；当前启动环境没有该配置文件。 | 四配置均未进入主体，原judge为0/0。eventfd接口差分及原eventfd2测例通过；逐程序得分/耗时见[eventfd与timerfd结果](data-path-budget-experiments.md#eventfd与timerfd2026-10-22)。 |
| `getdents01` 的四种入口 | 旧 getdents syscall 在两种架构上无入口，部分 libc 也没有导出相应 wrapper；这些分项记 TCONF。 | 四配置共七条 TCONF；有效 getdents64 分项通过。对应分项、得分和时间见[目录与路径结果](oscomp-official-delivery.md#目录与路径当前结果)。 |
| RV 原 musl 的浮点 ABI 标注 | 原测例标注double-float ABI，原libc标注soft-float；设置`LTP_TIMEOUT_MUL=0.1`时观察到异常巨大时限。 | 信号等待的正式复核沿用未设置该变量的默认30秒；该原输入差异独立于EINTR重试，保留在定向运行记录。 |
| libc-test 的部分共同失败 | 固定 Linux 上同一原 ELF 也失败，例如缺 UTF-8 locale、glibc 的 ERE 扩展与原断言预期不同。 | 逐项 Linux/BoarOS 输出见[对照附件](oscomp-libctest-original-failures.tsv)及[具体触发条件](oscomp-compat-baseline.md#rv静态clock_gettime的errno来源)。 |

## 内核语义与错误分类

### socketpair01：创建阶段错误分类（已修复）

此前四个架构/libc组合的原 `socketpair01` 均为6/10、退出1：一项期望 EAFNOSUPPORT(97)、三项期望 EPROTONOSUPPORT(93)，实际均为 EOPNOTSUPP(95)。这是当时内核的错误分类缺口，[上轮得分、耗时和原始结果](oscomp-ltp-analysis.md#unix-seqpacket-接入后的-socket-原程序结果)继续保留。main 基线 `f0943f2` 已有相同的非 UNIX family 提前返回行为，源码说明它早于 SEQPACKET 接入；该历史运行没有另用旧核重跑。

当前 `kernel/syscall/socket.c` 已让 socket 和 socketpair 共用 family/type/protocol 创建校验，区分创建失败与不支持配对操作。固定 `references/linux/net/socket.c` 的 `__sys_socketpair` 先预留两个 FD、逐个写入用户输出，再创建 socket，最后调用协议的 `ops->socketpair`；当前文件表事务采用这一顺序，失败时归还预留 FD，并保留已经写入用户空间的输出前缀。不能创建的地址族或协议保留对应创建错误，能够创建而不支持配对的 INET TCP/UDP 请求仍返回 EOPNOTSUPP。

修复后保留同一批原 ELF、原断言和原 judge，四配置均为10/10、退出0，无 TFAIL、TBROK、TCONF 或超时，见[本轮分项得分与耗时](oscomp-ltp-analysis.md#sysv-信号量与-socketpair-错误分类修复后的原程序结果)。

### semctl03：libc 变体主动跳过两项 EFAULT

原 `semctl03` 在四配置均为6/8、退出0、两条 TCONF。固定源码的测试表把 GETALL、IPC_SET 的非法 union 参数列为 EFAULT；`verify_semctl()` 遇到 `tc->error == EFAULT && tv->semctl == libc_semctl` 时直接输出 `EFAULT is skipped for libc variant` 并返回，不执行该分项的 semctl 调用。libc 和原始 syscall 两种变体合计八项，因此 libc 变体的这两项记 TCONF，其余六项实际通过。

依据：[固定 semctl03 源码](https://github.com/oscomp/testsuits-for-oskernel/blob/8b58dd16d26d30f7c74d48d5832d870d3051b703/ltp-full-20240524/testcases/kernel/syscalls/ipc/semctl/semctl03.c)。本轮原输出与原 judge 的6/8继续保留，见[分项结果](oscomp-ltp-analysis.md#sysv-信号量与-socketpair-错误分类修复后的原程序结果)；这两项跳过由程序的上述条件直接触发，不据此推断环境或内核缺口，也不把独立故障检查的通过数补入原程序分数。

## fs_fill：评测预算取舍

`fs_fill` 的两个细项已经通过，相关磁盘检查正常。它以三名 writer 分别执行向量和
4KiB 小写入填盘；近期原 RV/glibc 为 270.703 秒，LA/musl 为 199.273 秒，各得
2 个原始分。历史每架构两种 libc 合计占 RV 约 578 秒、LA 约 444 秒。

经用户确认，默认评测在共享跳过表中加入 `fs_fill`，原因是 `evaluation-time-budget`，
覆盖 RV/LA 的 glibc/musl。仍保留完整原始 ELF，存储专项回归直接运行它。工作量、
校验和修复及测量见[fs_fill 成本分析](oscomp-ltp-analysis.md#fs_fill-的工作量内核成本与优化)。

当前表共36个名字：35个按控制器、输入或配套端点等调用条件过滤，1个 `fs_fill`
按预算跳过。两张固定盘每架构各匹配 glibc 36项、musl 33项。匹配时适配器返回125，
控制台原因取自对应条目的reason；原judge对跳过项记零。

当前内核能力与后续实现方向仍由 [goals](../goals.md) 和各模块维护。例如
O_DIRECT、普通命名AF_UNIX socket、CPU-time sleep等缺口按内核能力处理；POSIX timer已支持的四类时钟及其余时钟缺口见[原程序结果](oscomp-ltp-analysis.md#posix-定时器原程序四类时钟完成版)。对应实际失败和
已修复语义见[原程序结果](oscomp-official-delivery.md)。
