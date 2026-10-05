# 单 hart 网络模块

## 入口与对象

`kernel/syscall/socket.c` 导入 RV64 Linux socket 参数和用户指针，`fs/files/socket.c` 把 socket 作为普通 fd/OFD 安装并在失败时回滚，`net/socket.c` 持有 endpoint、数据包、待 accept 队列及等待队列。`fs/open_file.c` 在最后一个真实 OFD 引用消失时销毁 socket；dup、fork 和 syscall 期间的 pin 共享同一 endpoint，close/exec/退出均沿既有 fd 生命周期回收。`fs/files/io.c` 把普通 read/write/readv/writev 接到 socket 队列，pread/pwrite/lseek 返回 `ESPIPE`，`fstat` 标识 `S_IFSOCK`，poll/select/epoll 读取 socket 就绪和等待队列。

协议实现来自官方 lwIP `STABLE-2_2_1_RELEASE` raw API，位于 `third_party/lwip/`；固定资料和许可证见 `references/README.md`、`references/sources.tsv` 与 `third_party/lwip/COPYING`。IPv4 重组有本地所有权和边界修补，具体范围见[第三方组件](../third-party.md)。本地移植层是 `net/lwip_port/`，使用 `NO_SYS=1`；loopback 与物理接口共用协议池。lwIP 协议、segment 和 pbuf 使用静态有界池（UDP PCB 16、TCP active 32、listen 16、segment 128、pbuf 64）；BoarOS socket、接收/accept 队列节点及 OFD 使用 kernel_heap。PCB 池耗尽映射 `ENOMEM`，创建失败不留下 OFD；分配器 `STATE` 和错误释放是 fatal 不变量。

IPv4 重组键包括源/目的地址、IP ID、协议号和输入 netif 身份。已完整覆盖的重复范围只丢弃新输入；部分重叠、冲突终点或越过已知终点丢弃整个对象。判重与边界校验先于资源淘汰，重复输入不能挤掉正在重组的有效数据。对象借用 netif 身份，不延长设备生命；移除接口前调用 `ip4_reass_cleanup_netif()`，释放该接口的所有重组引用。最后一个 pbuf 引用释放后，custom 回调才归还 DMA 缓冲。IPv4 头选项仍不支持。宿主反例和所有权检查由 `make test-lwip-reassembly-host` 重建，包含普通/乱序、重复/部分重叠、协议/接口隔离、终点冲突、预算、年龄和实际 fragmenter 的最大 UDP 内容；宿主结果不代替真实网卡验收。

改变 MF 的重复范围也不直接触发交付：即使已有数据覆盖整个声明长度，新输入仍被
判为重复，原引用保持到协议期限；超时后同一键可以重新使用。这与固定 Linux
`net/ipv4/ip_fragment.c` 先记录终点、再在 DUP 分支释放新片的处理顺序一致，
不把未接纳的终片当作完整内容已交付。`duplicate_final_timeout` 覆盖持有、超时
归还和槽复用；资源始终受重组预算约束。

## 已验收 ABI 与等待

当前支持 `AF_INET`/`AF_INET6` 的 `SOCK_DGRAM`/`SOCK_STREAM`，`SOCK_CLOEXEC`、`SOCK_NONBLOCK`，UDP bind/getsockname/sendto/recvfrom 和 `SO_RCVTIMEO`，TCP bind/listen/connect/accept，以及连接后的普通读写和就绪。`ioctl(SIOCGIFFLAGS/SIOCSIFFLAGS)` 让真实用户程序启用 `lo`；接口对象从公开 `netif_list` 查找。地址在 syscall 边界使用 RV64 `sockaddr_in`/`sockaddr_in6` 布局；内核地址携带族、16 字节网络序地址、宿主序端口和 scope。官方 loopif 提供 `::1`；IPv6 通配监听默认接收 IPv4，accept 返回映射地址。V6ONLY 在绑定前生效，双栈通配与纯 IPv6/IPv4 的端口交集由非持引用的 endpoint 登记补齐 lwIP TCP bind 的 ANY 检查缺口。登记在 OFD 销毁前摘除。非阻塞、坏 fd/地址/指针和协议错误由固定 Linux 同一 ELF 差分约束。未覆盖的地址族、选项和操作返回明确 errno，不伪造成功。

单 hart 下，就绪订阅与睡眠沿已有短临界区。`kernel_socket_poll` 是局部只读快照，不推进协议、不清理 accept、不解除写重试、不消费错误。监听就绪由接纳/失效回调维护 live 数；失效 child 的销毁由待工作集合处理。TIME_WAIT 可由工作批次提前解绑，在协议释放边界仍同步确保借用撤销。poll/epoll 与阻塞 syscall 只订阅对象通知和调用者期限。

短 syscall 在完整 raw 调用退出后可执行有界服务；网络 worker 与无 NIC worker 使用同一 `kernel_socket_service_pending`。每批分别限制 RX 八帧、loopback 八包、socket 八个工作单元及一个 timer 回调。socket 单元包含容量转交、到期重试、实际对象工作，三类轮转；协议池、NIC、接收堆等待集合也轮转。零预算不消费对应工作，返回值报告处理量、是否可立即继续及下一期限。unsent 等待远端窗口或 ACK 不会自行重新入队；预算耗尽且仍有工作时开放中断并让出，睡前再次查软件工作与设备完成。

`kernel_socket_protocol_enter/leave` 记录当前任务的执行资格，禁止 raw 调用期间堆回收进入可睡眠 I/O；单 hart SIE 仍负责串行化。用户复制、设备等待和调度不持此资格，回调仅发布弱引用工作。socket 销毁前解绑所有队列；NIC 完成、每次 segment/pbuf/协议堆归还、接收堆释放、loopback 入队和 timer 到期各自发布通知。失败 NIC 不影响 loopback 和协议期限。无 NIC 运行期 IRQ 只唤醒 worker；尚未建立 worker 的模块启动边界保留无堆分配的有限 timer 入口。这不是 SMP 锁或硬实时上界，单个 raw 调用和单个 timer 回调仍完整执行。

非阻塞 connect 短路径推进握手，后台保证调用者不再调用 socket 时仍能进展；阻塞 connect 等待握手结果。信号沿既有 syscall restart 协议，带接收超时的中断返回 `EINTR`。未 listen 的 stream 和 datagram accept 立即返回类型对应错误，监听 socket 的 `SO_RCVTIMEO` 约束阻塞 accept。

普通 socket read/readv 使用“预留队首片段→按偏移复制→提交/取消”。用户容量决定 UDP 的截断长度，一个报文可经请求持有的 4 KiB 页反复复制，最终只消费一次；不能以内部暂存容量截断报文。零长度 datagram 也必须完成 reservation；TCP EOF 没有 reservation。TCP 在用户容量内继续读取已排队片段，取得进展后不等待新数据。每段完整复制后才消费；当前段 fault 保留整段，返回先前已提交的字节数，没有先前进展则 EFAULT。UDP fault 丢弃当前 datagram。read reservation 独占队首，第二个 read/recvfrom 不得越过它。

请求 scratch 页由任务持有并在多次调用间复用（首次使用时分配），调用期间登记在当前任务；分配失败返回 ENOMEM，不预留或消费队首。正常返回解除登记，物理页保留至任务最终存储释放；强制退出先取消 read reservation／释放其 OFD pin，再解除登记，之后才释放任务文件表和栈。没有用户任务的模块测试按调用分配并沿正常返回路径释放。socket 普通 write/writev 同样使用请求页，按用户页和协议剩余空间提交；不再每 64 字节调用 tcp_write。`kernel_socket_get_statistics` 提供单 hart 累计 tcp_write 调用及成功复制字节数，`kernel_uaccess_page_resolutions` 记录用户页解析尝试。计数没有新增用户 ABI。

`recvfrom` 的缓冲区范围在等待空 UDP socket 前检查，负 socklen_t 返回 EINVAL；accept/recvfrom 的地址输出错误发生在协议 dequeue 后，与固定 Linux 顺序一致。

生产收发入口沿完整临时包或队首reservation持有请求与OFD pin。线程组强制退出
先标记/唤醒，保存的syscall栈返回后才在user-return处理终止；正常和取消路径
均须平衡临时owner。以后若允许直接抛弃内核调用栈，必须重新审计这条契约。

TCP `POLLOUT` 同时要求发送缓冲、队列空间及未被真实 `ERR_MEM` 阻塞。失败后登记协议池等待及 250 ms 兜底；每次真实资源归还更新容量代次，仅恢复旧代次等待者，避免失败分配自行释放资源造成忙等。不能仅采用 lwIP 的“空池变为非空”通知：多 segment 请求可能在空闲池已有一个元素时仍失败，第二个元素释放必须促成进展。期限只由后台服务推进，等待 syscall 不再为协议设置兜底轮询。`F_SETFL(F_GETFL|O_NONBLOCK)` 接受已有 access mode 位，只更新可变状态位。

关闭活动 TCP 连接先解绑全部指向 BoarOS socket 的回调，再 `tcp_close`；协议 FIN/TIME_WAIT 可能暂占静态 PCB/segment 池，随后由定时器回收。这与内核堆对象生命周期分开。

`socket->tcp` 是对 lwIP PCB 的借用，不延长协议对象寿命。普通工作批次可提前
解绑 TIME_WAIT，但预算不足时不能把生命周期责任留到下一批。固定 lwIP 的
`tcp_alloc()` 会在 PCB 池满时通过 `tcp_kill_timewait()` / `tcp_abort()` 回收
TIME_WAIT；该分支不执行普通错误回调，并且 `tcp_pcb_remove()` 会把状态改为
CLOSED。当前在该分支 remove 之前，以及慢定时器从 TIME_WAIT 链摘除对象后、
free 之前调用本地可选 `LWIP_HOOK_TCP_TIMEWAIT_FREE`。socket 层仅接受
`callback_arg` 与自身错误回调配对的 PCB，验证借用身份后清空借用及回调、撤销
容量/重试等待并唤醒。hook 不分配、不睡眠、不重新调用 raw API、不销毁堆
socket，也不遍历全局 socket registry。OFD 销毁仍先清回调，再释放 socket，
因此稍后的 TIME_WAIT 回收不会回调已经销毁的 owner。

固定依据为 `references/lwip/src/core/tcp.c`，commit
`77dcd25a72509eb83f72b033d219b1d40cd8eb95` 的 `tcp_abandon()`、
`tcp_pcb_remove()`、`tcp_slowtmr()` 与 `tcp_alloc()`；生产本地补丁位于
`third_party/lwip/src/core/tcp.c`。`python3 -B tests/host/network_owner.py --sanitize`
连接实际 socket、Ethernet 与 lwIP，使用真实堆 socket 完成握手和半关闭，并在
关闭工作尚未服务时验证容量回收及 timer 到期均清空借用；同时检查非 socket
raw PCB 和已经销毁的 socket，不用无 owner 的 raw PCB 代替主要所有权用例。

支持 `AF_UNIX` (domain=1) 的 `socketpair(199)` 系统调用，支持 `SOCK_STREAM` 和 `SOCK_DGRAM` 类型以及 `SOCK_CLOEXEC`、`SOCK_NONBLOCK`。`kernel_files_socketpair_create` 保证双向 OFD 的原子分配与双 fd 安装，失败时完整回滚不泄露 fd 或 OFD。两个 endpoint 在内核中互相绑定 peer；流和数据报在接收端堆上排队，每个 socket 拥有 64 KiB 独立接收缓冲配额（超出时返回 `-EAGAIN` 并在接收端读取后唤醒对端写者）。向已关闭或断开的对端写入向调用任务产生 `SIGPIPE` 并返回 `-EPIPE`；读取已关闭对端返回 0 (EOF)；`SOCK_DGRAM` 将一次 write/writev 聚合为一条消息，64 KiB 上限之外返回 EMSGSIZE；用户复制全部成功后才移动整包 owner，fault/OOM/取消不发布前缀。队列按 `max(length, 1)` 收取预算，零长度消息可入队。容量不足时等待整条消息可容纳，不以普通 POLLOUT 作为重试条件；短读、复制 fault 或销毁释放整包及其全部预算。发送请求登记在任务上，退出前撤销临时 packet 与 OFD pin。poll/ppoll/epoll 准确反映对端关闭时的 `POLLHUP`/`POLLIN` 就绪。命名 AF_UNIX 端点、SCM_RIGHTS 凭据传递、带 ancillary 的 sendmsg/recvmsg、更多 sockopt、外部 IPv6 与 SMP 并发仍在 `docs/goals.md`，不能由本切片推出。

## 验证

```sh
make test-lwip-host test-lwip-reassembly-host test-ethernet-worker-host
make test-network-riscv         # IPv6 UDP/TCP、双栈 accept，同一 ELF 对照 Linux
make test-scale-riscv
make test-syscall-riscv test-userland-riscv test-diff-abi-riscv test-stack-usage
python3 tests/program-inventory/run.py --suite libc \
  --case libc.static.socket --case libc.dynamic.socket \
  --require-pass --output build/socket-program-check
```

host 入口实测 UDP loopback、PCB 16 个用尽后第 17 个失败、全部释放和再分配，另在 `sndbuf>0` 时耗尽全局 TCP segment 池触发真实 `tcp_write ERR_MEM`，以及 TCP 握手/关闭后推进 200 秒协议计时、池用量回到基线。此前的 Linux/BoarOS 差分记录完全一致，包含 12 条新增 socketpair 差分记录，覆盖坏族、坏标志、坏协议、空指针、stream 双向读写、关闭 EOF、dgram 边界截断与 flags 校验。真实 pthread U-mode 另覆盖零长度 datagram、共享 socket 双读、阻塞读时 close/fd 复用、双读线程组 SIGKILL、全局池压力下错误可写事件及释放后写入进展。原版 hackbench 原 ELF 在 4 进程模式下传递消息并成功运行（Time: 0.014s），关机检查 `heap-live=0`。原版 libc-test `functional/socket.c` 的静态、动态直接 entry 用未改源码和同一 ELF 在双方通过；整合内核全量 228 项为 227 pass、1 BusyBox 包装失败，见[程序清单](../learning/user-program-inventory.md)。

Linux ABI 依据本地 `references/linux/net/socket.c`、`net/ipv4/af_inet.c`、`fs/read_write.c`，固定 Linux v7.2；测试输入来自 `references/oscomp-testsuits` 固定 pre-2025 版本，精确身份在来源清单和机器归档。核对后运行 `make prune-build` 清理日志和镜像。

规模回归补充 0/255/256/257/1500/4096/8192 字节 UDP 的三种入口、完整与不足容量、空 iovec、后继报文和跨页 fault；TCP 用错位缓冲与向量写累计传输 1 MiB，检查内容、非 256 字节接收和跨片段 fault 的字节守恒；`socketpair_scale`/`unix_datagram_budget` 及真实 pthread 补充 AF_UNIX 向量/跨页、零长度、64 KiB 上限、整包 OOM、满队列阻塞/非阻塞、fault 无前缀、截断/接收 fault 后预算复用与取消；固定 Linux 差分累计 1086 条一致。`socketpair_scale` 覆盖 STREAM 全双工读写、对端关闭 EOF、写入关闭对端返回 EPIPE 以及 DGRAM 数据报截断边界。成本与边界见[单核规模回归](../learning/single-hart-scale.md)。

本轮选项阶段接入 getpeername(205)、getsockopt(209)，以及 REUSEADDR、TYPE、
ACCEPTCONN、ERROR、KEEPALIVE、收发预算/超时、TCP_NODELAY、TCP_MAXSEG 和
IPV6_V6ONLY。SO_ERROR 读取清除待观察错误，连接阶段 RST 返回 ECONNREFUSED；
已建立 RST 保留 ECONNRESET。收发预算默认/上限 64 KiB，设置按 Linux 倍增，
固定 RV64 最小值分别为 4608/2304；不预分配。接收队列和 TCP 发送接受量执行
预算，lwIP 窗口和全局池仍可更紧。UDP connect 设置/重置默认对端并由协议过滤。
监听启用官方 backlog 计量：待 accept child 保留资格，accept/abort 归还。
原版应用与完整组合的实测结论见学习记录；旧 glibc 使用兼容配置。

## 流发送、整包与半关闭

send/recv 的 DONTWAIT、NOSIGNAL 只约束一次调用，不改变共享 OFD。普通 writev
与 sendmsg 共用内核向量接口；INET UDP 在完整复制向量后才构造协议 pbuf，一次
调用发送一条数据报。复制 fault/OOM/取消不发布前缀。UNIX 仍按整条配额等待，
现在使用同一次调用的 SO_SNDTIMEO 期限。recvfrom/recvmsg 共用队首 reservation，
空向量的 datagram receive 也消费一条；MSG_TRUNC 输出与完整长度返回分别处理。
recvmsg 不生成未请求的控制消息；非空 ancillary 发送明确不支持。

阻塞 stream 发送遇到暂时的窗口/池压力时继续等待剩余字节，非阻塞、fault、信号
和超时可以返回已接受前缀。期限不会因每次重试延长。原 netperf 把短发送视为定时
结束，因此仅验证退出码会漏掉 0.00 秒就结束的无效 TCP_STREAM；小发送预算下
一次 64 KiB 阻塞发送是独立回归，检查接收内容和完整进展。

SHUT_RD 保留已排队内容，空队列返回 EOF；SHUT_WR 让已接受 TCP 内容先发送，
FIN 在其后，读方向仍能接收反向响应。dup/fork 共享方向状态。UNIX 的对端关闭
接收方向使发送 EPIPE/SIGPIPE，NOSIGNAL 只抑制该调用的信号。FIN、reset 和本地
关闭分开决定 IN/OUT/HUP/RDHUP。SYN 拒绝是 ECONNREFUSED，建立后 reset 是
ECONNRESET；SO_ERROR 或读/写消费 pending error，后续 EOF/EPIPE 由真实协议
终止状态决定，不能永久重复历史错误。

accept callback 延迟释放 backlog 资格，真正 accept 时归还；失败安装销毁 child，
reset 排队 child 在下次检查摘除。监听器的预算、超时和选项传给 child，NODELAY
同时设置实际 PCB。TIME_WAIT 在移交协议池前摘除所有堆回调；LAST_ACK 的
ERR_CLSD 解除 PCB 借用。运行期 timer IRQ 只通知后台 owner；最后 OFD 关闭后的协议回收不依赖另一个用户 syscall。

模块启动的 IRQ fallback 中，TCP refused-data 只保留 pbuf 并登记重试，不申请内核堆；运行期由 worker 处理接收堆归还或到期重试。scale 的独立堆包装器保留首次接收 OOM/IRQ 零堆分配回归。
UDP 分别记录显式地址与非零端口绑定，AF_UNSPEC 断开释放自动端口；TCP
MSG_TRUNC 沿 reservation 消费而跳过 scratch/usercopy，UDP 和 UNIX 保持各自语义。

UDP 排队同时受每 socket 配额与共享协议堆约束。队列不能耗尽给下一条最大报文
及 TCP 控制所需的余量；过载按 UDP 语义丢包并归还 pbuf。五条不限速 UDP 原负载
曾先耗尽池，修复后控制连接仍进展；丢包量必须连同吞吐报告。默认/最大预算仍
64 KiB，未扩大池或伪造 POLLOUT。pthread 压力回归现在保护 UDP 过载时 TCP 控制
仍能传输；raw TCP segment ERR_MEM 的重试契约继续由 host fixture 验证。

窄验证：`python3 -B tests/network-riscv.py` 同 ELF 对照 Linux；固定内容用
`--workload content`，包括 16 MiB、五连接各 8 MiB、IPv6 UDP 一万次 64 字节
请求响应。原 ELF 执行入口为 `tests/network-consumers.py`，按原客户端参数、真实
服务端 ready、逐项有效时间/接收量及 wait status 判定；原脚本退出零不替代传输。


## 原版应用与诊断入口

原 ELF 输入来自公共镜像，版本为 iperf 3.13、netperf 2.7.0。执行器保存实际
argv、工作目录、接收端结果、wait status、时间及机器输入身份。旧 glibc 必须
在 oscomp-rv-compat 运行；main 不改 uname。原脚本与每项新服务端的受控流程
分开验收，listener ready 使用实际输出握手，客户端参数保持原值。

```sh
python3 -B tests/network-consumers.py --libc both --suite both --case all
python3 -B tests/network-consumers.py --only boaros --libc both --suite both --case script
python3 -B tests/network-consumers.py --only boaros --libc both --suite both --case representative --repeat 3
make COST_DIAGNOSTICS=1 all
python3 -B tests/network-consumers.py --only boaros --libc both --suite both --case representative --kernel build/cost/kernel-rv --observe
```

只有 COST 构建提供只读 `/proc/boaros_net_stats`，复用现有协议统计，不保存对象
引用。TCP写入调用/字节为64位，lwIP包/错误计数为16位，会环绕，不能当作大流量
窗口的完整总包数；pool用量和高水位是即时值。窗口仍使用已有 cost v1 schema，
聚合64794字节、每任务64字节，默认构建没有新增诊断节点。观测影响吞吐，关闭
观测的三次分布才是性能结果。此前纠错轮1179条ABI、完整RV64、真实musl/glibc 2.44、
socket scale和栈检查通过；修复后的用户态契约覆盖reset、accept输出fault、
TCP MSG_TRUNC、UDP自动端口释放，IRQ重试由独立heap包装器保护。

结果与限制见[网络记录](../learning/network-ownership.md#原版网络应用交付2026-10-02)。
该段是此前loopback验收；现已接入VirtIO-net，仍没有命名AF_UNIX、SCM_RIGHTS或TCP_INFO，相关应用字段不作为已验证统计。

## 接收状态纠错（2026-10-02）

新建未连接INET stream的read/recv立即返回ENOTCONN，poll报告OUT/HUP（完整请求另有WRNORM）；连接中仍按
连接进展等待，已建立连接保留队首数据优先和错误/FIN/reset后的终止语义。
read/readv零长度仍直接返回；recv/recvmsg零长度检查协议状态：空已连接socket
非阻塞返回EAGAIN，有数据返回0且不消费，EOF返回0。零容量请求无需scratch页。
内部空队列状态查询不消费错误，接收入口才交付pending_error。
同ELF反例由`tests/workloads/network/contract.c`保护，差分新增fresh IPv4/IPv6记录；
固定依据为`references/linux/net/ipv4/tcp.c`和`include/net/sock.h`（Linux v7.2）。

UNIX DGRAM自身SHUT_RD后，即使空队列也有IN/RDNORM和RDHUP；双向关闭另有HUP。
活动read reservation仍排除第二个数据消费者，方向事件不提前解除reservation。
真实U-mode反例核对poll/select/epoll、dup共享方向和关闭前排队内容。空已关闭
数据报的阻塞接收返回0，O_NONBLOCK/MSG_DONTWAIT返回EAGAIN；就绪掩码仍有IN。
收到EAGAIN后的非阻塞调用不能因poll可读而重新循环。固定Linux和BoarOS现均通过；
依据为固定Linux的`net/unix/af_unix.c`和`net/core/datagram.c`。

扩大实际SO_RCVBUF预算会在单hart保护区内通知UNIX对端；STREAM和DGRAM发送者
醒来后仍重查容量、关闭和错误，不延长peer引用。相同/缩小预算不通知对端。
独立U-mode预算入口：`python3 -B tests/network-riscv.py --only boaros --workload budget`。
该测试先握手并从proc确认发送者已阻塞，再核对扩容、缩容、对端关闭、SIGKILL和内容。
它保护BoarOS的接收端字节预算，不把Linux的SO_RCVBUF当成相同排队模型。

流发送的请求页持有有效内容和已发送游标：EAGAIN后等待及部分发送后继续消费暂存尾部，
不再次读取同一段用户数据。非阻塞/fault/信号/期限仍返回已接受前缀；不借用用户页到ACK。
预算fixture的COST窗口独立核对全阻塞和部分发送各1000字节只有1000字节stream usercopy，
旧全阻塞实现为2000字节。`--workload content`校验单TCP16MiB、五TCP各8MiB及UDP内容。
游标不跨系统调用保留内容。原iperf使用非阻塞socket，短写/EAGAIN后再次write仍可能
重复暂存未接受字节；三次关闭观测的单TCP仅改善0.4%/2.2%，不宣称解决主要吞吐差距。
结果、前台复制放大与代价见[本轮网络结果](../learning/network-ownership.md#本轮应用结果与剩余复制2026-10-02)。

实际阻塞接收等待使用`kernel_socket_receive_ready`，活动reservation始终未就绪；
对外poll仍保留终止事件。HUP/ERR不能让第二读者绕过owner并空转。io-sleep fixture
暂扣真实usercopy调用，确认第二读者阻塞；finish/fault后唤醒，期限和信号可打断等待，
最后页数回到基线。四种transport/cache均保护这一交错，不新增用户ABI或长寿命引用。

## Ethernet与真实宿主应用（2026-10-02）

根启动对象持有独立VirtIO-net、netif与joinable worker，DTB发现device ID和IRQ。
官方Ethernet入口接纳custom pbuf，启用ARP及48 pbuf/8对象的IPv4重组；MTU1500、
MAC来自设备、静态地址由构建配置决定。RX loan直到最后协议/reservation引用释放；
回退与UDP接纳保留控制余量。IRQ只收割，raw API在单hart临界区串行；worker每批
八帧，失败NIC也继续共享loopback与协议定时器。无NIC时同一owner退化为
timer-only worker：推进协议定时器并按min(下一socket期限, now+5×frequency)
睡眠，最后的OFD定时回收不再依赖用户再次进入syscall。停止join、清理接口引用、
reset确认；设备失败不等于DMA停止，TX在途owner与RX借用一律保留到reset确认，
已完成owner在worker归还。接口查询来自真实netif，DOWN撤下RUNNING。
详细预算、寄存器、失败与验证命令见[网卡模块](riscv-virtio-net.md)。

两种transport与两种libc的原wget/httpd均完成双向GET、CGI上传与文本POST。
固定单/五TCP及UDP内容、压力下TCP进展、块/RNG混合IRQ、最终heap live=0已核对；
完整1196 ABI、RV64、真实libc、scale和栈检查通过。TX 已用 indirect 表做零拷贝
（每包最多两段镜像内 payload，其余回退复制），用户接收复制仍在；没有外部IPv6、
DHCP/DNS/TLS或默认网关。结果、效率、
程序与最终卸载计时边界见[真实网卡记录](../learning/network-ownership.md#真实-virtio-net-与宿主应用交付2026-10-02)。

TX 的 SG 与复制回退在槽满时都登记 NIC 容量等待。无 indirect 特性或 pbuf 链
不满足 SG 条件时，复制发送返回 `EAGAIN` 仍属于 NIC 槽不足，不能落入协议池
等待。上述 `network_owner.py` 分别覆盖两种回退原因，冻结时钟、禁用包与 timer
服务并确认协议池没有归还，仅通知 NIC 新容量就能重新输出实际 TCP 内容；
该边界测试不把正常 timer 重传当作容量通知正确的证据。


纯查询及预算回归位于 `tests/riscv/scale_main.c`：16 个无关 socket 下的 64 次 poll 没有协议调用；零/一单元服务遵守预算并最终交付完整内容。协议池回归冻结时钟且禁用包与 timer 服务，证明“空闲 segment 已有一个，再释放一个”足以恢复真正等待者。`make test-cache-growth-riscv` 的 COST 构建同时核对 poll 触发服务数和全局扫描数均为零；全局 registry 只用于 bind 冲突、销毁解绑与设备故障。重建与证据层次见[网络记录](../learning/network-ownership.md#纯就绪与有界协议服务2026-10-05)。


## TCP 发送接纳与复制（2026-10-05）

INET stream 的 write/writev/send/sendmsg 使用任务登记的 `kernel_socket_write_request`，
与数据报请求共用退出登记但不拥有数据报 packet。请求持有原 OFD pin，
`stream_reserve/commit/cancel` 只预留本 socket 的 TCP 接纳字节，按 sndbuf、socket 预算、
队列空间及已经预留量决定上限，不预占 segment、pbuf 或 NIC 槽。其他 writer、sendfile
及 POLLOUT 都扣除现存预留；SO_SNDBUF 缩小或连接状态变化后，commit 再检查真实状态。
用户复制可缺页睡眠，不持 raw 资格；取消、fault、错误、返回和任务退出均归还未提交预算。

扩展现有 iovec cursor 提供页内有界 span。先验证参数的数值范围及连接状态，再取得接纳量，
最后才取得任务 scratch 并复制 payload；预先无容量不解析用户页。协议资源可能在复制时变化，
此时仍可能发生有界的零进度失败。阻塞调用保留暂存后缀，重取预算后继续提交，
不再次复制后缀；非阻塞返回已接受前缀，未接受用户内容不跨调用持有。仍用
`tcp_write(COPY)`，scratch 不借给 TCP 到 ACK。本阶段不改变 UNIX stream 和数据报的缓冲策略。

固定 Linux 的 `net/socket.c` 和 `net/ipv4/tcp.c` 约束状态与 payload fault 顺序：
数值越界地址先 EFAULT；有效地址范围中的不可读页在满缓冲时为 EAGAIN，
在关闭/未连接时为 EPIPE，NOSIGNAL 仅抑制 SIGPIPE。空 TCP 发送也检查终止状态。
`tests/workloads/network/admission.c` 由 `network-riscv.py --workload admission` 在同 ELF 双侧验证，
包含头/iovec fault、范围错误、部分接受字节守恒、SIGPIPE 与实际阻塞发送者 SIGKILL。
已有进展时的 shutdown 返回正前缀且不发 SIGPIPE；RST 后也保留 pending error，下一次调用才观察并消费 ECONNRESET。
跨页 fault 的提交片段大小是内部策略：本输入 Linux 未提交返回 EFAULT，BoarOS 返回已提交
4096 字节，测试分别验证接收量与返回值，不能将该项写成逐值差分一致。

scale 保护竞争 reservation、abort 归还及零容量下零页解析/零 stream usercopy；
真实调度的 io-sleep 包装用户复制边界，覆盖保留 reservation 时关闭并复用 fd、并发 shutdown、
用户页撤销后的 fault 和预算重取。它模型化可睡眠复制的交错，不宣称复现硬件缺页的具体时序。
