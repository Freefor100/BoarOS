# 网络端点与协议执行

## 入口与对象

`kernel/syscall/socket.c` 导入 RV64/LA64 Linux socket 参数和用户指针，`fs/files/socket.c` 把 socket 作为普通 fd/OFD 安装并在失败时回滚，`net/socket.c` 持有 endpoint、数据包、待 accept 队列及等待队列。`fs/open_file.c` 在最后一个真实 OFD 引用消失时销毁 socket；dup、fork 和 syscall 期间的 pin 共享同一 endpoint，close/exec/退出均沿既有 fd 生命周期回收。`fs/files/io.c` 把普通 read/write/readv/writev 接到 socket 队列，pread/pwrite/lseek 返回 `ESPIPE`，`fstat` 标识 `S_IFSOCK`，poll/select/epoll 读取 socket 就绪和等待队列。

协议实现来自官方 lwIP `STABLE-2_2_1_RELEASE` raw API，位于 `third_party/lwip/`；固定资料和许可证见 `references/README.md`、`references/sources.tsv` 与 `third_party/lwip/COPYING`。IPv4 重组有本地所有权和边界修补，具体范围见[第三方组件](../third-party.md)。本地移植层是 `net/lwip_port/`，使用 `NO_SYS=1`；loopback 与物理接口共用协议池。生产预算为 8 MSS/池 4 倍/堆 2 倍：UDP PCB 16、TCP active 32、listen 16、segment 512、pbuf header/pool 各 256、协议堆 512 KiB。BoarOS socket、接收/accept 队列节点及 OFD 使用 kernel_heap。PCB 池耗尽映射 `ENOMEM`，创建失败不留下 OFD；分配器 `STATE` 和错误释放是 fatal 不变量。

IPv4 重组键包括源/目的地址、IP ID、协议号和输入 netif 身份。已完整覆盖的重复范围只丢弃新输入；部分重叠、冲突终点或越过已知终点丢弃整个对象。判重与边界校验先于资源淘汰，重复输入不能挤掉正在重组的有效数据。对象借用 netif 身份，不延长设备生命；移除接口前调用 `ip4_reass_cleanup_netif()`，释放该接口的所有重组引用。最后一个 pbuf 引用释放后，custom 回调才归还 DMA 缓冲。IPv4 头选项仍不支持。宿主反例和所有权检查由 `make test-lwip-reassembly-host` 重建，包含普通/乱序、重复/部分重叠、协议/接口隔离、终点冲突、预算、年龄和实际 fragmenter 的最大 UDP 内容；宿主结果不代替真实网卡验收。

改变 MF 的重复范围也不直接触发交付：即使已有数据覆盖整个声明长度，新输入仍被
判为重复，原引用保持到协议期限；超时后同一键可以重新使用。这与固定 Linux
`net/ipv4/ip_fragment.c` 先记录终点、再在 DUP 分支释放新片的处理顺序一致，
不把未接纳的终片当作完整内容已交付。`duplicate_final_timeout` 覆盖持有、超时
归还和槽复用；资源始终受重组预算约束。

## 已验收 ABI 与等待

当前支持 `AF_INET`/`AF_INET6` 的 `SOCK_DGRAM`/`SOCK_STREAM`，`SOCK_CLOEXEC`、`SOCK_NONBLOCK`，UDP bind/getsockname/sendto/recvfrom 和 `SO_RCVTIMEO`，TCP bind/listen/connect/accept，以及连接后的普通读写和就绪。绑定非零低端口（小于 1024）要求当前调用者的 CAP_NET_BIND_SERVICE，端口 0 仍由协议选择临时端口。`SIOCSIFFLAGS` 要求 CAP_NET_ADMIN，查询接口保持普通读取规则。`ioctl(SIOCGIFFLAGS/SIOCSIFFLAGS)` 让真实用户程序启用 `lo`；接口对象从公开 `netif_list` 查找。地址在 syscall 边界使用 RV64 `sockaddr_in`/`sockaddr_in6` 布局；内核地址携带族、16 字节网络序地址、宿主序端口和 scope。官方 loopif 提供 `::1`；IPv6 通配监听默认接收 IPv4，accept 返回映射地址。V6ONLY 在绑定前生效，双栈通配与纯 IPv6/IPv4 的端口交集由非持引用的 endpoint 登记补齐 lwIP TCP bind 的 ANY 检查缺口。登记在 OFD 销毁前摘除。非阻塞、坏 fd/地址/指针和协议错误由固定 Linux 同一 ELF 差分约束。未覆盖的地址族、选项和操作返回明确 errno，不伪造成功。

输入地址按 Linux 的有符号 32 位长度校验，bind/connect/sendto 拒绝负值及超过 128 字节的声明，并完整复制声明范围后解析。sendmsg 对空 name、负长度及超过 128 字节的输入使用其独立的规范化规则；先完成 name 复制和 iovec 导入，再解释地址，错误路径保留协议状态和报文队列。

IPv4 的 connect、UDP connect/sendto 将目的 `0.0.0.0` 按 Linux 本地目的语义转换为 `127.0.0.1`，bind 的 `0.0.0.0` 继续表示通配监听。socket 的 family/type/protocol 分别检查；RAW protocol 0 返回 `EPROTONOSUPPORT`。

端点队列、读预约、超时、错误消费、发送额度、socketpair 两端关系和工作链由共同端点短锁保护。同一状态修改中的队列发布、额度查询及更新复用外层保护；嵌套 guard 只记录借用，通知游标仅由实际持锁者初始化和维护。RV 用户的 read/write/readv/writev、sendto/recvfrom/sendmsg/recvmsg 和普通文件到 socket 的 sendfile 在发起 CPU 执行：用户复制、接收预约、发送额度和等待属于原调用者。创建、连接、监听、accept、配置、shutdown、最终销毁以及 lwIP 协议工作继续在 CPU0。就绪查询及等待登记不取得 lwIP mutex，就绪检查和等待登记在同一状态保护内完成，释放状态锁后才 park。通知使用已有等待游标借用，在端点锁外执行回调；最后销毁先关闭等待队列，并等游标与登记实际归还。`kernel_socket_poll` 不访问 PCB、不推进协议、不消费错误；TCP 可写性来自协议 owner 发布的接纳额度，实际发送预约共同扣减它。监听就绪由接纳/失效回调维护，失效 child 的销毁由现有工作集合处理。epoll 真实候选查询在 token 外，prepared 阶段只读刷新提示。

短 syscall 在完整 raw 调用退出后可执行有界服务；网络 worker 与无 NIC worker 在自己的外层 scope 中调用 `kernel_socket_service_owned`，沿同一资格登记等待；独立调用者使用 `kernel_socket_service_pending` 取得资格。每批分别限制 RX 八帧、loopback 八包、socket 八个工作单元及一个 timer 回调。socket 单元包含容量转交、到期重试、实际对象工作，三类轮转；协议池、NIC、接收堆等待集合也轮转。零预算不消费对应工作，返回值报告处理量、是否可立即继续及下一期限。unsent 等待远端窗口或 ACK 不会自行重新入队。服务先按包预算消费旧输入，再处理 socket 工作；后者产生的新回环包使用本批尚余的包预算交付，预算总量保持原值。预算耗尽且仍有工作时开放中断并让出，睡前再次查软件工作与设备完成。

`kernel_socket_protocol_enter/leave` 用栈上 scope 保存 CPU0 服务资格、IRQ 状态与 mutex guard。最外层先在 IRQ 保护下核对当前 CPU0 并发布已有的调度资格，再取得任务 mutex、开放 IRQ；持有或等待 mutex 期间允许抢占，恢复仍限于 CPU0。同一 owner 的同步回调只增加嵌套深度，借用外层资格。请求的本地进入失败时沿原工作队列执行，不依赖此前观察到的 CPU；普通强制协议入口仍可在没有业务锁和用户 pin 时沿既有服务入口迁移。CPU0 本地取得资格不迁移、不调度等待，因此已有 TCP pin 批次继续 inline，归还协议 owner 和 mutex 后才结束本 scope 实际取得的服务资格；try-lock 失败也归还它，借来的资格仍由原 owner 结束。UP 编译不增加服务字段和资格操作。lwIP 的 PCB、协议池、接口、重试链与 loopback 由该 owner 串行；用户复制、端点查询和设备等待不占用它。协议池归还代次和 pool 等待链归 CPU0 协议 owner 维护，每次真实资源归还推进代次；存在等待者时才取得端点保护并通知 worker，普通 pbuf 归还直接处理 owner 私有状态。持有期间的 allocation_depth 禁止堆回收进入 I/O。NIC IRQ 只收割并通知，timer IRQ 只请求已有 worker 工作。

读预约持有 OFD 和固定队列前缀，用户复制在保护外完成。finish 在 IRQ 开启时找到消费边界，短临界区一次摘下前缀，并把原 packet 与接收窗口额度转交已有 worker。worker 在协议保护内归还 pbuf 和 `tcp_recved`；最后关闭也排空这些实际引用。RX loan 因此一直保留到 pbuf 最后归还，不能在用户复制完成前复用 DMA 缓冲。AF_UNIX 载荷在端点临界区外释放；关闭清理结束后才把发送者的末次释放权交给剩余 packet。

TCP 发送先在端点保护内预约额度，连续输入复用任务 scratch 保存最多四页的 pin 与片段描述。若一页输入含超过四段不连续地址，复用该页合并载荷，减少远端协议请求；部分接纳时保留尚未提交的暂存前缀。首个页可缺页；后续只接纳已驻留页，遇到缺页、撤权或关闭许可就提交已固定前缀，归还全部 pin 后继续。一次协议资格处理整个批次。远端调用或 CPU0 协议忙时，同一个栈请求进入 socket 的现有工作集合，由已有 worker 执行 COPY；调用者等到实际接纳或错误完成后才归还 pin。没有每页一次的 FIFO mutex 交接，也没有新增执行线程。发送预约在 COPY 完成前保留，其他发送者不能重复占用同一额度；worker 将异步错误和 SIGPIPE 交付给原调用者。同一请求队列还承载 UDP 的真实 pbuf 准备、发送、归还及 TCP 延迟输出；载荷由调用者直接填入最终对象。实际写请求在端点活动链中保持到全部协议阶段完成，停止时先拒绝新接纳，向包括空队列读者在内的 INET 等待者发布 EIO 并通知，借用实际 OFD 等待活动读写归还。worker 通知在端点锁内取得真实 progress 队列游标，锁外只投递该游标；撤下通知入口后排空已捕获的队列借用，再停止原 worker，通知不再保存网络对象的裸 context。设备在途 DMA 仍由原 quiesce／loan 协议确认。

socket 在最终 OFD 释放时取得协议资格，先摘除弱引用工作链并解除 PCB 回调。跨用户复制和待执行请求持有实际 OFD/pin，因此 worker 借用的对象不会被提前销毁。队列只在从无工作变为有工作时通知 worker；容量、接收归还、loopback 与期限使用各自的原有通知来源。

TCP 的一批 COPY 保持完整发送预约，协议 owner 在批末一次发布实际 PCB 额度并归还预约；批内已复制的字节仍包含在预约中，远端接纳无法重复使用。队列满、ERR_MEM、成功前缀及末批输出分别按实际结果处理；需要输出时，先在锁外执行 tcp_output，再发布容量并归还本批预约。逐页片段不再重复发布相同容量。UNIX 成功发送后的请求摘除不改变就绪状态，通知由消息发布、额度归还、取消及对端变化产生；INET 停止仍等待活动请求摘除并接收对应通知。recvfrom/recvmsg 仅在用户请求源地址时取得临时名称引用，消息长度和截断消费独立处理。

`netif_loop_output()` 在成功发布完整回环包队列后调用可选的
`LWIP_HOOK_NETIF_LOOPBACK_QUEUED`，经端口进入 `protocol_work_ready()`，设置软件
待处理状态；从无包变为有包时请求 worker 唤醒，已有待处理包时合并通知。每次轮询结束按真实剩余队列更新状态，包含同批产生又被消费的 ACK。该回调不消费包或重入 raw API。分配/复制失败不
发布通知。socket/timer 阶段产生的新包若未被本批余量消费，服务返回值保持 runnable，由 worker 继续推进。
`make test-lwip-host` 已纳入真实 lwIP/端口/socket 的 `loopback-work` 反例：固定
协议时钟，覆盖 IPv4/IPv6、晚入队、OOM 与重试、内容交付和内存基线。

非阻塞 connect 短路径推进握手，后台保证调用者不再调用 socket 时仍能进展；阻塞 connect 等待握手结果。普通 socket read/readv、write/writev 及 sendfile 在调用入口把 OFD 的 O_NONBLOCK 折入本次 MSG_DONTWAIT；后续 F_SETFL 不改变已开始调用的等待资格。非阻塞 EAGAIN 直接返回，不查询或建立接收/发送超时。阻塞接收在首次需要等待时读取一次 SO_RCVTIMEO 并保存绝对期限；即使就绪后数据被另一 reader 取走，后续重试也不重新取得预算。发送沿用调用级绝对期限，等待函数不再把剩余时长重新换算为新期限。每次先尝试实际读写，再对 EAGAIN 检查到期，保留已有数据与错误的优先次序。信号沿既有 syscall restart 协议，带接收超时的中断返回 `EINTR`。未 listen 的 stream 和 datagram accept 立即返回类型对应错误，监听 socket 的 `SO_RCVTIMEO` 约束阻塞 accept。

普通 socket read/readv 使用“预留当前队列前缀→锁外复制→一次提交/取消”。STREAM 预留把当前可读 packet 的总字节数截到用户容量，队首借用与固定字节数限定本次稳定版本；成功提交后本次读调用返回该前缀，后来追加的数据与排在前缀之后的协议错误留给下次调用。请求直接遍历 packet/pbuf 链写入用户 iovec，复用现有用户页写游标，换页及退出前归还 pin，不增加接收中转页。预留与提交只修改端点状态，不取得协议资格；取消仍由请求归还 OFD pin。跨包复制故障只提交此前完整复制成功的 packet 前缀并返回短读，故障 packet 保留；UDP 仍只预留一个数据报、按用户容量截断，复制故障仍丢弃该数据报。零长度数据报也完成 reservation，TCP EOF 没有 reservation。

TCP标量和向量发送均在容量接纳后准备有界页引用与片段，输入组织及部分接纳规则统一见[文件I/O](kernel-files.md#请求暂存与成本)。协议继续持有ACK前的COPY版本，ERR_MEM后先归还pin再等待。描述符scratch页由任务持有并在调用间复用，分配失败返回 ENOMEM；正常返回解除请求登记，页保留至任务最终存储释放，强制退出先取消实际请求／释放其 OFD pin，再解除登记，最后释放任务文件表和栈。没有用户任务的模块测试按调用分配并沿正常返回路径释放。请求末批、容量阻塞点和退出时集中推动 tcp_output，每次接纳批次只在一个边界执行有界协议服务。已接纳前缀后的异步错误留给下一次调用观察。`kernel_socket_get_statistics` 提供单 hart 累计 tcp_write 调用及成功复制字节数；用户页解析尝试由诊断构建的 `COST_USER_RESOLUTIONS` 记录，普通构建不为此维护全局原子计数。直接 pin 的 TCP 数据不经过 uaccess 复制函数，搬运字节由 `copy_from_user` 和 `stream_copy` 记录；`uaccess_copy_ticks/bytes` 仍只覆盖显式复制路径。计数没有新增用户 ABI。

INET UDP 发送请求直接持有最终 PBUF_RAM，用户 iovec 填入该载荷后交给协议发送，不再分配临时 socket_packet 和第二份数据区。用户复制在协议临界区外执行；复制失败、正常完成或强制退出都由同一请求释放尚未转交的 pbuf。

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

连接UNIX发送在同一次端点保护内检查状态、登记请求、借用对端并预留额度；
需要等待时沿原等待协议释放实际外层保护并重试。复制完成后同次发布消息并撤销
请求登记，载荷复制在锁外进行。DGRAM在分配和usercopy前完成内存预留，复制完成后才移动整包owner；
fault/OOM/取消先释放取得的内存，再归还预留与OFD pin，不发布前缀。截断读取或
接收fault丢弃整条datagram并归还其全部计费；DGRAM出队时在同一保护内归还
sender额度，实际名称、端点和packet存储在锁外释放，释放后仍推进
共享heap的INET接收容量等待。sender关闭后仍可读其queued内容。
等待者被发送缓存增大、packet最后释放或peer状态变化唤醒，重新核对状态。
STREAM的新write在peer关闭后返回EPIPE/SIGPIPE；已有写等待在peer带未读内容
关闭时可交付一次ECONNRESET。DGRAM关闭peer后的首次send为ECONNREFUSED，
后续未连接send为ENOTCONN，DGRAM的EPIPE不发送SIGPIPE。关闭状态、异步错误
与poll/recv均按各类型处理，不把datagram peer关闭变成stream EOF。

固定依据为`references/linux` commit`f4cdf7ca9a1fdcca413157df19753f388a5a224e`的
`net/unix/af_unix.c`、`net/core/sock.c`、`include/net/sock.h`。每消息控制数据见下节。命名连接和 socketpair 共用上述跨核状态与队列保护。socketpair先预留两个FD并逐项输出编号，再执行共同的family/type/protocol创建校验及配对支持判断；创建失败保留已输出的编号前缀并归还预留，实际FD仅在成功后发布。

SEQPACKET按连接管理与整条记录收发：监听、connect和accept与STREAM共用，实际SO_TYPE及子端点保持类型5；一次发送发布一条完整记录，短接收截断并消费该记录，MSG_TRUNC可返回原长度。MSG_EOR和MSG_MORE发送标志接受后忽略，MSG_WAITALL接收仍停在一条记录，返回标志不添加MSG_EOR。SEQPACKET未连接或监听时接收返回ENOTCONN，EPIPE不发送SIGPIPE。

空记录与EOF独立：空记录可交付SCM_RIGHTS和凭据；SEQPACKET无记录EOF返回零且控制区长度为零。STREAM保留自身空cookie规则。头、iovec及控制区字节先导入，随后按协议检查待决错误和连接，再解析SCM；不支持的SEQ标志在协议规定位置拒绝，输入故障与错误消费顺序和固定Linux一致。

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

TCP 请求由 syscall 栈持有，OFD 与最多四页 pin 覆盖 CPU0 的实际 COPY 接纳。排队请求使用自己的等待 token，端点锁内登记、发布 result/queued 并定向通知；锁外 park/finish。ACK 和发送额度变化只通知 socket 就绪等待者，避免尚未完成 COPY 的请求被反复唤醒。请求入工作集合只登记待服务，STREAM 的 IO_MORE、窗口耗尽及明确 FLUSH 决定何时输出。服务先处理此前捕获的 OUTPUT，再接纳新 COPY，保留新前缀的合批资格；纯 tcp_write 成功不产生生命周期回收工作，真实输入和关闭仍负责安排回收。UDP 分两次交接：CPU0 分配真实 pbuf，调用者直接填入载荷，CPU0 发送并释放；用户复制失败也通过同一个请求归还 pbuf。UNIX 调用者准备最终 packet，发布前在端点锁内重新核对对端。调用者只在完成状态发布之后归还请求、pin 和 OFD。单核及 CPU0 空闲协议资格保留直接调用。

普通 I/O 与 socket syscall 共用 `kernel_socket_wait_ready`。调用者传入一次捕获的绝对超时，端点锁内检查条件并登记，锁外 park/finish；就绪先于超时判断。接收预约限制读取的原有字节前缀，复制到该前缀末端即停止，锁外复制不读取并发追加修改的尾链接。

用户复制可以触发文件页的冷缺页，调用链继续进入 ELF 源读取、页缓存、ext4 和块设备等待；这些调用共同使用当前任务的 8 KiB 内核栈。sendmsg/recvmsg直接进入消息处理入口，不跨载荷缺页保留标量及控制调用的栈帧；发送预检复用已经持有的端点保护。UNIX 载荷复制与复制后的 packet 发布使用独立栈帧，TCP pin 工作区与普通文件暂存路径也分别构帧；MM 的映射文件缺页与 ELF 缺页分开，避免无关工作区同时占栈。检查栈余量需要实际触发冷页并等待任务退出后的高水位报告，单个函数的静态帧大小不足以说明整条调用链安全。

epoll控制仍在CPU0，等待扫描和事件复制在发起CPU执行，详见[文件接口](kernel-files.md#epoll-事件通知子系统)。独立状态锁维护候选、控制代次、ONESHOT 和扫描引用；目标就绪查询、用户复制、上层通知及等待游标排空在锁外执行。端点通知取得现有队列/节点借用后释放端点锁，因此远端回调与 DEL、MOD、关闭可以交错，实际回调归还前存储保持有效。

## 验证

静态/动态 musl 的原生并发短例复用 `tests/network_runner.py --workload smp`。私有 `USER_SMP_COMPOSITION=1` 内核配合 `--observe` 检查每个发起 CPU 的预约、复制、pin 与等待；三项握手分别暂停真实接收预约、发送页 pin 和 epoll 通知借用，核对追加/最后关闭、mprotect 和 DEL 的实际等待及资源归还。正式性能内核关闭这些观察。

`tests/socket-accept-allocation-host.sh` 在真实 OFD 分配处注入 EMPTY，检查连接保留、重试双向通信及 FD、凭据和堆页归还；另覆盖等待失败和地址输出失败的不同副作用。`tests/host/network_owner.py --case unix-space-topology` 检查互连流量不触发空间回调、断连恢复单向订阅及重连唤醒；`unix_named` 用户程序通过实际 epoll 等待与队列空间释放核对跨核通知。

`unix-packet-boundary` 覆盖默认、最小和最大SNDBUF的完整分配大小、向量前缀及计费归还。`unix-write-notify` 直接检查成功发送不产生无状态变化的本端通知，同时检查消息消费和复制取消仍通知发送者。`tcp-batch` 在真实 tcp_write 的 COPY 之间握手，让另一 CPU 预约剩余额度，检查整批接纳期间额度保持保守且最终归还；相同数据分段不会增加端点保护次数。

```sh
make test-lwip-host test-lwip-reassembly-host test-ethernet-worker-host
make test-network-riscv         # IPv6 UDP/TCP、双栈 accept，同一 ELF 对照 Linux
make test-scale-riscv
make test-syscall-riscv test-userland-riscv test-diff-abi-riscv test-stack-usage
python3 tests/program-inventory/run.py --suite libc \
  --case libc.static.socket --case libc.dynamic.socket \
  --require-pass --output build/socket-program-check
```

`tests/network-riscv.py --memory 512M --memory 1G`分别启动同一ELF的两种RAM，
每个配置的镜像/日志独立；result记录实际启动参数。RV official profile固定1GiB，
冲突的显式RAM参数返回错误，不能忽略参数后给启动贴错标签。

host 入口实测 UDP loopback、PCB 16 个用尽后第 17 个失败、全部释放和再分配，另在 `sndbuf>0` 时耗尽全局 TCP segment 池触发真实 `tcp_write ERR_MEM`，以及 TCP 握手/关闭后推进 200 秒协议计时、池用量回到基线。此前的 Linux/BoarOS 差分记录完全一致，包含 12 条新增 socketpair 差分记录，覆盖坏族、坏标志、坏协议、空指针、stream 双向读写、关闭 EOF、dgram 边界截断与 flags 校验。真实 pthread U-mode 另覆盖零长度 datagram、共享 socket 双读、阻塞读时 close/fd 复用、双读线程组 SIGKILL、全局池压力下错误可写事件及释放后写入进展。原版 hackbench 原 ELF 在 4 进程模式下传递消息并成功运行（Time: 0.014s），关机检查 `heap-live=0`。原版 libc-test `functional/socket.c` 的静态、动态直接 entry 用未改源码和同一 ELF 在双方通过；整合内核全量 228 项为 227 pass、1 BusyBox 包装失败，见[程序清单](../learning/user-program-inventory.md)。

Linux ABI 依据本地 `references/linux/net/socket.c`、`net/ipv4/af_inet.c`、`fs/read_write.c`，固定 Linux v7.2；测试输入来自 `references/oscomp-testsuits` 固定 pre-2025 版本，精确身份在来源清单和机器归档。核对后运行 `make prune-build` 清理日志和镜像。

规模回归补充 0/255/256/257/1500/4096/8192 字节 UDP 的三种入口、完整与不足容量、空 iovec、后继报文和跨页 fault；TCP 用错位缓冲与向量写累计传输 1 MiB，检查内容、非 256 字节接收和跨片段 fault 的字节守恒；`socketpair_scale`/`unix_datagram_budget` 及真实 pthread 补充 AF_UNIX 向量/跨页、零长度、按sndbuf的消息上限、整包 OOM、满队列阻塞/非阻塞、fault 无前缀、截断/接收 fault 后预算复用与取消；固定 Linux 差分累计 1086 条一致。`socketpair_scale` 覆盖 STREAM 全双工读写、对端关闭 EOF、写入关闭对端返回 EPIPE 以及 DGRAM 数据报截断边界。成本与边界见[单核规模回归](../learning/single-hart-scale.md)。

本轮选项阶段接入 getpeername(205)、getsockopt(209)，以及 REUSEADDR、TYPE、
ACCEPTCONN、ERROR、KEEPALIVE、收发预算/超时、TCP_NODELAY、TCP_MAXSEG 和
IPV6_V6ONLY。SO_ERROR 读取清除待观察错误，连接阶段 RST 返回 ECONNREFUSED；
已建立 RST 保留 ECONNRESET。INET收发预算默认/上限 64 KiB，设置按 Linux 倍增，
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

TCP refused-data 在接收内存不足时保留 pbuf 并登记重试，由任务上下文的有界服务处理接收堆归还或到期重试。scale 的独立堆包装器检查首次接收 OOM、IRQ 仅通知，以及显式服务后的成功接收。
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
在 oscomp-compat 运行；main 不改 uname。原脚本与每项新服务端的受控流程
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
协议排队次数、park 次数、排队到完成的累计 ticks 与实际协议执行 ticks 分开记录；排队等待进一步分出入队至执行、完成至调用者恢复，两者包含调度和状态锁等待。多任务累计时间与墙钟分别解释，排队总量已包含执行部分。聚合存储沿用内核的编译期预算约束，默认构建没有新增诊断节点。观测影响吞吐，关闭
观测的独立批次才是性能结果。此前纠错轮1179条ABI、完整RV64、真实musl/glibc 2.44、
socket scale和栈检查通过；修复后的用户态契约覆盖reset、accept输出fault、
TCP MSG_TRUNC、UDP自动端口释放，IRQ仅通知由独立heap包装器检查。

结果与限制见[网络记录](../learning/network-ownership.md#原版网络应用交付2026-10-02)。
该段是此前loopback验收；现已接入VirtIO-net；命名 AF_UNIX 的当前机制见上文，TCP_INFO 仍待实现，相关应用字段按实际验证范围解释。

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

发送者预算入口：`python3 -B tests/network-riscv.py --workload budget`与
`tests/network-loongarch.py --workload budget`，现在均对照Linux。测试先填满实际
sender并从proc确认子任务已阻塞，检查接收缓存变更不解锁、发送缓存缩小/增大、
peer关闭的类型对应错误、SIGKILL和内容守恒。相同ELF双侧运行，不按具体packet
个数推断内核布局；旧接收配额成本契约由用户明确选择的Linux发送者模型替换。

标量及多iovec的TCP流发送共用有界页pin批次，正式游标只推进实际COPY接纳量；
EAGAIN前归还pin，等待后重新准备未接纳输入，具体复制规则见[文件I/O](kernel-files.md)。
非阻塞/fault/信号/期限仍返回已接受前缀；用户页不借用到 ACK。
预算fixture的COST窗口独立核对全阻塞和部分发送各1000字节只有1000字节stream usercopy，
旧全阻塞实现为2000字节。`--workload content`校验单TCP16MiB、五TCP各8MiB及UDP内容。
游标不跨系统调用保留内容。原iperf使用非阻塞socket，短写/EAGAIN后再次write仍可能
重新处理未接纳输入；此前三次关闭观测的单TCP仅改善0.4%/2.2%，不宣称解决主要吞吐差距。
结果、前台复制放大与代价见[本轮网络结果](../learning/network-ownership.md#本轮应用结果与剩余复制2026-10-02)。

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

TCP 默认采用 Nagle 的 Minshall 规则，与固定 Linux `tcp_minshall_check/update` 的边界一致：已经发出的完整 MSS 包不阻止第一个小包，尚未确认的小包会阻止后续小包。PCB 尾部记录最后一个新发小包的结束序列；主动／被动建连初始化该字段，实际输出成功才推进，重传和输出失败保留原值。判断使用 TCP 序列号比较，覆盖 32 位回绕；累计 ACK 越过该边界后解除等待。这样保留小包合并，同时减少整 MSS 在途数据与延迟 ACK 组合产生的等待。

开启 `TCP_NODELAY` 时先禁用 Nagle，再立即尝试输出已有 unsent 数据。NIC 暂无容量时登记原有 NIC 容量等待，协议内存不足时使用原有 pool 等待；容量事件推动重试，不依赖收到 ACK 或推进定时器。关闭 NODELAY 只恢复 Nagle。用户数据报发送使用任务登记的请求和 iovec cursor，退出清理沿同一请求归还 OFD 和包引用。

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
两种输入都由 `tcp_write(COPY)` 持有 ACK 前的版本，非阻塞返回已接受前缀，
未接受用户内容不跨系统调用持有。
`FILE_IO_HOST_TCP=1 sh tests/file-io-concurrency-host.sh` 将生产 TCP/lwIP、MM 和 syscall I/O
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

scale 保护竞争 reservation、abort 归还及零容量下零页解析/零 stream usercopy；
真实调度的 io-sleep 包装用户复制边界，覆盖保留 reservation 时关闭并复用 fd、并发 shutdown、
用户页撤销后的 fault 和预算重取。它模型化可睡眠复制的交错，不宣称复现硬件缺页的具体时序。

`python3 -B tests/network-owner-riscv.py --kernel kernel-rv` 在 legacy/modern
两种 VirtIO 传输下关闭 indirect 特性，以真实 TAP 的双向 bulk/control 内容检查
验证复制路径，并要求 driver 的 `tx-copy>0`、`tx-sg=0`、`errors=0`。该入口只运行
功能 smoke；它与冻结资源边界的 host 用例共同验证，不单靠最终传输成功证明
NIC 容量通知没有遗漏。

TCP 的 27 组窗口/池/堆候选由 `BOAROS_LWIP_WINDOW_MSS`、`BOAROS_LWIP_POOL_SCALE`、
`BOAROS_LWIP_MEM_SCALE` 选择，当前默认 8/4/2，PCB 数不变。工具、静态/动态内存口径、
控制流 tail 与固定输入约束见[预算实验](../learning/data-path-budget-experiments.md)，
可运行候选不等于吞吐已经验收，默认值须依据匹配结果由人选择。

`python3 -B tests/network-budget-config.py` 独立验证无覆盖宏的生产默认 8/4/2，
并显式覆盖全部 27 个候选，保留非法配置与统计计数宽度检查。
`tests/network-budget-selftest.py` 用临时头文件默认值漂移验证门禁会失败。

阶段七已完成 TCP 27 组预算的发布筛选、30 项组合扩展与独立 COST 诊断；当时默认为 8/1/1。用户依据近池与 1/10 ms 结果批准改为 8/4/2，保留 8 MSS 和 32 PCB。历史默认的吞吐回退、全局池/堆饱和和 8/4/2 的有限负载收益保留在[实验报告](../learning/data-path-budget-experiments.md#正式匹配结果2026-10-06)，不重跑完整矩阵，也不将预算调整宣称为单连接吞吐保证。

共用Ethernet的入口为`kernel_network_start(owner, heap, device, frequency)`。device
由平台/root持有，网络层仅借用；NULL创建独立timer-only worker。正常停止先
关闭工作接纳，把尚未执行的发送请求完成为 EIO，唤醒原调用者归还 pins，再 join。已经执行的 COPY 在同一协议资格内先完成；确认DMA停止后清理netif并归还TX/RX引用，再释放网络控制对象；
平台随后销毁device/transport。LA真实PCI与无NIC双侧结果见[net模块](riscv-virtio-net.md)。

2026-10-07新增`--workload unix_sender`保护>64KiB单消息、sndbuf-32边界、有限零
消息、坏用户页、sender关闭后读取、关闭peer的两次错误及各类型信号规则。
LA的budget/sendfile/unix_sender均在Linux/BoarOS两种RAM检查实际退出和根基线；
内部owner另由`tests/host/network_owner.py --case unix-sender --sanitize`验证两种
关闭顺序，真实RV scale覆盖预留后的两处OOM、截断、接收fault及对端回收，pthread
覆盖真实等待/取消。完整双架构/程序回归继续按整个LA能力计划收口。

`tests/workloads/network/unix_seqpacket.c`复用共同控制消息检查，另覆盖命名连接、记录边界、空记录/EOF、实际用户入口的来源名称、错误顺序、shutdown及等待。宿主network_owner中的SEQ记录、预约、SCM、pending child和shutdown专项检查真实引用与额度归还。
