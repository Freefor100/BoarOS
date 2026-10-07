# 网络对象与 loopback 事件推进

固定 libc-test `references/oscomp-testsuits` 的固定 pre-2025 版本 的 `libc-test/src/functional/socket.c` 真实调用 IPv4 UDP 的 bind/getsockname、微秒级 `SO_RCVTIMEO`、sendto/recvfrom，随后建立 TCP listener、非阻塞 connect 和 accept；它不消费 AF_UNIX，也不能单凭测试名推断所需协议。固定 Linux v7.2 的 `net/socket.c`、`net/ipv4/af_inet.c` 和 `fs/file.c` 提供 syscall、协议错误与 fd 生命周期比较基线。

取舍比较过三条路线：自写有限 TCP/UDP 子集能控制所有状态，但协议重传、定时器和互操作验证成本最高；宿主转发可快速启动，但 fd、错误和恢复语义被宿主环境决定；固定成熟 C 栈的 raw API 保留 BoarOS 的 fd/OFD、用户复制、等待、就绪和 errno owner，同时复用已测协议状态机。用户选择第三条，固定官方 lwIP `STABLE-2_2_1_RELEASE` 采用 `NO_SYS` 单 hart 事件驱动。分配 owner 另经用户确认：协议/packet 用静态有界池，BoarOS socket/OFD/队列节点用 kernel_heap，以池计数与 root `heap-live=0` 分别核对。

最初接入时，`NO_SYS` 尚没有内核后台服务（下面记录当时的设计，当前已由文末有界服务取代）。若服务端已睡在 accept，客户端非阻塞 connect 返回后不再调用 socket，SYN 若只留在 lwIP loopback 队列，服务端会永久睡眠。因此发起 connect 的系统调用必须推进一次 loopback，轮询与定时等待也推进协议。两进程握手测试固定顺序，排除了同进程立即 accept 偶然泵送队列的伪通过。另一条生命周期边界是 TCP `tcp_close` 在已连接状态可能暂保留 FIN/TIME_WAIT PCB；销毁 OFD 前必须先解绑指向 BoarOS 堆对象的回调，host 测试随后推进 200 秒计时并检查静态池回到基线。待 accept 子连接的 reset 也必须在 dequeue 前剔除。

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
该边界由同一ELF双侧验证；本轮应用结果与性能限制见下文。

UNIX数据报半关闭不要求队列中有数据才报告可读：SHUT_RD给出IN和RDHUP，
SHUT_RDWR另有HUP。Linux空接收的阻塞路径返回EOF，非阻塞路径却返回EAGAIN；
因此poll掩码不是每种调用返回值的充分判断。旧实现遗漏空队列事件，也把非阻塞
误当EOF。修复让接收入口使用真实非阻塞状态，EAGAIN后先检查非阻塞再查询poll，
避免在关闭方向上空转。队首数据、dup、reservation和两种接收模式由双侧反例保护。

预算等待必须通知真正的等待队列。UNIX发送者睡在自己的socket上，消费接收内容时
已有peer通知，但扩大接收预算过去只唤醒接收者。反例确认孩子已进入S状态，扩容
后旧实现仍无进展；修复后STREAM/DGRAM均完成。重复设置、缩容、关闭和取消也
核对，结束heap回到零。等待确认采用握手与proc状态，不用sleep猜测发送是否开始。

发送等待应复用已经拥有的内容。旧代码在EAGAIN后返回外层usercopy循环，同一1000字节
阻塞发送实际复制2000字节。新游标保留请求页里的有效区，部分发送推进偏移，等待后
继续发送尾部；全阻塞和先发送596字节的受控场景均核对有效1000字节只复制1000字节。
信号、期限与非阻塞按已接受前缀返回，scratch与OFD pin的正常/取消owner未增加。
`--workload budget`在COST构建核对该成本，关闭观测仍校验内容及关闭/取消；
`--workload content`完成16MiB单TCP、五连接各8MiB和一万次UDP内容。此处证明去掉了
重复工作；原程序吞吐、UDP代价和最终清理在下文单独报告。

终止事件不等于已取得接收资格。汇总审查发现活动reservation加HUP/ERR可使第二读者
在EAGAIN和立即重试之间空转；如果owner正因usercopy等待，后继反而妨碍其恢复。
内部等待现使用实际接收谓词，保持对外poll终止事件。io-sleep反例暂扣真实复制调用，
旧机制在发布HUP后卡住；新机制让后继真正阻塞，并核对完成、复制fault、期限与信号，
四种设备配置均完成且页数回到基线。这是受控内核fixture，不冒充已测实板或多核。
固定ABI同时纠正新建TCP的OUT/HUP组合；本轮清单由1166扩为1179项。

## 本轮应用结果与剩余复制（2026-10-02）

本轮三项状态/通知纠错与暂存游标已交付；它们解除错误等待、事件遗漏和请求内重复
usercopy，但没有显著提高原版TCP吞吐。最终两种libc的22项受控原iperf/netperf均有
真实接收或事务，逐客户端wait status为0、无超时。单TCP16MiB、五TCP各8MiB、
UDP一万次64字节内容负载也完成；这些保护数据与owner，不替代应用成绩。
原连续脚本既有listener重建竞态仍单列，没有通过忽略失败提高通过率。

### 关闭观测的成绩

旧新与固定Linux均三个串行独立启动，原ELF、参数、协调器与前置环境相同。
使用既有网络执行器的legacy/writeback、512MiB、单hart配置，QEMU 11.1.1；
两种libc消费者均在兼容配置验收。这个配置与存储对照的modern不能混为同一实验。
固定时长内比较真实接收速率/事务，不把运行时长本身当效率。TCP为接收端Mbit/s，
五连接为逐连接接收速率之和；UDP_RR为事务/s。括号为最小–最大值。

| libc / 负载 | 旧版中位 | 新版中位 | 固定Linux中位 |
|---|---:|---:|---:|
| musl 单TCP | 249（245–266） | 250（249–252） | 8220（8150–8250） |
| glibc 单TCP | 268（268–270） | 274（273–277） | 8170（8120–8260） |
| musl 五TCP | 354.1（352.5–355.0） | 359.9（350.4–360.6） | 7700（7610–7800） |
| glibc 五TCP | 328.5（326.8–358.8） | 355.5（349.7–361.7） | 7650（7560–7650） |
| musl UDP_RR | 6514.21（6265.90–6561.08） | 6312.74（6277.13–6513.98） | 7897.88（7648.55–7941.42） |
| glibc UDP_RR | 6633.90（6548.23–6692.81） | 6587.16（6469.12–6638.60） | 7861.92（7705.86–8069.02） |

单TCP接收量中位musl59.4→59.7MiB、glibc64.1→65.3MiB；五TCP总量84.7→86.3、
78.9→85.2MiB。五连接全部进展，最慢连接速率中位67.3→70.6、60.9→68.0Mbit/s。
单连接仅改善0.4%/2.2%，五连接1.6%/8.2%，后者范围也有重叠；UDP_RR中位下降
3.1%/0.7%，旧新范围重叠。这些副本不支持稳定的大幅TCP收益，也不支持永久饥饿
或整个调度器退化的判断。TCP仍比该Linux参考慢约21–33倍，UDP_RR差约19%–25%。

### 为什么暂存优化没有同比改善iperf

请求内的反例有明确因果关系：发送1000字节，旧阻塞重试复制2000字节，新实现完整
阻塞和596+404字节部分发送均只复制1000字节。可是原iperf传输阶段采用非阻塞socket。
固定上游`references/oscomp-testsuits`的pre-2025 iperf源码中，
`iperf_client_api.c`在TEST_RUNNING设置`setnonblocking(...,1)`，`iperf_tcp_send`
调用`Nwrite`；`net.c::Nwrite`在短写后继续write，遇EAGAIN返回已发送前缀。
应用以后再次发起系统调用，已经超出本次暂存页owner的生命周期。

BoarOS仍先复制一个用户页片段，再查询协议能接受多少；非阻塞容量不足时返回短写或
EAGAIN并释放请求页。下一调用会重新复制尚未被接受的用户字节。此次游标消除的是
同一调用内部的重复，不跨调用缓存用户内容，也没有取消lwIP的TCP_WRITE_FLAG_COPY。
把阻塞反例的收益直接套到非阻塞iperf是不成立的。

旧新各一次定点观测的实际字节如下，不将固定时长的不同总量直接当复制加速比。

| 窗口 | 接受字节旧→新 | usercopy字节旧→新 | 复制/接受旧→新 |
|---|---:|---:|---:|
| musl 单TCP | 47830925 → 47754311 | 125130925 → 124895599 | 2.616 → 2.615 |
| glibc 单TCP | 48666284 → 45667811 | 126924640 → 119457163 | 2.608 → 2.616 |
| musl 五TCP | 65949943 → 63963953 | 143020151 → 141332741 | 2.169 → 2.210 |
| glibc 五TCP | 68907130 → 68302031 | 146823850 → 144822035 | 2.131 → 2.120 |

单TCP用户页解析32250→32201、32758→30777次，按有效接受字节归一后没有明显下降。
前台运行记账仍约占TCP窗口78%–89%，不是精确CPU指令占比；按调用准备、查页、
两层复制、分段/ACK及小协议窗口仍存在。没有证据将Linux差距单独归于调度策略。
若下一应用也呈现该放大，候选是在可发送credit约束下减少非阻塞无效暂存，先验证
坏指针/错误优先级、短写与取消，不直接引入跨请求缓存或异步借用用户页。

这些是开启观测的单次解释窗口，正式吞吐来自上表关闭观测的三次启动。
UDP_RR的stream指标只包含控制连接，不能拿它当数据报复制量。16位协议计数继续
不用于大流量总包数；最大pool用量是即时协议资源，不能代替全内核内存峰值。
当前COST聚合64794字节、每任务64字节，默认构建关闭；未扩大观测框架。
22项运行均清理子进程，PID1退出heap live为零；定点观测另等待120662ms，核对
active/listen/segment/UDP及协议堆用量归零。
TIME_WAIT对象在到期前仍有协议owner，不将暂留资源误报为已释放。

### 验证边界与下一任务

完整RV64、真实musl/glibc、1179条固定Linux ABI、socket scale、四组合io-sleep和栈
已验收；最后reservation谓词修复只重跑相关范围。集中收口的一次审查发现该遗漏后
补了真实复制暂停交错，没有安排多轮审查循环。旧对照中的未定位页释放与VirtIO异常
见[存储对照风险](cost-baseline.md#版本量封口与socket纠错对照2026-10-02)，不声称已修复。

重建用网络模块的`--case representative --repeat 3`，旧新分别冻结内核与协调器，
`--case all`验收22项；`--observe`只做一次定点，`--workload budget/content`保护内容
与请求内复制。逐调用身份与原输出在忽略的build中，本文保存结论和可重建入口。
没有接入真实网卡、扩大窗口、零拷贝或更换调度器。下一主线是N3：单队列VirtIO-net、
静态IPv4＋ARP及隔离宿主双向TCP/HTTP传输；性能改进持续按实际应用成本选择。

## 真实 VirtIO-net 与宿主应用交付（2026-10-02）

这一轮使内核从 loopback 走到真实 MMIO 网卡：QEMU legacy/modern split ring、Ethernet、
ARP 和静态 IPv4 均实际收发。设备和协议仍在单 hart 上运行；隔离 TAP 的宿主是
10.77.0.1，客体 eth0 是 10.77.0.2/24，不设置网关、vhost 或 offload。
`::1` 内容探针继续通过。它证明隔离宿主双向网络成立，不能推出公网、DNS、TLS
或外部 IPv6 已交付。

### 解锁的程序与实际工作

固定原镜像的 BusyBox wget/httpd 未修改。main 使用原 musl，兼容分支保留旧 glibc
需要的版本身份；两个配置的 legacy/modern 都完成以下流程：

- 宿主向客体、客体向宿主分别进行 16 MiB HTTP GET，逐字节核对内容。
- 宿主向客体 CGI 上传任意 16 MiB 文件，CGI 输出、落盘文件内容和子进程状态均核对。
- 客体原 wget 向宿主发送 4 KiB 文本 POST，宿主核对内容并返回确认。
- 单 TCP 双向各 16 MiB；五个工作者统一开始、各双向 8 MiB，全部完成才结束。
- UDP 一万次 64 字节请求响应，另核对 1472、1473 和 65507 字节的往返内容；
  后两项确实经过 IPv4 分片和重组。

原 httpd 的 GET 调用 sendfile，首次有效外部流程因此暴露 syscall 71 缺口。
本轮实现有界内核复制，保护 fd/OFD pin、位置、短进展、同步错误和整包数据报。
它不是发送零拷贝。固定 Linux 还证实 sendfile 生成的 pipe 片段不能被普通 write
并尾；原 BoarOS 会多接纳一个字节，现已修正。重建入口和特殊输入限制见
[文件模块](../modules/kernel-files.md#sendfile-与来源片段)。

服务端由 fixture 有意终止：httpd 使用 SIGKILL，netserver 使用 SIGTERM；
记录其真实 wait status，不把这种清理写成服务程序自行正常退出。
兼容配置的 iperf/netperf 两种 libc 共 22 项受控原 ELF 流程再次全部完成，
各客户端 wait=0 且有实际接收或事务数。此前原连续 iperf 脚本的参考侧竞态仍单列，
此次没有把受控启动说成原连续脚本已经全部通过。

### 零拷贝减少的是哪一层工作

RX 完成后先归还描述符资格，DMA 槽的内容仍由 custom pbuf 持有。
最后一个引用释放才允许该槽再次发布；reset 确认也不能提前释放借出的内容。
超过 32 个借用时复制到有界备用池，立即归还 DMA。worker 最多处理八帧，
批次间开放中断并让出运行机会；IRQ 不分配、不重入 raw API 或堆。

实际 Ethernet input 的宿主边界探针把同一 1514 字节帧交给两条路径：
custom pbuf 直接引用原 payload、复制零字节；借用满时恰好复制 1514 字节。
它另核对第二个引用、最后释放、OOM、控制余量和 input 错误的清理；
驱动模型独立保护旧 DMA 内容、非法完成及 reset owner。
这是字节和生命期的因果证据，不是宿主模拟的吞吐成绩。

三个关闭观测启动中，112897–113369 个帧使用借用，117–124 个帧使用复制回退；
借用占接纳帧约 99.89%，累计直接引用约 99 MB 的 Ethernet 内容。
被省掉的是 DMA 到协议 pbuf 的一跳复制，包含帧头；不能把它当成应用有效字节，
也不能称为全链路零拷贝。TCP 到用户缓冲的复制、发送暂存、lwIP COPY 和 TX DMA
复制均保留。此前没有真实网卡基线，不能据此宣称相对旧版某个吞吐倍数。

压力阶段先投递 128 个 1472 字节 UDP 包，接收端暂不读取，再要求 TCP 双向各完成
8192 字节。BoarOS 接纳 44 包、64768 字节并丢弃至少 84 包，TCP 内容仍完整；
固定 Linux 接纳 22 包，不能用同一个 SO_RCVBUF 数值要求它们有相同包数。
BoarOS 定点窗口明确记录 84 次 UDP 接纳资源错误，驱动 drops=0 表示此处在协议层
拒绝，不表示没有丢包。备用池保留 16 个控制 pbuf，判断包括即将借用或分配的版本。

### 关闭观测的实际完成效率

modern、512 MiB、单 hart，三次串行独立启动；每次重建隔离宿主 namespace，
固定 Linux 使用独立的 VirtIO-net 配置，同一 ELF、数据、请求顺序和 TAP 条件。
参考源码是本地 Linux v7.2、QEMU v11.1.0、lwIP 2.2.1；实际 QEMU 二进制为 11.1.1，
DTB timebase 为 10 MHz。精确工具和输入身份仅保存在 runner 的机器记录。

下表为中位数（最小–最大）。TCP 吞吐按双向串行的实际总量计算：单连接 32 MiB，
五连接 80 MiB，使用客体计时。单连接从 accept 完成后开始；五连接从最早工作者
开始到最晚完成，已排除全部 accept、fork 和 gate 准备。区间包含内容检查、发送、
半关闭和 close，是传输阶段的完成效率，不能当成单向峰值或与此前 loopback iperf
直接换算。

| 指标 | BoarOS | 匹配 Linux |
|---|---:|---:|
| 单 TCP 阶段，秒 | 0.902（0.899–0.950） | 0.229（0.229–0.248） |
| 单 TCP 双向总量，Mbit/s | 297.5（282.4–298.5） | 1171.2（1083.4–1173.1） |
| 五 TCP 阶段，秒 | 2.359（2.302–2.706） | 0.606（0.594–0.608） |
| 五 TCP 双向总量，Mbit/s | 284.5（248.0–291.6） | 1106.6（1103.0–1130.7） |
| UDP 64 B 请求响应，次/秒 | 7777（7657–7795） | 20255（19092–21284） |
| HTTP 完整阶段，秒 | 7.563（7.466–7.581） | 1.158（1.156–1.202） |
| 程序自身完成，秒 | 12.151（12.044–12.540） | 2.572（2.529–2.613） |
| 程序完成后到最终关机，秒 | 2.035（1.991–2.046） | 0.023（0.021–0.025） |

BoarOS 五个工作者全部接收、发送 8 MiB 且退出 0；最早和最晚完成的差为
18–107 ms，中位 25 ms，不能用平均值掩盖慢任务。UDP 往返 p99 中位为
225 μs（186–228），Linux 为 84 μs（78–118）；最大值中位为 725 μs 与
2028 μs。这是宿主逐请求计时，最大值对偶发宿主停顿敏感，不作硬实时保证。

程序后的时间由读取 PASS 到 QEMU 最终退出计量：BoarOS 包含 fixture 内部根卸载、
剩余 durable/checkpoint、协议及 owner 收口；Linux 包含其关机流程。
它们分别公开，但不是匹配的 fsync 或纯 checkpoint 微基准，不把差值全归给日志。
总 runner 墙钟还含启动和宿主 HTTP 服务结束，不能替代程序自身完成时间。
legacy 的同一内容流程完成；旧 glibc 的两个 transport 也完成，单次结果用于能力验收，
不拼入 main/musl 的三次分布。

### 剩余成本与本轮发现

一次 COST 启动使用原有五个窗口，没有增加诊断字段或节点。
64 KiB 聚合预算内实际为 64794 字节，每任务为 64 字节，时钟分辨率为 100 ns。
单/五 TCP 的 stream usercopy 分别恰好为 16/40 MiB，与发送接受字节相等；
请求内没有背压重复复制。但这次是阻塞内容负载，不推翻此前非阻塞 iperf 跨调用
复制放大的结论。

单 TCP 窗口约 1.098 秒，前台运行记账 0.742 秒、后台 0.335 秒；
五 TCP 窗口约 3.145 秒，对应 2.248 秒和 0.821 秒。
两者没有块设备请求，主要时间已在客体运行上下文中，不能归给 FLUSH。
运行记账包括用户内容检查与内核/协议处理，不是函数指令占比。
ready 和 blocked 是多任务累计等待，与运行时间、窗口时间不能直接相加。
TCP 仍使用 11680 字节收发窗口、128 个全局 segment，用户页解析、复制、分段和
ACK 等固定工作仍存在；没有此次独立证据把差距单独定位为某种调度算法。

HTTP 观测窗口约 10.505 秒，前台/后台运行为 7.252/1.546 秒；
文件接受约 32 MiB，同时包含文件下载、上传、检查和后台持久化。
后台 576 次 FLUSH、约 1.490 秒提交和 1.075 秒 checkpoint 累计时间说明它已是
网络与文件系统组合流程，不能用裸 TCP 的机制解释全部差距。
unknown read 约 17.74 MB 保持 unknown；journal 内存峰值取 max 为 862912 字节，
不将累计峰值采样之和写成内存占用。观测程序耗时 18.388 秒，明显扰动负载，
正式成绩只用关闭观测的三个启动。

实际 DMA 为 legacy 272 KiB、modern 264 KiB，控制对象 4896 字节。
关闭观测的 root heap 峰值为 1139–1145 页（约 4.45–4.47 MiB），它不包含全部
物理页缓存、静态协议池和 DMA，不能称为全内核峰值。任务栈实际最大使用
3176–3192 字节，最小余量 4984 字节；所有启动最终 heap live 为零。
同一启动另有真实块设备 IRQ 与 RNG 完成，RNG 读取 64 字节且无错误/超时。

本轮收口修复 modern DEVICE_NEEDS_RESET 的发布缺口、失败 NIC 停止共享协议
定时推进、行政 DOWN 仍报告 RUNNING，以及 connected UDP 的设备故障路由归因。
57 项驱动模型、22 项重组宿主契约及实际 ARP/UDP 内容覆盖它们所属的边界。
此前旧对照的物理页释放 fatal 和 QEMU queue-excess 仍未定位：新增诊断输出具体
页状态/失败原因与 reset 前队列现场，但后续通过不能证明历史根因已关闭。

### 验证、重建与后续方向

集中一次完整 RV64、真实 musl 与 glibc 2.44、1196 条固定 Linux ABI、socket scale
及生产栈检查通过；编译器检查 1892 个函数，最大单帧 2352 字节。
另有原 22 项 loopback、两种 transport 的真实外部内容和兼容 glibc HTTP。
不重跑 iozone、C0–C6 或不相关的存储恢复矩阵，持久化协议本轮未改。
一次汇总源审查完成，设备状态缺口在该次收口中修正，没有多轮独立审查循环。

```sh
make test-virtio-net-host test-lwip-reassembly-host test-ethernet-worker-host
make all
python3 -B tests/network-external.py --transport both
python3 -B tests/network-external.py --transport modern --repeat 3
python3 -B tests/network-external.py --only linux --transport modern --repeat 3
# 旧镜像的 glibc 使用兼容版本身份；独立构建目录避免混用内核。
git switch oscomp-rv-compat
make BUILD_DIR=build/network-compat KERNEL_RV=build/network-compat/kernel all
python3 -B tests/network-external.py --libc glibc --transport both --kernel build/network-compat/kernel
python3 -B tests/network-consumers.py --only boaros --libc both --suite both --case all --kernel build/network-compat/kernel
git switch main
make BUILD_DIR=build/network-cost COST_DIAGNOSTICS=1 KERNEL_RV=build/network-cost/kernel all
python3 -B tests/network-external.py --observe --kernel build/network-cost/kernel
```

输出只在忽略的 build；核对后清理，Git 保存上述可理解结论和可重建负载。
本段记录时完整Harness仍缺kernel-la；当前LA指定矩阵已交付，原盘容器基线
由oscomp-compat独立记录，见[LA模块](../modules/loongarch-boot.md)。本段历史候选
仍由真实应用需求选择：地址/路由与DNS、
更大的离线 C 构建或交互式 shell；TLS 需另核对随机、时间和证书。
窗口、非阻塞发送暂存和接收复制等优化仍按目标应用证据选择，不设固定倍数，
也不让继续优化挡住功能交付。

## 无NIC定时器owner与审查修正（2026-10-05）

协议回调不在IRQ上下文执行：IRQ只标记完成并唤醒owner。此前无NIC时最后的OFD
TIME_WAIT回收依赖用户再次进入syscall顺带推进lwIP定时器，没有后续syscall的最后
一份状态会一直停留。现在`kernel_network_start`在无NIC时也创建一个joinable
timer-only worker（复用同一`struct kernel_network`与progress等待队列），循环调用
`kernel_socket_network_process()`并按min(下一socket期限, now+5×frequency)阻塞；
IRQ/期限只负责唤醒它，停止路径与有NIC一致（禁止新工作、join、释放）。

设备失败打印的快照从设备/队列级扩到槽级：loaned/ready/pending/done、失败队列的
available/consumed/posted、RX used索引与avail-rx/avail-tx，以及每个非空闲RX/TX槽
的状态、长度、age、owner指针，只读驱动自有数组，不分配、不追描述符地址，失败
只打印一次。owner为内核指针，会进入syslog：在固定root、无KASLR、控制台本就可读
的威胁模型下，定位卡住的pbuf比隐藏地址更有价值，按诊断信息接受。

提交审查与自审发现的两个真实问题，作为后续审查的参考模式：

- `riscv_image_va_to_pa` 用 `size > end - va` 做上界检查，在 `va > end` 时减法下溢，
  放行越界地址后再返回 `va - load_offset` 的垃圾物理地址。边界检查必须先排除两端
  之外的值再做减法；sv39 用例现固定未绑定拒绝、va=end 与 va=end+0x1000 的拒绝，
  以及端内换算的正确值。
- `device_failed` 在设备未复位时就 `abandon` 全部在途TX owner（pbuf_free），而设备
  仍可能读取已投递的描述符与payload，这是DMA-after-free。失败不等于DMA停止：归还
  只在stop()复位确认后发生，与RX借用同一策略；host worker模型现要求失败路径只做
  drain(0)、abandon=0，并会在旧行为下失败（旧代码abandon=2）。

```sh
make test-virtio-net-host test-ethernet-worker-host
make test-network-riscv          # linux/boaros 同一ELF内容合同
make test-sv39-riscv             # 含镜像VA->PA越界用例
```

## 完成归还与发送进展（2026-10-05）

旧 worker 在协议重试后才释放 TX_DONE owner。生产 worker 的边界测试设置
64个已完成但未归还的槽，旧实现得到 attempts=1/sent=0/held=0，随后睡眠。
调整为先收割/释放再运行RX及协议，并在最后收割后复核容量代次和RX ready。
容量代次覆盖SG owner释放和复制发送完成，不能只看SG done链。

`make test-ethernet-worker-host test-virtio-net-host`通过：初始已有完成槽时一轮发送；
最后service才出现SG或复制完成时两轮发送、一次yield；对端窗口关闭时一轮后睡眠，
没有发送或yield循环。失败NIC仍推进无关定时器，DMA未停止前不abandon在途owner。
驱动测试核对两种transport的释放返回值、容量代次和重复释放，保留原有DMA/reset矩阵。

`python3 -B tests/network-external.py --transport both --only boaros`在QEMU11.1.1
通过实际TAP双向内容、HTTP和清理检查；无NIC的`network-riscv.py --only boaros --workload timer`
通过。两个TAP运行分别14.523/14.816秒，只有一个副本，属于契约运行时长，
不是与旧版匹配的吞吐结果。RX八帧仍不约束全部协议成本，有界协议服务是后续独立阶段。


## 纯就绪与有界协议服务（2026-10-05）

沿用户确认的短 syscall + 统一后台路线实现，保留单 hart 执行资格与 `tcp_write(COPY)`。
query 不承担协议执行：64 次 poll、16 个无关 socket 的生产函数规模测试，旧实现的包装器
记录 128 次协议调用，当前为 0；COST 门禁也要求 poll 服务数和全局 socket 扫描均为 0。
监听 live 计数由回调维护，失效 child 与 TIME_WAIT 由工作集合回收，查询不消费错误。

独立弱队列区分可运行、NIC、协议池及接收堆等待；socket 销毁前全部解绑。
每次真实容量释放更新代次，失败者只等待之后的新代次。stock lwIP 的 MEMP_AVAILABLE
只在空池变为非空时通知；冻结协议时钟、禁用包与 timer 服务后，先释放一个 segment、
尝试需要两个的写入、再释放第二个，旧通知方式不能恢复可写，新 RELEASED hook 通过。
回调只发布工作，不能在释放栈中直接再次尝试发送。250 ms 仍是未知资源变化的兜底，
不把它作为用户 poll/epoll/阻塞 I/O 的额外唤醒期限。

服务分别限制 RX 八帧、loopback 八包、socket 八个单元、一个 timer 回调。
容量转交、重试期限、实际 socket 工作轮转；三个资源等待集合也轮转。完整 raw 调用
返回后才检查预算，单个 timer callback 可能处理多个协议对象，预算不是固定 CPU 时间。
执行资格记录当前 I/O owner 并抑制可能进入块 I/O 的堆回收；用户复制与睡眠不持资格。
批次之间开放中断并调度；NIC 失败和无 NIC 均继续同一软件服务。关闭窗口的 unsent
不自动重新排队，睡前重新检查容量、设备 ready 与软件工作，避免无进展忙等。

本地依据 `references/lwip` 2.2.1 commit
`77dcd25a72509eb83f72b033d219b1d40cd8eb95` 的 `tcp_out.c`、`memp.c`、
`netif.c`、`timeouts.c`。本地补丁逐项登记于第三方文档，原无预算 API 保持兼容包装。
最终发布内核 SHA-256 `bcea1a6972abe6589eaabf6e95f798baec62387d68b8cb51a0693274a8a03868`，
QEMU 11.1.1：lwIP/重组/worker/epoll 宿主测试、真实 RV64 scale、COST growth/poll 门禁、
完整 userland 和栈预算通过。网络 contract 与 epoll fault 用同 RV64 ELF 在固定 Linux
`f4cdf7ca9a1fdcca413157df19753f388a5a224e` 及 BoarOS 通过；无 NIC timer 和内容通过。
TAP legacy/modern 各单次应用与清理通过，程序 17.138/18.535 秒，仅为本轮组合验收，
并行运行其他测试且未匹配三次基线，不能解释为吞吐改善或回退。重建命令：

```sh
make all test-scale-riscv test-ethernet-worker-host test-lwip-host test-lwip-reassembly-host
make test-epoll-host test-epoll-riscv test-userland-riscv test-cache-growth-riscv test-cost-host test-stack-usage
python3 -B tests/network-riscv.py --workload contract
python3 -B tests/network-riscv.py --only boaros --workload timer
python3 -B tests/network-riscv.py --only boaros --workload content
python3 -B tests/network-external.py --only boaros --transport both --repeat 1
```

发送 reservation、复制量优化与 TCP 预算比较仍在后续独立阶段；本阶段不声明其完成。


## TCP接纳预算与复制（2026-10-05）

先用同 RV64 ELF 对照固定 Linux，发现旧路径对不可读但数值合法的 payload 总是先
EFAULT：新建 TCP、SHUT_WR、发送缓冲已满都遮蔽了实际 EPIPE/EAGAIN。依据
`references/linux/net/socket.c::__sys_sendto`、`lib/iov_iter.c::import_ubuf`、
`net/ipv4/tcp.c::tcp_sendmsg_locked`，固定 commit
`f4cdf7ca9a1fdcca413157df19753f388a5a224e`，数值范围/头导入在前，协议状态和
内存接纳判断在实际 payload 读取前。send 的范围错误甚至先于坏 fd；普通 write
仍先检查 fd。这些区别由新增 admission 契约保护，未把数值检查当作缺页不会失败的保证。

在原任务 write request 上扩展 stream reservation 与进展位，不新增 task 指针槽。
请求持 OFD pin，容量来自 sndbuf、发送预算及已有 reservation；协议池和 NIC 独立。
copy 允许睡眠，提交前重新检查连接和真实资源。失败、取消、退出归还未提交预算；
短阻塞提交保留暂存后缀。只保留 `tcp_write(COPY)`，不把可复用 scratch 借到 ACK。
COST 将预先无容量、复制后协议失败及零进展失败的新复制字节分开，原 stream_copy
仍是实际总复制。scale 保留整个接纳量后再 write 4 KiB，页解析和 stream_copy 都为 0；
另验证两个请求竞争、abort 归还和恢复资格，不按内部链布局断言。

扩展实际阻塞程序后另发现旧控制流的部分成功错误：已有 15972 字节进展后本地
shutdown，虽然返回正值却发了 SIGPIPE；固定 Linux 不发。请求进展位使后续错误
返回已接受前缀，不消费待观察错误或发 SIGPIPE。真实对端关闭未读队列产生 RST
后，下一次 send 观察 ECONNRESET，再下一次才 EPIPE，双方通过。不同协议预算
导致的正前缀长度不作为逐值差分项。跨页 fault 在本输入上 Linux 返回 EFAULT，
BoarOS 返回已接纳 4096 字节；分别核对无额外字节和返回/接收守恒，保留 BoarOS
已有页内接纳策略，不把该测试写成返回值完全相同。

真实 RV64 io-sleep 在复制边界调度另一任务，验证 reservation 仍在任务登记、
close/fd 复用保持目标 pin、shutdown 后 EPIPE、撤销用户页后 EFAULT 并可重新预留。
最初给裸地址 fixture 加 TCP 时，lwIP 静态表的高半区指针不可访问；已在初始化
调度器前映射双地址别名，不能在运行中单改 satp 破坏调度器地址空间不变量。
这仅调整测试启动映射，生产映射不变。legacy/modern × writeback/writethrough
四组 I/O 暂扣矩阵通过，原收包 reservation、pipe、存储进展门禁保持。

本阶段发布内核 SHA-256 `d9274411b022ff29b1819d0be8a1f46a0f27cf3de7adc7083c2883a90e2e6a71`，
QEMU 11.1.1。发布构建通过新增同 ELF admission（包括实际阻塞 sender SIGKILL）、网络 contract/content、
UNIX budget、完整 musl userland；glibc 2.44 五种形态与固定 Linux 通过。
现有差分 suite 为 1344 条逐值一致，独立 admission 不混入这个数量。
scale、诊断 growth/admission/COST 和编译栈上界通过；此阶段未重测匹配吞吐。

```sh
make all test-scale-riscv test-cache-growth-riscv test-cost-host
python3 -B tests/network-riscv.py --workload admission
python3 -B tests/network-riscv.py --workload contract
python3 -B tests/network-riscv.py --only boaros --workload content
python3 -B tests/network-riscv.py --only boaros --workload budget
make test-io-sleep-riscv test-userland-riscv test-glibc-riscv test-diff-abi-riscv test-stack-usage
```

该阶段没有扩大窗口、pbuf/segment、NIC 或块队列默认值；后续批量读/写及 TCP 预算已完成匹配测量，见文末收口。


## 资源分类与 TIME_WAIT 借用的审查修复（2026-10-06）

有界服务独立审查发现两个遗漏。复制回退 `send_copy(-EAGAIN)` 未像 SG 分支一样
登记 NIC blocked，导致进入错误的协议池等待集合；完成归还 NIC 容量后仍需额外事件。
另一项是 TIME_WAIT 的 socket 借用解除仍依赖普通工作预算，而 lwIP 容量回收和
慢定时器可以无普通 err 回调地释放 PCB。固定依据为 `references/lwip/src/core/tcp.c`
`77dcd25a72509eb83f72b033d219b1d40cd8eb95`；容量回收还会在 free 前把状态改成 CLOSED。

复制回退现对两种回退原因统一发布 NIC 容量等待。两条 TIME_WAIT 释放路径在回收
之前通知本层，配对 callback_arg/errf 并核对 PCB 身份后清借用、清回调和摘等待项；
不重入 raw API、不分配、不睡眠、不在该栈销毁 socket，也不增加全局扫描。

实际 socket/Ethernet/lwIP 的 host 模型先在旧代码失败，修复后五组通过：未协商
indirect 与 SG 不适用的复制容量归还、真实堆 socket 的 TIME_WAIT 池回收与定时
到期，以及已销毁/非 socket opaque owner 控制组。复制用例冻结时钟并禁止包/timer
服务，确认协议池代次没变，仅 NIC 归还即可继续 TCP 输出。TIME_WAIT 由真实握手
与半关闭形成，普通 socket 关闭工作尚未服务；不能用无 owner raw PCB 代替此门禁。
ASan/UBSan 及独立复审均通过，最终堆、PCB、segment 和协议内存回到基线。

独立 RV64 内核 SHA-256
`3b89b53663e52b211dfc9aea9c7b4959d3b20fbad36b947033488eff69ee1f6f` 的真实 U-mode
contract/admission/timer 与 scale 通过；poll 服务计数仍为零。关闭 indirect 的
legacy/modern × 双向 TAP 五 bulk 加一控制连接四次功能烟测也通过，均满足
`tx-copy>0`、`tx-sg=0`、`errors=0`；该层检查内容与整合，槽满的因果关系由上述
host 门禁证明。烟测不计入发布吞吐。可先重建窄门禁：

```sh
python3 -B tests/host/network_owner.py --sanitize
```


## 最终预算与协调器复核（2026-10-06）

固定生产源码 faf8e63 的 TCP 27 组候选、新 ELF 的 702 次发布启动及独立诊断已完成，
见[完整指标与每连接完成时间](data-path-budget-experiments.md#正式匹配结果2026-10-06)。
默认 8/1/1 在部分吞吐负载回退；近池的 8/4/2 避免实测 segment/heap 饱和后重复复制，
恢复接近旧基线的吞吐并降低控制尾延迟。它不是通用最优或硬控制流预留，默认未变。

扩大验证曾在固定 Linux 的 RR 五连接超时：两轮屏障共用 gate，快客户端能取走慢
客户端的上一轮 token。实际 workload 的到达偏斜 host 测试先稳定失败，再以独立
管道修复；重新冻结 ELF 后全套网络筛选/扩展完成，旧 ELF 数字不混入最终表。
QEMU 11.1.1 的实际 echo RTT、诊断扰动、TX 软件收割到归还及同槽再用的范围独立
报告；1/10 ms netem 在 host 创建 qdisc 时被拒绝，未启动客体，不推断远程网络收益。
