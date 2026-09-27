# 网络对象与 loopback 事件推进

固定 libc-test `references/oscomp-testsuits` commit `8b58dd16d26d30f7c74d48d5832d870d3051b703` 的 `libc-test/src/functional/socket.c` 真实调用 IPv4 UDP 的 bind/getsockname、微秒级 `SO_RCVTIMEO`、sendto/recvfrom，随后建立 TCP listener、非阻塞 connect 和 accept；它不消费 AF_UNIX，也不能单凭测试名推断所需协议。固定 Linux commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e` 的 `net/socket.c`、`net/ipv4/af_inet.c` 和 `fs/file.c` 提供 syscall、协议错误与 fd 生命周期比较基线。

取舍比较过三条路线：自写有限 TCP/UDP 子集能控制所有状态，但协议重传、定时器和互操作验证成本最高；宿主转发可快速启动，但 fd、错误和恢复语义被宿主环境决定；固定成熟 C 栈的 raw API 保留 BoarOS 的 fd/OFD、用户复制、等待、就绪和 errno owner，同时复用已测协议状态机。用户选择第三条，固定官方 lwIP `STABLE-2_2_1_RELEASE` peeled commit `77dcd25a72509eb83f72b033d219b1d40cd8eb95`，采用 `NO_SYS` 单 hart 事件驱动。分配 owner 另经用户确认：协议/packet 用静态有界池，BoarOS socket/OFD/队列节点用 kernel_heap，以池计数与 root `heap-live=0` 分别核对。

`NO_SYS` 没有独立网络线程。若服务端已睡在 accept，客户端非阻塞 connect 返回后不再调用 socket，SYN 若只留在 lwIP loopback 队列，服务端会永久睡眠。因此发起 connect 的系统调用必须推进一次 loopback，轮询与定时等待也推进协议。两进程握手测试固定顺序，排除了同进程立即 accept 偶然泵送队列的伪通过。另一条生命周期边界是 TCP `tcp_close` 在已连接状态可能暂保留 FIN/TIME_WAIT PCB；销毁 OFD 前必须先解绑指向 BoarOS 堆对象的回调，host 测试随后推进 200 秒计时并检查静态池回到基线。待 accept 子连接的 reset 也必须在 dequeue 前剔除。

固定参考是本地 `references/lwip/` 和导入的 `third_party/lwip/` 同一 commit；移植只在 `net/lwip_port/`，不修改上游 core。重建入口与当前能力边界见[网络模块](../modules/kernel-network.md)。外部网卡、AF_UNIX、半关闭和 SMP 的 owner/同步仍需要单独验证。

后续固定 Linux read/readv 差分暴露两个消费边界：TCP 的用户复制跨页 fault 返回 `EFAULT`，下一次读取仍得到完整那段数据；UDP 的 recvfrom 复制 fault 则丢弃整个 datagram。先从 lwIP 队列摘数据再做可 fault 的用户复制会丢 TCP 字节；只 peek 后不保留身份又允许共享 OFD 的第二线程在复制时改动队首。解决办法是把队首 reservation 登记在任务和 socket，并暂移 OFD pin。提交或取消时验证同一 packet；强制退出在文件表清理前取消，避免被抛弃的内核调用栈留下悬空 reservation 或永久 pin。固定 Linux 依据为 `references/linux/net/ipv4/tcp.c`、`net/ipv4/udp.c`、`net/socket.c`，commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e`。

另一个边界是 lwIP `tcp_write` 在 `sndbuf>0` 时仍可能因 `snd_queuelen` 或全局 `MEMP_TCP_SEG`/pbuf 用尽而返回 `ERR_MEM`。只用 `sndbuf` 宣告 `POLLOUT` 会让阻塞 write 在同一 hart 上反复得到 `EAGAIN` 并立即再醒。固定 lwIP `third_party/lwip/src/core/tcp_out.c` 的检查和 host 池耗尽测试证明了触发条件；BoarOS 在真实失败时撤下可写事件，将 bounded retry 纳入 poll/epoll 与 socket 等待期限，ACK 和池释放均可促成后续写入。真实 pthread U-mode 用 UDP 接收队列占满静态 lwIP 内存，验证非阻塞 TCP write 得到 `EAGAIN` 后 `poll(POLLOUT,0)` 不虚报，释放队列后 2 秒内可写并完成写入。这个测试在旧只看 `sndbuf` 的实现上会于立即 POLLOUT 检查失败。

零长度 UDP datagram 揭示另一种 owner 漏洞：`read_buffer` 已登记 reservation 并移走 OFD pin，却返回 0；调用方原来只在正字节数时 finish，导致返回后 socket 指向失效的栈请求。固定 Linux 的普通 read/readv 对空 datagram 都返回 0 且消费它；同 ELF 红测在 BoarOS 第一笔空包之后 fatal，修复后继续读取下一包并检查用户态关机 `heap-live=0`。由 `request.socket` 是否登记而非返回字节数决定是否 finish；TCP EOF 没有登记。强制退出 owner 也需按真实控制流判定：`kernel/sched/process.c::request_thread_termination` 只置位并唤醒，保存的 syscall 栈继续返回，`kernel/sched/signal.c::kernel_signal_select` 在 user-return 才终止；`kernel_socket_sendto/recvfrom` 的栈局部 pbuf/packet 在这条路径仍可清理。若以后引入绕过 syscall unwind 的非局部退出，需重审它们。
