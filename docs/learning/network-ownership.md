# 网络对象与 loopback 事件推进

固定 libc-test `references/oscomp-testsuits` 的固定 pre-2025 版本 的 `libc-test/src/functional/socket.c` 真实调用 IPv4 UDP 的 bind/getsockname、微秒级 `SO_RCVTIMEO`、sendto/recvfrom，随后建立 TCP listener、非阻塞 connect 和 accept；它不消费 AF_UNIX，也不能单凭测试名推断所需协议。固定 Linux v7.2 的 `net/socket.c`、`net/ipv4/af_inet.c` 和 `fs/file.c` 提供 syscall、协议错误与 fd 生命周期比较基线。

取舍比较过三条路线：自写有限 TCP/UDP 子集能控制所有状态，但协议重传、定时器和互操作验证成本最高；宿主转发可快速启动，但 fd、错误和恢复语义被宿主环境决定；固定成熟 C 栈的 raw API 保留 BoarOS 的 fd/OFD、用户复制、等待、就绪和 errno owner，同时复用已测协议状态机。用户选择第三条，固定官方 lwIP `STABLE-2_2_1_RELEASE` 采用 `NO_SYS` 单 hart 事件驱动。分配 owner 另经用户确认：协议/packet 用静态有界池，BoarOS socket/OFD/队列节点用 kernel_heap，以池计数与 root `heap-live=0` 分别核对。

`NO_SYS` 没有独立网络线程。若服务端已睡在 accept，客户端非阻塞 connect 返回后不再调用 socket，SYN 若只留在 lwIP loopback 队列，服务端会永久睡眠。因此发起 connect 的系统调用必须推进一次 loopback，轮询与定时等待也推进协议。两进程握手测试固定顺序，排除了同进程立即 accept 偶然泵送队列的伪通过。另一条生命周期边界是 TCP `tcp_close` 在已连接状态可能暂保留 FIN/TIME_WAIT PCB；销毁 OFD 前必须先解绑指向 BoarOS 堆对象的回调，host 测试随后推进 200 秒计时并检查静态池回到基线。待 accept 子连接的 reset 也必须在 dequeue 前剔除。

固定参考是本地 `references/lwip/` 和导入的 `third_party/lwip/` 同一版本；移植只在 `net/lwip_port/`，不修改上游 core。重建入口与当前能力边界见[网络模块](../modules/kernel-network.md)。外部网卡、命名 AF_UNIX 和 SMP 的 owner/同步仍需要单独验证。

后续固定 Linux read/readv 差分暴露两个消费边界：TCP 的用户复制跨页 fault 返回 `EFAULT`，下一次读取仍得到完整那段数据；UDP 的 recvfrom 复制 fault 则丢弃整个 datagram。先从 lwIP 队列摘数据再做可 fault 的用户复制会丢 TCP 字节；只 peek 后不保留身份又允许共享 OFD 的第二线程在复制时改动队首。解决办法是把队首 reservation 登记在任务和 socket，并暂移 OFD pin。提交或取消时验证同一 packet；强制退出在文件表清理前取消，避免被抛弃的内核调用栈留下悬空 reservation 或永久 pin。固定 Linux 依据为 `references/linux/net/ipv4/tcp.c`、`net/ipv4/udp.c`、`net/socket.c`，固定 v7.2。

另一个边界是 lwIP `tcp_write` 在 `sndbuf>0` 时仍可能因 `snd_queuelen` 或全局 `MEMP_TCP_SEG`/pbuf 用尽而返回 `ERR_MEM`。只用 `sndbuf` 宣告 `POLLOUT` 会让阻塞 write 在同一 hart 上反复得到 `EAGAIN` 并立即再醒。固定 lwIP `third_party/lwip/src/core/tcp_out.c` 的检查和 host 池耗尽测试证明了触发条件；BoarOS 在真实失败时撤下可写事件，将 bounded retry 纳入 poll/epoll 与 socket 等待期限，ACK 和池释放均可促成后续写入。早期真实 pthread 曾用 UDP 接收队列占满协议堆触发这个条件；本轮增加共享堆余量后，
该场景改为保护 UDP 过载时 TCP 控制仍可进展。全局 TCP segment ERR_MEM 的独立
host 回归仍保护真实 POLLOUT 抑制，不能把旧触发条件继续写成现行配额政策。

零长度 UDP datagram 揭示另一种 owner 漏洞：`read_buffer` 已登记 reservation 并移走 OFD pin，却返回 0；调用方原来只在正字节数时 finish，导致返回后 socket 指向失效的栈请求。固定 Linux 的普通 read/readv 对空 datagram 都返回 0 且消费它；同 ELF 红测在 BoarOS 第一笔空包之后 fatal，修复后继续读取下一包并检查用户态关机 `heap-live=0`。由 `request.socket` 是否登记而非返回字节数决定是否 finish；TCP EOF 没有登记。强制退出 owner 也需按真实控制流判定：`kernel/sched/process.c::request_thread_termination` 只置位并唤醒，保存的 syscall 栈继续返回，`kernel/sched/signal.c::kernel_signal_select` 在 user-return 才终止；`kernel_socket_sendto/recvfrom` 的栈局部 pbuf/packet 在这条路径仍可清理。若以后引入绕过 syscall unwind 的非局部退出，需重审它们。

2026-10-01 开始原版网络应用阶段。iperf 3.13 的 `netannounce` 在没有显式
地址族和监听地址时主动选择 IPv6；`IPV6_V6ONLY=0` 用于接受 IPv4 客户端。
只补 IPv4 会停在服务端 socket 创建，不能完成原脚本。以前无服务器的客户端
试跑不能作为已建立连接 reset 的证据。调用依据为本地
`references/oscomp-testsuits` 的 `iperf/src/net.c`（固定 pre-2025 输入）和
`references/linux/net/ipv6/`；精确版本在既有来源清单。

地址阶段沿官方 lwIP 2.2.1 loopif 启用 IPv6，把地址转换限制在 syscall 边界。
`tests/workloads/network/contract.c` 同 ELF 验证 ::1 的 UDP、TCP 和 IPv4 到
IPv6 通配监听的映射 accept。旧实现先在 IPv6 socket 创建以 EAFNOSUPPORT
失败，修复后双方完成内容交换；协议静态池由 host 回归核对，socket 堆由
真实关机零占用核对。重建：`make test-lwip-host test-network-riscv`。
这只是当时的地址阶段证据；选项、半关闭和原版流程的后续交付见文末。

双栈端口回归进一步发现 lwIP TCP bind 源码留下 ANY 与纯 IPv6 交集的 TODO：
IPv6 双栈通配已占端口时，::1 的另一次 bind 居然成功。固定 Linux 拒绝该操作。
BoarOS 用不持引用的 endpoint 列表检查真实地址交集，生命周期仍由 OFD 决定，
补丁没有修改 lwIP core。V6ONLY 两族隔离、UDP 默认对端/解除及 SYN 拒绝的
SO_ERROR 清除，均由相同用户态回归验证。TCP_MAXSEG 来自协议 PCB，未知
TCP_INFO 继续报不支持，不能当成已提供统计。

原 netperf 的 UDP_STREAM 暴露了网络之外的直接依赖：libc alarm 经 RV64
setitimer 设置 SIGALRM，旧内核返回 ENOSYS，发送循环因此没有结束信号。有效
服务器下的 syscall 观察确认了设置定时器后持续发送的路径。补真实 ITIMER_REAL
后负载能自行进入结果交换；不能把原程序被超时杀掉当作网络链路完成。定时器
属于线程组，fork/exec/退出及信号消费的协议见[时间模块](../modules/kernel-time.md)。

阻塞发送的实质差异由原 netperf 暴露：内核在已经发送一个前缀后遇到 EAGAIN，
立即返回短写。原程序将这种短写当作计时结束，TCP_STREAM 退出 0 但有效时间
只有 0.00 秒。固定 Linux 小预算、一次 64 KiB 的发送会继续等待并完整接收。
BoarOS 改为等待剩余空间，保留真正 fault/信号/超时与非阻塞短写。修复后原
TCP_STREAM 持续约一秒并交换接收结果；这说明有效工作成立，不只是流程退出。

另一种边界是原 iperf 的 listener 重建：空控制探测连接会结束一轮，再建立
listener；连续原脚本也可能撞到这个间隙，Linux 同样会拒绝/reset。受控执行器
为每项启动新服务端，用官方 --forceflush 在 listen 后发布的 banner 握手，不
制造探测连接；客户端 argv 与原脚本一致。原脚本另留真实子项状态，不能用这条
受控流程掩盖其启动竞态。服务端输出与客户端输出分开，长运行服务端的主动
终止信号、wait status 和后代回收单独记录。


## 原版网络应用交付（2026-10-02）

本轮解锁了未经修改的 iperf 3.13 和 netperf 2.7.0：服务端真实监听、双栈连接、
内容传输、接收结果交换、客户端正常退出以及后代回收均成立。两种 libc 的受控
流程共22项，每项保留原客户端参数。旧 glibc 使用 oscomp-rv-compat 的兼容
版本身份；main 保留自身 uname，glibc 2.44 的启动/TLS/pthread 另有主线回归。
最终22项确认、三个性能副本及定点观测统一使用兼容分支，让两种libc共用一版
内核。musl早期完整流程及主线系统/内容回归在main运行。兼容分支的uname差异
不能推成main旧glibc支持。这项交付为真实网卡提供socket基线，不代表外部网络已经可用。

| 原版方法 | musl | 旧 glibc | 验收依据 |
|---|---|---|---|
| iperf BASIC TCP/UDP、PARALLEL TCP/UDP、REVERSE TCP/UDP | 6/6 | 6/6 | 接收端有实际字节、完整有效时间，五连接逐一有进展 |
| netperf TCP/UDP STREAM、TCP/UDP RR、TCP CRR | 5/5 | 5/5 | 有效测量时间、接收消息或事务率、结果交换与退出状态 |
| 原 netperf 连续脚本 | 5/5 | 5/5 | 原输出逐项有效；保留包装器状态 |
| 原 iperf 连续脚本 | 3/6 | 4/6 | 部分下一客户端撞到 listener 重建间隙，返回 ECONNREFUSED |

原 iperf 脚本的失败没有被忽略：本次 musl 的 BASIC_TCP、PARALLEL_TCP、REVERSE_UDP
失败，glibc 的 PARALLEL_UDP、REVERSE_UDP 失败。固定 Linux 原 glibc 脚本也在
REVERSE_UDP 出现同类拒绝连接；其 musl 脚本6/6。原脚本不等待 listener 重建，
错误属于参考侧也可达的时序；不为它更换调度策略。受控流程每项新建服务端、
以 listen 后的 banner 握手，保留原 ELF 和客户端参数，同时单列服务端的主动
终止信号。不能将受控22项完成写成原连续脚本22项全通过。原脚本只能给出子项
success/fail，不能恢复真实 child wait status；受控流程保存真实 wait status。

独立内容负载确认单 TCP 连接16 MiB、五连接各8 MiB、IPv6 UDP一万次64字节
请求响应。TCP检查字节模式、checksum、FIN后的EOF和反向响应，全部工作者
完成才结束。它保护内容和生命周期；原消费者保护真实应用的组合接口。

### 关闭观测的效率与任务进展

固定 QEMU virt、单 hart、512 MiB、legacy/writeback、timebase 10 MHz，三次
独立启动串行运行，每次两种 libc 都执行单连接TCP、五连接TCP和UDP_RR。
Linux一列是同输入的一次有效参考启动，不是三个副本的中位数。iperf使用接收端
输出；五连接合计是各接收端速率之和，原输出的传输量/速率存在显示舍入。
netperf原输出只给事务率，不能把率乘显示时间伪装成精确事务总数。

| libc / 方法 | BoarOS中位 | 三次范围 | 固定Linux一次参考 |
|---|---:|---:|---:|
| musl 单TCP，Mbit/s | 242.0 | 239–246 | 7830 |
| musl 五TCP合计，Mbit/s | 352.6 | 346.8–353.9 | 7600 |
| musl UDP_RR，事务/s | 6443.34 | 6181.57–6645.22 | 7770.23 |
| glibc 单TCP，Mbit/s | 261.0 | 260–263 | 8240 |
| glibc 五TCP合计，Mbit/s | 346.7 | 324.8–349.8 | 7600 |
| glibc UDP_RR，事务/s | 6611.65 | 6416.00–6642.29 | 7828.06 |

所有五连接均收到数据。三个副本每连接速率musl为64.6–72.7 Mbit/s，glibc为
60.2–71.9 Mbit/s；不能用最快连接抵消慢连接。原固定时长不是效率成绩，表中
比较的是这段时间内真实完成的数据或事务。

不限速UDP本来就没有TCP流量控制，不能以sender吞吐代表接收成功。最终22项
确认中，musl单向/反向UDP接收50.6/51.9 Mbit/s，丢包约80%/77%；五连接每条
约28.3–28.6 Mbit/s，约56%–57%丢包。glibc单向/反向49.6/51.4 Mbit/s、约
81%/80%丢包；五连接约28.5–28.7 Mbit/s、约60%–61%丢包。netperf UDP_STREAM
分别发送64905/66317条，接收3232/3200条，接收吞吐25.68/25.51 Mbit/s。
接收队列和共享协议堆有界，过载会丢弃并归还pbuf；目前没有把每个丢包细分到
单socket预算、全局余量或调度，因此不宣称唯一丢包原因已经量化。

### 成本、观测扰动与剩余限制

一次定点观测的六个窗口保持完整、无溢出、无在途操作；诊断存储63938字节、
每任务64字节。TCP窗口前台运行记账占窗口约78.5%–89.5%；musl单连接接受
47225929字节，却发生21912次stream写调用和32027次用户页解析。当前实现每次
写取得暂存页，复制用户数据，再以TCP_WRITE_FLAG_COPY交给协议；raw窗口只有
8×1460字节，loopback泵送还处理分段、ACK和接收队列。前台按调用/页/段的工作
仍明显，不能把TCP差距归为磁盘等待。单连接和五连接的TCP参考差距分别约
32倍和22倍，而Linux的UDP_RR事务率仅高约18%–21%，也说明不同负载的首要成本不同。

这不是各阶段CPU比例的完整归因：当前没有独立分解复制、协议计算、查页和
分配各自占比。ready和blocked是多个任务的累计等待，互相重叠，不能与运行
时间直接相加。所选TCP窗口未见全局池分配失败，唤醒到运行最大值约11–12ms；
它只说明响应尾部仍存在，不证明所有请求都多等一个tick，也不支持现在重写
整个调度器。后续优化由真实应用成本触发。

观测改变速度：单TCP为189/190 Mbit/s、五TCP265.6/253.4 Mbit/s，UDP_RR
3995.14/4118.49事务/s。相对关闭观测中位，吞吐降低约22%–27%（TCP）、38%
（UDP_RR），不能把这些数当默认内核成绩。协议计数16位会环绕，只使用有明确
宽度的64位TCP写调用/字节和即时pool用量；不能把取模差值当整个窗口总包数。
观测窗口的协议堆高水位126432字节；它不含全部静态pool或任务/文件堆占用。

最终正常关闭后，协议期限在120119ms内把active/listen PCB、segment、UDP PCB
及协议堆已用量归零，PID 1退出时kernel_heap live也为零。TIME_WAIT期间pool仍有
真实owner，静态pool存储本身属于内核常驻内存，零用量不表示这些静态页被释放。

收口的一次所有权审查补出了IRQ拒收重试、UDP自动端口释放和TCP MSG_TRUNC
三个问题。独立heap包装器强制首个接收分配OOM，旧timer路径在模拟IRQ区间
申请heap，修正后零heap调用且调用上下文完整接纳原数据；不声称已经复现了
真实堆损坏。UDP断开和TCP不可写用户页的MSG_TRUNC由同一ELF在固定Linux
对照，旧实现失败。修正后1166条ABI、socket scale、真实musl与栈检查通过；
完整RV64及glibc 2.44集中验收已通过，后续只重跑受影响范围。未重跑iozone、
C0–C6或存储恢复。本轮没有改事务、调度策略或通用堆同步。

### 重建与证据

命令见[网络模块](../modules/kernel-network.md#原版应用与诊断入口)，内容验证用
`python3 -B tests/network-riscv.py --workload content`，计时器依赖用
`--workload timer`。固定源码依据为本地`references/linux`（v7.2）、
`references/lwip`/`third_party/lwip`（2.2.1），原输入为固定公共RV镜像及
`references/oscomp-testsuits`的pre-2025源码。执行器在`build/`运行目录记录逐项
argv、配置、原始输出、wait status和观测快照；本文保留验收范围、分布与解释，
大型运行记录不纳入Git。运行QEMU 11.1.1，参考源码为11.1.0；Linux的loopback
环境另加127.0.0.2本地别名
以满足旧glibc AI_ADDRCONFIG，没有借此提供外部网卡。

一次COST内核版本失配的无效启动未计入验收；阶段样本与最终修正版分开，
不混算副本。重建时由执行器核对输入身份，不要求读者手工比对校验值。
最终仅单向合入兼容分支，不push。
VirtIO-net与宿主双向应用是下一阶段；命名UNIX、SCM_RIGHTS、TCP_INFO、真实
网卡、SMP与LoongArch均未由此次loopback结果交付。

## socket状态与发送纠错（2026-10-02）

独立评审指出未连接TCP非零读取缺少ENOTCONN。真实U-mode反例在固定Linux通过，
旧BoarOS返回EAGAIN；零长度recv还被文件层的read(0)快捷返回绕过。修复让消息接收
经过协议状态检查，保留read(0)与recv(0)的区别；空已连接socket的recv(0)也会等待，
非阻塞为EAGAIN，已有内容则返回0且不消费。队首数据、待交付错误与EOF继续分开。
重建：`make all && python3 -B tests/network-riscv.py`，固定资料见网络模块。
本段只记录该边界的双侧验证；本轮整体应用和性能结论在收口后追加。

UNIX数据报半关闭不要求队列中有数据才报告可读：SHUT_RD给出IN和RDHUP，
SHUT_RDWR另有HUP。Linux空接收的阻塞路径返回EOF，非阻塞路径却返回EAGAIN；
因此poll掩码不是每种调用返回值的充分判断。旧实现遗漏空队列事件，也把非阻塞
误当EOF。修复让接收入口使用真实非阻塞状态，EAGAIN后先检查非阻塞再查询poll，
避免在关闭方向上空转。队首数据、dup、reservation和两种接收模式由双侧反例保护。

预算等待必须通知真正的等待队列。UNIX发送者睡在自己的socket上，消费接收内容时
已有peer通知，但扩大接收预算过去只唤醒接收者。反例确认孩子已进入S状态，扩容
后旧实现仍无进展；修复后STREAM/DGRAM均完成。重复设置、缩容、关闭和取消也
核对，结束heap回到零。等待确认采用握手与proc状态，不用sleep猜测发送是否开始。
