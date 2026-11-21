# 网络端点与协议执行

## 入口与对象

`kernel/syscall/socket.c` 导入 RV64/LA64 Linux socket 参数和用户指针，`fs/files/socket.c` 把 socket 作为普通 fd/OFD 安装并在失败时回滚，`net/socket.c` 持有 endpoint、数据包、待 accept 队列及等待队列。`fs/open_file.c` 在最后一个真实 OFD 引用消失时销毁 socket；dup、fork 和 syscall 期间的 pin 共享同一 endpoint，close/exec/退出均沿既有 fd 生命周期回收。`fs/files/io.c` 把普通 read/write/readv/writev 接到 socket 队列，pread/pwrite/lseek 返回 `ESPIPE`，`fstat` 标识 `S_IFSOCK`，poll/select/epoll 读取 socket 就绪和等待队列。

协议实现来自官方 lwIP `STABLE-2_2_1_RELEASE` raw API，位于 `third_party/lwip/`；固定资料和许可证见 `references/README.md`、`references/sources.tsv` 与 `third_party/lwip/COPYING`。IPv4 重组有本地所有权和边界修补，具体范围见[第三方组件](../third-party.md)。本地移植层是 `net/lwip_port/`，使用 `NO_SYS=1`；回环与物理接口共用协议池。UDP PCB 16、TCP active 32、listen 16、segment 512、pbuf header/pool 各 256、协议堆 512 KiB。协议堆按实际分配块计费，回环共享的载荷只计一次，最后一个引用归还后才释放。TCP 批量数据分配保留少量堆空间给 ACK 和接收描述；遇到内存压力沿原等待链，实际归还后重试。BoarOS socket、接收/accept 队列节点及 OFD 使用 kernel_heap。PCB 池耗尽映射 `ENOMEM`，创建失败不留下 OFD；分配器 `STATE` 和错误释放是 fatal 不变量。

实际从内置回环接口建连、且双方都是回环地址的 TCP 连接，使用 8 KiB MSS 上限和 32 KiB 的通告窗口及发送载荷额度。外部连接仍以 1460 字节为 MSS 上限，按路径 MTU 和对端通告收紧，保留原来的 11,680 字节额度。只在内置回环接口跳过 TCP 校验和生成及验证，物理接口保持完整检查。 软件互联网校验和先处理缓冲区起始处对齐所需的字节，再将 32 位字累加到 64 位累加器。余下的字节处理完后执行回卷进位，得到协议要求的 16 位结果。显式设置 `SO_RCVBUF` 后，TCP 用它限制此后的接收通告；监听端的设置在 SYN-ACK 前由子连接继承。运行中降低上限时，已经向对端通告的字节仍可接收，应用消费后才逐步收紧。socket 不再在 TCP 之后重复按同一额度拒绝报文；实际分配失败仍走接收重试。

IPv4 重组键包括源/目的地址、IP ID、协议号和输入 netif 身份。已完整覆盖的重复范围只丢弃新输入；部分重叠、冲突终点或越过已知终点丢弃整个对象。判重与边界校验先于资源淘汰，重复输入不能挤掉正在重组的有效数据。对象借用 netif 身份，不延长设备生命；移除接口前调用 `ip4_reass_cleanup_netif()`，释放该接口的所有重组引用。最后一个 pbuf 引用释放后，custom 回调才归还 DMA 缓冲。IPv4 头选项仍不支持。宿主反例和所有权检查由 `make test-lwip-reassembly-host` 重建，包含普通/乱序、重复/部分重叠、协议/接口隔离、终点冲突、预算、年龄和实际 fragmenter 的最大 UDP 内容；宿主结果不代替真实网卡验收。

改变 MF 的重复范围也不直接触发交付：即使已有数据覆盖整个声明长度，新输入仍被
判为重复，原引用保持到协议期限；超时后同一键可以重新使用。这与固定 Linux
`net/ipv4/ip_fragment.c` 先记录终点、再在 DUP 分支释放新片的处理顺序一致，
不把未接纳的终片当作完整内容已交付。`duplicate_final_timeout` 覆盖持有、超时
归还和槽复用；资源始终受重组预算约束。

socket 的信号驱动通知由 OFD 的 [异步状态](kernel-files.md#信号驱动-io) 管理。
`FIOSETOWN/SIOCSPGRP/FIOGETOWN/SIOCGPGRP` 与 fcntl 所有者接口共享同一状态。
端点另有 async_events 队列：接纳接收包或监听子连接产生 POLL_IN，发送容量实际归还
产生 POLL_OUT，协议/设备错误与断连接分别使用 POLL_ERR/POLL_HUP。源游标在端点短锁
内取得，状态保护归还后同步执行回调。同一事务的普通 poll 唤醒仍按队列合并，异步事件
分别保存游标，因此两次已接纳事件不会因共享 poll 唤醒而丢失。读预约、取消和一般状态
唤醒本身不发送输入信号。最终关闭先撤销 OFD 订阅，端点销毁再排空异步队列借用。

## 已验收 ABI 与等待

当前支持 `AF_INET`/`AF_INET6` 的 `SOCK_DGRAM`/`SOCK_STREAM`，`SOCK_CLOEXEC`、`SOCK_NONBLOCK`，UDP bind/getsockname/sendto/recvfrom 和 `SO_RCVTIMEO`，TCP bind/listen/connect/accept，以及连接后的普通读写和就绪。绑定非零低端口（小于 1024）要求当前调用者的 CAP_NET_BIND_SERVICE，端口 0 仍由协议选择临时端口。网卡启停、IPv4 地址/掩码/MTU 和默认网关的实际接口及权限见[网络设备](riscv-virtio-net.md#地址与用户接口)。地址在 syscall 边界使用 RV64 `sockaddr_in`/`sockaddr_in6` 布局；内核地址携带族、16 字节网络序地址、宿主序端口和 scope。官方 loopif 提供 `::1`；IPv6 通配监听默认接收 IPv4，accept 返回映射地址。V6ONLY 在绑定前生效，双栈通配与纯 IPv6/IPv4 的端口交集由非持引用的 endpoint 登记补齐 lwIP TCP bind 的 ANY 检查缺口。登记保留指向自身的前驱链指针，最后关闭可直接摘除。非阻塞、坏 fd/地址/指针和协议错误由固定 Linux 同一 ELF 差分约束。未覆盖的地址族、选项和操作返回明确 errno，不伪造成功。

INET TCP 的 `recv/recvfrom/recvmsg/recvmmsg` 支持 `MSG_WAITALL`：每次复制并提交当前可读前缀后归还预约和接收窗口，再沿同一次调用的绝对 `SO_RCVTIMEO` 期限等待剩余数据。非阻塞调用只取现有数据；EOF、信号、超时和后续复制错误返回已完成前缀，首字节前失败保留原 errno。已有前缀的异步协议错误不在此次短读中消费，下一次读取仍可取得它；已交付字节不登记 syscall restart。`MSG_PEEK|MSG_WAITALL` 在本次调用内推进观察偏移，不重复复制队首，也不把已观察字节的 POLLIN 当作新进展；并发消费使观察位置回到新的队首。等待时不持有读预约、用户页 pin 或协议锁。普通读取仍在当时可读前缀结束，不因该实现改变为 WAITALL。

定向检查为 `sh tests/host/tcp-waitall.sh`（4 KiB/16 KiB 页、真实预约/park、两次等待共用期限、消费竞争和资源归还）及 `tests/workloads/network/tcp_waitall.c`（同一源代码在 Linux host 与 RV/LA guest 检查真实 syscall）。guest 执行器 `tests/userland/tcp-waitall.py --arch <riscv|loongarch> --kernel <kernel> --program <static-ELF>` 使用独立临时盘并检查退出清理。语义参考 Linux `tcp_recvmsg_locked` 的[官方 v6.6 源码](https://github.com/torvalds/linux/blob/v6.6/net/ipv4/tcp.c)及[recv(2)](https://man7.org/linux/man-pages/man2/recv.2.html)。

`AF_INET/SOCK_RAW/IPPROTO_ICMP` 提供 IPv4 ICMP 原始报文接口，创建要求 CAP_NET_RAW。protocol 从系统调用经 FD 创建传给协议后端，不由 socket 类型猜测。发送方提供 ICMP 头和校验和，内核添加 IPv4 头；接收包含完整 IPv4 头，来源端口为零。端口绑定不参与 TCP/UDP 的低端口权限与分配。每个 raw PCB 的回调只观察原包，为自己的接收队列保存独立内容；过滤、队列满或分配失败只影响该接收者，不吞掉其他 raw 接收者或内核 Echo 响应的输入。读写、向量、PEEK/TRUNC、就绪和超时共用现有消息路径，关闭在协议资格下撤销回调和 PCB 后归还接收 owner。DGRAM ping、其他 raw 协议、IP_HDRINCL、PMTU 配置和远端 MTU 缓存尚未接入。

`SO_BROADCAST` 默认关闭，在选定实际 IPv4 出接口后限制广播发送，未许可返回 EACCES；接收广播不要求该选项。`IP_TTL` 控制 IPv4 发包，默认 64；`IP_MULTICAST_TTL` 默认 1，`IP_MULTICAST_IF` 保存组播发送接口与地址，组播 TTL 为零时不向物理接口发送。UDP connect、UDP send 与 raw send 共用出接口和源地址选择，随后使用选定结果发送，不再次路由。`SO_BINDTODEVICE` 保存数值接口索引，影响发送与接收；接口 down 后保留绑定，不静默回退。按固定 Linux，首次绑定允许无 CAP_NET_RAW，已有绑定的修改与解除要求该能力。TCP 更新同时协调所属执行者与四元组索引，监听连接继承实际绑定。

IPv4 任意源组播支持 `IP_ADD_MEMBERSHIP/IP_DROP_MEMBERSHIP` 的地址和接口索引形式，以及
`MCAST_JOIN_GROUP/MCAST_LEAVE_GROUP` 的通用套接字地址形式。每个套接字独立持有订阅，
以接口和组地址定位成员，并共同计入接口的互联网组管理协议第二版（Internet Group
Management Protocol version 2，IGMPv2）成员状态。以太网接口由此发送入组报告和最后订阅
解除时的离组报文。关闭套接字或移除接口时归还相应订阅。IPv4 的通用入组选项也可用于
TCP 套接字，接纳产生的子连接拥有独立的成员列表。

`IP_MULTICAST_ALL=0` 要求接收套接字自己订阅入站组；默认值允许接收本接口已经加入的
组。组播数据报按端口、地址、设备和已连接对端筛选，多个接收者分别计费和保存独立的
读取位置，共享同一份不可变报文。IPv4 源过滤与 IGMPv3 留待实现。

`SO_SNDBUFFORCE` 要求调用者具有网络管理能力 `CAP_NET_ADMIN`。它绕过普通
`SO_SNDBUF` 的设置上限，按 Linux 的规则对请求值做倍增和整数裁剪；实际值通过
`SO_SNDBUF` 查询。设置只提高套接字接纳额度，协议内存池、TCP 窗口和设备队列仍按各自
资源限制推进。

输入地址按 Linux 的有符号 32 位长度校验，bind/connect/sendto 拒绝负值及超过 128 字节的声明，并完整复制声明范围后解析。sendmsg 对空 name、负长度及超过 128 字节的输入使用其独立的规范化规则；先完成 name 复制和 iovec 导入，再解释地址，错误路径保留协议状态和报文队列。

sendmmsg(269)/recvmmsg(243) 使用原生64位 mmsghdr（64字节，msg_len位于偏移56），与单消息入口共用收发核心。整批固定原socket OFD；每条才导入自己的header、iovec和控制区，完成后立即释放消息资源，复制和等待仍在发起CPU。sendmmsg将vlen限制为1024，并在部分发送后停止；recvmmsg保留unsigned vlen，MSG_WAITFORONE在首条完成后启用DONTWAIT。已完成条数优先于后续错误和信号重启，接收的后续非EAGAIN错误留给下一次调用或SO_ERROR。msg_len在协议消费和控制FD安装后输出，输出故障保留这些副作用。recvmmsg的timespec在每条完成后检查并更新，阻塞接收仍由SO_RCVTIMEO约束；仅正条数才输出剩余时间，输出故障返回EFAULT。参数顺序与错误规则依据固定[`net/socket.c`](../../references/linux/net/socket.c)。

IPv4 的 connect、UDP connect/sendto 将目的 `0.0.0.0` 按 Linux 本地目的语义转换为 `127.0.0.1`，bind 的 `0.0.0.0` 继续表示通配监听。socket 的 family/type/protocol 分别检查；INET RAW protocol 0 返回 `EPROTONOSUPPORT`。

每个 INET 和 NETLINK 端点用自身的短锁保护收包队列、读预约、活动写请求、超时、错误消费和发送额度。监听者持有待 accept 的 FIFO 和可接纳子连接数。共享协议工作链、资源重试链使用公共短锁。UNIX 的入队、发送者计费和对端引用须同时发布，仍由公共短锁保护。同一状态修改中的队列发布、额度查询及更新复用外层保护；嵌套 guard 只记录借用，通知游标仅由实际持锁者初始化和维护。RV 用户的 read/write/readv/writev、sendto/recvfrom/sendmsg/recvmsg 和普通文件到 socket 的 sendfile 在发起 CPU 执行：用户复制、接收预约、发送额度和等待属于原调用者。创建、监听、端口分配、初始握手和网络配置仍在 CPU0。建立的 TCP 连接有固定协议归属核；查询普通选项、对端地址和连接结果只读取端点快照。读取 MSS、设置 TCP 窗口等访问 PCB 的操作取得连接的协议保护。就绪查询及等待登记不取得 lwIP mutex，就绪检查和等待登记在同一状态保护内完成，释放状态锁后才 park。通知使用已有等待游标借用，在端点锁外执行回调；最后销毁先关闭等待队列，并等游标与登记实际归还。`kernel_socket_poll` 不访问 PCB、不推进协议、不消费错误；TCP 可写性来自协议 owner 发布的接纳额度，实际发送预约共同扣减它。监听就绪由接纳/失效回调维护，失效 child 的销毁由现有工作集合处理。epoll 真实候选查询在 token 外，prepared 阶段只读刷新提示。

短 syscall 在完整 raw 调用退出后可执行有界服务；网络 worker 与无 NIC worker 在自己的外层 scope 中调用 `kernel_socket_service_owned`，沿同一资格登记等待；独立调用者使用 `kernel_socket_service_pending` 取得资格。每批分别限制 RX 八帧、loopback 八包、socket 八个工作单元及一个 timer 回调。socket 单元包含容量转交、到期重试、实际对象工作，三类轮转；协议池、NIC、接收堆等待集合也轮转。零预算不消费对应工作，返回值报告处理量、是否可立即继续及下一期限。unsent 等待远端窗口或 ACK 不会自行重新入队。服务先按包预算消费旧输入，再处理 socket 工作；后者产生的新回环包使用本批尚余的包预算交付，预算总量保持原值。预算耗尽且仍有工作时开放中断并让出，睡前再次查软件工作与设备完成。

在串行处理的以太网接收批次内，同一连接相邻的纯确认报文（Acknowledgment，ACK）可以合并。每帧仍经过传输控制协议（Transmission Control Protocol，TCP）校验和及入口过滤。合并限于已建立连接、无 TCP 选项、序号符合接收预期、确认号严格递增且未超出已发送数据、对端通告窗口不变的报文。重复确认、恢复中的连接、窗口变化和其他控制报文沿各自处理路径。批次保留各个确认号用于拥塞控制，按最后一个确认号一起摘除已确认的报文段并发布发送额度。接收队列耗尽、遇到其他流量或批次结束时，当场处理剩余确认。

普通发送的数据段描述和报文缓冲区保存在同一堆块。描述位于报文缓冲区之前，对协议首部和载荷的空间独立保留。TCP 收到确认后归还自己的引用，设备发送完成后归还直接内存访问（Direct Memory Access，DMA）引用。只要还有任一所有者或不可变载荷视图，全块继续存活。普通控制段和接收乱序段继续使用独立描述符池。

仅启动一核时，八核容量构建使用相同的 IPv4 直连路由快照。归属协议在发送正文时检查快照的地址、绑定接口和代次，复用已解析的链路地址。邻居、路由或接口状态变化会推进代次，下一次发送重新解析。网关转发、邻居尚未解析以及超出路径传输单元的报文继续沿通用输出流程。

CPU0 保有监听、未接纳连接和 UDP PCB。UDP 输入按本地端口找到候选桶，按地址、接口与已连接对端选出接收者，在该 socket 的短锁内核对接收额度并排入报文。RV／LA 实际启动多核时，各副核有一份 TCP 状态和执行线程。主动连接在握手的输入处理完成后迁移，被动连接在 accept 归还监听 backlog 额度后迁移。首次交接沿用四元组的散列选择，首选核已有四个活动控制块时才选择活动控制块更少的核。已登记的同一四元组及其反向四元组沿用原归属核。进入 TIME_WAIT 时释放活动计数，保留路由及定时器直到控制块退休。登记、状态交接和撤销沿用路由索引锁。迁移同时交接活 PCB、定时起点、就绪请求和收包索引；PCB 释放前撤销索引。实际仅启动一个处理器时，网络执行线程和用户请求都在 0 号核。用户请求直接取得协议互斥锁，省去仅为限制跨核迁移而设置的服务资格和退出该资格时的调度检查。网络执行线程继续处理设备到达和协议定时工作。

网卡输入先在共享 IP 层做地址检查和分片处理，随后按 TCP 四元组把报文引用交给归属核。收包队列携带源／目的地址、输入接口和四元组的散列桶编号。归属核持有自己的协议锁，完成 TCP 报文校验，并按桶编号查找活动连接。归属核重新核对地址、端口和接口资格后，再处理序号、ACK、窗口更新和重传。活动连接优先于 TIME_WAIT，后者仍按归属核的 TIME_WAIT 链表查找。连接退出时，归属核在控制块释放前撤销本核索引。收包队列只保存编号，归属核找不到匹配连接时直接丢弃报文。

物理接口收到已建立连接的纯确认报文时，如果接收队列还有已完成的报文，收包线程把本报文交给连接归属核，再取下一条。接收队列当时为空且归属核正在等待工作时，收包线程可尝试就地处理。它先保存输入参数并释放共享 IP 锁，再尝试取得连接归属核的互斥锁，取得后重新核对是否有先到的报文。满足条件时，收包线程沿同一校验、过滤和确认路径执行，归还连接归属锁后才通知发送端的就绪等待者。需交接的报文沿原入包队列交付，由归属核合并同一批报文产生的通知。收包线程返回共享 IP 路径前重新取得 IP 锁。

内置回环的两个方向落到同一归属核。第二端迁入时在该归属锁下关联反向 PCB，任一端退出路由索引时先清掉双向关联。普通数据和 ACK 从反向 PCB 取得散列桶编号，直接放入收包队列。尚未配对、旧回环队列未交付完或接口条件变化时，沿原 IP 路径处理。

回环直接交付在归属核的入包环短锁内预留输入槽，锁外准备接收端自己的协议头和 pbuf 链，只借用已复制到协议堆的不可变载荷。发布时重新核对接口代次和停止状态；撤销接口资格后，配置端逐个经过各入包环，等已进入发布段的执行者结束再修改接口。失败归还接收引用和槽位；同一流的新包不会越过尚未发布的报文。接收修剪不改变发送描述；发送端的 ACK 或重传只操作自己的描述和引用，接收者读完前载荷不会释放。原回环队列也复用载荷；外部引用和 DMA 缓冲仍按原复制规则处理。共享 IP 路由、邻居状态和设备仍各自串行保护；协议池只在分配或归还时加短锁。不同归属核使用独立 TCP 执行锁和入包环锁；分到同一核的连接共用该核的资格。

协议 scope 保存实际执行资格和 IRQ 状态。同一 owner 的同步回调借用外层 scope。持有连接资格需要共享 IP 时按 TCP → IP 顺序取得；CPU0 迁移或关闭远端连接前先归还 IP 资格。设备 IRQ 收割 DMA 完成并通知执行线程；RX loan 持续到最后一个 pbuf 引用归还。停止时关闭新接纳、排空实际读写与通知借用，归还各归属核报文、连接和设备引用后才撤下接口。

读预约持有 OFD 和固定队列前缀，用户复制在保护外完成。finish 在 IRQ 开启时找到消费边界，短临界区一次摘下前缀，并把原 packet 与接收窗口额度转交已有 worker。worker 在协议保护内归还 pbuf 和 `tcp_recved`；最后关闭也排空这些实际引用。RX loan 因此一直保留到 pbuf 最后归还，不能在用户复制完成前复用 DMA 缓冲。AF_UNIX 载荷在端点临界区外释放；关闭清理结束后才把发送者的末次释放权交给剩余 packet。

TCP 发送先在端点保护内预约额度，随后在调用者上下文取得有界的输入页引用。已迁入 TCP 分片的连接，一批至少有两个 MSS 的载荷时，调用者用预约时的 MSS 将数据直接复制到最终 pbuf 和 TCP 段；分配与复制均在分片锁外。请求持有这些段，并在争用分片锁前归还用户页。持有分片锁的执行者核对连接状态、顺序、当前 MSS、字节额度和段队列余量，设置序列号并接入实际可接纳的完整段前缀。未接纳的段由原请求在锁外释放。小写入留在原合并路径，可以填充已有段的尾部；单核及尚未迁移的连接沿用原路径。

调用者可以尝试取得连接归属核的协议锁并直接完成接纳，只把出包交给归属核。锁繁忙或已有旧请求时，保留原 FIFO 和同步完成机制；已准备好的大批在此交接时不再持有用户页。发送预约持续到接纳结束，其他发送者不能重复占用同一额度；worker 将异步错误和 SIGPIPE 交付给原调用者。同一请求队列还承载 UDP 的 PCB 发送及 TCP 延迟输出。UDP 在协议内存支持并行分配时由调用者准备最终 pbuf，用户复制失败也可在原核释放。实际写请求在端点活动链中保持到全部协议阶段完成，停止时先拒绝新接纳，向包括空队列读者在内的 INET 等待者发布 EIO 并通知，借用实际 OFD 等待活动读写归还。worker 通知在工作链锁内取得真实 progress 队列游标，锁外只投递该游标；撤下通知入口后排空已捕获的队列借用，再停止原 worker，通知不再保存网络对象的裸 context。设备在途 DMA 仍由原 quiesce／loan 协议确认。

socket 在最终 OFD 释放时取得协议资格，先摘除弱引用工作链并解除 PCB 回调。跨用户复制和待执行请求持有实际 OFD/pin，因此 worker 借用的对象不会被提前销毁。TCP 分片的 worker 只在睡眠登记时同时取得工作链锁和入包环锁，分别核对就绪请求和输入。两类生产者各用自己的锁发布工作，共用一次唤醒资格并在锁外投递。已在运行或已经有人通知时不再创建游标。关闭先阻止新接纳，再结束等待并排空借用。

TCP COPY 在本批接纳结束前保留整批发送预约，远端接纳不能重复使用已复制的额度；协议 owner 计算实际 PCB 额度时排除已经处理的预约，避免再次扣减。接纳后在端点锁内发布容量并归还预约，随后在工作链锁内交付已排队请求的成功前缀或错误，最后执行本批需要的 `tcp_output`。调用者此时可以归还请求栈和 OFD pin；输出仅借用仍受协议资格保护的 socket，最后关闭等待该资格归还。原服务预算和同流请求顺序不变，后续输出错误留给下一次调用观察。实际可接纳字节增加才通知额度等待者；完整 COPY 通常只是把预约转成协议占用，短前缀和取消则归还未用额度。其他调用者尚未 COPY 的预约耗尽接纳额度时，不安排无助于归还预约的 CPU0 输出；本请求的 deferred 前缀及真实协议阻塞仍推进输出。SMP 的 INET 活动发送链按 socket 保护，登记和摘除与其他连接独立。停止时同时保护活动链与接收预约，借用实际 OFD 后登记等待；发送者摘链后在锁外通知。recvfrom/recvmsg 仅在用户请求源地址时取得临时名称引用，消息长度和截断消费独立处理。

TCP 输入和接收窗口更新按 lwIP 的实际决定尝试输出。输入前不再先挂一份输出和回收工作。实际输出遇到 `ERR_MEM` 时，根据当次网卡额度标记登记设备或协议池重试；无载荷但待发的 ACK 也可重试，ACK 分配失败继续由 TCP 期限处理。ACK 回调在端点锁内归还发送额度并发布一次新状态；入站载荷只发布接收队列，端点的待归还数据或窗口额度从空变为非空时发布回收工作。

单核运行时，若阻塞式 TCP 发送即将因额度不足进入等待，发送线程先释放端点保护，再尝试取得网络协议资格。取得资格后，它最多处理八个网卡已经收取的报文，然后重新检查套接字状态并决定是否登记等待。网卡接收线程继续处理其余报文、定时器和设备完成。这个过程不在硬件中断中执行。

归属核在一批输入内可以合并同一套接字的可写通知。最后一次额度发布后，在端点锁内取得等待游标，归还协议资格后再通知，覆盖在批次内新登记的等待者。异步信号仍随各次额度事件产生。归属核上的发送者遇到已用尽的额度时等待实际归还。先前由本次调用延期的输出，以及网卡或协议内存池拒绝的输出，分别由该调用和对应资源的重试队列推进。 套接字最后关闭时，如果传输控制块仍有等待网卡槽位的输出，内核把待重试标记交给该控制块。驱动归还容量时，原协议归属核检查这类待重试的控制块。套接字释放和对端确认都不决定这次重试的时机。

多核下，已分配归属核的 TCP 连接可以复用同一子网内已确认的 IPv4 发送路径。连接控制块保存出接口、接口最大传输单元（Maximum Transmission Unit，MTU）和两端的链路地址副本。地址解析协议（Address Resolution Protocol，ARP）表项变更或到达重新确认时间、出接口配置改变时，网络协议层更新版本号。归属核在使用前核对版本，过期后重新解析。仍有效的路径可在连接自身的保护下生成首部并提交网卡。其他路径继续进入共同的互联网协议（Internet Protocol，IP）输出。停用网卡时，在释放接口存储前排空连接归属核的执行，设备完成仍按实际报文引用归还。

对于已归属到分片的物理接口连接，`tcp_ip_output` 在互联网协议（Internet Protocol，IP）锁内构造完整帧并取得路径最大传输单元（Path Maximum Transmission Unit，PMTU），随后在连接归属锁内提交已构造的帧和更新 `TCP_INFO`。接口关闭先停止接纳，再等待连接执行者结束，最后撤下接口。报文捕获在提交成功后借用原帧，取共享 IP 锁访问订阅列表。地址解析请求和分片等独立生成的报文沿用同步链路路径。设备接纳时取得载荷引用，设备完成时归还。连接继续持有发送段，直到收到对端确认（Acknowledgment，ACK）或关闭。网卡发送槽耗尽时，以本次输出前的容量代次登记重试，覆盖设备先归还而重试请求尚未登记的交错。

`netif_loop_output()` 在成功发布完整回环包队列后调用可选的
`LWIP_HOOK_NETIF_LOOPBACK_QUEUED`，经端口进入 `protocol_work_ready()`，设置软件
待处理状态；从无包变为有包时请求 worker 唤醒，已有待处理包时合并通知。每次轮询结束按真实剩余队列更新状态，包含同批产生又被消费的 ACK。该回调不消费包或重入 raw API。分配/复制失败不
发布通知。socket/timer 阶段产生的新包若未被本批余量消费，服务返回值保持 runnable，由 worker 继续推进。
`make test-lwip-host` 已纳入真实 lwIP/端口/socket 的 `loopback-work` 反例：固定
协议时钟，覆盖 IPv4/IPv6、晚入队、OOM 与重试、内容交付和内存基线。

非阻塞 connect 短路径推进握手，后台保证调用者不再调用 socket 时仍能进展；阻塞 connect 等待握手结果。普通 socket read/readv、write/writev 及 sendfile 在调用入口把 OFD 的 O_NONBLOCK 折入本次 MSG_DONTWAIT；后续 F_SETFL 不改变已开始调用的等待资格。非阻塞 EAGAIN 直接返回，不查询或建立接收/发送超时。阻塞接收在首次需要等待时读取一次 SO_RCVTIMEO 并保存绝对期限；即使就绪后数据被另一 reader 取走，后续重试也不重新取得预算。发送沿用调用级绝对期限，等待函数不再把剩余时长重新换算为新期限。每次先尝试实际读写，再对 EAGAIN 检查到期，保留已有数据与错误的优先次序。信号沿既有 syscall restart 协议，带接收超时的中断返回 `EINTR`。未 listen 的 stream 和 datagram accept 立即返回类型对应错误，监听 socket 的 `SO_RCVTIMEO` 约束阻塞 accept。

普通 socket read/readv 使用“预留当前队列前缀→锁外复制→一次提交/取消”。STREAM 预留把当前可读 packet 的总字节数截到用户容量，队首借用与固定字节数限定本次稳定版本；成功提交后本次读调用返回该前缀，后来追加的数据与排在前缀之后的协议错误留给下次调用。请求直接遍历 packet/pbuf 链写入用户 iovec，复用现有用户页写游标，换页及退出前归还 pin，不增加接收中转页。预留与提交只修改端点状态，不取得协议资格；取消仍由请求归还 OFD pin。跨包复制故障只提交此前完整复制成功的 packet 前缀并返回短读，故障 packet 保留；UDP 仍只预留一个数据报、按用户容量截断，复制故障仍丢弃该数据报。零长度数据报也完成 reservation，TCP EOF 没有 reservation。

TCP标量和向量发送均在容量接纳后准备有界页引用与片段，输入组织及部分接纳规则统一见[文件I/O](kernel-files.md#请求暂存与成本)。协议继续持有ACK前的COPY版本，ERR_MEM后先归还pin再等待。描述符scratch页由任务持有并在调用间复用，分配失败返回 ENOMEM；正常返回解除请求登记，页保留至任务最终存储释放，强制退出先取消实际请求／释放其 OFD pin，再解除登记，最后释放任务文件表和栈。没有用户任务的模块测试按调用分配并沿正常返回路径释放。请求末批、容量阻塞点和退出时集中推动 tcp_output，每次接纳批次只在一个边界执行有界协议服务。已接纳前缀后的异步错误留给下一次调用观察。`kernel_socket_get_statistics` 提供累计 tcp_write 调用及成功复制字节数；用户页解析尝试由诊断构建的 `COST_USER_RESOLUTIONS` 记录，普通构建不为此维护全局原子计数。直接 pin 的 TCP 数据不经过 uaccess 复制函数，搬运字节由 `copy_from_user` 和 `stream_copy` 记录；`uaccess_copy_ticks/bytes` 仍只覆盖显式复制路径。计数没有新增用户 ABI。

INET UDP 发送请求直接持有最终 PBUF_RAM，用户 iovec 填入该载荷后交给协议发送，不再分配临时 socket_packet 和第二份数据区。用户复制在协议临界区外执行；复制失败、正常完成或强制退出都由同一请求释放尚未转交的 pbuf。

`recvfrom` 的缓冲区范围在等待空 UDP socket 前检查，负 socklen_t 返回 EINVAL；accept/recvfrom 的地址输出错误发生在协议 dequeue 后，与固定 Linux 顺序一致。

生产收发入口沿完整临时包或队首reservation持有请求与OFD pin。线程组强制退出
先标记/唤醒，保存的syscall栈返回后才在user-return处理终止；正常和取消路径
均须平衡临时owner。以后若允许直接抛弃内核调用栈，必须重新审计这条契约。

TCP `POLLOUT` 同时要求发送缓冲、队列空间及未被真实 `ERR_MEM` 阻塞。失败后登记协议池等待及 250 ms 兜底；每次真实资源归还更新容量代次，仅恢复旧代次等待者，避免失败分配自行释放资源造成忙等。不能仅采用 lwIP 的“空池变为非空”通知：多 segment 请求可能在空闲池已有一个元素时仍失败，第二个元素释放必须促成进展。期限只由后台服务推进，等待 syscall 不再为协议设置兜底轮询。`F_SETFL(F_GETFL|O_NONBLOCK)` 接受已有 access mode 位，只更新可变状态位。

关闭活动 TCP 连接先解绑全部指向 BoarOS socket 的回调，再 `tcp_close`；协议 FIN/TIME_WAIT 可能暂占静态 PCB/segment 池，随后由定时器回收。这与内核堆对象生命周期分开。

连接控制块（Protocol Control Block，PCB）的额度由整个协议栈共享。多核下，监听核用尽额度且本地回收不足时，可以从仍有 TIME_WAIT 记录的归属核归还一个连接控制块。监听核保留监听和输入的执行资格，先释放网际协议（Internet Protocol，IP）保护，再取得归属核的协议互斥锁并重查其等待关闭链。回收沿原有路径撤下四元组索引和套接字借用，释放控制块后才重试分配。其他归属核上的活动连接仍由其原有协议状态机维护。

`socket->tcp` 是对 lwIP PCB 的借用，不延长协议对象寿命。输入处理进入 TIME_WAIT 后，
在当次协议输出结束时清除 socket 借用和回调；TIME_WAIT PCB 仍由原协议索引和定时器管理。固定 lwIP 的
`tcp_alloc()` 会在 PCB 池满时通过 `tcp_kill_timewait()` / `tcp_abort()` 回收
TIME_WAIT；该分支不执行普通错误回调，并且 `tcp_pcb_remove()` 会把状态改为
CLOSED。回收分支在 remove 之前，以及慢定时器从 TIME_WAIT 链摘除对象后、
free 之前仍调用本地可选 `LWIP_HOOK_TCP_TIMEWAIT_FREE`。socket 层仅接受
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

`AF_NETLINK` 的 `NETLINK_ROUTE`（protocol 0）提供 `SOCK_RAW`/`SOCK_DGRAM` 端点，
支持单播绑定、接口和地址枚举及有限的路由查询。路由修改和订阅组当前无实现。
消息队列的 `mq_notify(SIGEV_THREAD)` 也使用该地址族：C 标准库创建未绑定的套接字，
将其文件描述符和 32 字节通知数据传给内核。通知报文不带 Netlink 头，通知数据最后一字节
表示唤醒（1）或移除（2），源端口标识为内核的 0。Netlink 原始套接字的创建不要求
`CAP_NET_RAW`。

通知准备在消息队列锁外取得真实 socket OFD，分配最终 packet，并按 packet 及载荷大小预约接收端内存。尚未触发的 cookie 与已入队报文共享 SO_RCVBUF 额度；首条允许超过当前额度，其余超额准备通过已有 task 请求和等待 token 等待。唤醒后重新查 FD，关闭或复用能改变此次查找结果。注册成功后 cookie 独立持有端点存储，OFD 临时引用随准备结束归还；消息队列移出通知状态后在锁外发送或取消 cookie。接收完成或失败消费报文时归还实际接收额度；PEEK 保留它。取消注册失败时直接取消预约，不投递报文。

socket 最后关闭立即撤下接收队列并关闭等待对象，尚未触发的 cookie 只保留已关闭端点的存储；其后发送会释放该 cookie，最后一个实际使用者归还 heap。通知不新增 worker，也不进入 lwIP。`sh tests/host/netlink-notify.sh` 用真实 FD/OFD、MM、heap 与 socket 检查两种类型的 cookie 内容、源地址、PEEK/read/readv、复制故障及关闭后的资源归还，并以明确握手检查满额度等待、接收归还额度和等待期间 FD 关闭。固定依据是 `references/linux/ipc/mqueue.c`、`net/netlink/af_netlink.c` 和 `include/uapi/linux/mqueue.h`，libc 消费方式来自固定 musl 1.2.5、glibc 2.44 源码归档。

`AF_PACKET` 提供链路层的 `SOCK_RAW` 和 `SOCK_DGRAM`，创建时检查 `CAP_NET_RAW`。
调用者用 `sockaddr_ll` 设置接口编号和网络字节序的以太网协议号。原始套接字收发带头的
帧，数据报套接字接收时省去以太网头，发送时根据接口和目的地址构造头。`ETH_P_ALL`
还接收本机发出的帧。回环接口为普通互联网协议（Internet Protocol，IP）报文合成以太网头，
使两种套接字看到与其类型相符的字节布局。

接收旁路在设备归还缓冲区之前检查接口、协议和已安装的过滤程序，随后把匹配帧的独立副本
交给套接字接收队列。每个队列独立计费，满队列只丢弃给该接收者的副本。取消绑定或
关闭套接字时，在协议保护内撤下接收登记，接口销毁时解除该接口上的绑定。
等待和用户复制复用普通消息接口。

接收端可通过 `PACKET_VERSION` 选择第三版共享报文环（`TPACKET_V3`），再通过
`PACKET_RX_RING` 配置块尺寸、块数和交付期限。配置在发布前验证块与帧尺寸、页对齐、
块头及保留区间，并分配清零的后备页。单环实际映射范围上限为 64 MiB。
用户通过偏移为零、长度等于整个环的 `MAP_SHARED` 映射访问接收块。

接收旁路直接把匹配帧复制进内核拥有的块，记录捕获长度、原报文长度、时间戳和链路层地址。
块空间不足或部分填充的块到期时，内核写全长度和报文计数，再以释放语义写入
`TP_STATUS_USER` 并通知等待读者。应用处理完后用释放语义写回 `TP_STATUS_KERNEL`，
内核取得所有权后才重用该块。生产者使用内核私有的块位置和尺寸，用户修改共享描述符中的
其他字段不会改变写入边界。满环只丢弃本接收者的报文，`PACKET_STATISTICS` 返回报文数、
丢包数和进入满环等待的次数。

映射与其派生视图持有打开文件和后备页的实际引用。有映射时重新配置返回 `EBUSY`。
最后一个用户归还后，关闭路径撤下接收登记和到期记录，再释放环页。当前接收环只实现
`TPACKET_V3`，发送环 `PACKET_TX_RING`、接收流哈希、混杂模式和分流组另行实现。
宿主 `packet-ring` 短例检查跨页写入、到期发布、满环和页面归还，
`tests/workloads/network/packet_ring.c` 使用原生套接字及映射检查相同的应用可观察行为。

`AF_UNIX` 提供 STREAM/DGRAM/SEQPACKET 的普通 socket、socketpair、文件路径名和抽象名称。创建、绑定、连接、监听、accept/accept4、配置及最后关闭由 CPU0 执行；载荷复制、接纳、读预约和等待保留在发起 CPU。socketpair 的双 OFD/FD 原子发布，失败归还未发布对象。

UNIX 名称单独分配、发布后不可修改。地址接口用带长度的借用视图，与原 INET 地址保持同样的24字节；导出地址和 DGRAM 消息各自持有名称引用。抽象名称按 socket 类型、完整长度和原始字节索引，允许内部零字节；family-only bind 查找未占用的自动名称。文件路径绑定持有真实 VFS 路径，索引以文件系统实例和 inode 区分对象；bind 在同一次目录创建中取得节点，复用 DAC、umask 和 SGID 继承。改名、硬链接和 unlink 不改变连接身份。关闭留下路径节点，最后一个节点引用结束后才回收已 unlink 的 inode。

逻辑关闭与端点存储寿命分别处理。命名索引发布活端点，查找在端点短保护内取得引用；连接、在途目的地址及非空的发送者计费队列各持有实际端点存储。关闭先撤名，按 incoming 链摘除真实连接和对端空间通知，排空接纳队列，等待通知借用归还；最后一个存储使用者释放 heap。STREAM 对端关闭后保留其末次名称，accept/getpeername 及已排队数据的地址输出继续有效。普通 I/O 不持有对端 OFD，所以对端最后关闭仍立即产生 EOF/EPIPE 等状态。

发送请求登记与对端存储借用共用一次端点状态锁，结束时摘除请求并归还该借用。发送者的排队消息逐条记录实际内存计费；只要该发送者还有未消费消息，就由一个端点存储引用保活，最后一条消息出队或被丢弃时再释放，避免让 packet 的 sender 指针越过发送端关闭而悬空。

STREAM connect 创建待接纳子端点并发布到 listener 队列，accept 转移子端点所有权；backlog 满时使用固定发送期限等待。DGRAM 可以逐次指定目的地址，或保存默认对端；接收端按连接关系过滤发送者。多对一队列采用固定 Linux `max_dgram_qlen=10` 的边界和互连例外，连同在途预约计算接纳资格。等待登记与条件检查处于同一短保护；只有受目标队列上限约束的单向连接订阅对端空间通知，双向互连和自连接不登记该回调。connect 在现有控制互斥下处理两端订阅变化：短锁外排空旧通知借用，发布连接后恢复变成单向的旧边，并通知就绪条件改变的端点。回调在锁外向实际等待者传播。DGRAM 消息消费后归还真实 sender 的计费并通知；STREAM 保留读完成时的原有对端通知。重连或断连清除未被读取预约借用的旧消息，活动读取保持原数据及其计费。

`SO_PEERCRED` 返回 Linux 指定时点的 PID、有效 UID/GID 快照：socketpair 在创建时捕获，STREAM listener 在 listen 时捕获，连接建立后分别使用监听者与连接发起者的记录。后续身份变更保持既有快照。accept4 的 NONBLOCK/CLOEXEC 只设置新 FD；接纳先预留 FD并准备持有打开凭据的真实 OFD，再等待并取走连接。分配失败保留待接纳连接；等待失败归还尚未接管端点的 OFD。成功后 OFD 接管 child，地址输出成功才安装 FD；输出 EFAULT 消费连接并释放未发布对象，符合 Linux。

未连接或正在监听的 UNIX STREAM 接收返回 EINVAL，阻塞调用立即结束；已建立连接的对端关闭仍按排队数据和 EOF 处理。read、向量读取和 recv 系列共用接收状态与预约路径。

UNIX stream 普通写先预留发送者容量，在协议临界区外把用户 iovec 直接填入最终队列载荷，再检查对端状态并发布。packet 头和内联载荷只分配一次；复制 fault 只发布成功前缀，首字节 fault 不发布。对端在复制期间关闭时归还预留与未发布 packet，保留真实 EPIPE/ECONNRESET。接收直接从该载荷复制到用户，因此普通 stream 不再经过读写中转页。

AF_UNIX按发送者内存限制等待，SO_RCVBUF仍可设置/查询但不能给对端发送者提供
credit。SO_SNDBUF按Linux倍增、下限与4MiB设置上限；固定双侧默认212992。
新packet可在当前内存低于sndbuf时分配一次，计费包含实际请求的payload及本地
packet元数据；内部不复制Linux私有skb布局，不能比较偶然的成功发送次数。
POLLOUT使用当前发送者内存的四分之一门限，实际write仍独立检查完整状态和
内存资格；poll不预留内存。STREAM把完整packet限制为min(sndbuf/2,32KiB)，载荷扣除实际包头，
避免包头使整块申请跨过buddy边界。部分读取不提前返还该packet的内存。DGRAM消息上限为sndbuf-32，一次write/writev形成
一条消息；零消息仍占有真实metadata，队列有限。

未显式指定目标的UNIX发送在同一次端点保护内检查状态、登记请求、借用对端并预留额度；
需要等待时沿原等待协议释放实际外层保护并重试。记录型sendmsg/sendmmsg先完成header、名称、iov和控制区导入及端点接纳，再进入载荷复制。UNIX请求持有已经解析的对端引用，显式INET目标保存在请求的已校验紧凑地址快照中；导入元数据与正式写请求复用互不重叠生命周期的栈存储；名称视图在接纳后结束，准备阶段持有锁游标的函数帧在复制前返回。载荷复制借用外层iov快照与已登记请求，其冷缺页可沿文件I/O和调度等待继续执行。复制完成后同次发布消息并撤销
请求登记，载荷复制在锁外进行。DGRAM在分配和usercopy前完成内存预留，复制完成后才移动整包owner；
fault/OOM/取消先释放取得的内存，再归还预留与OFD pin，不发布前缀。UNIX发送者的预留按自身内存计费归还并通知等待者，不使用TCP的发送预算判断；正常发布只是把预留转为队列计费，请求摘除不额外通知。截断读取或
接收fault丢弃整条datagram并归还其全部计费；DGRAM出队时在同一保护内归还
sender额度，实际名称、端点和packet存储在锁外释放，释放后仍推进
共享heap的INET接收容量等待。sender关闭后仍可读其queued内容。
等待者被发送缓存增大、packet最后释放、发送预留取消或peer状态变化唤醒，重新核对状态。
STREAM的新write在peer关闭后返回EPIPE/SIGPIPE；已有写等待在peer带未读内容
关闭时可交付一次ECONNRESET。DGRAM关闭peer后的首次send为ECONNREFUSED，
后续未连接send为ENOTCONN，DGRAM的EPIPE不发送SIGPIPE。关闭状态、异步错误
与poll/recv均按各类型处理，不把datagram peer关闭变成stream EOF。

固定依据为`references/linux` commit`f4cdf7ca9a1fdcca413157df19753f388a5a224e`的
`net/unix/af_unix.c`、`net/core/sock.c`、`include/net/sock.h`。每消息控制数据见下节。命名连接和 socketpair 共用上述跨核状态与队列保护。socketpair先预留两个FD并逐项输出编号，再执行共同的family/type/protocol创建校验及配对支持判断；创建失败保留已输出的编号前缀并归还预留，实际FD仅在成功后发布。

SEQPACKET按连接管理与整条记录收发：监听、connect和accept与STREAM共用，实际SO_TYPE及子端点保持类型5；一次发送发布一条完整记录，短接收截断并消费该记录，MSG_TRUNC可返回原长度。MSG_EOR和MSG_MORE发送标志接受后忽略，MSG_WAITALL接收仍停在一条记录，返回标志不添加MSG_EOR。SEQPACKET未连接或监听时接收返回ENOTCONN，EPIPE不发送SIGPIPE。

空记录与EOF独立：空记录可交付SCM_RIGHTS和凭据；SEQPACKET先交付排队记录；没有记录和待决错误时，本端SHUT_RD、对端SHUT_WR或对端最后关闭均返回EOF，O_NONBLOCK/MSG_DONTWAIT也返回零，控制区长度为零。DGRAM本端SHUT_RD后无记录的非阻塞接收仍返回EAGAIN。STREAM保留自身空cookie规则。头、iovec及控制区字节先导入，随后按协议检查待决错误和连接，再解析SCM；不支持的SEQ标志在协议规定位置拒绝，输入故障与错误消费顺序和固定Linux一致。

UNIX报文持有发送端存储；接收时从该端点取得当前名称引用，所以发送后bind和发送端关闭后仍可返回正确来源。逻辑关闭摘除名称索引，名称对象随真实存储使用者释放；用户地址输出在端点锁外完成。带地址输出的UNIX STREAM和记录型接收共用此路径。STREAM/SEQ未接纳子连接由listener保活，listener关闭时即使子队列为空，也向client发布一次ECONNRESET；已接纳子端点归其真实OFD所有。

UNIX合法shutdown对未连接、监听和已连接对象均返回0，修改规定的本端／对端关闭状态；非法how在修改前返回EINVAL。STREAM/SEQ的就绪位由监听、队列、shutdown和待决错误共同决定；缺少peer本身不生成POLLERR。

连接建立、消息边界和SIGPIPE分别按实际类型判断；共同文件I/O依据消息边界选择整包接纳，发送信号仅使用STREAM规则。接收结果区分取得报文、STREAM空状态、复制故障与SEQPACKET EOF，控制区字节导入与SCM语义解析分别处理，使协议错误能够按其规定顺序返回。

## UNIX 控制消息与 FD 所有权

INET与UNIX共用MSG_PEEK接收预约，重复查询保留原报文与接收窗口。UNIX STREAM/DGRAM/SEQPACKET 的 sendmsg/recvmsg 接受 SCM_RIGHTS、SCM_CREDENTIALS；SO_PASSCRED控制消息凭据输出。原生64位 cmsghdr 为16字节、8字节对齐，ucred 为12字节，每次最多传253个FD。发送控制区完整复制后按记录解析；FD表查询取得真实OFD，包含O_PATH。发送者关闭原FD或复用编号后，排队消息仍持有原对象；接收者共享其offset、状态标志和打开权利，新FD的CLOEXEC由MSG_CMSG_CLOEXEC决定。

自动消息凭据捕获发送时TGID与真实UID/GID，普通write/writev/send/sendto也使用这一规则。显式凭据依据调用者real/effective/saved身份及CAP_SETUID、CAP_SETGID、CAP_SYS_ADMIN检查。排队消息保留PID身份引用与数字UID/GID；发送者退出或改身份不改变已有消息。SO_PEERCRED继续使用连接时的有效身份快照。PASSCRED还参与listener选项继承、尚未accept的child凭据捕获及未命名发送端自动绑定。

STREAM在首个成功接纳片段转移rights，第一次recv消费分离控制记录（含零长度recv；普通read(0)仍直接返回）；读批次在rights或不同凭据的边界停止，剩余数据保留应有的凭据。普通read/recv消费并丢弃控制引用。MSG_PEEK保留原消息，每次为接收者复制新的OFD引用。DGRAM空包仍可携带控制数据；载荷fault的整包消费和STREAM完成前缀分别遵循各自规则。STREAM载荷fault仍输出已捕获凭据并保留应有的错误，rights留在未成功消费的包上；EOF或EAGAIN的空cookie与选到无凭据旧包分别使用零身份和overflow身份，信号中断保留原控制区。

接收逐项预留FD、复制编号、安装OFD。FD表满或控制容量不足时保留已交付前缀，设置MSG_CTRUNC，并释放剩余引用。控制输出故障与最终msghdr故障的返回值、副作用按固定Linux处理，已安装FD保留。调用期请求登记socket pin、地址名、向量快照、控制对象及FD预留；强制退出先结束端点借用，再释放这些所有者，无须原C栈继续返回。普通发送的自动凭据由已登记write request持有。

所有排队FD按发送时真实UID账户计入inflight，消费后归还原账户。Linux额度检查与报文发布在同一次端点短锁内完成：已有计数严格大于RLIMIT_NOFILE时返回ETOOMANYREFS，CAP_SYS_RESOURCE/CAP_SYS_ADMIN可豁免。报文队列持有控制对象；端点短锁维护UID计数、排队控制对象总数和退休／锁外释放所有者。仅含普通FD的发送、接收预约、PEEK、消费和控制对象释放不取得rights图锁。包含UNIX socket OFD的控制对象另接入接收端的图链，混合控制对象仍整体参与图同步；外部根来自OFD总引用减去inflight，未accept的child由listener保持可达。图的可睡眠资格按UNIX控制资格→rights资格→端点短锁取得。存在UNIX引用边时才运行非递归标记；摘除不可达控制记录后，归还图锁才执行最后put。被收集socket的普通FD报文随socket销毁释放；失败退休对象的重试仅使用端点短锁维护所有权。

CPU0已有网络执行者推进收集和退休清理。最后关闭暂时失败时，退休对象继续持有原OFD并沿已有期限重试；停止网络前后主动drain，排队控制对象、退休链或锁外释放仍在途时返回EBUSY并保留owner。原生布局、解析及完成前缀依据仓库固定Linux的net/core/scm.c、net/unix/af_unix.c和fs/file.c。直接消费者为tests/workloads/network/unix_control.c，宿主所有权与失败交错由network_owner的unix-control-owner、unix-control-cleanup用例检查。

## 发起 CPU 与等待寿命

TCP 请求由 syscall 栈持有，OFD 覆盖整个接纳过程；用户页只借到实际 COPY 完成。排队请求使用自己的等待 token，工作链锁内登记、发布结果并定向通知，锁外休眠和结束等待。ACK 和发送额度变化只通知 socket 就绪等待者。归属核的执行者一次从同一 socket 取出至多四个已排队的连续普通发送请求，共用一次输出；每个请求仍占一个服务单元，剩余工作放回队尾。带 IO_MORE 或用户 MSG_MORE 的请求单独处理；先完成此前捕获的 OUTPUT，再接纳新前缀，以免提前发送仍可合并的字节。发送请求本身不安排接收资源和 TIME_WAIT 回收，真实输入和关闭仍负责安排回收。UDP 由调用者在最终 pbuf 中填入载荷，持有 UDP PCB 的协议执行者按请求发送并释放。只有协议内存未启用并行保护时，分配和放弃发送的回收也需要协议资格。UNIX 调用者准备最终 packet，发布前在端点锁内重新核对对端。请求和 OFD 借用一直保留到完成状态发布。

普通缓存文件通过 `sendfile` 向传输控制协议（Transmission Control Protocol，TCP）套接字发送多页数据时，调用线程每批借用最多 32 KiB 的连续缓存页，将实际取得发送额度的前缀作为一个向量提交。缓存借用保活来源页面，传输控制协议继续持有自己的载荷副本，直到收到确认或连接终止。调用线程在等待发送额度前归还缓存页，强制退出先撤下套接字请求，再归还仍有借用的缓存页。共享或显式文件位置按实际接纳字节推进，非阻塞短写和中断后的已完成前缀沿原接口返回。

单核运行时，阻塞式发送在登记等待前可以借用协议执行资格，处理已到达的确认报文。每批最多八个接收报文。若该批取满且本连接仍无发送额度，调用线程可以继续处理下一批，至多四批。队列取空、额度恢复、出现挂断或待决错误时结束辅助，再按普通路径核对发送条件。调用线程在进入辅助前释放原端点短锁，剩余报文由原网卡线程推进。

普通 I/O 与 socket syscall 共用 `kernel_socket_wait_ready`。调用者传入一次捕获的绝对超时，端点锁内检查条件并登记，锁外 park/finish；就绪先于超时判断。接收预约限制读取的原有字节前缀，复制到该前缀末端即停止，锁外复制不读取并发追加修改的尾链接。

用户复制可以触发文件页的冷缺页，调用链继续进入 ELF 源读取、页缓存、ext4 和块设备等待；这些调用共同使用当前任务的 8 KiB 内核栈。sendmsg/recvmsg直接进入消息处理入口，不跨载荷缺页保留标量及控制调用的栈帧；发送预检复用已经持有的端点保护。UNIX 载荷复制与复制后的 packet 发布使用独立栈帧，TCP pin 工作区与普通文件暂存路径也分别构帧；MM 的映射文件缺页与 ELF 缺页分开，避免无关工作区同时占栈。检查栈余量需要实际触发冷页并等待任务退出后的高水位报告，单个函数的静态帧大小不足以说明整条调用链安全。

epoll控制仍在CPU0，等待扫描和事件复制在发起CPU执行，详见[文件接口](kernel-files.md#epoll-事件通知子系统)。独立状态锁维护候选、控制代次、ONESHOT 和扫描引用；目标就绪查询、用户复制、上层通知及等待游标排空在锁外执行。端点通知取得现有队列/节点借用后释放端点锁，因此远端回调与 DEL、MOD、关闭可以交错，实际回调归还前存储保持有效。

## 验证

`tests/userland/api8.py --program ipv4_abi` 用同一 ELF 对照 Linux 的 raw 创建、五种选项、短缓冲、fault 输出顺序、设备权限与组播源地址。参考内核须启用 NETDEVICES，否则 Linux 自身不提供设备绑定选项。`tests/userland/run.py --workload ipv4_basic` 检查 raw 双接收、PEEK/TRUNC、大包回环和广播许可；`tests/userland/network-external.py --workload ipv4_basic --only both` 使用同一程序和原 BusyBox 进行 TAP 对照，核对线上 TTL、分片、来源、不同设备同端口的 UDP/TCP 隔离及 ping 参数。`--cpus 2` 将实际调用者固定在 CPU1 并检查记录；RV 支持 legacy/modern，LA 使用 modern。原 LA BusyBox 可用显式 `--busybox` 指定静态原 ELF，执行器记录路径与哈希，不替代为另一工具链构建身份。这些既有消费者继续使用普通短读循环完成精确收包；MSG_WAITALL 的独立契约见上述接收说明。

`tests/host/network_owner.py --case raw-icmp-owner --case raw-icmp-close --case ipv4-options --sanitize` 以真实协议实现检查独立报文版本、过滤/队列/分配失败、实际回调期间关闭、广播、设备隔离和握手中的绑定继承；`--page-shift 14` 覆盖 LA 页大小。暂停发生在实际过滤回调和协议资格交接处，不以延时猜测交错。

静态/动态 musl 的原生并发短例复用 `tests/userland/run.py --workload smp`。私有 `USER_SMP_COMPOSITION=1` 内核配合 `--observe` 检查每个发起 CPU 的预约、复制、pin 与等待；三项握手分别暂停真实接收预约、发送页 pin 和 epoll 通知借用，核对追加/最后关闭、mprotect 和 DEL 的实际等待及资源归还。正式性能内核关闭这些观察。

`tests/host/socket-accept-allocation.sh` 在真实 OFD 分配处注入 EMPTY，检查连接保留、重试双向通信及 FD、凭据和堆页归还；另覆盖等待失败和地址输出失败的不同副作用。`tests/host/network_owner.py --case unix-space-topology` 检查互连流量不触发空间回调、断连恢复单向订阅及重连唤醒；`unix_named` 用户程序通过实际 epoll 等待与队列空间释放核对跨核通知。

`unix-packet-boundary` 覆盖默认、最小和最大SNDBUF的完整分配大小、向量前缀及计费归还。`unix-write-notify` 直接检查成功发送不产生无状态变化的本端通知，同时检查消息消费和复制取消仍通知发送者。`tcp-batch` 在真实 tcp_write 的 COPY 之间握手，让另一 CPU 预约剩余额度，检查整批接纳期间额度保持保守且最终归还；相同数据分段不会增加端点保护次数。`tcp-quota-notify` 和 `remote-quota-notify` 检查两发送者共享预约时不产生空输出或无额度变化的广播，短前缀与取消仍恢复可写通知，CPU0 完成远端排队请求后交还真实请求和 OFD。`remote-direct` 在 worker 运行前确认远端已完成 COPY 并归还借用，再检查 CPU0 输出及接收字节。`queued-completion` 暂停已接纳后的实际输出，核对调用者先归还请求和 OFD pin，最后关闭在协议资格处等待，解除暂停后内容完整交付并归还对象。

```sh
make test-lwip-host test-lwip-reassembly-host test-ethernet-worker-host
make test-network-riscv         # IPv6 UDP/TCP、双栈 accept，同一 ELF 对照 Linux
make test-userland-riscv test-diff-abi-riscv test-stack-usage
python3 tests/userland/inventory/run.py --suite libc \
  --case libc.static.socket --case libc.dynamic.socket \
  --require-pass --output build/socket-program-check
```

`tests/userland/run.py --memory 512M --memory 1G`分别启动同一ELF的两种RAM，
每个配置的镜像/日志独立；result记录实际启动参数。RV official profile固定1GiB，
冲突的显式RAM参数返回错误，不能忽略参数后给启动贴错标签。

host 入口实测 UDP loopback、PCB 16 个用尽后第 17 个失败、全部释放和再分配，另在 `sndbuf>0` 时耗尽全局 TCP segment 池触发真实 `tcp_write ERR_MEM`，以及 TCP 握手/关闭后推进 200 秒协议计时、池用量回到基线。此前的 Linux/BoarOS 差分记录完全一致，包含 12 条新增 socketpair 差分记录，覆盖坏族、坏标志、坏协议、空指针、stream 双向读写、关闭 EOF、dgram 边界截断与 flags 校验。真实 pthread U-mode 另覆盖零长度 datagram、共享 socket 双读、阻塞读时 close/fd 复用、双读线程组 SIGKILL、全局池压力下错误可写事件及释放后写入进展。原版 libc-test `functional/socket.c` 的静态、动态 ELF 入口由程序清单 runner 运行，入口及结果规则见[程序环境](program-environment.md)。

Linux ABI 依据本地 `references/linux/net/socket.c`、`net/ipv4/af_inet.c`、`fs/read_write.c`，固定 Linux v7.2；测试输入来自 `references/oscomp-testsuits` 固定 pre-2025 版本，精确身份在来源清单和机器归档。核对后运行 `make prune-build` 清理日志和镜像。

本轮选项阶段接入 getpeername(205)、getsockopt(209)，以及 REUSEADDR、TYPE、
ACCEPTCONN、ERROR、KEEPALIVE、收发预算/超时、TCP_NODELAY、TCP_CORK、TCP_MAXSEG 和
IPV6_V6ONLY。SO_ERROR 读取清除待观察错误，连接阶段 RST 返回 ECONNREFUSED；
已建立 RST 保留 ECONNRESET。INET收发预算默认/上限 64 KiB，设置按 Linux 倍增，
固定 RV64 最小值分别为 4608/2304；不预分配。接收队列和 TCP 发送接受量执行
预算，lwIP 窗口和全局池仍可更紧。UDP connect 设置/重置默认对端并由协议过滤。
监听启用官方 backlog 计量：待 accept child 保留资格，accept/abort 归还。
旧 glibc 使用兼容配置。

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
发送方向只产生接收 EOF，不阻止向它回复；预留容量前和复制完成后都按本地发送
方向、对端接收方向及对端存活判断能否发送。对端关闭接收方向使发送 EPIPE/SIGPIPE，
NOSIGNAL 只抑制该调用的信号。FIN、reset 和本地
关闭分开决定 IN/OUT/HUP/RDHUP。SYN 拒绝是 ECONNREFUSED，建立后 reset 是
ECONNRESET；SO_ERROR 或读/写消费 pending error，后续 EOF/EPIPE 由真实协议
终止状态决定，不能永久重复历史错误。

accept callback 延迟释放 backlog 资格，真正 accept 时归还；失败安装销毁 child，
reset 排队 child 在下次检查摘除。监听器的预算、超时和选项传给 child，NODELAY
同时设置实际 PCB。TIME_WAIT 在移交协议池前摘除所有堆回调；LAST_ACK 的
ERR_CLSD 解除 PCB 借用。运行期 timer IRQ 只通知后台 owner；最后 OFD 关闭后的协议回收不依赖另一个用户 syscall。

TCP refused-data 在接收内存不足时保留 pbuf 并登记重试，由任务上下文的有界服务处理接收堆归还或到期重试。
UDP 分别记录显式地址与非零端口绑定，AF_UNSPEC 断开释放自动端口；TCP
MSG_TRUNC 沿 reservation 消费而跳过 scratch/usercopy，UDP 和 UNIX 保持各自语义。

UDP 排队同时受每 socket 配额与共享协议堆约束。队列不能耗尽给下一条最大报文
及 TCP 控制所需的余量；过载按 UDP 语义丢包并归还 pbuf。五条不限速 UDP 原负载
曾先耗尽池，修复后控制连接仍进展；丢包量必须连同吞吐报告。默认/最大预算仍
64 KiB，未扩大池或伪造 POLLOUT。pthread 压力回归现在保护 UDP 过载时 TCP 控制
仍能传输；raw TCP segment ERR_MEM 的重试契约继续由 host fixture 验证。

窄验证：`python3 -B tests/userland/run.py` 同 ELF 对照 Linux；固定内容用
`--workload content`，包括 16 MiB、五连接各 8 MiB、IPv6 UDP 一万次 64 字节
请求响应。原 ELF 执行入口为 `tests/userland/network-consumers.py`，按原客户端参数、真实
服务端 ready、逐项有效时间/接收量及 wait status 判定；原脚本退出零不替代传输。

## 原版应用与诊断入口

原 ELF 输入来自公共镜像，版本为 iperf 3.13、netperf 2.7.0。执行器保存实际
argv、工作目录、接收端结果、wait status、时间及输入版本。原脚本与逐项启动新服务端的受控流程
分开验收，listener ready 使用实际输出握手，客户端参数保持原值。

```sh
python3 -B tests/userland/network-consumers.py --libc both --suite both --case all
python3 -B tests/userland/network-consumers.py --only boaros --libc both --suite both --case script
python3 -B tests/userland/network-consumers.py --only boaros --libc both --suite both --case representative --repeat 3
make COST_DIAGNOSTICS=1 all
python3 -B tests/userland/network-consumers.py --only boaros --libc both --suite both --case representative --kernel build/cost/kernel-rv --observe
```

只有 COST 构建提供只读 `/proc/boaros_net_stats`，复用现有协议统计，不保存对象
引用。TCP写入调用/字节为64位，lwIP包/错误计数为16位，会环绕，不能当作大流量
窗口的完整总包数；pool用量和高水位是即时值。窗口仍使用已有 cost v1 schema，
协议排队次数、park 次数、排队到完成的累计 ticks 与实际协议执行 ticks 分开记录；排队等待进一步分出入队至执行、完成至调用者恢复，两者包含调度和状态锁等待。多任务累计时间与墙钟分别解释，排队总量已包含执行部分。聚合存储沿用内核的编译期预算约束，默认构建没有新增诊断节点。观测影响吞吐，关闭
观测的独立批次才是性能结果。

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

发送者预算入口：`python3 -B tests/userland/run.py --workload budget`与
`tests/userland/run.py --workload budget`，现在均对照Linux。测试先填满实际
sender并从proc确认子任务已阻塞，检查接收缓存变更不解锁、发送缓存缩小/增大、
peer关闭的类型对应错误、SIGKILL和内容守恒。相同ELF双侧运行，不按具体packet
个数推断内核布局；旧接收配额成本契约由用户明确选择的Linux发送者模型替换。

标量及多iovec的TCP流发送共用有界页pin批次，正式游标只推进实际COPY接纳量；
EAGAIN前归还pin，等待后重新准备未接纳输入，具体复制规则见[文件I/O](kernel-files.md)。
非阻塞/fault/信号/期限仍返回已接受前缀；用户页不借用到 ACK。
`--workload content`校验单TCP16MiB、五TCP各8MiB及UDP内容。
游标不跨系统调用保留内容。原iperf使用非阻塞socket，短写/EAGAIN后再次write仍可能
重新处理未接纳输入。

实际阻塞接收等待使用`kernel_socket_receive_ready`，活动reservation始终未就绪；
对外poll仍保留终止事件。HUP/ERR不能让第二读者绕过owner并空转。io-sleep fixture
暂扣真实usercopy调用，确认第二读者阻塞；finish/fault后唤醒，期限和信号可打断等待，
最后页数回到基线。四种transport/cache均保护这一交错，不新增用户ABI或长寿命引用。

## Ethernet与真实宿主应用（2026-10-02）

根启动对象持有独立VirtIO-net、netif与joinable worker，DTB发现device ID和IRQ。
官方Ethernet入口接纳custom pbuf，启用ARP及48 pbuf/8对象的IPv4重组；MTU1500、
MAC来自设备、静态地址由构建配置决定。RX loan直到最后协议/reservation引用释放；
回退与UDP接纳保留控制余量。IRQ只收割，raw API由任务mutex串行并开放IRQ；worker每批
八帧，失败NIC也继续共享loopback与协议定时器。无NIC时同一owner退化为
timer-only worker：推进协议定时器并按min(下一socket期限, now+5×frequency)
睡眠，最后的OFD定时回收不再依赖用户再次进入syscall。停止join、清理接口引用、
reset确认；设备失败不等于DMA停止，TX在途owner与RX借用一律保留到reset确认，
已完成owner在worker归还。接口查询来自真实netif，DOWN撤下RUNNING。
详细预算、寄存器、失败与验证命令见[网卡模块](riscv-virtio-net.md)。

TX 使用 indirect 表借用每包最多两段镜像内 payload，其余回退复制，用户接收仍复制。IPv4 外部应用测试入口见 `tests/userland/network-external.py`；默认网关可由已实现的 IPv4 配置接口设置。内核不提供 DHCP/DNS/TLS 服务，用户态协议库与内核网络能力分别评价。

### Ethernet IPv6 静态配置、RA/SLAAC 与通信

eth0 从设备 MAC 派生 `fe80::/64` 地址，使用已有 ND 定时器执行 DAD（重复地址检测），不增加 worker、轮询器或新缓存。DAD 完成前不能绑定地址；收到指向 tentative 地址的 solicited-node NS 或 all-nodes NA 会标记重复，不能以未验证的地址发送普通包。链路本地 bind 要求有效 scope/interface index，也可由 SO_BINDTODEVICE 提供；bind 的显式 scope 同时选择绑定设备，非法 index 返回 ENODEV，未配置或尚未可用的本地地址返回 EADDRNOTAVAIL。connect 的显式 scope 与已绑定设备冲突返回 EINVAL，sendto 则优先使用已绑定设备。rtnetlink GETADDR 显示实际地址、tentative/dadfailed 状态，GETROUTE dump 显示可用地址的本地及链路路由；IPv6 单条 route lookup 与 netlink 修改仍明确返回 EOPNOTSUPP。

已支持真实 Ethernet 上 IPv6 TCP/UDP、ND 的 NS/NA 与 ICMPv6 echo responder。通过 AF_INET6 socket 的 SIOCSIFADDR/SIOCDIFADDR（Linux `in6_ifreq`）配置/删除静态单播地址，前缀 0–128；保留自动 link-local 槽位，另有两个静态地址槽位，满时返回 ENOBUFS。新地址必须完成 DAD。管理 DOWN 期间可添加永久地址，其 DAD 等到 UP 才进行；MTU<1280 时添加返回 EINVAL。当前静态地址配置限 Ethernet，不修改 `lo`。SIOCADDRT/SIOCDELRT 使用 Linux `in6_rtmsg`，支持唯一 IPv6 default gateway（metric 0/1024）；connected route 按实际地址前缀选择最长匹配，不再套用 lwIP 的隐含 /64 或 IPv4 default。源选择比较真实公共前缀并限制到源地址前缀，只有完整 128 位相等才当作本地地址完全匹配；配置、网关与前缀随 netif 生命周期，使用已有协议所有权，不引入动态 FIB、worker 或每包配置分配。非默认手工路由、source route、非支持 metric/flags 明确返回 EOPNOTSUPP。单独删除地址清理 ND/PMTU，但不偷偷删除已配置 default，也不修改既有 socket 的绑定或同步 abort TCP；重新添加同地址并完成 DAD 后既有 TCP 可以继续。最终输出拒绝失效源和任何物理出口的 `::1` 源/目的；显式设备或 /0/default 不得绕过 loopback 边界。提供下述 RA/SLAAC；未实现 IPv6 raw/ping socket、MLDv2 或 SSM，忽略 Redirect、RIO、RDNSS/DHCPv6，不提供 Linux accept_ra/sysctl 配置面。应用 ASM 组播订阅见下节。IPv6 不进行发送分片或分片重组；atomic fragment（offset=0 且 M=0）可通过普通输入路径，但任何含 Fragment Header 的 ND 报文按 RFC6980 丢弃，包括其后还有扩展头的情形。UDP 超所选链路/路径 MTU 返回 EMSGSIZE，IPv6 零 checksum 与错误 checksum 丢弃，IPv4 的零 UDP checksum 规则不变。

接口管理 DOWN 删除静态/动态 IPv6 地址/default、撤链路本地地址并清理 ND；UP 自动重建链路本地地址、重新 DAD 并发 RS，动态配置只能重新从 RA 学习。carrier 暂时断开不等同管理 DOWN。运行时 MTU 变更清理该接口 ND 队列及目的 PMTU 缓存；低于 1280 删除静态 IPv6 地址/default 并停止该接口 IPv6 收发，恢复后自动重建 link-local 并重新发 RS；这对应 Linux 默认 keep_addr_on_down=0，不支持地址保留 sysctl。最终 Ethernet IPv6 输出再次校验源地址可用性，防已绑定 socket 或延迟 ND 报文跨越地址失效继续使用旧源。UDP 与可能携带扩展头的非直接 TCP/ICMPv6 接收共用原有预算，保留控制报文余量。

RA 在完整 Ethernet/IPv6/ICMPv6 校验后处理：来源必须 link-local、Hop Limit255、Code0，先扫描全部 option 边界和固定长度，再修改任何地址、路由或 MTU；未知合法 option（包括跨 pbuf 的长 option）跳过。同链路恶意但格式合法的 RA 仍可改变网络状态，本实现不是认证或 RA Guard。

固定资源复用现有 ND 表与一秒定时器：两个静态槽、两个独立 SLAAC 槽、五个 L prefix 和三个 RA default router，不驱逐静态地址给自动配置让位。L 支持0–128任意前缀，A仅支持/64并从MAC派生IID；二者独立，A-only不生成隐含 connected route。静态同址不转换为动态。默认路由优先显式静态（metric1024），否则在RA（metric2048）内按邻居可达状态与router preference选择；始终先最长 connected prefix。NUD有限探测失败后保留广告寿命，但将邻居降级、清等待包/目的cache并停止空闲重试；活router优先，新实际发送或新RA才重新开启有限探测。该固定策略不是完整Linux FIB或多策略路由。

SLAAC valid/preferred lifetime按RFC4862更新：preferred归零立即deprecated；两小时下限只保护已有地址valid，不保护L prefix或默认路由；无限值不递减。到期撤地址而保留绑定PCB，清ND排队及目的cache，最终输出仍拒绝失效源。RA MTU仅接受1280至物理MTU，可下降也可恢复；清旧PMTU并在TCP发送既有unsent/retransmit前按当前有效MSS切分，保留对端MSS限制。RA的hop-limit/reachable/retrans参数本阶段仍使用本地固定策略。

定向验证：

```sh
python3 tests/host/network_owner.py --case ipv6-ra --case ipv6-global --case ipv6-linklocal --page-shift 12
python3 tests/host/network_owner.py --case ipv6-ra --case ipv6-global --case ipv6-linklocal --page-shift 14
python3 tests/host/tcp-ipv6-mtu.py --page-shift 12
sh tests/host/rtnetlink-ipv6-ra.sh
python3 tests/userland/ipv6-external.py --arch riscv --cpus 4 --kernel <kernel> --program <ipv6_external ELF>
python3 tests/userland/ipv6-external.py --arch loongarch --cpus 2 --kernel <kernel> --program <ipv6_external ELF> --qemu <qemu>
```

静态 global 验证复用下列 runner 的 `--global-address`，ELF 编译增加 `-DIPV6_STATIC_GLOBAL`：先以 `2001:db8:1::100/64` 完成 UDP，再改 /128并增加 `fe80::2` static default通过TCP；删除/重加绑定地址保持TCP并清理静态配置。RA路由可同时存在，删除static default不再假定必然不可达。SLAAC用 `--slaac` 配合 `-DIPV6_SLAAC`，不调用地址配置ioctl，由真实slirp RA生成全局地址并通信；注入畸形尾option、错误source/hop/checksum、atomic及atomic+options RA，后续1452字节UDP证明非法MTU更新未生效。

ELF 来源 `tests/workloads/network/ipv6_external.c`，以目标 musl 静态编译（LA 链接页对齐 16384）。外部 runner 显式配置 QEMU slirp 的 IPv6 prefix/host 为 `fe80::/64`、`fe80::2`，由独立宿主 IPv6 TCP/UDP socket 验证内容，不是 guest 的 `::1` 回环。UDP 检查 64/1232/1452 字节内容、peer/scope、1453 字节错误边界；TCP 检查 64 KiB 全内容和半关闭/EOF。QEMU Ethernet 注入通道依次送零 checksum、错误 checksum 和正确响应，以正确响应作为处理顺序屏障。host 用例另外覆盖 DAD 冲突、地址失效、ND 队列清理、MTU 恢复、RA完整预检、L/A独立、寿命与路由到期和设备失败。`ipv6-prepare-withdraw` 在真实 parallel prepare 与 commit 之间撤销并释放模型接口，验证准备阶段仅保存数值 scope、提交阶段持协议资格查找接口，并完整归还 payload/request/pin；真实 NIC stop 另外先 drain 活动调用者。每次外部运行保存命令和 kernel/program SHA256 manifest，并在结束时再次核对身份。测试不声称公网、Linux TAP 差分、实板或性能收益。

Linux ioctl/DOWN/MTU/删除匹配参考 [Linux v6.12 addrconf.c](https://github.com/torvalds/linux/blob/v6.12/net/ipv6/addrconf.c)、[route.c](https://github.com/torvalds/linux/blob/v6.12/net/ipv6/route.c)。本环境不具备 Linux NET_ADMIN/TAP，未执行 namespace 配置差分。

语义参考：[Linux ipv6(7)](https://www.man7.org/linux/man-pages/man7/ipv6.7.html)、[RFC 4862 §5.4](https://www.rfc-editor.org/rfc/rfc4862.html#section-5.4)、[RFC 8200 §8.1](https://www.rfc-editor.org/rfc/rfc8200.html#section-8.1)。

TX 的 SG 与复制回退在槽满时都登记 NIC 容量等待。无 indirect 特性或 pbuf 链
不满足 SG 条件时，复制发送返回 `EAGAIN` 仍属于 NIC 槽不足，不能落入协议池
等待。上述 `network_owner.py` 分别覆盖两种回退原因，冻结时钟、禁用包与 timer
服务并确认协议池没有归还，仅通知 NIC 新容量就能重新输出实际 TCP 内容；
该边界测试不把正常 timer 重传当作容量通知正确的证据。

## TCP 发送接纳与复制（2026-10-05）

TCP 默认采用 Nagle 的 Minshall 规则，与固定 Linux `tcp_minshall_check/update` 的边界一致：已经发出的完整 MSS 包不阻止第一个小包，尚未确认的小包会阻止后续小包。PCB 尾部记录最后一个新发小包的结束序列；主动／被动建连初始化该字段，实际输出成功才推进，重传和输出失败保留原值。判断使用 TCP 序列号比较，覆盖 32 位回绕；累计 ACK 越过该边界后解除等待。这样保留小包合并，同时减少整 MSS 在途数据与延迟 ACK 组合产生的等待。

TCP 的 `send/sendto/sendmsg/sendmmsg` 接受用户 `MSG_MORE`，`splice` 的同一提示也沿 TCP COPY 路径生效。用户提示沿协议控制块保留，输入回调、定时器和普通输出共用同一状态。未曾发送的短尾段继续暂留，独立确认报文（Acknowledgment，ACK）、完整报文段、重传和结束连接的报文均可推进。收到首个有序数据时，接收方立即安排 ACK，后续数据继续使用正常的延迟确认。这样在新连接上连续发送的小报文无需等首个延迟确认周期，发送端仍遵守 Nagle/Minshall。接受未带 MORE 的数据后解除当次提示。零长发送和首字节前 fault 保留此前提示，已有前缀后的 fault 保留当次提示，空间不足则在等待前显式推送成功前缀。

`TCP_CORK` 是共享 socket 的持久配置，监听端点传给 accepted 子端点，IPv4/IPv6 相同。它对普通输出也抑制未满的新尾段，优先于已开启的 NODELAY；关闭 CORK 尝试发送已排队数据，但 NODELAY 未启用时仍遵守 Minshall。显式设置 NODELAY=1 会推送当前前缀，不清除 CORK，之后新短尾仍被 cork。SHUT_RD 不解 cork，SHUT_WR 和最后 close 仍保证 FIN 前的载荷进展。

暂留尾段的绝对毫秒期限保存在 PCB 内，首次阻塞时设为当前时间加 200ms，追加不延后。CPU0 网络执行者和各 TCP owner 只把已暂留的 PCB 按期限排序，等待时取首项期限，到期在原协议资格内尝试发送。实际到线仍受调度、拥塞/接收窗口和 NIC 容量约束，不承诺 200ms 墙钟内送达。期限随 PCB 迁移，摘链/free 同时结束计时，不新增 timer 对象或 socket 引用。显式 push 保存已经排队的序号边界，窗口和 NIC 重试不能遗失这次推送；不会为了区分新旧字节切开已有尾段。

`tests/host/network_owner.py --case tcp-more --case tcp-cork` 以冻结时钟检查短尾、MSS、普通输出、期限/溢出、owner 迁移与回收。`tests/workloads/network/tcp_cork.c` 是 Linux/BoarOS 共用 libc 消费者，`tests/userland/tcp-cork.py` 使用已构建的静态程序与内核运行；它核对字节及有限推进，不把 PSH、接收批次或精确墙钟值当作接口保证。语义依据为固定 Linux `f4cdf7ca9a1f` 的 `net/ipv4/tcp.c`（tcp_push、tcp_sendmsg_locked、NODELAY）、`tcp_output.c`（Nagle、probe）与 [tcp(7)](https://man7.org/linux/man-pages/man7/tcp.7.html)。

开启 `TCP_NODELAY` 时先禁用 Nagle，再立即尝试输出已有 unsent 数据。NIC 暂无容量时登记原有 NIC 容量等待，协议内存不足时使用原有 pool 等待；容量事件推动重试，不依赖收到 ACK 或推进定时器。关闭 NODELAY 只恢复 Nagle。用户数据报发送使用任务登记的请求和 iovec cursor，退出清理沿同一请求归还 OFD 和包引用。

网络数据报的 `MSG_MORE` 由同一个套接字的协议执行者维护。第一次带标志的发送固定目的地址和端口，后续发送使用这些值。已复制的报文缓冲区按顺序交给待发送的数据报，最终一次无标志发送把它们作为单个数据报提交，并返回本次接纳的字节数。按数据报及套接字的大小上限限制待发送数据，同时约束许多小分片占用的协议内存。追加失败和最后关闭会归还已保留的缓冲区。

INET stream 的 write/writev/send/sendmsg 使用任务登记的 `kernel_socket_write_request`，
与数据报请求共用退出登记但不拥有数据报 packet。请求持有原 OFD pin，
`stream_reserve/commit/cancel` 只预留本 socket 的 TCP 接纳字节，按 sndbuf、socket 预算、
队列空间及已经预留量决定上限，不预占 segment、pbuf 或 NIC 槽。其他 writer、sendfile
及 POLLOUT 都扣除现存预留；SO_SNDBUF 缩小或连接状态变化后，commit 再检查真实状态。
用户复制可缺页睡眠，不持 raw 资格；取消、fault、错误、返回和任务退出均归还未提交预算。

扩展现有 iovec cursor 提供页内有界 span。先验证参数的数值范围及连接状态，再取得接纳量，
最后才解析用户 payload；预先无容量不解析用户页。标量及向量pin有界来源页并直接 COPY 到协议，
一个页大小前缀内不超过四段不连续来源时，载荷直接进入协议；提交返回后归还 pin，等待后重新取得未接纳输入。超过四段时使用已有 scratch 合并这个前缀，避免将小片段拆成多次 CPU0 请求；部分接纳后保留尚未提交的 scratch 尾部，继续按原预算机制提交。
多 iovec 的直接路径复用任务工作区保存页引用及片段描述，阻塞后按已接纳位置继续准备。
两种输入都复制到 TCP 持有的缓冲，发送端保留至 ACK，非阻塞返回已接受前缀，
未接受用户内容不跨系统调用持有。
`FILE_IO_HOST_TCP=1 sh tests/host/file-io-concurrency.sh` 将生产 TCP/lwIP、MM 和 syscall I/O
连接起来，以真实协议内存耗尽制造 ERR_MEM，等待时撤权并替换来源映射，检查新内容
恰好接纳一次、跨页 fault 前缀及页引用归还。

固定 Linux 的 `net/socket.c` 和 `net/ipv4/tcp.c` 约束状态与 payload fault 顺序：
数值越界地址先 EFAULT；有效地址范围中的不可读页在满缓冲时为 EAGAIN，
在关闭/未连接时为 EPIPE，NOSIGNAL 仅抑制 SIGPIPE。空 TCP 发送也检查终止状态。
`tests/workloads/network/admission.c` 由 `network-riscv.py --workload admission` 在同 ELF 双侧验证，
包含头/iovec fault、范围错误、部分接受字节守恒、SIGPIPE 与实际阻塞发送者 SIGKILL。
已有进展时的 shutdown 返回正前缀且不发 SIGPIPE；RST 后也保留 pending error，下一次调用才观察并消费 ECONNRESET。
跨页 fault 的提交片段大小是内部策略：本输入 Linux 未提交返回 EFAULT，BoarOS 返回已提交
4096 字节，测试分别验证接收量与返回值，不能将该项写成逐值差分一致。

真实调度的 io-sleep 包装用户复制边界，覆盖保留 reservation 时关闭并复用 fd、并发 shutdown、
用户页撤销后的 fault 和预算重取。它模型化可睡眠复制的交错，不宣称复现硬件缺页的具体时序。

`python3 -B tests/riscv/network-owner.py --kernel kernel-rv` 在 legacy/modern
两种 VirtIO 传输下关闭 indirect 特性，以真实 TAP 的双向 bulk/control 内容检查
验证复制路径，并要求 driver 的 `tx-copy>0`、`tx-sg=0`、`errors=0`。该入口只运行
功能 smoke；它与冻结资源边界的 host 用例共同验证，不单靠最终传输成功证明
NIC 容量通知没有遗漏。

TCP 的 27 组窗口/池/堆候选由 `BOAROS_LWIP_WINDOW_MSS`、`BOAROS_LWIP_POOL_SCALE`、
`BOAROS_LWIP_MEM_SCALE` 选择，当前默认 8/4/2，PCB 数不变。工具、静态/动态内存口径、
控制流 tail 与固定输入约束见[预算实验](../learning/data-path-budget-experiments.md)，
可运行候选不等于吞吐已经验收，默认值须依据匹配结果由人选择。

共用Ethernet的入口为`kernel_network_start(owner, heap, device, frequency)`。device
由平台/root持有，网络层仅借用；NULL创建独立timer-only worker。正常停止先
关闭工作接纳，把尚未执行的发送请求完成为 EIO，唤醒原调用者归还 pins，再 join。已经执行的 COPY 在同一协议资格内先完成；确认DMA停止后清理netif并归还TX/RX引用，再释放网络控制对象；
平台随后销毁device/transport。LA真实PCI与无NIC双侧结果见[net模块](riscv-virtio-net.md)。

2026-10-07新增`--workload unix_sender`保护>64KiB单消息、sndbuf-32边界、有限零
消息、坏用户页、sender关闭后读取、关闭peer的两次错误及各类型信号规则。
LA的budget/sendfile/unix_sender均在Linux/BoarOS两种RAM检查实际退出和根基线；
内部owner另由`tests/host/network_owner.py --case unix-sender --sanitize`验证两种
关闭顺序，pthread 用例覆盖真实等待与取消。

`tests/workloads/network/unix_seqpacket.c`复用共同控制消息检查，另覆盖命名连接、记录边界、空记录/EOF、实际用户入口的来源名称、错误顺序、shutdown及等待。宿主network_owner中的SEQ记录、预约、SCM、pending child和shutdown专项检查真实引用与额度归还。

## BPF 接收过滤

socket 持有已校验程序的引用，替换或 detach 在端点锁下交换，执行方取得自己的引用后锁外运行。UDP 和 TCP 的协议入口先过滤再改变接收与连接状态；直接从 pbuf 链读取，返回 0 丢弃，正返回截断但保留协议处理所需的头部。TCP accept 继承过滤器与锁定状态；UNIX DGRAM/SEQPACKET 在队列接纳前过滤，STREAM 的接收保持 Linux 行为。过滤后丢弃仍按发送成功处理，SCM_RIGHTS 和载荷沿请求原有归还路径释放。

lwIP 入口传递实际 UDP/TCP PCB，按已登记的 socket 回调识别关联；raw API 调用者的任意 callback_arg 不解释为 socket。TCP SYN_RCVD 在 accept 前借用仍活跃监听 PCB 的关联，已建立连接使用自己的关联；本地快速输出使用同一判断。设备停机及 socket 销毁先解除回调，已执行过滤所借用的程序和包/DMA 引用各自持有到停止使用。

被动建连的首次握手应答使用同一传输控制协议（Transmission Control Protocol，TCP）输出入口。网卡暂时没有发送槽时，半开连接在自己的协议控制块上登记重试，由下一次真实容量归还触发。此时尚未创建已接纳连接的套接字。连接撤销时协议控制块及其待发报文一起释放，已登记的重试不会继续持有该对象。

接口、权限、共同执行器和工具入口见 [BPF 模块](kernel-bpf.md)。

### TCP keepalive policy

`TCP_KEEPIDLE`、`TCP_KEEPINTVL`、`TCP_KEEPCNT` 分别以秒、秒、探测次数读写；默认
7200/75/9，合法范围 1..32767、1..32767、1..127。设置这些值不隐式开启
`SO_KEEPALIVE`。策略属于共享 socket，dup 共享，listen 转换不丢失，accept 子端点
继承接受时的值，之后父子各自独立。关闭/错误回收 PCB 后仍可查询策略。

查询仅取 endpoint 锁；设置在既有协议生命周期保护内更新策略和活跃 PCB。
LISTEN PCB 比活跃 PCB 小，不能写入其 active-only keepalive 字段；在 accept 时安装。
沿用 lwIP 的 500ms slow timer、最近活动和探测计数，没有新增逐包统计、周期任务或
独立 socket 定时器。空闲 ESTABLISHED/CLOSE_WAIT 连接在开启 keepalive 后发送探测，
ACK 恢复活动计时；探测预算耗尽沿现有 timer error 路径返回 ETIMEDOUT。
这里不承诺与 Linux 精确相同的墙钟探测时刻。

`tests/workloads/network/tcp_keepalive.c` 是 Linux/BoarOS 共用消费者，检查范围、
短 optlen、错误指针、optlen 先于 optval 发布、dup、IPv4/IPv6 accept 继承及协议错误。
`tests/userland/tcp-keepalive.py` 接收预构建 kernel/static ELF 做定向 guest 验证。
`tests/host/tcp-keepalive.sh` 用生产 lwIP 与 socket service、冻结时钟推进现有 timer，
检查真实探测、禁用状态、超时、策略存续和资源归还（4K/16K 页）。
Linux 语义参考 [v6.6 tcp.c](https://github.com/torvalds/linux/blob/v6.6/net/ipv4/tcp.c)。

Keepalive 与 `TCP_INFO` 分开交付。曾评估仅到 snd_mss 的 20-byte 前缀，但
iperf 成功后不检查返回长度便读取后续统计，不能将这种短返回当作完整消费者兼容。
当前统计接口与边界见下面的 TCP_INFO 小节。

定向验证命令（编译器、QEMU、pahole 由调用者配置，不下载/跑宽泛基线）：

```sh
cc -O2 -Wall -Wextra -Werror tests/workloads/network/tcp_keepalive.c -o build/keepalive-linux
build/keepalive-linux
sh tests/host/tcp-keepalive.sh
riscv64-buildroot-linux-musl-gcc -static -O2 tests/workloads/network/tcp_keepalive.c -o build/keepalive-rv
loongarch64-linux-musl-gcc -static -O2 -Wl,-z,max-page-size=16384 tests/workloads/network/tcp_keepalive.c -o build/keepalive-la
make -j4 SMP=1 SMP_CPUS=2 USER_SMP=1 COST_DIAGNOSTICS=0 all
make -j4 SMP=1 LA_SMP=1 LA_SMP_CPUS=2 USER_SMP=1 COST_DIAGNOSTICS=0 kernel-la
python3 tests/userland/tcp-keepalive.py --kernel build/riscv-user-smp/kernel-rv --program build/keepalive-rv --cpus 2
python3 tests/userland/tcp-keepalive.py --arch loongarch --kernel build/loongarch-smp2/kernel-la --program build/keepalive-la --cpus 2
```

本次 Linux 同源测试、RV/LA 双核 USER_SMP guest 均 PASS；host Linux 沙箱不允许创建
AF_UNIX socket，该错误路径在 guest 验证。LA 使用 stock QEMU 10.0.13 的本地临时
wrapper，仅过滤其不支持的 `-global ls7a_rtc.toy-enabled=on`；不改仓库 harness，
不据此声称验证 RTC。host 定时器测试不测真实外网丢包、性能或精确墙钟时刻。

### TCP_INFO：真实状态与消费者范围

返回 Linux ABI 前 232 字节，直到偏移 228 的 `tcpi_snd_wnd`，不是只返回经典
104 字节。`optlen` 返回 `min(用户容量,232)`，剩余用户缓冲区不写；负长度、错误
指针、optlen 先写以及只读选项的 set 错误，使用同源 Linux/BoarOS 用例核对。
232 字节之后的新 Linux 字段尚未提供，依赖它们的消费者必须检查返回长度。
依据为 [Linux v6.6 tcp_info](https://github.com/torvalds/linux/blob/v6.6/include/uapi/linux/tcp.h)
及 [tcp_get_info](https://github.com/torvalds/linux/blob/v6.6/net/ipv4/tcp.c)。

- 状态、拥塞恢复状态、MSS、窗口缩放、RTO、persist/keepalive 探测、窗口与发送阈值
  来自真实 PCB；listen 仅访问小 PCB 公共字段，unacked/sacked 按 Linux 别名报告
  可 accept 的子连接数和 backlog。cwnd/ssthresh 从字节除以 MSS 转为整段，不能表达
  小于一个 MSS 的余量。接收窗口阈值/空间报告固定接收容量，不伪装成 Linux 自动调优。
- total_retrans、bytes_retrans 只在成功交付 IP 输出路径后计数，失败入队、重传准备、
  新发 SYN 都不冒充重传。segment 的既有 flags 和一个 payload-prefix 长度记录真实
  发送历史，分段传递、尾部追加新数据及一字节 persist probe 都保留正确字节边界。
  这是协议输出记账，不保证物理网卡或远端实际收到了包；下层拒绝不计入输出统计，
  与 Linux 在 queue_xmit 前记部分计数的错误路径不完全相同。
- bytes_acked/received 跟随确认/接收序列推进，包含 FIN；active SYN 在 bytes_acked
  中计 1，passive SYN-ACK 不计。segs/data_segs、已确认段数和接收乱序包计数来自事件。
  unacked/lost/retrans/notsent 在查询时遍历现有队列；RTO 仅在已有恢复事件中批量标记
  待重传段，不增加正常逐包队列遍历。LOST 是尚未确认序列的丢失标记，成功重发
  后仍保留；重传在途同时计 lost/retrans，再次丢失后仅计 lost，ACK 归还段后两者清零。
- RTT、RTT variance、min RTT、最近数据/ACK 时间复用 lwIP 的 500ms TCP 时钟。
  短连接可能得到 RTT=0；这代表现有估计的量化值，不能据此判断真实延迟为零。
  不为 TCP_INFO 新增逐包高精度时钟读取或 RTT 算法；无 RTT 样本时 min_rtt 为
  UINT32_MAX。跨 owner 迁移同步重基时间字段；PCB 释放前保存快照，释放后查询
  不读悬空指针，时间年龄通过查询时单调时钟继续增长。
- 0 的明确含义：没有启用的 SACK scoreboard/FACK、ECN、DSACK、发送端乱序检测，
  对应字段为 0；未实现的接收 RTT、delivery-rate、busy/rwnd/sndbuf chronograph
  采样为 0，**不是测得零速率或零繁忙时间**。app_limited 位也未采样。
  last_ack_sent 与 Linux 一样不记录、为 0；pacing/max_pacing 为 UINT64_MAX，表示
  未限速，而不是零速率。rcv_mss 是按序接纳的新数据段的最大观察值，未见数据时为 0，
  不是 Linux 的启发式初始 MSS。PMTU 是最近成功数据/SYN 输出所选接口 MTU；
  本栈不因此新增路径 MTU 探测。无记录时为 0，不伪造默认路径测量。

最低成本的不可省记账是事件整数加法/已有时钟值存储，持有现有协议 owner，
无新增原子计数、锁、worker、逐包分配或周期任务。PCB 保存累计值，socket 保存
末次 232-byte 快照以覆盖错误回调先于/晚于 PCB 释放的不同路径。这里不声称通过
性能基准；内存和事件成本是支持可靠接口所需的显式取舍。

验证入口：

```sh
cc -O2 -Wall -Wextra -Werror tests/workloads/network/tcp_info.c -o build/tcp-info-linux
build/tcp-info-linux
sh tests/host/tcp-info.sh
# 和 keepalive 相同的 static 交叉编译与双核 USER_SMP kernel 构建参数
python3 tests/userland/tcp-info.py --kernel build/riscv-user-smp/kernel-rv --program build/tcp-info-rv --cpus 2
python3 tests/userland/tcp-info.py --arch loongarch --kernel build/loongarch-smp2/kernel-la --program build/tcp-info-la --cpus 2
# 将 tests/workloads/network/tcp_info_iperf.c 编为对应架构的 static driver
python3 tests/userland/tcp-info-iperf.py --kernel build/riscv-user-smp/kernel-rv --program build/iperf-init-rv --iperf "$IPERF_RV" --cpus 2
```

host 用实际 lwIP 输出回调验证失败/成功、两次 persist probe、重传、分段、追加新数据、
ACK/字节、owner 迁移、退役后查询及资源回收（4K/16K 页）。同源 libc 用例检查 SYN/FIN
字节规则、232-byte 的 snd_wnd、状态、backlog、长度和 fault ordering。
实际 RV iperf3 3.13（SHA256 `8a87cda6b79966699bc68c72894288ba9aa4a5d44dc0036cfb257a4a4d73860b`）
做双端 guest loopback 32768-byte 功能传输；解析两端 JSON、核对字节和真实读取的
retransmits/cwnd/snd_wnd，并要求无 TCP_INFO 错误及干净关机。不把吞吐、零 RTT 或
iperf CPU 百分比作为性能结论。没有 LA iperf 二进制，本次 LA 只覆盖同源 syscall
用例；stock QEMU 的 RTC 参数过滤限制同 keepalive。

补充定向回归：`ASAN_OPTIONS=detect_leaks=0 python3 -B tests/host/network_owner.py
--case tcp-detached --case tcp-batch --case tcp-more --case tcp-cork --case timewait-timer
--case tcp-batch-error --sanitize`。当前沙箱不支持 LeakSanitizer 的线程枚举，明确关闭
LSan，仍运行 ASan/UBSan；分配归还由上述 host 计数断言独立验证。64-bit host 配置
测得 tcp_pcb 从 320 增至 408 字节，tcp_seg 仍为 32 字节（新 prefix 长度落在原 padding）。
这些是结构大小检查，不是性能基准。


### IPv6 ASM UDP / MLDv1

提供 `IPV6_JOIN_GROUP`/`IPV6_LEAVE_GROUP`（20 字节 `ipv6_mreq`）、
`IPV6_MULTICAST_IF`/`HOPS`/`LOOP`/`ALL`。普通 UDP 入组不要求管理员权限。
显式索引检查完整 32 位值，不截断成 lwIP 的 8 位索引；索引零入组只选择一个接口，
离组零索引删除该 socket 第一个匹配成员，不重新选路。同 socket 重复入组返回
EADDRINUSE，无成员离组返回 EADDRNOTAVAIL；最后一个 open-file 引用关闭时自动离组。
默认 MULTICAST_ALL=1，匹配地址、端口、设备和 connected peer 的 socket 可接收主机已加入的组；
ALL=0 还要求该 socket 自己的成员记录。它不使网卡接受主机未加入的任意组。

组引用按接口聚合，应用成员和 solicited-node/DAD 成员各有独立引用，应用离组不撤销 DAD。
最多 64 个聚合组，新应用组在已有 48 个聚合组时返回 ENOBUFS，为 ND 留出容量；
每 socket 最多 32 个成员。MAC filter 回调失败会回滚入组，普通离组失败保留记录；
最终关闭和接口销毁必须释放软件资源。生产 virtio 尚未协商 CTRL_RX/CTRL_VQ，
没有声称实现硬件组表或 ALLMULTI 控制，接收后由 IPv6 成员表进行软件准入。
外部验收依赖已测 QEMU virtio-net 默认接收模式，不保证任意 virtio 或物理网卡默认接收全部组播。
接口 DOWN/link-down 暂停 MLD 控制包，UP 后恢复报告。MTU<1280 撤销 IPv6 聚合组，
保留 socket 成员记录但将其聚合引用标为已退役；恢复 MTU 不自动恢复旧应用组，
须先 LEAVE 再 JOIN（旧记录直接 JOIN 仍 EADDRINUSE）。旧成员离组/关闭不消费恢复后新成员的引用。
接口删除清理成员记录，
不把旧成员继承给随后复用的索引。loopback 成员只用于本地交付，不发送 MLD 控制包。

入站 UDP 组播只保留原 ingress pbuf，各目标队列持有独立引用、offset 和 BPF 截断长度，
逐目标执行 BPF 与匹配，先发布的读者不会看到后续目标修改共享数据。正常内核 socket
fanout 不复制 payload；原始 lwIP 回调的兼容分支保留独立副本。IPv4 原有 fanout 不变。
PEEK、短读、TRUNC、关闭和错误释放沿各目标队列处理。MLD 仅在有待报告成员时安排
一个 100ms owner timeout，静止状态不增加永久 10Hz 扫描，也不增加 worker。

MLDv1 接收验证 Hop Limit=1、Router Alert=0、link-local 源、组/目的地址关系和完整 checksum；
含 Fragment Header 的 MLD 不接收。按 RFC2710 忽略 Code/Reserved 和合法尾部扩展，
不把 MLDv2 宣称为已支持。Query 的零响应上限立即报告，合法 Report 抑制同组待发报告。
Done 发往 ff02::2，payload 保留原组且 checksum 使用实际目的地址。
没有可用 link-local 源时只允许严格识别的 MLD bootstrap 以 :: 发出；接收 :: 源 MLD 丢弃。
发送 HOPS=0 或 interface-local 组不进入物理出口，LOOP 控制本地副本。

验证入口：`tests/host/network_owner.py --case ipv6-mld`（可加 `--page-shift 14 --sanitize`）、
`tests/host/mld-protocol.py`、`tests/host/ipv6-multicast-sockopt.py`。
`tests/userland/ipv6-multicast.py --arch riscv|loongarch --cpus N --kernel ... --program ...`
与 `tests/workloads/network/ipv6_multicast.c` 配套，外部 peer 经 QEMU Ethernet 注入组播及 Query，
逐字节核对 257 字节内容和抓包中的 Report/Done、Router Alert、目的 MAC、Hop Limit、checksum；
阶段 barrier 校验多 socket 引用及最后离组。runner 保存 kernel/program SHA256、guest log 和 pcap。
这是 slirp/filter 注入验证，不是 TAP/物理交换机验证；Linux 行为依据源码及可用 syscall probe，
未做需要 NET_ADMIN 的 Linux network namespace 差分。仍不支持 MLDv2/SSM、IPv6 分片重组或组播路由。

协议依据：[RFC2710](https://www.rfc-editor.org/rfc/rfc2710)、
[RFC3590](https://www.rfc-editor.org/rfc/rfc3590)；ABI/成员语义对照
[Linux v6.12 mcast.c](https://github.com/torvalds/linux/blob/v6.12/net/ipv6/mcast.c) 和
[ipv6_sockglue.c](https://github.com/torvalds/linux/blob/v6.12/net/ipv6/ipv6_sockglue.c)。
