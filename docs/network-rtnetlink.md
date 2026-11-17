# 只读 rtnetlink

## 支持边界

`AF_NETLINK` / `NETLINK_ROUTE` 的 RAW 和 DGRAM socket 复用已有有界
消息队列、poll、recvmsg、PEEK、TRUNC 与 OFD 生命周期。原 POSIX MQ
32-byte notification cookie 保持原格式，可以与 rtnetlink 回复共存。

- `bind` 支持显式唯一 port ID 和自动绑定；自动绑定优先当前 TGID，冲突时
  分配独立 ID。回复的 sockaddr 来源是 kernel port 0，nlmsg_pid 是接收端 ID。
- `RTM_GETLINK` 支持 dump，以及按 index / `IFLA_IFNAME` 点查询。
  `AF_PACKET`（真实 `ip link` 使用）、UNSPEC、INET、INET6 均可查询 link。
- `RTM_GETADDR` / `RTM_GETROUTE` 支持 UNSPEC、IPv4、IPv6 dump。
  非 dump GETADDR 明确返回 `NLMSG_ERROR(-EOPNOTSUPP)`。
- IPv4 `RTM_GETROUTE` 点查询支持 `ip route get`：本地/loopback、当前直连
  网段、已配置的默认网关，以及无路由 `ENETUNREACH`。按 snapshot 中的
  lwIP 接口顺序选择首个 UP、link-up、有地址的匹配接口，不伪造最长前缀 FIB。
  接受 DST、已配置的 SRC（含 loopback 子网）、OIF 和 UID；OIF 不存在返回
  ENODEV，不可用返回 ENETUNREACH。显式 OIF 的 off-link 目标仍需设备已配置
  gateway，否则 ENETUNREACH；不模拟 Linux 可合成的 on-link route。
  UID 不影响当前无用户策略的选择。
  `RTM_F_LOOKUP_TABLE` 返回真实 local/main 分类；普通回复是 /32 resolved
  route，不带 MULTI/DONE，保持 sequence，并支持请求的成功 ACK。
  非零 IIF、MARK、TOS、额外路由表/header selector、notify、fibmatch、
  multicast/broadcast 与 IPv6 点查询明确返回 EOPNOTSUPP；未知 attribute
  不会被忽略后假装按它查询成功。非法前缀长度、attribute 长度返回 EINVAL。
  不宣称支持策略路由、增删改、netlink multicast 或用户态单播。
- LINK 导出真实名称、ifindex、硬件类型/MAC、MTU、UP/RUNNING/LOWER_UP、
  operstate。内核没有 qdisc，导出 `noqueue` / txqlen 0，不伪造统计计数。
- ADDR 导出现有单 IPv4 地址/掩码、广播地址，以及 netif 确实持有的 IPv6
  loopback、link-local 与最多两个静态地址；包含真实前缀和 tentative/dadfailed。
- ROUTE 导出现有 IPv4 直连网段、本地地址与唯一 IPv4 default，以及 IPv6
  本地/connected 路由和显式 IPv6 default gateway。IPv6 相同 connected prefix
  去重，tentative/duplicate 地址不生成可用路由，没有 gateway 时不输出默认路由。
  不把 Linux 的任意 FIB / namespace / rule / 多地址模型假装为已有能力。

回复使用四字节对齐并清零 padding，保持请求 sequence；dump 项标记 MULTI，
以携带零状态的 DONE 结束。点查询 ACK 与错误包含原请求头，错误还回显原
payload。短正文/非法 attribute 返回 EINVAL，找不到 link 返回 ENODEV；不支持
操作返回 EOPNOTSUPP。没有完整 nlmsghdr 的尾部按 netlink framing 丢弃。

## 生命周期与开销

只在请求发生时进入网络服务域，在一次协议保护范围内复制全部接口/网关状态，
随后释放协议对象保护并编码回复。排队的是不可变字节，不保存 netif 指针，不在
普通 TCP/UDP 数据路径添加统计、缓存失效或新锁。

当前已有接口数量很小，使用显式上限：16 个 snapshot、4096-byte 请求、
16384-byte 编码空间。超限返回 EMSGSIZE / ENOBUFS，不截断成“成功的完整 dump”。
一次 send 的所有消息回复在一个 datagram 内原子排队，因此不会 interleave；
MULTI / DONE 可以处于同一 datagram。消费端仍须按 nlmsg_len 迭代。

编码空间不长期占据队列：发布前复制到精确大小的 packet，并按真实 packet
allocation charge 计入 receive quota。MQ 待发布 cookie 的额度仍独立保留。
最终关闭按队列中各包的实际长度释放额度，不再假定所有包都是 32 bytes。

## 定向验证

宿主入口（需要常规 C 编译器与 e2fsprogs）：

```
sh tests/host/rtnetlink.sh
FILE_IO_HOST_SHIFTS='12 14' sh tests/host/netlink-notify.sh
```

前者覆盖真实 FD/OFD/socket/heap owner、绑定冲突、三类 dump、sequence、
MULTI / DONE、点查询/无路由/selector 错误、成功 ACK、错误回显、截断、接收额度与混合 MQ 关闭；后者保留既有 MQ
RAW/DGRAM、故障、等待/关闭、取消与最终 owner 清零回归。两个入口按 4K/16K
页配置运行，而不是只测试独立 wire encoder。

客户机程序与 runner：

```
$RV_MUSL_CC -static -O2 -Wall -Wextra -Werror \
  tests/workloads/network/rtnetlink.c -o build/rtnetlink-rv
python3 tests/userland/rtnetlink.py --kernel "$RV_KERNEL" \
  --program build/rtnetlink-rv --cpus 4 --network --ip "$RV_STATIC_IP"

$LA_MUSL_CC -static -O2 -Wall -Wextra -Werror -Wl,-z,max-page-size=16384 \
  tests/workloads/network/rtnetlink.c -o build/rtnetlink-la
python3 tests/userland/rtnetlink.py --arch loongarch --kernel "$LA_KERNEL" \
  --program build/rtnetlink-la --cpus 2 --network --ip "$LA_STATIC_IP"
```

使用仓库默认 RV unknown-elf 内核编译器；musl 编译器仅构建用户 ELF。
LA 内核使用其默认 loongarch unknown-linux-gnu 工具链。
客户机包含 4 个进程共享同一 netlink socket 发起 dump，检查回复隔离、序列、
PEEK/TRUNC、错误以及最终 root owner 回收。`--network` 时测试通过现有 ioctl
配置真实 gateway，然后检查 iproute2 输出确有 Ethernet 和 default route。
点查询还覆盖接口 down/up、删除/恢复 gateway 后结果立即变化、SRC/OIF、
非法 source、无网卡的 ENETUNREACH；不从旧 dump 构造持续缓存。

`--ip` 接受未修改的 iproute2 静态 `ip` ELF。6.1.0 的真实 `ip link show`、
`ip address show`、`ip route show table all`、`ip -6 address show` 和
`ip -6 route show`、`ip route get 127.0.0.1` 是验收消费者；网络场景还运行
`ip route get 192.0.2.1` 及其 `oif eth0` 变体。runner 不仅检查退出码，还拒绝隐藏的
`RTNETLINK answers:` / ioctl 错误。内核测试不是吞掉 stderr 的“退出码成功”。

本环境 Linux 宿主创建 `NETLINK_ROUTE` socket 返回 EPERM，未能执行 Linux
运行时差分，不能把以上验收称为 Linux 差分通过。点查询 wire/header/source
语义核对 Linux v6.1 `net/ipv4/route.c` 的 `inet_rtm_getroute`、`rt_fill_info`
与 `ip_route_output_key_hash_rcu`；这不是动态差分的替代。跨架构专项与真实 iproute2
验证独立于该限制；未来有可运行的参考 Linux 后仍应补充差分。
