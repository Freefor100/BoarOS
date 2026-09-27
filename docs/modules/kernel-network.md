# 单 hart 网络模块

## 入口与对象

`kernel/syscall/socket.c` 导入 RV64 Linux socket 参数和用户指针，`fs/files/socket.c` 把 socket 作为普通 fd/OFD 安装并在失败时回滚，`net/socket.c` 持有 endpoint、数据包、待 accept 队列及等待队列。`fs/open_file.c` 在最后一个真实 OFD 引用消失时销毁 socket；dup、fork 和 syscall 期间的 pin 共享同一 endpoint，close/exec/退出均沿既有 fd 生命周期回收。`fs/files/io.c` 把普通 read/write/readv/writev 接到 socket 队列，pread/pwrite/lseek 返回 `ESPIPE`，`fstat` 标识 `S_IFSOCK`，poll/select/epoll 读取 socket 就绪和等待队列。

协议实现是原样导入的官方 lwIP `STABLE-2_2_1_RELEASE` raw API，peeled commit `77dcd25a72509eb83f72b033d219b1d40cd8eb95`，位于 `third_party/lwip/`；固定资料和许可证见 `references/README.md`、`references/sources.tsv` 与 `third_party/lwip/COPYING`。本地移植层是 `net/lwip_port/`，启用 `NO_SYS=1`、IPv4、TCP/UDP 和 loopback。当前没有网卡 netif。lwIP 协议、segment 和 pbuf 使用静态有界池（UDP PCB 16、TCP active 32、listen 16、segment 128、pbuf 64）；BoarOS socket、接收/accept 队列节点及 OFD 使用 kernel_heap。PCB 池耗尽映射 `ENOMEM`，创建失败不留下 OFD；分配器 `STATE` 和错误释放是 fatal 不变量。

## 已验收 ABI 与等待

当前支持 `AF_INET` 的 `SOCK_DGRAM`/`SOCK_STREAM`，`SOCK_CLOEXEC`、`SOCK_NONBLOCK`，UDP bind/getsockname/sendto/recvfrom 和 `SO_RCVTIMEO`，TCP bind/listen/connect/accept，以及连接后的普通读写和就绪。`ioctl(SIOCGIFFLAGS/SIOCSIFFLAGS)` 让真实用户程序启用 `lo`；接口对象从公开 `netif_list` 查找。地址使用 RV64 `sockaddr_in` 布局；非阻塞、坏 fd/地址/指针和协议错误由固定 Linux 同一 ELF 差分约束。未覆盖的地址族、选项和操作返回明确 errno，不伪造成功。

单 hart 下，登记/检查就绪与睡眠用已有关中断临界区。socket syscall 与轮询入口推进 lwIP loopback 队列和协议定时器；非阻塞 connect 发出 SYN 后立即推进一次，保证已经睡眠的另一进程 accept 能被唤醒，而不依赖客户端下一次系统调用。阻塞 connect 等到握手结果，阻塞接收以 socket 队列或协议定时器唤醒；通用 poll/ppoll 和 epoll/epoll_pwait 在监听 socket 时也把最近协议定时器纳入睡眠期限，包含混合普通 fd 和无限等待。信号沿既有 syscall restart 协议，带接收超时的中断返回 `EINTR`。待 accept 子连接在对端 reset 后从队列摘除；未 listen 的 stream 和 datagram accept 立即返回类型对应错误，监听 socket 的 `SO_RCVTIMEO` 约束阻塞 accept。

普通 socket read/readv 把队首数据暂存到内核栈后登记任务级 read reservation，并把 syscall 的 OFD pin 暂交给 reservation；用户复制完整成功才提交 TCP 消费，复制 fault 保留整段 TCP 数据，UDP fault 丢弃该 datagram。reservation 期间同一 socket 的 read/recvfrom 不得越过队首；同一 OFD 的第二线程等待其释放。正常返回恢复原 pin，强制退出在文件表释放前取消 reservation 并释放 pin。固定 Linux 的坏指针和跨页 read/readv 返回 `EFAULT`，后续 read 可读回完整 TCP 数据。`recvfrom` 的缓冲区范围在等待空 UDP socket 前检查，负的 `socklen_t` 返回 `EINVAL`；accept/recvfrom 的地址输出错误发生在协议 dequeue 后，与固定 Linux 顺序一致。

TCP `POLLOUT` 同时要求发送缓冲和队列空间。`tcp_write` 还可能因全局 segment/pbuf 池满而返回 `ERR_MEM`，此时 socket 撤下可写事件并登记有界重试期限；ACK、成功写、错误或销毁解除登记。等待者取最近的 lwIP 协议和写重试期限，池释放后即使没有 ACK 也能继续，而不会因虚假的 `POLLOUT` 在单 hart 上空转。`F_SETFL(F_GETFL|O_NONBLOCK)` 接受已有 access mode 位，只更新可变状态位。

关闭活动 TCP 连接先解绑全部指向 BoarOS socket 的回调，再 `tcp_close`；协议 FIN/TIME_WAIT 可能暂占静态 PCB/segment 池，随后由定时器回收。这与内核堆对象生命周期分开。当前只验收单 hart loopback；AF_UNIX/socketpair、shutdown/半关闭、通用 sendmsg/recvmsg、更多 sockopt、外部网卡与 SMP 并发仍在 `docs/goals.md` N2/N3，不能由本切片推出。

## 验证

```sh
make test-lwip-host
make test-syscall-riscv test-userland-riscv test-diff-abi-riscv test-stack-usage
python3 tests/program-inventory/run.py --suite libc \
  --case libc.static.socket --case libc.dynamic.socket \
  --require-pass --output build/socket-program-check
```

host 入口实测 UDP loopback、PCB 16 个用尽后第 17 个失败、全部释放和再分配，另在 `sndbuf>0` 时耗尽全局 TCP segment 池触发真实 `tcp_write ERR_MEM`，以及 TCP 握手/关闭后推进 200 秒协议计时、池用量回到基线。当前 64 条 socket 差分记录覆盖两进程握手、读/向量读 fault 保留、UDP fault 丢弃、负地址长度、accept 超时、F_SETFL access mode、ppoll/epoll 的协议定时器和混合 fd；本分支 460 条 Linux/BoarOS 记录一致。真实 pthread U-mode 另覆盖共享 socket 双读、阻塞读时 close/fd 复用、双读线程组 SIGKILL、全局池压力下错误可写事件及释放后写入进展，关机检查 `heap-live=0`。线程组 SIGKILL 测试可控地覆盖等待和竞争路径，但公开 ABI 无法精确钉住 staging 到 usercopy 的极短窗口。原版 libc-test `functional/socket.c` 的静态、动态直接 entry 用未改源码和同一 ELF 在双方通过；全量 228 项清单尚须在合并内核上重跑。

Linux ABI 依据本地 `references/linux/net/socket.c`、`net/ipv4/af_inet.c`、`fs/read_write.c`，commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e`；测试构建来自 `references/oscomp-testsuits` commit `8b58dd16d26d30f7c74d48d5832d870d3051b703`。核对后运行 `make prune-build` 清理日志和镜像。
