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

### musl 信号等待：库重试 EINTR，测例等待它返回 EINTR

`sigtimedwait01`、`sigwaitinfo01`的[首项](https://github.com/linux-test-project/ltp/blob/20240524/libs/libltpsigwait/sigwait.c#L12)使用空等待集合，要求SIGUSR1打断调用并返回EINTR。原musl库收到syscall的`-EINTR`后会重新调用；空集合没有可成功取走的匹配信号，于是反复等待，最后由LTP的30秒watchdog结束。

原RV库标注musl1.2.0、LA库为1.2.5，两份原库反汇编均确认该重试。固定Linux使用同一原ELF和原libc，两个架构均得到：glibc分别11/11、9/9通过，musl各30秒后以0通过、1个TBROK（运行超时）结束。BoarOS的直接syscall对照覆盖空集合与SA_RESTART，返回EINTR的行为与Linux相同。

当前沿用原输入和原judge计分。调整这一差异应落在测试调用方式或显式libc接口适配；原结果、运行身份与短回归见[CPU定时器与信号等待](oscomp-official-delivery.md#cpu-间隔定时器与信号等待)。

## 原脚本调用与判分差异

| 项目 | 具体问题与影响 | 当前处理 |
|---|---|---|
| iozone 向量 I/O | 原命令 `./iozone -t 4 -i 11 -i 12 -r 1k -s 1m` 请求 pwritev/preadv，发布二进制却报 `Selected test not available on the version`，随后输出默认 initial writers/rewriters。 | 保留原命令；分析按实际输出判断。该次运行只证明默认写入路径完成，向量能力由独立 ABI 和原 preadv/pwritev 测例验证。 |
| libc-test 脚本 | `run-static.sh`、`run-dynamic.sh` 没有 shebang；原 LA musl BusyBox 在固定 Linux 也拒绝直接执行，后面的程序尚未开始。 | 临时外层脚本用该 libc 自己的 BusyBox `sh` 显式解释这两行；内层列表和参数原样执行。 |
| BusyBox 的 `kill 10` | judge 的固定清单要求此标签，发布 `busybox_cmd.txt` 实际执行 `sh -c 'sleep 5' & ./busybox kill $!`，成功标签按整行输出。judge 另补的 `kill 10` 因无对应标签得零。 | 保留原脚本和 judge；**54/55** 的差额按标签错位解释，后台 PID 的实际 kill 单列。 |
| LTP 旧 API 输出 | `ftest01/05`、`inode02`、`hackbench` 等会输出 TPASS，部分正常退出，但没有现代 `Summary:`；原 judge 只从 Summary 累计 passed，记 **0/0**。 | 结果中同时列功能输出和原判分。两架构 ftest 的实际内容与磁盘一致性已通过。 |
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
| RV 原静态 glibc 的 clock 测试 | malloc 初始化遇 getrandom/EAGAIN 后把 errno 留到 main；后续 clock 调用成功，原断言仍看到旧 errno。 | 当前启动种子在运行测例前完成随机源初始化；指令级对照见[errno 来源](oscomp-compat-baseline.md#rv静态clock_gettime的errno来源)。 |
| RV 原 musl 的浮点 ABI 标注 | 原测例标注double-float ABI，原libc标注soft-float；设置`LTP_TIMEOUT_MUL=0.1`时观察到异常巨大时限。 | 信号等待的正式复核沿用未设置该变量的默认30秒；该原输入差异独立于EINTR重试，保留在定向运行记录。 |
| libc-test 的部分共同失败 | 固定 Linux 上同一原 ELF 也失败，例如缺 UTF-8 locale、glibc 的 ERE 扩展与原断言预期不同。 | 逐项 Linux/BoarOS 输出见[对照附件](oscomp-libctest-original-failures.tsv)及[具体触发条件](oscomp-compat-baseline.md#rv静态clock_gettime的errno来源)。 |

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
O_DIRECT、普通 AF_UNIX socket、CPU-time sleep 与 POSIX timer 的缺口按内核能力处理；对应实际失败和
已修复语义见[原程序结果](oscomp-official-delivery.md)。
