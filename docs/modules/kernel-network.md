# 单 hart 网络模块

## 入口与对象

`kernel/syscall/socket.c` 导入 RV64 Linux socket 参数和用户指针，`fs/files/socket.c` 把 socket 作为普通 fd/OFD 安装并在失败时回滚，`net/socket.c` 持有 endpoint、数据包、待 accept 队列及等待队列。`fs/open_file.c` 在最后一个真实 OFD 引用消失时销毁 socket；dup、fork 和 syscall 期间的 pin 共享同一 endpoint，close/exec/退出均沿既有 fd 生命周期回收。`fs/files/io.c` 把普通 read/write/readv/writev 接到 socket 队列，pread/pwrite/lseek 返回 `ESPIPE`，`fstat` 标识 `S_IFSOCK`，poll/select/epoll 读取 socket 就绪和等待队列。

协议实现是原样导入的官方 lwIP `STABLE-2_2_1_RELEASE` raw API，peeled commit `77dcd25a72509eb83f72b033d219b1d40cd8eb95`，位于 `third_party/lwip/`；固定资料和许可证见 `references/README.md`、`references/sources.tsv` 与 `third_party/lwip/COPYING`。本地移植层是 `net/lwip_port/`，启用 `NO_SYS=1`、IPv4、TCP/UDP 和 loopback。当前没有网卡 netif。lwIP 协议、segment 和 pbuf 使用静态有界池（UDP PCB 16、TCP active 32、listen 16、segment 128、pbuf 64）；BoarOS socket、接收/accept 队列节点及 OFD 使用 kernel_heap。PCB 池耗尽映射 `ENOMEM`，创建失败不留下 OFD；分配器 `STATE` 和错误释放是 fatal 不变量。

## 已验收 ABI 与等待

当前支持 `AF_INET` 的 `SOCK_DGRAM`/`SOCK_STREAM`，`SOCK_CLOEXEC`、`SOCK_NONBLOCK`，UDP bind/getsockname/sendto/recvfrom 和 `SO_RCVTIMEO`，TCP bind/listen/connect/accept，以及连接后的普通读写和就绪。`ioctl(SIOCGIFFLAGS/SIOCSIFFLAGS)` 让真实用户程序启用 `lo`；接口对象从公开 `netif_list` 查找。地址使用 RV64 `sockaddr_in` 布局；非阻塞、坏 fd/地址/指针和协议错误由固定 Linux 同一 ELF 差分约束。未覆盖的地址族、选项和操作返回明确 errno，不伪造成功。

单 hart 下，登记/检查就绪与睡眠用已有关中断临界区。socket syscall 与轮询入口推进 lwIP loopback 队列和协议定时器；非阻塞 connect 发出 SYN 后立即推进一次，保证已经睡眠的另一进程 accept 能被唤醒，而不依赖客户端下一次系统调用。阻塞 connect 等到握手结果，阻塞接收以 socket 队列或协议定时器唤醒；信号沿既有 syscall restart 协议，带接收超时的中断返回 `EINTR`。待 accept 子连接在对端 reset 后从队列摘除，不能把失效的 PCB 变成致命文件表错误。

关闭活动 TCP 连接先解绑全部指向 BoarOS socket 的回调，再 `tcp_close`；协议 FIN/TIME_WAIT 可能暂占静态 PCB/segment 池，随后由定时器回收。这与内核堆对象生命周期分开。当前只验收单 hart loopback；AF_UNIX/socketpair、shutdown/半关闭、通用 sendmsg/recvmsg、更多 sockopt、外部网卡与 SMP 并发仍在 `docs/goals.md` N2/N3，不能由本切片推出。

## 验证

```sh
make test-lwip-host
make test-syscall-riscv test-diff-abi-riscv test-stack-usage
python3 tests/program-inventory/run.py --suite libc \
  --case libc.static.socket --case libc.dynamic.socket \
  --require-pass --output build/socket-program-check
```

host 入口实测 UDP loopback、PCB 16 个用尽后第 17 个失败、全部释放和再分配，以及 TCP 握手/关闭后推进 200 秒协议计时、池用量回到基线。差分的 35 条 socket 记录覆盖两进程服务端先阻塞 accept、客户端非阻塞 connect 后不再调用 socket 的时序，以及普通/向量 I/O、`fstat` 和 `ESPIPE`；当前总计 431 条 Linux/BoarOS 记录一致。原版 libc-test `functional/socket.c` 的静态、动态直接 entry 用未改源码和同一 ELF 在双方通过；BoarOS PID 1 退出时要求 `heap-live=0`。全量 228 项清单尚须在合并内核上重跑。

Linux ABI 依据本地 `references/linux/net/socket.c`、`net/ipv4/af_inet.c`、`fs/read_write.c`，commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e`；测试构建来自 `references/oscomp-testsuits` commit `8b58dd16d26d30f7c74d48d5832d870d3051b703`。核对后运行 `make prune-build` 清理日志和镜像。
