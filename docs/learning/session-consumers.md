# 会话与调度的真实消费者窄诊断

本诊断不计比赛分数，不修改原程序 ELF，也不据单个退出码宣称应用完整支持。
固定输入先经过 SHA-256 校验，再把同一 fixture 分别交给单 hart Linux 和 BoarOS。

## 重建

先按 `docs/modules/program-environment.md` 构建完整 BusyBox 环境，再运行：

```sh
python3 tests/session-consumer-diagnostics.py \
  --output build/session-consumers-check --kernel kernel-rv
```

输出目录必须不存在。可用 `--linux-kernel <Image>` 选已有固定 Linux 镜像，
runner 会验证其上级 `identity.json`；`--only linux` 或 `--only boaros` 可缩小重跑。
需要本机 QEMU 11 插件头、C 编译器、glib 开发文件、debugfs 和现有 RV64 musl 工具链。
这里的 guest driver 使用 `tests/workloads/diagnostics/session-consumers.c`，
QEMU observer 使用同目录 `syscall-entry-plugin.c`。

观察器仅记录 `priv == U` 的 RV64 `ecall` 入参，不修改寄存器、内存或返回值。
每个诊断前后的负 PID `getpgid` 标记划分调用链；它们不属于原消费者。
`report.json` 保存输入、driver、观察器、kernel 的哈希、QEMU 版本、命令、
串口记录和分用例入口计数，`*.ecall` 保留原始入口。
入口不是成功返回的证明：同时检查消费者错误输出、后继程序查询及循环计数。
观察器不跟踪 syscall 返回值；首次失败的 errno 来自原程序错误输出。

## 固定资料与输入

使用本地 Linux7.2 的 `references/linux`、QEMU v11.1.0 的 `references/qemu`、
固定 BusyBox1.33.1，以及原镜像的 musl iperf3/cyclictest。具体版本与输入校验由
`references/sources.tsv`、既有程序清单和执行器机器记录管理；实跑QEMU11.1.1。

## 2026-09-29 窄诊断的历史结论

最终快照以同一driver和固定Linux重跑，两边七条独立调用链全部到达
`PROBE complete`，BoarOS最终heap-live=0。该轮没有TTY或真实网卡验收；网络能力
以[后来交付的应用与网卡](network-ownership.md)为准，不能将旧试跑错误当作当前连接缺陷。

| 消费者 | 两边实际入口与观察结果 | 后续限制 |
| --- | --- | --- |
| BusyBox `setsid /init endpoint` | 157；exec 后 PID=PGID=SID | 无 TTY 覆盖 |
| BusyBox `chrt -f 10 /init endpoint` | 119；exec 后 policy=1、priority=10 | 不是负载公平性测试 |
| BusyBox `chrt -r 10 /init endpoint` | 119；exec 后 policy=2、priority=10 | 不是 RR 延迟测量 |
| BusyBox `taskset 1 /init endpoint` | 122、123；exec 后 CPU0 有效 | 单 hart |
| libc `daemon(1,1)` | 157；返回 0，第二次 fork 后 SID=PGID≠PID | 独立 musl probe，非原 iperf 的 daemon 模式 |
| 原 `iperf3 -c 127.0.0.1 -A 0 -t 1` | 122 后到 socket198/connect203 | 无服务器/网络设施；Linux ENETUNREACH，BoarOS ECONNRESET，均 exit1；不证明吞吐支持 |
| 原 `cyclictest -q -t1 -p10 -i1000 -l10` | 119 四次、120/121 各一次、123 四次；线程报告 C:10，exit0 | 两边缺 cpu_dma_latency；Linux 非高精度定时器警告；BoarOS mlock228 报 ENOSYS 后继续。无性能结论 |

环境必须提供 `/dev/urandom` 和 `/dev/shm`；缺失它们会在新增接口之前或循环之前
失败。BusyBox 自识别依赖 basename，fixture 名称使用 `/busybox`。
cyclictest 的 `shm_open` 错误也可能 exit0，必须检查真实 `C:` 输出。
构建 driver 的既有 musl `sched_getscheduler/getparam` 包装未发 syscall 就返回 ENOSYS；
endpoint 用明确的 `syscall(SYS_sched_*, ...)` 查询，避免把 libc stub 误判成内核失败。
Linux 裸 PID1 的 PGID/SID 是 0，BoarOS 为 1；这里比较关系与策略，不比较分配的数字。

原始运行目录是可清理证据，不是永久档案；上述版本与命令提供重建入口。

## 原串口ash/stty与控制终端（2026-10-03）

这一轮交付的是原BusyBox ash/stty的串口完整流程，不是给ioctl补成功返回值。
launcher执行fork→setsid→open ttyS0→dup2→exec ash；TTY通过线程组持有的ctty
连接稳定SID/前台PGID。键盘Ctrl-C/Ctrl-Z由行规程向整个前台组发信号，shell通过
wait看到停止状态，bg/fg改变前台资格并继续任务。后台read触发TTIN，TOSTOP时
后台write触发TTOU；忽略/阻塞、孤儿条件和修改型ioctl各自处理，不只是发送一个信号。

UART从DTB取MMIO、clock和PLIC route，IRQ只收割有界字符/状态；worker负责输入
规程与有界输出。默认115200/8N1、canonical/echo/ISIG。软件行规程、UART队列、OFD
实例、线程组ctty和稳定PID身份分别有真实owner；最后pgrp成员退出不等于立即删除
TTY仍引用的身份号码。hangup使旧OFD代次失效，新会话取得终端不会复活它。
root在用户/OFD/会话清理后drain、stop/join worker，再释放队列与设备。

同一RV64 ELF在固定Linux与BoarOS通过27项读取/termios记录和80项作业控制记录：

| 验证 | 实际结论 |
|---|---|
| canonical、EOF、63/64/65字节与四种VMIN/VTIME | 整次readv维持64字节scratch、资格与deadline，不为下一块重新等待 |
| fault与部分信号 | 已交付前缀和已消费staging分别核对；SIGUSR1返回真实2字节，不遗留restart标记 |
| 阻塞readv9线程组取消 | leader及存活pthread被SIGKILL清退后，向量、OFD与读资格释放，独立后续read可继续 |
| 控制终端与错误 | console不自动取得ctty；无ctty的tty返回ENXIO；setsid、dup/fork、外会话及坏指针一致 |
| 后台任务 | 默认TTIN/TTOU真实stop并kill/reap；ignored/blocked TTIN返回EIO，TTOU允许写进展 |
| detach、steal、leader退出 | HUP/CONT真实到达；旧OFD EOF/EIO/HUP，新代次能重新打开，旧代次不复活 |

新增探针找出了一个实现错误：TIOCSPGRP把不存在身份与外会话统一返回EPERM。
固定Linux `tty_jobctrl.c` 在身份不存在时返回ESRCH，存在但不属该session才EPERM。
修复先经真实core反例红测，再验证registry查询、PID fallback、失败不改前台引用，
最终80项与Linux一致。冻结旧raw内核首个TCGETS返回ENOTTY，证明新增探针覆盖新增能力。

原BusyBox流程在modern/legacy完成stty -a/-g、raw/cooked/sane与原设置恢复、winsize，
DEL行编辑、管线/重定向，前台cat/计算程序Ctrl-C、停止sleep再jobs/bg/fg，以及后台
cat与TOSTOP输出。shell与子进程正常回收；launcher真实wait status=0，PID1退出42，
最终heap-live=0。完整tty能力仍有限：没有客体PTY、termios2、其他行规程、break生成、
完整modem控制或多用户凭据。宿主PTY只用于投递UART字节，不能当作客体PTY实现。

测试器的两处修正也有机制依据。QEMU stdio会重新开启宿主OPOST；宿主PTY必须同时
清除残留ONLCR，才能保留客体真实CRLF，不能事后全局删CR掩盖字节错误。ash作业号
随异步回收变化，清理使用实际PID与退出握手；恢复foreground后由真实SIGCONT handler
确认资格才投递Ctrl-C，避免把仍在后台的任务当作已恢复。原终端探针完整保留原始
字节，不进行文本归一化。普通测试的console与内核消息可交错，用户标记不依赖偶然
行首；唯一完整标记、真实退出、fatal检查与资源检查仍保留。

fatal必须走同步raw sink，绕过console级别与worker：否则console关闭、IRQ关闭或
worker被中断时会丢失现场。宿主反例保护这一入口，UART模型验证TEMT drain、队列满、
配置、失败回滚与stop超时owner；这些证据没有还原历史页释放fatal或Virtqueue的唯一原因。

重建使用固定BusyBox环境与musl，不修改原程序源码：

```sh
make musl-toolchain
python3 -B tests/program-inventory/environment.py --busybox
make test-uart-host test-tty-host
make test-tty-riscv test-tty-diff-riscv
python3 -B tests/tty/riscv.py --only boaros --transport legacy
```

runner关闭monitor混用，通过prompt、任务状态和客体握手推进；保存kernel/ELF/fixture、
调用及wait status到忽略的build。低层依据为本地Linux7.2的n_tty.c、tty_io.c、
tty_jobctrl.c和asm-generic/termbits.h，UART硬件参照本地QEMU v11.1.0的serial.c。
本轮不据交互完成宣称实板延迟、硬实时或多用户隔离；PTY＋原BusyBox script另行确认。

最终系统检查包含原1207条通用ABI、另107条终端记录、完整RV64、真实musl与glibc
五种ELF形态、scale、FIFO及栈预算。编译栈检查1971个函数，最大单函数3152字节；
它与运行期canary/高水位共同保护8KiB任务栈，不等于整个调用链只有3152字节。
集成后的原Lua完整功能流程再次完成干净/增量/无变化重建、语法错误恢复、默认FIFO
jobserver及其中断清理、-j2和全部产物运行，PID1退出42、heap-live=0，文件系统检查正常。
这一次是集成功能验证，不与CPU候选的三次正式构建混作新的性能分布。

收口还修复了新UART路径的一个准备阶段缺口：合法THRE=1/TEMT=0时，旧raw shift
尾字节不引用新port/core软件内存，不能让随后IRQ/worker资源失败变成fatal。现在仅
对尚未发布且未接受TX的对象，在撤IRQ、join及断言后逆序回滚；已发布TTY的drain与
stop仍要求真实TEMT，超时继续保留owner。首次空worker也记录初始忙状态，以10ms
请求期限观察完成并通知等待者。实际core的两个失败反例与一个等待反例先红后绿，
正常stop超时/恢复、原串口两种transport和根启动/ABI重验通过。

main保留BoarOS uname；通用实现单向合入兼容分支，原旧glibc BASIC_TCP正常完成、
后代清理为零，原串口应用同样完成。该入口不等于默认main支持全部旧glibc程序。
没有重跑iozone、完整成本矩阵或无关存储恢复，也没有改变journal/durable/FLUSH语义。

## PTY 的身份、传输与应用边界

Unix98 PTY 不是给原 UART ioctl 增加几个成功返回值。devpts 管目录身份和元数据，
配对持有两个 TTY core，行规程管理编辑、信号和等待，transport 管已经接受但尚未
交给对端的字节。应用持有的 OFD 与控制终端引用保活配对；core 的内部 base 引用
归配对拥有，不能反过来保活配对，否则最后关闭后永远不能回收。

slave 从未打开、最后关闭后可再开，以及 master 永久关闭是不同状态。前两种需要
master 保持身份与进展能力；后一种撤销可见 slave，使旧 OFD 和旧路径继续指向旧对象。
编号只是当前目录中的地址，不能作为跨回收的身份。只要旧路径仍被持有，旧元数据就
必须独立存在，但它不因此获得新配对的打开资格。

一个挂载的 worker 负责多对终端，不能因一对 raw 输入满或 mode guard 忙而睡死在
该终端内部。计数式非阻塞 receive 保留未处理后缀；信号立即发送，flush 在 guard
归还后完成。没有进展时等真实事件，输出读者归还容量后再次唤醒。有界 read/poll
推进用于收齐已经接受的尾部，不是伪造可读或靠忙轮询等待子进程。

固定 Linux v7.2 的 `references/linux/drivers/tty/{pty,n_tty,tty_ioctl}.c` 区分 N_TTY
flush 与 flip-buffer flush；不能把每个输入 flush 都扩大成无条件清空传输 FIFO。
packet 的 STOP/START 和 DOSTOP/NOSTOP 是互斥状态，flush 位可合并；数据前缀只在
规定的读取分支产生，PRI 表示尚未消费的真实控制状态。
流控事件对应实际输出状态变化，不能把每次设置都当成新事件。软件 VSTART 和
IXANY 不解除 TCOOFF；信号、关闭 IXON 与显式恢复也必须经过同一停止资格。
master 上取得控制终端绑定的是 slave，与组查询、proc 身份和最后关闭的 hangup 一致。

原 BusyBox 1.33.1 的 `references/oscomp-testsuits/busybox/util-linux/script.c` 有两个
应用边界：它返回成功并不代表被录制命令成功，也没有转发 SIGWINCH。验收需额外收集
被收养子进程的真实 wait 状态，并独立验证内核 resize；不能改内核迎合应用缺失的转发。
`-f` 不是 fsync 承诺。录制完成、显式持久化和最终挂载排空也需要分别理解。
原 `scriptreplay` 按 timing 的字节数读取录制文件，检查完整尾部比只观察 shell prompt
更能发现关闭与传输交错中的丢失。
