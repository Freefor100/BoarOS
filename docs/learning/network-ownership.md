# 网络对象与 loopback 事件推进

固定 libc-test `references/oscomp-testsuits` commit `8b58dd16d26d30f7c74d48d5832d870d3051b703` 的 `libc-test/src/functional/socket.c` 真实调用 IPv4 UDP 的 bind/getsockname、微秒级 `SO_RCVTIMEO`、sendto/recvfrom，随后建立 TCP listener、非阻塞 connect 和 accept；它不消费 AF_UNIX，也不能单凭测试名推断所需协议。固定 Linux commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e` 的 `net/socket.c`、`net/ipv4/af_inet.c` 和 `fs/file.c` 提供 syscall、协议错误与 fd 生命周期比较基线。

取舍比较过三条路线：自写有限 TCP/UDP 子集能控制所有状态，但协议重传、定时器和互操作验证成本最高；宿主转发可快速启动，但 fd、错误和恢复语义被宿主环境决定；固定成熟 C 栈的 raw API 保留 BoarOS 的 fd/OFD、用户复制、等待、就绪和 errno owner，同时复用已测协议状态机。用户选择第三条，固定官方 lwIP `STABLE-2_2_1_RELEASE` peeled commit `77dcd25a72509eb83f72b033d219b1d40cd8eb95`，采用 `NO_SYS` 单 hart 事件驱动。分配 owner 另经用户确认：协议/packet 用静态有界池，BoarOS socket/OFD/队列节点用 kernel_heap，以池计数与 root `heap-live=0` 分别核对。

`NO_SYS` 没有独立网络线程。若服务端已睡在 accept，客户端非阻塞 connect 返回后不再调用 socket，SYN 若只留在 lwIP loopback 队列，服务端会永久睡眠。因此发起 connect 的系统调用必须推进一次 loopback，轮询与定时等待也推进协议。两进程握手测试固定顺序，排除了同进程立即 accept 偶然泵送队列的伪通过。另一条生命周期边界是 TCP `tcp_close` 在已连接状态可能暂保留 FIN/TIME_WAIT PCB；销毁 OFD 前必须先解绑指向 BoarOS 堆对象的回调，host 测试随后推进 200 秒计时并检查静态池回到基线。待 accept 子连接的 reset 也必须在 dequeue 前剔除。

固定参考是本地 `references/lwip/` 和导入的 `third_party/lwip/` 同一 commit；移植只在 `net/lwip_port/`，不修改上游 core。重建入口与当前能力边界见[网络模块](../modules/kernel-network.md)。外部网卡、AF_UNIX、半关闭和 SMP 的 owner/同步仍需要单独验证。
