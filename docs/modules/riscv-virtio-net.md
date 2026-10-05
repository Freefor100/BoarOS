# VirtIO-net 与 Ethernet 接收 owner

`arch/riscv/virtio_mmio_net.c` 实现 MMIO legacy/modern split ring；`net/ethernet.c`
持有 netif、custom pbuf 记录和 joinable worker。根启动对象保存设备 owner，在
PID 1 与后代收口后停止网络，最后检查堆与物理页基线。DTB 的 device ID 1 和
PLIC route 决定设备与 IRQ，不按槽位或测试配置硬编码。无设备保留 loopback。

每个 RX/TX 队列 32 个描述符，分别预分配 64 个 2 KiB 缓冲。legacy 的两个队列
占 16 KiB，modern 占 8 KiB；DMA 总量分别为 272/264 KiB。控制对象大小在启动时
报告，worker 的独立栈由调度器管理，最终栈统计另计。协商 MAC、可用的 STATUS
和 modern 的 VERSION_1；不协商 mergeable RX、checksum/GSO、packed ring 或
多队列。网络头分别为 10/12 字节，帧起点统一留在槽内 16 字节之后。

## 发布、完成与借用

RX 描述符资格与缓冲内容的 owner 分开。设备完成后描述符可再次使用，原缓冲
先变成 worker 的 CPU lease，再移交给 custom pbuf。最多同时借出 32 个缓冲；
最后一个 pbuf 引用归还前，设备不能覆盖原内容。custom 回调只归还槽、唤醒
worker，不分配、不睡眠。达到借用上限则复制到有界 PBUF_POOL，并立即归还
DMA 槽。UDP 接纳同时检查 DMA 借用、协议堆和备用池占用，保留至少 16 个
备用 pbuf 给 TCP/控制流；不足计丢弃，不承诺不限速 UDP 无丢包。

TX 在协商 indirect（bit28）后按包发布槽内 indirect 表：首项为设备头（驱动缓冲），其余为最多两段 pbuf（PBUF_RAM/POOL 且地址落在内核镜像映射内），驱动 `pbuf_ref` 持有到完成；IRQ 只把完成槽标为待归还，worker 在非 IRQ 上下文释放引用。设备失败/超时不提前归还：失败不等于 DMA 停止，设备仍可能读取已投递的描述符与 payload，在途 owner 保留到 stop 复位确认后才 abandon，与 RX 借用同一策略；已完成 owner 仍由 worker 归还。未协商 indirect、段数超限、volatile 或地址不可换算时回退为复制路径；`tx-sg`/`tx-copy` 统计两条路径的包数。最多 64 个排队，32 个可发布。
`linkoutput` 不睡眠，容量不足返回 ERR_MEM。TCP 已接受的内容仍归协议，完成
唤醒 worker 后先释放已完成 owner 和槽，再重试 unsent 数据，无需用户再次进入 syscall。零拷贝 TX 持引用期间 TCP 段保持 busy，完成后才可重传。

IRQ 先确认事件，再校验并收割完成，只唤醒 worker。used-index 差值、head ID、
发布资格、长度和未协商 offload 头都必须合法；modern 运行期 NEEDS_RESET 在
IRQ、service 与直接 send 入口都检查，不能因无 TX 或配置 IRQ 丢失而继续发布。
失败保留冷路径队列现场并停止
新发布：除设备/队列现场外打印槽级快照（loaned/ready/pending/done、队列索引与
每个非空闲 RX/TX 槽的状态、长度、age、owner），只读驱动自有数组，不追描述符
地址、不分配。只有已发布 TX 使用五秒设备完成期限，正常 RX 空闲不是设备故障。
worker 在首次收割后、RX与协议回调前释放完成owner；最后一次service及release后
比较容量代次并复核RX ready。SG release返回归还数，容量代次还覆盖复制TX在IRQ/service
中的直接归还。有新容量或RX才继续服务，单纯窗口关闭的unsent不会形成自旋。
worker 在任务上下文推进 Ethernet/ARP、协议定时器和重试，每批 RX 最多八帧，另独立限制 loopback 八包、socket 八个工作单元、一个 timer 回调；批次
之间开放中断并让出运行机会。失败 NIC 不再收发，但共享的 loopback 和协议期限
仍继续推进；不能在错误分支永久睡眠并停止 TIME_WAIT/重组回收。
raw API 沿既有单 hart 临界区串行化，IRQ 不重入堆。无 NIC 时 `kernel_network_start`
不创建收发 worker，但保留同一 `kernel_network` owner 与 timer-only worker：循环
使用相同有界服务，先排空可立即执行的软件工作，再按 min(下一 socket 期限, now+5×frequency) 阻塞，期限/IRQ 只唤醒
它，最后的 OFD 定时回收不再依赖用户再次进入 syscall，`kernel_network_stop`
同样禁止新工作、join 后释放。

停止先禁止新工作、唤醒并 join worker，清理接口的重组/ARP，移除 netif，然后
reset 并确认 DMA 已停。netif 移除会同步按旧本地地址 abort active/bound TCP PCB，
包括已脱离 socket 登记的 FIN_WAIT，释放其乱序 RX 引用；TIME_WAIT 转入前已 purge，
此处不依赖已 join worker 的后续 timer。reset 未确认保留 DMA；reset 已确认但协议或接收请求
仍持 pbuf，也保留整个设备 owner，返回 EBUSY。最后引用归还后才可释放队列页、
RX/TX 页和控制对象。设备故障与连接拒绝、RST、协议超时分别交付；错误回调
不读取已经释放的 PCB。该协议未覆盖非一致 DMA、IOMMU、多 hart 或实板。

## 地址与用户接口

默认 `eth0` 为 10.77.0.2/24、MTU 1500，MAC 来自设备。构建参数 `NET_IPV4` 和
`NET_NETMASK` 写入独立生成配置；未配置默认网关。bind 接受真实本地地址，路由
由 netif 决定；保留 IPv6 loopback 与双栈监听，不宣称外部 IPv6、公网或 DNS/TLS。

`SIOCGIFFLAGS/SIOCSIFFLAGS` 查询并控制 UP/DOWN；`SIOCGIFADDR/NETMASK/HWADDR/MTU/INDEX/NAME/CONF`
输出实际接口快照，RV64 ifreq/ifconf 为 40/16 字节。快照不借用 netif 给用户复制；
copy fault 不延长设备生命。地址、掩码、MTU setter、路由配置及 netlink 尚未实现。
loopback 的协议 MTU=0 表示无 L2 限制，其公开逻辑 MTU 为 Linux 的 65536。

## 验证与固定资料

`COST_DIAGNOSTICS=1` 的 final 行另报 `tx-done-free-*` 与 `tx-free-post-*` 的
count、ticks、max、时钟频率和 overflow 标志，发布构建不增加时间戳或计数。
DONE 从软件收割 used ring 开始，不是硬件 DMA 完成时刻；copy 完成直接归还计零。
FREE→post 跟踪同一槽再次发布，包含正常空闲和排队时间，不能直接解释成可运行工作的
延迟。首次使用没有 FREE 样本，reset 撤销未完成 owner 不计完成；统计范围为设备整个
生命期，溢出会显式拒绝该实验结果。宿主模型在两种构建/传输下核对已知时间差、重复
release、copy 与 abandon，实际进展仍由 worker/NIC-only 契约单独验证。

```sh
make test-virtio-net-host test-lwip-reassembly-host test-ethernet-worker-host
python3 -B tests/network-riscv.py --only boaros --workload interface
python3 -B tests/network-external.py --transport both
python3 -B tests/network-external.py --only linux --transport modern
```

宿主模型独立解码寄存器银行与 split-ring 字节，63 项覆盖两种传输、头部、64 位
地址、队列满、非法完成、索引绕回、indirect 表展开与未协商回退、abandon 归还
语义、启动失败、超时、借用、reset owner 与失败槽级快照。它不能
替代真实 DMA/IRQ。`python3 -B tests/host/ethernet_worker.py` 另编译实际 worker，
用协议/调度边界模型验证设备失败后仍推进期限并按下一期限等待，且失败只归还
已完成 owner、不 abandon 在途引用；同一1514字节
输入的custom路径零复制、回退路径精确复制1514字节，最后引用、OOM、控制余量
及input失败回收均核对。边界模型不验证实际协议时钟或DMA。真实 TAP runner 在无特权 user/network namespace 内运行，
不启用 vhost/offload、不设置网关，输出只留 build。应用、性能和最终资源证据见
[网络学习记录](../learning/network-ownership.md)，当前收口状态见[开发路线](../goals.md)。

协议依据固定 `references/qemu` v11.1.0 的 MMIO 与 virtio_net 标准头、
`references/linux` v7.2 的 VirtIO-net 实现、`references/lwip` 2.2.1 的 Ethernet
入口；精确身份由 sources.tsv 管理。重组的本地修补范围见[第三方组件](../third-party.md)。
