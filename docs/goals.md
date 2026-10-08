# 开发路线与验收

主线 CI 正在从 RV 单侧扩展为共用 host＋双架构核心/ABI/平台/GNU 常驻门禁，
完整原程序与设备组合逐日运行，原存储恢复矩阵保持每周/手动层。代码与本地
入口已验证；首次托管冷环境、完整 job 与定时组合结果分别收口，不能以配置
存在代替真实通过。见[主线 CI](modules/continuous-integration.md)。

本文是唯一开发路线入口。P/N/L 编号保留为能力与依赖索引，不是机械执行顺序。
`[x]` 只表示具体交付已验收；历史测量、输入身份和可重建命令归现有 learning。
固定 Linux 位于 `references/linux`，精确版本与其他资料由 `references/sources.tsv` 管理。评审是调查输入，不自动成为实现或验收证据。

## 当前状态与未关闭风险（2026-10-08）

N3已经交付：legacy/modern VirtIO-net、受限DMA借用与复制回退、静态IPv4＋ARP、
有界分片重组，以及隔离宿主双向TCP/HTTP。1196条ABI、原22项网络客户端和相关系统
回归属于该轮已完成证据，见[网卡记录](learning/network-ownership.md#真实-virtio-net-与宿主应用交付2026-10-02)。
随后buddy/slab的timer抢占竞态已确定性复现、修复并验收；纯块超时也已补reset前快照。
这些内存与诊断修复已单向合入兼容分支；网络、FIFO和重启的适用范围见对应模块。

| 条目 | 当前证据与下一步 |
|---|---|
| 历史Virtqueue告警 | 已还原到旧兼容内核的五连接loopback；原启动只有块盘和RNG，没有VirtIO-net。具体报错队列与超额原因仍未知；后续失败必须先保留新快照，按队列身份定位，不以重复通过关闭。net 失败路径现已有槽级快照（2026-10-05，见网卡模块），原告警仍缺历史现场。 |
| 历史页释放fatal的具体现场 | 已修复能够产生同类fatal的分配器双owner竞态；原事件没有owner快照，不能反推唯一触发链。 |
| extent 冷读 OOM 清理 | 真实冷缓存碎片文件已复现未取得 buffer 引用却按非零块号释放的错误；get 失败直接返回后，1/4 KiB 块与两种 inode 大小的全部分配失败点、读取失败、重试、卸载及堆引用清零通过。独立入口为 `make test-lwext4-extent-host`，没有放宽非法释放契约。 |
| pthread取消与旧libc输入 | 原镜像动态glibc的cancel/exit缺libgcc_s；独立glibc运行环境已固定unwind依赖并保护取消/cleanup。静态cancel-points的join结果在固定Linux也失败，按库/测试契约继续核对，不能归给内核。历史偶发现场仍保留P0c边界，不安排无目的重复次数。 |
| 内核抢占边界 | allocator修复不等于所有共享状态已审完。限定检查开中断worker到共享对象的调用链、睡眠前引用和发布临界区；发现具体错误才扩大。 |
| TX 完成进展纠错 | worker先收割并释放完成槽，再推进协议；睡眠前覆盖SG与复制路径的新容量。宿主顺序/关闭窗口、真实两种TAP传输和无NIC定时器验证通过；见[网络owner](learning/network-ownership.md)。 |
| 就绪查询与协议服务 | poll 为局部只读快照，短 syscall 与后台 worker 共用有界服务；协议池/NIC/接收堆归还按代次通知等待者。模型与真实 RV64 验证见[网络记录](learning/network-ownership.md#纯就绪与有界协议服务2026-10-05)，接纳 reservation 已接入 TCP 流复制；27 组窗口/池/堆实验已完成，默认预算部分负载吞吐回退，控制尾延迟与资源峰值分开列于[预算结果](learning/data-path-budget-experiments.md)。 |
| TCP 接纳约束复制 | 参数/状态/容量优先于 payload 复制，任务登记的 byte reservation 和 OFD pin 允许复制睡眠。预先 EAGAIN 为零页解析/零复制；全局协议资源变化仍允许有界失败；近池诊断实际捕获 segment/heap 饱和后的重复复制，8/4/2 在本负载消除该项失败，尚非跨层预约。Linux 同 ELF 契约、交错和退出验收见[网络记录](learning/network-ownership.md#tcp接纳预算与复制2026-10-05)。 |
| 缓存覆盖与脏范围 | 冷页完整覆盖不预读旧页，独立脏页组织与哈希选择最小候选集合，快照只复制脏段。64 MiB 成本门禁及文件/映射/交错回归通过；完整 lwext4 及 SQLite DELETE/WAL 恢复矩阵通过，候选序号未触发的条目单列于[规模记录](learning/single-hart-scale.md#冷页覆盖与脏范围2026-10-06)；块层与 VFS/ext4 八项批量读已通过真实暂扣门禁，写回 1/2/4/8 页候选已通过独立门禁，预读 0/1/2/4/8 页和取消/真实暂扣门禁已通过；默认与 RA8/WB8 的阶段七后完整 DELETE/WAL 恢复已通过；身份绑定的 TCP 27 组/存储 20 组及扩展实验共 1,218 次发布启动、184 次诊断通过，TCP 默认已按用户选择 8/4/2，存储保持 RA0/WB1；见[预算实验](learning/data-path-budget-experiments.md)。 |
| 追加增长纠错 | 增长与截断分离；对齐增长不扫描缓存页链，非对齐增长仅处理旧尾页。1/4/16/64 MiB 成本门禁与真实文件/映射回归通过，另有匹配吞吐：64 MiB、1 KiB 缓存追加默认约 20.86 MiB/s，旧基线约 3.70；不推广成整体倍数。见[规模成本](learning/single-hart-scale.md)和[测量边界](learning/data-path-budget-experiments.md)。 |
| epoll 交付纠错 | 完整 event 复制后提交 ET/ONESHOT；独立扫描/pending、MOD 代次与任务退出 owner 已接入。生产函数宿主边界及固定 Linux 同 RV64 ELF 验证通过；见[事件交付](learning/epoll-delivery.md)。 |
| 双盘控制协议纠错 | 原基线 `3c34091`/`5687377` 的暂扣阶段超时已定位为规范模式终端等待行结束：host只发送单字节g，尚未出现B盘暂扣READ。guest显式设置并恢复控制终端后，暂扣/故障/重启和FIFO/RR四组合通过；最终复核还修复主动kill与NBD响应写入的收口竞态，受控cut确认后30次额外故障/重启通过。`test-multi-disk-io-riscv` 已在托管 CI 通过；新版双架构整体门禁仍按实际 job 收口。见[可睡眠存储](learning/sleepable-storage.md)。 |

[风险证据与重建](learning/cost-baseline.md#旧版内存释放与-virtqueue-告警2026-10-02)
区分已经修复的机制与缺少历史现场的归因。固定root、单hart、QEMU和选定应用验收
均不代表多用户隔离、SMP、实板或完整Linux兼容。LA 的指定单核 CPU、平台与原程序矩阵已验收；更广原程序、客体原生开发和完整比赛容器基线仍待独立结果。

## 近期方向

本轮单核数据路径阶段一至七已交付，保留以下由测量暴露的后续边界：默认 TCP 预算在部分 bulk/高并发负载吞吐回退；全局协议资源不足后的复制与重试仍有成本；WB8 可能降低仅缓存完成的小写速度。优先按[匹配结果](learning/data-path-budget-experiments.md)选择目标程序和候选，网络默认已按用户选择调整到 8/4/2，存储保持 RA0/WB1。受控延迟补测和具体原程序问题由兼容分支记录；双架构原盘固定容器调用已在oscomp-compat完成，剩余失败和总预算未到达另列，不能计为全部测例通过。

文件元数据的活inode复用、创建句柄初始化、O_PATH与路径truncate已交付，通用实现
已单向合入兼容分支。路径资格、睡眠前节点引用、初始时间与显式改权的区别、截断属性
清理和恢复由对应模块维护。局部准备成本的减少不代表连续文件I/O瓶颈已经解决。
逐次成绩、原始输出与机器快照只留在忽略的build，不进入文档或Git。

空路径stat的Linux边界、免路径缓冲及全局sync/syncfs已接入；通用VFS快照保护
挂载与节点，复用写回和durable等待，独立维护挂载错误观察；相关系统回归已验收。
这不代表连续I/O或TCP瓶颈已经消失，也不以某个分数作为其他应用开发的前置条件。
Unix98 PTY、独立 devpts、peer 打开、packet 模式和 PTY/串口 termios2 已交付。
原 BusyBox script/scriptreplay、真实 libc PTY API、作业控制及录制文件重启读取已成立；
接口、身份与有界资源归[TTY契约](modules/kernel-tty.md)，应用边界归
[消费者机制](learning/session-consumers.md#pty-的身份传输与应用边界)。
原终端不需要特制程序或改变 main 身份；TTY 的有限传输成本仍需由目标应用选择优化，
不将更换调度策略或某个吞吐倍数设为后续能力开发的前置条件。

双架构原盘固定官方容器调用已在oscomp-compat完成，原联合分2130；40组结束，
两侧LTP-glibc总预算中断、LTP-musl未到达，正常根owner回收未验证。该分支保留
300秒监督、源码helper跳过、显式shell和LA原musl调度DSO适配，成绩和详细输入
身份在该分支维护；main不默认预加载比赛运行时。当前维护main与oscomp-compat，
通用修复继续先落main再单向合入；共同/proc/cpuinfo等真实接口缺口按消费者跟进，
Bash/Perl及控制器环境另定范围。客体原生开发随后独立验收。
完整凭据/权限、运行时网络配置和无 RNG 平台的可信熵接入由目标应用确定交付范围；
LA 更广用户环境继续按真实消费者验收，SMP 单独规划。

2026-10-05 第三方审计已逐条核实（对照 5c82103 与当前工作树）：机制描述基本属实，
性能数字全部可溯源但存在口径混用；其归纳的基础问题与仓库已知边界一致，建议中的
数值目标不作验收门禁。本轮据此实施 B（scratch 复用，已交付）、A（到期索引与最早
期限重装，已交付）与 D（内核栈 guard，已交付）；C（网络关中断区/协议所有权）只出
设计对比。

2026-10-05 后续轮：审计剩余项经调查后确认三项实施、一项推迟。ASID 推迟到实板
轨道——固定 QEMU 没有 ASID 化 TLB（任何 satp 变化与任意 sfence.vma 都全刷、翻译
不使用 ASID），收益为零，且收窄 fence 的正确性缺陷会被 QEMU 全刷掩盖，只能在
ASID 标签 TLB 的硬件上验证。本轮实施：TX indirect+SG 零拷贝（保留复制回退）、
块设备在途深度测量（时间加权深度/queue-wait/服务时间，测后再定是否加深）、无 NIC
的 lwIP timer 出 IRQ 与网络 fail 槽表诊断（均已交付）。提交审查与本轮自审另发现
两处真实缺陷并修复：镜像 VA→PA 换算在 va>end 时边界减法下溢（新增越界拒绝与
sv39 越界用例），以及设备失败路径在复位确认前 abandon 在途 TX owner（失败不等于
DMA 停止，改为保留到 stop 复位确认；host worker 模型断言失败路径 abandon=0）。
块设备在途与页 owner 身份的加深仍按"先测量/发现具体错误才扩大"的既有纪律。

## 按证据触发的性能候选

| 触发证据 | 候选与必要代价 |
|---|---|
| 重复读取/未命中仍支配目标应用阶段 | 回收预算、必要预读或可省读取；核对内存峰值、压力和脏数据一致性 |
| 新封口政策下仍有细碎版本或重复准备 | 增量组织/合并；先量化，核对脏数据年龄、内存/日志空间和同步尾延迟 |
| checkpoint阻塞提交或最终排空成本显著 | 有界批次和调度；核对积压、日志环绕、低内存、卸载及完整恢复 |
| 非阻塞发送复制放大或固定热缓存运行成本高 | 发送credit约束暂存、重复解析/复制/查询；先核对错误优先级，保留短写、EFAULT前缀、取消和页生命周期 |
| Lua构建仍有差距，选定heap清零/搬迁与页内复制不足以解释 | 核对用户程序运行、页解析/ELF及未测固定成本，再选一个机制；VMA指标是比较次数，不能当查询数或时间 |
| 热路径stat/open无大量设备请求仍慢，fstat更轻 | 核对路径临时缓冲、逐级后端查询及对象周转；累计请求容量不是峰值。空文件创建删除另分解目录、inode/位图、事务及真实资源等待；保护身份、orphan、同步与复用，不顺势扩大缓存重构 |
| UDP过载丢包或TCP受协议credit约束 | 核对接收/丢弃量、每连接进展、lwIP窗口/池和socket预算；限速可靠性与饱和效率分开，TCP_INFO未支持的字段不作为重传证据 |
| 真实大映射/多等待者负载规模退化 | resident范围索引、deadline索引或安全长操作边界；核对维护成本、OOM和取消 |

每次选择一个有独立证据的机制，不把这张候选表当成新排期，不换调度器来解释尚未归因的分布。

## 已交付与历史证据

| 阶段/能力 | 事实与证据入口 |
|---|---|
| TX indirect+SG 零拷贝 | 协商 bit28 后每包发布间接表（设备头+≤2 段 pbuf），驱动持引用至完成、IRQ 标记/worker 释放；未协商或不可换算回退复制。真实 TAP 两 transport：tx-sg≈12.7 万包、tx-copy=47、errors=0。见[网卡模块](modules/riscv-virtio-net.md)。 |
| 无NIC timer owner与失败快照 | `kernel_network_start` 无 NIC 时创建 timer-only worker（按 min(下一socket期限, now+5×frequency) 睡眠），最后的 OFD 定时回收不再依赖用户 syscall；设备失败打印设备/队列/槽级快照并停止新发布，在途 TX owner 保留到 reset 确认（复现越界 used id 与 abandon 语义的 host 场景）。见[网卡模块](modules/riscv-virtio-net.md)、[网络](modules/kernel-network.md)与[本轮记录](learning/network-ownership.md#无nic定时器owner与审查修正2026-10-05)。 |
| 块在途测量与最终统计 | 块统计新增时间加权在途积分/忙时/总span/queue-wait/服务时间，设备销毁打印最终行；真实窗口实测忙时平均 1.31、空闲 86%、峰值 8，按触发条件不加深队列。见[块模块](modules/riscv-virtio-block.md)与[成本基线](learning/cost-baseline.md#块设备在途深度实测2026-10-05)。 |
| 内核栈窗口与未映射 guard | 生产任务栈映射到独立 128 MiB 窗口槽位，低 4 KiB 无 PTE 作为 guard，越界经窗口 VA 立即 store fault；窗口骨架构建期预留、运行期受限 map/unmap、空 level-0 表释放；无页表 fixture 回退 direct-map。见[调度](modules/kernel-scheduler.md)与[Sv39](modules/riscv-sv39.md)。 |
| 任务常驻 I/O scratch 页 | 每任务首次 I/O 分配一页并跨调用复用，正常调用只解除登记，任务销毁路径归还；musl 自动窗口物理页分配 42744→18066（差 24678 与 24576 次调用吻合）。见[调度](modules/kernel-scheduler.md)与[网络](modules/kernel-network.md)。 |
| 到期有序索引与最早期限重装 | blocked deadline 使用 (deadline, tid) 有序索引，到期只弹出已到期者；SBI 事件取 min(RT 预算, RR 片尾, 最早睡眠 deadline)；socket 写重试链按期限有序。COST deadline 门禁改为到期处理数与 timeout 一致、不随无期限 blocked 增长。见[调度](modules/kernel-scheduler.md)、[时间](modules/kernel-time.md)与[定时器](modules/riscv-timer.md)。 |
| 文件元数据与路径资格 | 活inode重开避免临时后端owner；创建权限按已有句柄初始化并保留创建时间。O_PATH只持路径身份，路径truncate接入共同截断和capability清理；见[文件契约](modules/kernel-files.md)、[VFS](modules/vfs-ext4.md)与[时间语义](learning/file-timestamps.md)。 |
| libc局部纠错与文件所有权 | flock错误优先级、无timer辅助的退出ready调度、固定glibc unwind依赖与取消清理已交付；fchown/fchownat修改ext4/tmpfs/匿名pipe真实元数据，capability删除与inode修改同事务，进程仍固定root。见[调度](modules/kernel-scheduler.md)、[文件契约](modules/kernel-files.md)和[所有权背景](learning/file-timestamps.md#文件所有权与进程身份)。 |
| T1–T3/P1–P4 终端 | UART IRQ/worker、行规程、ctty/作业控制、Unix98 devpts/PTY、packet 与36/44字节termios；原ash/stty、script/replay和真实libc PTY API已交付。稳定节点、背压、关闭和可信栈回收见[TTY契约](modules/kernel-tty.md)。 |
| M1–M3 有限CPU归因 | 六项消费者诊断已交付；宽字原语仅实验验证，未带来原Lua工程收益，未上线。生产保留字节路径，见[工具链机制](learning/offline-toolchain-probe.md)。 |
| 命名FIFO与原Lua工程 | 管道元数据、ext4/tmpfs FIFO、默认make jobserver、构建/增量/失败恢复及产物运行；1207 ABI与集中回归、兼容入口已交付。关闭观测-j1/-j2中位135.542/133.225秒；工作目录tmpfs不足以解释整体差距，见[工具链记录](learning/offline-toolchain-probe.md)。 |
| N3网卡与宿主应用 | legacy/modern、零拷贝RX及回退、有界重组、实际双向TCP/HTTP和1196 ABI；性能、原应用与历史风险见[网卡记录](learning/network-ownership.md#真实-virtio-net-与宿主应用交付2026-10-02) |
| D1–D3/T1/J1/V1纠错 | socket状态/扩容/接收资格、请求内暂存、版本量封口已交付；自动程序＋durable 11.541→5.129/12.108→5.359秒；完整恢复/双盘、1179 ABI及TCP复制限制见[存储](learning/cost-baseline.md#版本量封口与socket纠错对照2026-10-02)与[网络](learning/network-ownership.md#本轮应用结果与剩余复制2026-10-02) |
| N2a–N2e网络应用 | IPv6双栈、选项、半关闭与生命周期、22项受控原ELF、内容负载和1166 ABI已交付；原iperf连续脚本参考竞态单列，见[网络记录](learning/network-ownership.md#原版网络应用交付2026-10-02) |
| A＋B缓存与环境 | 已交付缓存命中先于回收、读来源和进展归因、真实日志/RTC/根盘；BusyBox55/55、1118 ABI及结果见[成本基线](learning/cost-baseline.md#缓存查询顺序纠错2026-10-01) |
| R1–R8组合边界 | SHM owner/attachment/权限、msync pin、整包DGRAM、同步故障与整次写门闩已交付；1091 ABI及RV64/真实libc/恢复证据见[组合边界](learning/cost-baseline.md)、对应模块和Git提交 |
| 进程身份、随机、调度 | TID/TGID/PGID/SID、coarse clock、可信VirtIO RNG、OTHER/FIFO/RR与实时预算已交付；见[消费者](learning/session-consumers.md)、[调度](learning/kernel-scheduling.md) |
| C0–C6成本测量 | 已收口的窗口/输入/开销/unknown与原消费者结果见[成本基线](learning/cost-baseline.md)、可重建命令；不重复保留测量待办 |
| S0–S5异步日志与交接 | 私有undo、running/frozen组、mount worker、idle IRQ和FIFO资格已交付；原五项写吞吐实测改善25.29–54.00倍，属于历史测量而非新准入条件 |
| S6–S8存储流水线 | 增量预留/有界池、sealed/durable/checkpoint分离、八span批量和共享relatime已实现及恢复验收；见[S9报告](learning/cost-baseline.md#s9-存储流水线验收2026-10-01) |
| S9消费者收口 | 测量/归档与正确性验收完成；原性能预期未达：Parent写0.98–1.88倍，自动程序+fsync收尾12.668/14.339秒，改善23.09%/22.04%；musl读Parent当时回退17.05%；后续A＋B已解释进展分布并单独测量根卸载余量 |
| 挂载/共享/数据库/工具链 | 统一后备、tmpfs、多盘、SysV shm、SQLite DELETE/WAL及固定小型离线GCC已交付；[多挂载](learning/memory-backed-mounts.md)、[恢复](learning/record-lock-sqlite-recovery.md)、[工具环境](modules/program-environment.md) |

S9完整恢复/双盘/RV64/libc/1091 ABI/scale/四组合io-sleep/栈通过。原1GiB专项
25.0271/25.3164不是完整Harness。mtime事务已组提交，后端重阻塞已为零，不能继续当作
尚未实现的下一轮任务。缓存顺序缺陷在未改HEAD宿主复现为0/800/100/0新增读，修复均为0；
匹配自动程序+durable中位改善7.76%/6.99%，四进程读Parent改善14.08%/1.20%，
glibc四进程整命令仍增加0.73%。该轮的prepare读取、1547次FLUSH和前台热读成本已有归因；后续版本量封口已减少组数与屏障，不能把这组历史计数继续当作当前值。
完整分布及观测扰动见[本轮纠错](learning/cost-baseline.md#缓存查询顺序纠错2026-10-01)；
没有因此新增性能准入门槛。

## 当前应用阻塞与能力边界

| 问题 | 当前边界/归属 |
|---|---|
| 原BusyBox55/55 | 日志/RTC/根设备内容已验收；当前共同229项已在RV和LA两种RAM逐ID通过，历史228项结果仍保留 |
| iperf/netperf | 22项受控原ELF完成；原iperf连续脚本的listener重建竞态在Linux也存在，原脚本结果和受控验收分别报告 |
| LTP执行角色 | 比赛目录遍历不是上游runtest；控制器helper、上游禁用shmat1和有限shm_test分开。原libc的EINTR重试、NULL栈clone、缺unwind库或账户可先于目标syscall阻塞，见[程序证据](learning/user-program-inventory.md#ltp的准备依赖与libc边界) |
| 所有权与接口子集 | fchown/fchownat已接入真实元数据；O_PATH和路径truncate已接入；完整凭据/权限、原生accept4仍有缺口；CPU-time clock、VIRTUAL/PROF timer、pipe容量操作、扩展clone/futex按具体子语义核对，不把已有整个模块记为缺失 |
| 全局文件同步 | sync/syncfs接入单一挂载树、节点快照与durable等待；syncfs维护独立的挂载错误观察。void sync的程序退出码仍不能单独证明持久化，匿名对象不触及根盘，见[VFS契约](modules/vfs-ext4.md)。 |
| mount/umount 扩展 | proc/tmpfs/devpts/ext4 的基础挂载、只读、覆盖与普通忙卸载已交付；mount 当前仅接受 `MS_RDONLY/MS_SILENT`，umount2 仅接受 flags=0。bind/rbind、remount、move、传播、其他挂载属性与 lazy/force/expire/nofollow 卸载仍未实现，分别列在 P1h；所有任务共用挂载树，尚无 mount namespace。 |
| 用户内存/信号 | mremap、按操作madvise、mlock、sigaltstack、实时信号队列、共享文件/PI futex待真实应用需求触发 |
| 系统与平台 | 固定root查询不等于完整凭据/权限；其他行规程、完整modem控制、外部IPv6/DNS/TLS、公网配置、SMP/实板及LA更广原程序/客体原生开发/完整Harness仍未验收，不声明完整Linux兼容或硬实时 |
| 架构专项边界 | RV F/D 与 LA FPU/LSX/LASX 的指定状态矩阵已交付，RV V 扩展未实现，固定 LA QEMU TCG 无 LBT。RV 客体离线 C/Lua 构建与 NBD 全恢复矩阵已有验收；LA 客体原生开发和相应全断电恢复矩阵仍需独立接入，不从共同 229 项或普通 SQLite 重启结果推定完成。 |

下面P/N/L小节保留稳定能力编号、契约、依赖和已有验证入口；只以上面的当前队列决定近期实施。

## P0：固定证据与时序问题

**入口**：`tests/program-inventory/{inputs.json,run.py,suites.py,reports.py}`、`tests/diff-abi/`、`.github/workflows/ci.yml`、[程序清单](learning/user-program-inventory.md)。2026-09-23 基线的 228 项为 223 pass、2 个直接 entry 退出不符、3 个包装失败；2026-09-27 整合内核为 227 pass、1 个 BusyBox 包装失败。静态/动态和脚本重叠不重复计算缺陷。

### P0c 历史取消异常的归因

- [x] 当时固定输入的静态/动态取消直接entry在Linux与BoarOS各30次通过；同步握手的后台sleep+kill和原BusyBox子项各20次通过。它们只证明对应输入，未还原旧脚本异常；运行目录已清理，重建入口归程序清单learning。
- [ ] 目标应用或原脚本再次出现失败时，先保存首次失败与真正wait status，缩小为取消登记、阻塞、join/clear_tid序列，再以确定性交错修所属机制；没有新证据不重复扩大次数。环境、辅助程序等待与reference-not-pass分别记录。

### P0d 持续证据与清单维护

- [x] 本轮证据保留工作树内核、ELF/loader、fixture、runner、QEMU 与固定 Linux/BusyBox/libc-test 身份；旧基线只保留历史定位用途。
- [x] 原七类直接失败按设备/身份、cwd、时间设置、文件系统统计、robust、socket 聚类；设备、cwd、显式时间、统计、robust 与 socket 静态/动态直接 entry 已关闭。整合内核的 228 项清单为 227 pass、1 upstream-failure，余下 BusyBox 官方包装脚本；不按非零退出码猜 syscall。
- [x] 已有硬回归严格通过；能力清单如实保存缺口。全量严格验收复用 `--require-pass`，不重建状态系统或跳过失败；相关静态/动态 entry、原包装器与 228 项清单均已重跑。
- [x] 限定集合的 `--require-pass` 只严格判定本次 selection，未选项目保持历史状态或 `not-run`；未知 ID、所选失败/未完成、中断、参考侧失败与全量严格模式均有 runner 回归。

**验收**：每个新增任务都有可证伪输入、有效 Linux 参考和归属机制；历史原始失败保留，未运行项目不计通过。不要求 P0 达到 228 项全绿。

## P1：路径、设备与文件元数据

**入口**：[文件模块](modules/kernel-files.md)、[VFS/ext4](modules/vfs-ext4.md)；`include/kernel/{vfs.h,open_file.h,fs_context.h}`、`fs/{vfs.c,open_file.c,fs_context.c}`、`fs/files/{path.c,metadata.c,io.c,table.c,poll.c,epoll.c}`、`kernel/syscall/{file.c,dispatch.c}`。

### P1b 最小对象与操作边界（最小 mount/路径对象路线已完成）

- [x] 文件身份使用 mount+inode，路径对象另持父目录项；OFD 持共享 flags/offset/引用，fd 槽持访问与 CLOEXEC。open→pin/dup/fork→close→末引用回收均有聚焦回归。
- [x] ext4 适配器提供按目录 inode 查找、按 inode 打开和读链接；字符节点按 `rdev` 选择后端，lwext4 类型未泄露到 task/syscall。普通文件、pipe、epoll 保留统一分派下的各自语义。
- [x] root/cwd、普通文件、目录和字符节点均持真实引用；unlink/rmdir 后旧对象存活，同名重建获得新 inode，外部引用使卸载返回忙。rename 子集已由 P1g 交付。
- [x] 发布流程按路径→后端→OFD→fd 逐级取得 owner；分配/打开/fd 满与 close/dup/fork/阻塞 pin 有失败和回收检查。ext4 close 失败转交 mount cleanup node，非法释放继续 fatal。

**交付**：一个可复用的 ext4/设备纵向入口及所有权说明；拟新增 `fs/path_walk.c`、`fs/mount.c` 等仅在职责确定时拆分，不要求一次重做 Linux dcache/RCU。

### P1c 真实设备文件

- [x] 统一路径/OFD 后端可打开 `/dev/null`、`/dev/zero`、`/dev/console`；身份来自 ext4 字符节点与 `rdev`，未知设备号返回 `ENXIO`，初始标准 fd 复用 console 实现。
- [x] null/zero/console 的读写、非阻塞、信号打断、零长度、坏指针、跨页部分 fault、向量与定位 I/O 均由真实 U-mode 或固定 Linux 差分验证。
- [x] 设备 `st_rdev`、seek、未知 ioctl、poll/epoll、访问模式与 `O_TRUNC` 已核对；OFD pin、dup/fork/close/退出及分配失败回到资源基线。
- [x] 原静态/动态 `stat`、`syscall_sign_extend` 已通过；后台 sleep+kill 同步重复 20 轮成功。daemon 的 cwd 阻塞后由 P1d 解除，P1e/f 收口时静态/动态原始 entry 均已通过；完整 session/TTY 仍归 P2d。

### P1d cwd 与目录 fd

- [x] 实现目录 fd 起点及 `chdir/fchdir/getcwd`；无效 fd 与非目录 fd 分开，绝对路径忽略坏 dirfd，空路径/尾斜线/过长路径和缓冲不足按固定契约返回。
- [x] 按分量处理 `.`/`..`、相对和绝对 symlink、循环上限与 mount 边界；保留 `symlinkat/readlinkat/O_NOFOLLOW/lstat`。不能先把整个字符串折叠再跟随 symlink。
- [x] fork 拥有独立 fs context，CLONE_FS 共享同一上下文，exec 保留；切换 cwd 时先取得新引用再提交，失败保持旧 cwd，退出释放准确 owner。
- [x] 测试不同任务/线程的 cwd 可见性、相对 symlink+`..`、打开目录后 rename 再 openat、unlink 后仍持目录句柄/getcwd 的行为。rename 完成前先建立身份契约，相应组合用例在 P1g 一起关闭。

### P1e 显式时间设置

- [x] 在活 inode 上实现 `utimensat` 与 libc `futimens` 所需入口；复用已有 realtime、relatime 和纳秒编码，不建立旁路时间戳缓存。
- [x] 对照固定 Linux 的 NOW/OMIT、空 times、非法纳秒、两项 OMIT、路径/nofollow、fd/dirfd 和权限检查顺序；保留未修改字段，ctime 的变化也必须符合契约。
- [x] 覆盖只读挂载、坏指针/跨页导入、无效 fd、不存在路径及真实写失败；明确元数据已变更后 I/O 错误的归属，不伪造回滚或错误成功。
- [x] 受控时钟测试与真实 Linux 差分共同验证；重跑原 `utime` 静态/动态及 BusyBox touch，保持 create/read/write/truncate/unlink 时间回归。

上述勾选对应程序清单的固定输入；2026-09-29 原 RV 评测镜像的 glibc `utime`
另出现时间比较失败；双侧原 ELF 与原 libc 探针已定位为 CLOCK_REALTIME_COARSE 缺失导致旧 time() 使用未初始化值。该能力随后已经补齐，当前REALTIME_COARSE/MONOTONIC_COARSE读取真实tick快照；本段保留旧失败原因，不再列为待办。

### P1f 文件系统统计

- [x] 从挂载和真实 ext4 superblock/分配状态提供 `statfs/fstatfs`；核对目标架构结构、块大小、块/inode 总量与空闲量、名称限制和只读标志，不返回固定容量数字。
- [x] 以 mount 为统计 owner，fd 查询不依赖旧路径重查；定义与正在进行的分配/释放及失败 I/O 的一致性范围。
- [x] 对比文件分配/截断/删除前后计数，覆盖 sparse hole 不等于已分配块、只读、坏 fd/用户指针和卸载；重跑原 `statvfs` 两种 entry。

### P1g 链接、rename 与权限相关文件操作

- [x] `linkat` 已接入同inode/nlink、活引用与跨mount约束，支持flags 0、AT_SYMLINK_FOLLOW、AT_EMPTY_PATH；见文件模块与多挂载证据，不再重复立项。
- [x] `renameat/renameat2` 支持普通/NOREPLACE；单事务文件/空目录覆盖、跨目录移动、祖先拒绝、同 inode、活目标及失败回滚，EXCHANGE/WHITEOUT 明确不支持。单根挂载的跨 mount 拒绝存在，真实多挂载验证归 P1h。
- [ ] `umask` 已按 fs context 的 fork 复制与 `CLONE_FS` 共享实现，真实inode的`fchmod/fchmodat/fchown/fchownat`已覆盖；匿名pipe改权和所有权也已交付；其余合成对象的改权和`faccessat`等完整凭据/权限消费者按实际需求推进，与 P2e 保持一致，不能总返回允许。已有 open 未知 bits 拒绝策略另用差分核对，不能写成 Linux 通用要求。

### P1h 虚拟文件系统与多挂载

- [x] main 已补普通文件/字符节点 mknodat，复用 null/zero/console；块节点已供设备识别和 ext4 挂载使用；FIFO已接入ext4/tmpfs；裸块I/O和devfs仍未交付。见[节点创建](modules/kernel-files.md#节点创建)。

- [x] 通用后端、mount/path 生命周期、null/zero/console 设备后端与首批真实 procfs 已分阶段交付；内部覆盖挂载遮蔽、根和 `..`、忙卸载及引用回收，真实第二盘及 tmpfs 的组合验证见本阶段 learning。
- [x] tmpfs 与真实第二 ext4 挂载具备独立路径身份；内存文件有真实后备对象/目录生命周期、配额耗尽、截断及最后引用回收；不宣称持久化或共享文件 futex。
- [x] 最小 procfs 的 uptime、meminfo、self/exe、self/fd、进程状态及挂载信息直接读取内核对象；PID 代次、线程退出、组长存活边界和非组长 exec 经固定 Linux 差分。本轮另补 256 轮 fd 关闭复用压力；完整 Linux 字段仍未覆盖。
- [x] 统一内存 owner 快照提供 Cached/MemAvailable/Shmem/Buffers/Dirty/Writeback，RV64 sysinfo 接入真实任务数和负载；无 swap/slab 回收时才返回对应零值。原 BusyBox free 与 LTP 已复验。
- [x] 阈值驱动后台写回及低/高水位回收，专用快照页、64 槽批次、有限失败、join 退出与资源回收；不加入周期清脏，fsync 错误/flush 契约保持。见物理页/VFS 模块和内存 learning。
- [x] 多挂载覆盖路径跨越、根和 `..`、挂载点被引用、卸载忙、跨挂载文件操作和失败交接；设备/内存/磁盘错误保持所属 owner。真实双盘延迟/写/flush 失败隔离已独立验证。
- [x] 用户态 `mount(2)` 已接 proc/tmpfs/devpts/ext4，接受 `MS_RDONLY/MS_SILENT`；`umount2(2)` 的 flags=0 路径检查 cwd、fd、映射与子挂载忙引用，停止 worker 和在途 I/O，成功后释放实例/设备。卸载再普通挂载的验证不代表 `MS_REMOUNT` 已实现。
- [ ] bind/rbind、move 和 shared/private/slave/unbindable 传播：先按真实消费者选择切片，明确挂载边、文件系统实例、路径/OFD/MM 各自引用与退出回收；普通路径别名或共享全局挂载树不能替代这些语义。
- [ ] remount 与其他挂载属性：按需求补只读状态切换、`MS_NOEXEC/MS_NODEV/MS_NOSUID`、atime/同步等实际执行点，核对已打开对象、写映射、dirty/flush 与失败回滚；凭据相关属性依赖 P2e，不把设置成功当作执行权限已生效。
- [ ] `umount2` 的 `MNT_DETACH/MNT_FORCE/MNT_EXPIRE/UMOUNT_NOFOLLOW` 及非法 flags/坏路径的错误优先级：当前所有非零 flags 明确不支持。分别核对摘树与末引用销毁、后端停止、expiry 状态和 symlink 查找；force 不允许提前释放仍被用户或 DMA 使用的对象。固定依据见[多挂载记录](learning/memory-backed-mounts.md#基础挂载与扩展操作的边界)。
- [ ] eventfd/timerfd 只在真实消费者提出需求后接统一 OFD，就绪、非阻塞、poll/epoll 和退出回收一起验收；signalfd 另依赖 P2c 队列。

**验证与退出**：先扩展 `tests/riscv/files_main.c` 等现有聚焦入口；当前入口为 `tests/userland/{namespace.h,metadata.h}` 与 `tests/diff-abi/{devices,namespace,metadata}.c`，均接入现有 runner。`make test-files-riscv test-vfs-riscv test-lwext4-host` → `test-userland-riscv`/`test-diff-abi-riscv`。设备、cwd、时间、统计分别解除对应真实程序阻塞，旧 ext4/pipe/epoll/COW/回收不退步；daemon 新出现的 setsid 依赖交给 P2d。

## P2：线程、futex、信号与资源

**依赖与入口**：可与 P1 独立推进；[调度模块](modules/kernel-scheduler.md)、[信号模块](modules/kernel-signal.md)、[时间模块](modules/kernel-time.md)；`kernel/sched/{process.c,futex.c,signal.c,wait.c,private.h}`、`kernel/syscall/{process.c,signal.c,time.c}`、`arch/riscv/signal.c`、`include/kernel/task.h`。不重写已有 pthread/普通 futex。

### P2a robust-list 退出协议（同步所有权路线已完成）

- [x] 增加 `set_robust_list/get_robust_list` 和每线程注册信息；RV64 长度、存活目标 TID、当前 root 凭据、输出故障顺序、坏地址注册与 fork/exec 生命周期有真实 U-mode 和固定 Linux 差分。
- [x] 同步清理发生在退出线程旧 MM 与原 TID 有效时；有界处理链、signed offset、pending、owner-died 和唤醒。普通退出、致命信号、exit_group 和 exec 清退成员共用退出调用链；存活 exec 线程使用换号前 TID。
- [x] 坏指针、循环链、非 owner、PI 标记、COW 与 OOM 的用户访问失败均有真实路径验证；2048 项上界和先读下一链接防止无限遍历及已释放页访问。robust 清理先于 clear_child_tid 和 MM 释放。
- [x] 未特改 musl 静态/动态 `pthread_robust_detach`、真实 pthread owner-died/consistent/不可恢复状态与原始 `SYS_exit` 探针均通过；PI 与跨 MM 共享语义不计入 P2a。

### P2b 等待、取消与 futex 扩展

- [x] 现有WAIT/WAKE/REQUEUE、bitset、deadline、clear_tid和组退出owner已在调度/信号模块记录并有对应回归；扩展操作继续独立验收，不代表所有futex命令已支持。
- [x] `WAIT_BITSET/WAKE_BITSET`、掩码与绝对超时已由 glibc/差分验收。
- [ ] 按实际调用补 `CMP_REQUEUE/WAKE_OP`；每个 operation 单独核对参数宽度、bitset、比较失败、relative/absolute 和 CLOCK_REALTIME，未知/未支持操作不算完成。PI futex 后置。
- [x] 无超时WAIT的SA_RESTART、带超时WAIT的EINTR与无handler时原deadline重启已有真实pthread和差分回归；新操作与跨MM信号组合仍按具体范围验证。
- [x] 致命取消/exec/exit_group沿原调用栈释放OFD pin、等待节点和MM引用，不提前释放执行栈；共享匿名key已接入P4d。历史pthread取消事件仍保留P0c归因边界。

### P2c 替代栈与实时信号

- [ ] `sigaltstack` 覆盖配置/查询、边界和禁用、嵌套 handler、主栈耗尽后的执行、sigreturn、普通 fork 继承、CLONE_VM 且无 CLONE_VFORK 时禁用、exec 重置；错误用户栈产生规定 fault，不破坏内核栈。
- [ ] 实时信号用可拥有的队列项表示，与标准信号合并位分开；验证重复排队、顺序、目标线程/组、屏蔽/等待/handler 消费，以及退出线程的队列回收；成功 exec 保留存活线程/组仍有效的 pending，不把 handler 重置当成 pending 清空。
- [ ] 队列 OOM、用户复制失败和取消时明确是否已消费；在真实队列计费后实现 `RLIMIT_SIGPENDING`。signalfd 等待队列稳定且有 OFD 后端后再接。
- [ ] 保留已有 `rt_sigtimedwait`、stop/continue 和各 syscall 不同的 EINTR/SA_RESTART；对嵌套、超时、信号排队与线程退出运行固定重复回归。

替代栈与 exec 生命周期依据本页固定 Linux 的 `kernel/fork.c`、`fs/exec.c`，pending 保留同时与现有 [信号模块](modules/kernel-signal.md) 契约对齐。

### P2d 会话、进程组与 TTY

- [x] 统一身份对象承担 TID/TGID/PGID/SID 角色引用；已实现 setsid/setpgid/getpgid/getsid，105 条差分覆盖组长、父子/exec、zombie、组信号与等待、身份继续存活及孤儿组。见[线程证据](learning/threads-and-futex.md)。
- [x] 原始消费者验证 daemon 的 SID/PGID 实际变化；孤儿组按固定 Linux 的退出/收养触发 HUP/CONT，SA_SIGINFO 与 sigwait 均保留 SI_KERNEL。该历史诊断未覆盖TTY；后来串口交付见[消费者](learning/session-consumers.md#原串口ashstty与控制终端2026-10-03)。
- [x] 串口与Unix98 PTY已接ctty、稳定前台组、终端信号和原ash/stty作业控制，覆盖末引用、挂断与取消；devpts、peer与packet保护编号复用、真实传输和回收。其他行规程和完整modem控制仍未交付。

### P2e clone、凭据与资源限制

- [x] child-TID 生命周期（main）：私有 fork 的首次 SETTID、按活跃 MM 使用者判断清零、vfork 成功/失败 exec 和线程唤醒。新增 8 条同 ELF 差分，完整 556/556；MM/exec/user 聚焦通过。基线 `fa38845` 的三处差异与交接检查见[线程证据](learning/threads-and-futex.md#child-tid-生命周期差分2026-09-28)。

- [ ] 逐项扩大合法 clone 组合，区分 flags 依赖错误与当前未支持组合；核对 MM/files/fs/disposition 各自复制/共享，TLS、TID 发布失败、组长先退、非组长 exec、收养和 wait 仍正确。
- [ ] 在 P1a 查询基础上设计显式凭据：真实/有效/保存 UID/GID、补充组及权限消费者；确认复制/共享/变更/exec 所有权后再支持 set 类接口。统一替换当前 root 假设，不出现查询身份与资源访问权限脱节。
- [ ] 保持 NOFILE/STACK：调低不关闭旧 fd、线程组共享、fork 继承、exec 保留、exec 后调高栈和非页对齐限制；用户输出 EFAULT 不回滚已生效的 prlimit 设置。
- [ ] 按需求增加 AS/DATA/FSIZE/CPU，分别说明记账 owner、生效点、越限 errno/信号和恢复；AS 是虚拟区间而非驻留页，VMA 合并/切分/mmap/brk/mremap 与 lazy allocation 保持一致。
- [ ] 先核对固定 Linux 对 RSS/LOCKS 的非执行语义、NPROC 的真实 UID 和特权例外，再决定支持范围；不机械地对每个历史枚举加限额，也不把有效但未支持资源成功空返回。

**验证与退出**：robust 已由 `tests/userland/pthread.c` 的原始退出/真实 mutex 消费者、`tests/riscv/uaccess_oom.S`、`tests/diff-abi/robust.c` 和静态/动态原 entry 验收；聚焦 `make test-scheduler-cases-riscv test-signal-riscv test-syscall-riscv`、`test-userland-riscv`、329 条差分、`test-riscv` 与栈检查均通过。拟新增 `tests/userland/signal_stress.c`、`tests/diff-abi/futex_signal.c` 仍属后续；旧取消异常继续 P0c，不以全部当前 pthread 通过冒充共享 futex 或 SMP 完成。

### P2f 单 hart 调度 ABI

- [x] OTHER/FIFO/RR、实时优先级、CPU0 affinity、RESET_ON_FORK 与真实查询；23 条策略差分，5 条 proc stat 差分。
- [x] 全局实时带宽默认 1 秒 / 950 毫秒，proc 参数原子更新并保留消费；36 条控制差分，真实 U-mode 验证节流、恢复、RR 余片与同时耗尽轮转。见[调度学习](learning/kernel-scheduling.md)。
- [ ] nice 权重、PI、SMP 和硬实时保证未交付；后续需求先独立比较方案。

## P3：同步持久化、文件锁与 SQLite

**依赖与入口**：文件/目录同步依赖稳定 VFS/OFD，块层可先做；`include/kernel/block.h`、`kernel/block.c`、`arch/riscv/virtio_mmio_block.c`、`fs/{vfs.c,page_cache.c}`、`fs/files/io.c`、`kernel/syscall/file.c`、`fs/lwext4_config/generated/ext4_config.h`。涉及 lwext4 变更时维护来源、许可证与本地 patch 记录。

### P3a 块设备 flush（已完成）

- [x] 块层已有缓存模式与 flush 回调；VirtIO legacy/modern 在 writeback/writethrough 四种组合通过真实请求测试。`make test-block-host` 验证能力拒绝、错误传播和可复用的易失缓存/稳定镜像故障模型；journal 与文件系统恢复证据另见 P3d。

- [x] 对照固定设备资料定义能力描述、缓存属性和 flush 请求：协商支持、提交、完成、I/O 错误、超时和只读边界；两种 VirtIO MMIO transport 都验证，不能把内存 fence 当介质同步。
- [x] 定义请求/描述符/bounce buffer 的 owner；超时/reset 后必须确认 DMA 不再访问，才能释放或复用请求内存。不可恢复设备错误由所属设备/mount 保留明确状态。
- [x] 无 flush 能力时依据设备真实缓存契约返回结果；不能忽略能力并承诺不可满足的持久性。测试正常完成、拒绝/失败、超时与 reset 回收。

### P3b fsync、fdatasync 与目录同步（已选逐 inode 路线）

- [x] 明确调用覆盖的文件数据、必要元数据、目录项和设备缓存；从 syscall/OFD 接到 ext4 提交与块 flush，顺序和成功承诺可解释。目录 fsync 单独验收。
- [x] 已选择并实现 inode 缓存页索引、脏范围/修改代次、定向写回及 OFD 错误观察；独立 open 与 dup、关闭后脏 owner、O_SYNC/O_DSYNC、真实 musl 和 260 条 Linux 差分验证。journal 事务依赖已接入 P3d。
- [x] 定义延迟错误属于哪个 mount/文件，以及后续哪些调用观察它；用户错误、真实写/flush 错误和内核不变量分开。关闭先摘 fd，不让历史 cleanup 改变当前有效 close 结果。
- [x] 同步链建立后才接 `O_SYNC/O_DSYNC`；覆盖普通/部分写、metadata 必要性、无效 fd、只读/设备及重复同步。新进程/重启看到已承诺内容只是正常路径证据，不能单独证明崩溃一致性。

### P3c 记录锁

- [x] 传统锁按共享 `kernel_files_record`，OFD 锁按打开文件对象；inode 增广 AVL 与 owner 索引支持六个命令、负长度/EOF、合并/拆分和跨类型冲突。独立区间模型核对随机操作、树不变量、稀疏/重叠规模及 OOM 不变性。
- [x] 固定 Linux 同 ELF 覆盖独立 open、dup/覆盖、fork、exec/CLOEXEC、任意相关 fd close、unlink、pthread 共享 owner、等待时 fd 关闭复用及组退出；末引用与清理 owner 仍由原文件层负责。
- [x] 阻塞、唤醒、信号打断/`SA_RESTART` 和有限传统锁死锁检测与固定 Linux 差分一致；等待请求 pin 与组强制退出的静态 musl 负载回到资源基线。OFD 不承诺死锁检测；所有结果限于单 hart。

### P3d ext4 journal 与恢复（已验收）

- [x] 已选择并启用 ordered journal/replay；事务 before-image、flush 顺序、checksum v2/v3 和 revoke、superblock 恢复、持久 orphan_file/传统链及分批 extent/间接块回收已接入生产。
- [x] 元数据事务提交与 checkpoint 分阶段 flush；关键日志错误 sticky，损坏或只读无法恢复时拒绝开放用户访问。已知日志提交前的资源不足可安全回滚，不能据此清除不确定 I/O 错误。
- [x] `make test-lwext4-recovery-host` 使用 512 字节原子写、易失缓存/稳定镜像，覆盖写与 flush 失败、丢失/重排、两次恢复和 e2fsck；完整 orphan 回收矩阵为 16 种组合、4,362 次断电执行。QEMU 正常退出和未验收实板不属于该恢复证据。
- [x] 真实 musl 已验收“临时文件→fsync→rename→两侧父目录 fsync”应用序列，后端 rename 另有断电/重排矩阵；记录锁与 SQLite DELETE 见 P3c/e，共享文件映射见 P4，IPv4 loopback 首切片见 N。

### P3e SQLite 回滚日志负载

- [x] 官方 SQLite 3.53.4 amalgamation 固定 URL/SHA-256，原生 Unix VFS 和线程/WAL 编译能力保留；启动核对版本、`THREADSAFE=1`、`DELETE/NORMAL/mmap_size=0` 与 EXTRA/FULL 实际 PRAGMA 值。
- [x] 静态/动态 CLI、建表、提交/回滚、多连接及独立进程 writer 冲突、未提交事务的进程直接退出与同次启动 hot journal 恢复、关闭重开和 `integrity_check` 在真实 U-mode 通过；同一恢复 ELF 的 setup/mutate/recover 也在固定 Linux 执行。
- [x] NBD 服务在宿主协议测试和 QEMU 接入通过；EXTRA/FULL 正常事务、FULL 写/flush 错误传播、热日志和已确认提交后的两次重启恢复通过，逐字节检查整事务与 ext4。小事务逐事件矩阵完成 441 次断电、100 次写失败、47 次 flush 失败；普通多进程 WAL 后续在 P4 另行验证，不以共享文件 futex 为统一前置。

**验证与退出**：`make test-record-lock-host test-record-lock-riscv test-diff-abi-riscv test-nbd-host test-sqlite-rollback-riscv test-sqlite-nbd-riscv test-sqlite-recovery-riscv test-sqlite-recovery-matrix-riscv` 分别覆盖模块、Linux 差分、正常运行与恢复；既有 files/userland/lwext4/栈/RISC-V 全套与 228 项清单已重跑，清单仍为 223/2/3 且失败 ID 不变。WAL 与实板持久性不计入本阶段。

## P4：共享后备对象、文件页与跨 MM futex

**依赖与入口**：[MM](modules/kernel-mm.md)、[VMA](modules/kernel-vma.md)、[VFS](modules/vfs-ext4.md)；`include/kernel/{mm.h,vma.h,file_mapping.h,memory_object.h}`、`mm/{vma.c,memory_object.c}`、`arch/riscv/mm.c`、`fs/{page_cache.c,vfs.c}`、`kernel/syscall/memory.c`、`kernel/sched/futex.c`。P4a 已迁移统一内存后备对象；文件页及共享 futex 复用稳定身份所需的接口按各自契约扩展，不预建通用插件框架。

### P4a 共享匿名对象（已完成）

- [x] mmap 时建立可引用身份，即使尚无驻留物理页；fork 共享后备对象，按对象+页索引发布页。已驻留页保留共享 PTE 权限，私有页继续 COW。
- [x] VMA 借用对象并保存连续 offset，MM registry 持对象引用；对象页槽与每个 PTE 分别持物理页引用。切分、合并、fixed replace、munmap 和 fork 后身份与 offset 连续，末引用回收页槽。
- [x] fork 前已驻留、fork 后子先缺页、fork 后父先缺页均用同步握手验收双向可见；固定 Linux 差分加入同一 ELF 的三页共享案例。
- [x] 覆盖 PROT_NONE/恢复、部分 unmap、替换、父/子先退出与 fault/PTE 分配 OOM；中途失败回滚新页槽，引用计数回到基线。

### P4b 共享文件页与权威数据源

- [x] 同挂载/inode/文件 offset 的独立 open/mmap 指向同一文件页；普通 read/write 与映射读写三向可见。MAP_SHARED 首次写故障标记 inode 缓存脏页，写回前经页别名反向索引重新保护 PTE，generation 防止写回中重写被误清。
- [x] 缓存页保留 clean/dirty/writeback 与 generation 状态；驻留共享 PTE 持物理引用并在缓存页中登记别名，回收不得驱逐仍映射的页。fd 关闭与路径删除不改变 inode 页身份。
- [x] `msync` flush EIO 后可重试；成功写回重新保护共享别名，之后再写能重新标脏，驱逐缓存并重读仍是新值。共享 fork metadata OOM 与固定替换分配失败保留原别名，结束时引用回到基线。
- [x] 测试专用 ext4 回调在旧值写入后、generation 比较前，通过另一个驻留共享 VA 写入新值；再次 `msync` 和驱逐重读证明新脏数据未被旧写回清除，两个别名及最终 OFD 回到基线。
- [ ] 第二个 hart 的真实并发写入、跨核 TLB 与多别名回收资格仍由 P6 验证；单 hart 的确定性重入不替代它。
- [x] 固定 Linux 同一 ELF 差分覆盖独立 open、多个 VA 别名、fork 后独立 MM、MAP_PRIVATE 隔离、共享页与普通读写可见性；SQLite WAL 子进程另经独立 open/mmap 使用共享索引。
- [x] 注入 `msync` flush 失败返回 EIO，真实 OFD/mount 保留可重试状态；持映射/临时 pin 的缓存页不按无引用页驱逐。
- [ ] 并发 miss/锁外 I/O 的跨核发布在 P6d 验收。

### P4c 截断与 msync

- [x] 已驻留页跨 fork 后在另一 MM 中截断仍撤映射，越 EOF 再访问产生 SIGBUS；`O_TRUNC`、非对齐尾页、先缩后扩与普通写跨 EOF 空隙由固定 Linux 差分验证。
- [ ] 私有修改及截断故障交错仍需补充聚焦验收，并保持已有 private COW/PROT_NONE 截断回归。
- [x] 关闭 fd 后映射仍活、unlink 后仍能访问，由持有 OFD 的 MM 来源维持 node 生命周期。
- [x] 固定替换分配失败仍保留共享文件 VMA、驻留 PTE、缓存别名和 OFD 来源；fork 的 metadata 分配失败不改变父映射，成功子 MM 释放后父写回及末引用回收通过。
- [ ] 退出清理时真实 I/O 错误与多别名回收交错仍需单独验证。
- [x] `msync(MS_SYNC)` 走真实 dirty→写回→同步与错误传播，不能只失效缓存；`MS_ASYNC`、`MS_INVALIDATE`、对齐、空区间、空洞和互斥 flags 经固定 Linux 同一 ELF 差分，注入 flush EIO 后可重试。

### P4d 共享 futex

- [x] private key 使用单调且不复用的 MM 身份号与虚拟地址；共享匿名 key 由稳定对象身份与连续字节偏移派生，不依赖物理页地址。非 private 操作解析用户映射并在坏页返回 EFAULT；共享文件 futex key 仍待实现。
- [x] WAIT 登记、WAKE 和 REQUEUE 持有共享匿名对象引用；unmap/最后映射消失与等待者恢复不会悬空或重用旧 key。fd 后备尚未接入。
- [ ] 跨 fork 的独立 MM 共享唤醒、不同对象同 VA 隔离、共享→私有 requeue、同对象不同偏移的同桶/跨桶迁移，以及最后映射撤销后的等待超时已有真实 U-mode 验证；基础唤醒、requeue 和坏地址另有固定 Linux 差分。共享文件映射现已提供不同 VA 别名的构造路径，但共享文件 futex key 和跨 MM 信号交错仍需聚焦验证。单 hart 先通过，跨核原子比较/登记属于 P6。

### P4e mremap、madvise 与 WAL

- [ ] 对象生命周期稳定后实现 mremap，覆盖移动/增长/收缩、旧/新地址失败原子性、共享身份和 AS/DATA 记账；不复制成意外私有对象。
- [ ] madvise 按真实消费者逐项增加；DONTNEED 区分 private/shared/file，已丢弃的私有内容不能再次读出，错误不能成功空返回。
- [x] SQLite 普通多进程 WAL 使用固定 3.53.4 原生 Unix VFS，在同一 ELF 的固定 Linux/BoarOS 双侧运行独立进程 writer 竞争、已提交/未提交事务、进程直接退出、重开与第二次启动恢复及 `integrity_check`；P3 回滚日志基线继续保留。
- [x] WAL 的独立 NBD 矩阵覆盖 EXTRA/FULL、hot、已确认提交、全部 42 个事务内块事件的 126 个断电组合和 42 个写/flush 失败位置；每次从稳定镜像恢复两次，并检查整事务与 ext4。实板持久性未验证。

**验证与退出**：P4a 已由 `test-vma-riscv`、真实 U-mode 的 `tests/userland/shared_mapping.h`、`tests/diff-abi/shared_mapping.c` 验证。P4b/P4c 的共享页别名、权限、fork、截断尾页、unlink、`msync` 参数及普通写扩展由 `make test-diff-abi-riscv` 双侧验证；聚焦错误交错由 `make test-files-partial-write-riscv` 验证。普通 WAL 与重启由 `make test-sqlite-wal-riscv`，存储恢复由 `make test-sqlite-wal-recovery-riscv test-sqlite-wal-recovery-matrix-riscv` 验证。P4d 基础路径由 `tests/userland/shared_futex.h`、`tests/diff-abi/futex_shared.c` 与 `tests/riscv/mm_cases.c` 验证；不同 VA 别名及交错矩阵仍按上一条跟踪。共享文件 futex 分开跟踪。

### P4f SysV IPC

- [x] 当前 shmget/shmat/shmdt/shmctl 均已接入；针对 IPC_PRIVATE 与命名 key、段大小对齐与 Linux 布局完成 194–197 系统调用接入。
- [x] 确认 key/id/代次、segment 与 attach 的 owner、IPC_RMID 后存活/末引用释放、fork 继承与 exec/退出分离，以及权限/限额/OOM/fault 回滚；统一后备对象作为物理页后备。本轮 R2 将 VMA 裸 segment 借用改为 attachment 引用，并在通用 VMA 编辑/销毁处维护 nattch。
- [x] 验证跨进程/不同地址、删除后已有映射、ID 反复复用、部分失败与资源基线；Linux 差分 ABI 新增 19 条全部一致（累计 1031 条），用户态多进程 fork/shmdt/IPC_RMID 与裸机 scale 测试全数通过，关机物理页完全回收。SysV semaphore/message queue 不自动纳入本阶段。

## P5：glibc、exec 与单核真实工具链

**入口**：[ELF](modules/user-elf.md)、[exec](modules/kernel-exec.md)、[程序环境](modules/program-environment.md)；`kernel/{elf64.c,elf64_source.c,exec.c,random.c}`、`arch/riscv/elf_image.c`、`kernel/syscall/`、`tests/program-inventory/inputs.json`。试跑可现在开始，完整应用依赖按真实调用落到 P1–P4。

### P5a glibc 独立矩阵

- [x] 固定 glibc 2.44 官方源码参考、宿主 RV64 loader/libc、相应 ELF、构建/运行环境与许可证/哈希；不修改二进制绕过内核缺失，不把 musl 结果套用为 glibc 结果。宿主二进制由逐文件 SHA 固定，尚未提供从官方源码逐位重建的工具链证明。
- [x] 基础矩阵双侧验证静态、动态、PIE、静态 PIE、额外 DSO、初始 TLS、dlopen TLS、pthread、信号及其组合；保留装载/main 之前、运行时和退出阶段的首个失败。更广 glibc 应用仍待试跑。
- [x] 已区分内核 PT_INTERP 装载和用户动态链接器/运行时职责；首次 pthread_join 失败经固定 Linux 差分定位到 futex bitset，而非动态重定位。

整合后 `make test-glibc-riscv test-diff-abi-riscv test-userland-riscv test-sqlite-wal-riscv test-riscv test-stack-usage` 通过，固定 Linux 差分为 408 条一致。228 项清单按 `python3 tests/program-inventory/run.py --reuse-builds --output build/glibc-futex-inventory` 全量重跑仍为 223/2/3，五个旧失败 ID 不变。输入和重建命令见[程序清单](learning/user-program-inventory.md)。

### P5b shebang 与 exec 组合

- [x] main 已交付 shebang 的解释器路径、单个可选参数、argv/envp、嵌套与错误边界。固定 Linux 开启 BINFMT_SCRIPT 后先复现四组差异；含递归错误优先级和空参数边界的 15 条脚本记录纳入完整 583 条差分，exec、musl、glibc 和栈检查通过。契约见[exec](modules/kernel-exec.md)，根因见[ELF 学习](learning/elf-loading.md#shebang-与-shell-回退2026-09-28)。原镜像 BusyBox 已验证无 shebang 回退依赖自执行路径；真实procfs与self/exe后来已在P1h交付。
- [ ] 保留 PT_PHDR/auxv、段重叠/对齐、文件尾页+BSS、PIE/解释器布局回归；与线程组 exec、信号、CLOEXEC 和文本写互斥组合验证，不在内核代替动态链接器重定位。

### P5c 随机数与系统环境

- [x] getrandom 区分可信熵就绪/未就绪、flags、阻塞/信号及用户 fault；VirtIO RNG legacy/modern 从宿主安全随机后端取得至少 32 字节后置 ready。DTB/用户写入和早期 ASLR 降级不计可信熵；实板熵源未验证。见[随机数来源](learning/random-source.md)与[RNG 传输](modules/riscv-virtio-rng.md)。
- [x] 完整klogctl 0–10、16KiB真实环、覆盖/消费/清空/阻塞/fault/权限及实际console过滤已交付，原dmesg及-r/-c/-n验收；见[日志模块](modules/kernel-log.md)。
- [x] Goldfish只读RTC_RD_TIME与10:135别名、OFD独占及exec/退出释放已交付，hwclock真实UTC、root设备来源/df内容已验收；写RTC/告警/事件读取保留缺口。
- [x] sysinfo 的 RV64 完整布局、内存/负载/任务数与 EFAULT 已交付。
- [ ] prctl 等只按真实调用链新增；版本和统计来自内核事实，未知能力返回规定错误，用户查询不能触发整机 fatal。

### P5d 离线编译闭环

- [x] 同一静态 musl 驱动和磁盘在固定 Linux/BoarOS 记录预处理、编译、汇编、链接、运行五阶段的独立结果，重放 journal 后检查逐阶段产物哈希与 ext4；无原生编译器时两侧明确停在 `preprocess:exec:2`。这是诊断基线，不是编译闭环。
- [x] 固定 Alpine v3.22 riscv64 GCC 14.2.0-r6 和 14 个依赖 APK，完成同一镜像中的预处理→编译→汇编→静态链接→运行；五阶段退出码、产物哈希与最终输出在固定 Linux/BoarOS 一致。该早期记录只覆盖小型C源码；当前已扩到Lua5.4.3原工程，其他项目仍须逐个验证。
- [x] 匿名pipe的mode、时间和身份归共享pipe，`fstat/fchmod`及两端、dup/fork/proc重开已交付；本轮未建立通用匿名inode框架。命名FIFO与默认make jobserver亦已验收，见[工具链记录](learning/offline-toolchain-probe.md#管道与默认fifo-jobserver2026-10-03)。
- [ ] 再固定 rustc/cargo、依赖锁定和离线最小项目，成功后扩大完整项目。先 `-j1` 建立正确性，不要求 SMP，也不把网络下载失败混入内核 ABI。
- [ ] 每个新失败最小化、对照固定 Linux，再修通用机制；保留可恢复的成功输入和失败样本，不能为构建脚本改写预期输出。

**验证与退出**：现有 `test-exec-riscv`、`test-elf-tail-riscv`、`test-userland-riscv`，新增 `test-glibc-riscv`、futex bitset 固定 Linux 差分和 `make test-offline-c-riscv` 的客体原生五阶段编译验收；`tests/userland/exec_scripts.c` 仍待后续能力建设。未特改动态 glibc 的基础矩阵和固定小型 C 程序的离线构建已有闭环；Lua5.4.3原工程、默认make jobserver及产物运行已验收；Lua之外的更大工程、C++与Rust仍未验收，不能把剩余运行失败合并成一个“动态链接未支持”。

整合后 `make test-diff-abi-riscv test-files-riscv test-userland-riscv test-lwext4-metadata-host test-references test-offline-c-riscv test-glibc-riscv test-sqlite-wal-riscv test-sqlite-wal-recovery-riscv test-riscv test-offline-c-baseline-riscv test-stack-usage` 通过；固定 Linux 差分为 430 条。228 项清单通过 `python3 tests/program-inventory/run.py --reuse-builds --output build/offline-gcc-inventory` 全量重跑仍为 223/2/3，五个旧失败 ID 不变。精确输入与历史证据见[程序清单](learning/user-program-inventory.md)。

## P6：SMP、TLB 与中断/I/O 并发

**前置与入口**：共享对象和线程契约已有单 hart 回归，平台具备可验证的 IPI/timer/IRQ 入口。`kernel/sched/{core.c,wait.c,process.c,futex.c}`、`kernel/physical_page.c`、`mm/heap.c`、`fs/{page_cache.c,vfs.c}`、`arch/riscv/{context.c,trap.c,mm.c,sv39.c}` 及设备后端。锁/每核/远端 TLB 接口数量由真实消费者决定，路线仍待确认。

### P6a 每核运行状态

- [ ] current、idle、trap 栈、FPU owner 和 timer 状态按核分离；2 hart 能进入内核只算启动探针，必须继续证明任务正常进出 U-mode。
- [ ] ready 发布、取出、迁移和退出具有唯一 owner，同一线程不能在两核同时执行；增加可检测重复运行的探针，避免只靠应用输出“看似正确”。
- [ ] 保持单 hart 基线和错误诊断，逐个消费者替换当前 tp/全局状态假设；页分配/引用/堆元数据同步也必须纳入。

### P6b 锁、等待与中断规则

- [ ] 列出哪些锁可在 IRQ 中获取、哪些允许睡眠、锁顺序及关中断范围；本地 SIE 不保护另一核，不能只把 refcount 改成原子就宣称 SMP 安全。
- [ ] futex compare→登记→阻塞由同桶/等价同步协议保护；跨核 wake 与入队、timeout、signal、clear_tid 对撞不漏唤醒，不重复运行或摘队。
- [ ] 缺页/设备 I/O 可能睡眠，不能持 spinlock 调 lwext4 或等磁盘；退出/取消期间，阻塞原栈释放已 pin 对象，IRQ 路径不做复杂生命周期回收。

### P6c TLB 完成与页回收

- [ ] PTE 失效/降权→通知相关 CPU→确认旧翻译不能再用→释放物理页、页表和映射引用；完成含义、目标核集合与并发地址空间切换必须明确。
- [ ] 避免持 MM 锁等待也需要该锁的远核；确定锁外完成及延期回收 owner。未确认不能安全复用旧页，也不能把超时转换成普通用户成功。
- [ ] 一核读写、另一核 unmap/truncate/mprotect，跨核 COW 同页与页表释放反复验证；保留单 hart 本地 SFENCE 顺序和 PROT_NONE 所有权。

### P6d 缺页和页缓存并发发布

- [ ] 锁内定位 VMA 并 pin 对象/版本；锁外读取/分配；回锁重查 VMA、权限、EOF、对象和版本。另核已 fault 或 unmap/truncate 时丢弃过期候选，释放准确引用后重试/返回。
- [ ] 同文件页并发 miss 只发布一个权威页；错误状态、dirty/writeback 和被固定页不会被后到的候选覆盖。不可持 spinlock 跨后端 I/O。
- [ ] 交错 fault、COW、fixed replace、退出和 OOM，核对页/对象/缓存/任务资源回到基线；不将所有资源耗尽升级成整机 fatal，不掩盖非法释放等核心不变量。

### P6e 请求完成与外部中断

- [x] 单 hart 已实现 IRQ/等待队列、八槽乱序完成、队列满等待、flush 屏障及超时 reset；双盘生命周期与失败隔离已验收。这里的剩余工作是跨核保护，不是重做轮询迁移。
- [ ] 明确提交、完成、取消、超时、reset、DMA 停止后内存可释放的 owner 转移；组退出时 I/O 尚未完成、另核关闭/复用 fd 不会回收在用对象。
- [ ] 在既有多请求队列上验证跨核提交、IRQ 归属、乱序完成/队列满、失败唤醒与设备停止；将跨核正确性与吞吐优化分开提交。

### P6f 多核验收矩阵

- [ ] 2→4→8 hart 各自重复：同页 COW、同文件页 fault、读写与 unmap/truncate/mprotect、wake 与入队、I/O 中 exit_group、fd 关闭/复用、内存耗尽与退出回收。
- [ ] 每一核数记录固定输入、重复次数、失败种类、泄漏/重复释放和同任务双核运行检查；保留单 hart 对照定位通用 ABI 与跨核错误。
- [ ] 正确性收口后进入 P7 并行构建；不先替换为 EEVDF。只有饥饿/公平性或性能证据要求时，再比较调度策略。

### P6g 真实内核栈 guard（可单 hart 先做）

- [x] 已选择并交付独立栈窗口：guard 为不建 PTE 的 4 KiB 页（无物理 owner），窗口骨架构建期预留、运行期 map/unmap、空表释放；direct-map 别名与 idle/boot 栈的保护边界记入[调度模块](modules/kernel-scheduler.md)与[Sv39](modules/riscv-sv39.md)。
- [x] 受控越界由 `make test-stack-guard-riscv` 验证：任务先写映射页，再写 guard 立即产生 store page fault（scause=0xf、stval=guard）；canary、高水位、`test-stack-usage` 与可信栈回收规则保留。

**验证与退出**：拟新增 `tests/userland/smp_memory.c`、`tests/userland/smp_wait.c`、`tests/riscv/ipi_tlb_main.c` 与对应多 hart runner；保留 `test-riscv`、`test-userland-riscv`、`test-stack-usage` 的单 hart 门禁。2/4/8 核各自有重复正确性和资源基线证据，不能由某一核数或 multi-hart boot 推定其余配置。

## N：socket 与网络支线

**依赖与入口**：P1 的 OFD/后端边界已用于单 hart IPv4 loopback。`net/socket.c` 持有 endpoint、数据队列和等待，`fs/files/socket.c` 持有 fd/OFD 提交，`kernel/syscall/socket.c` 导入 Linux ABI，现有 poll/epoll 使用 socket 就绪。固定输入与验收见[网络模块](modules/kernel-network.md)。

### N1 对象与协议栈选型（路线已确认，首个切片完成）

- [x] BoarOS 持有 fd/OFD、socket、请求 pin、队列和等待；协议回调由最后真实 OFD 引用销毁时解绑。首切片实现 IPv4 UDP/TCP loopback、阻塞/非阻塞与读写/就绪；IPv6、选项和半关闭在 N2 补齐。
- [x] 已比较自写协议子集、成熟 C 栈与宿主转发，确认 BoarOS ABI owner + 固定 lwIP 2.2.1 raw API、NO_SYS 事件驱动。协议/pbuf 使用有界静态池，socket/OFD/请求使用 kernel_heap；来源、所有权和成本理由见[学习记录](learning/network-ownership.md)。

### N2 本地与 loopback 链路（已交付）

IPv4/IPv6、双栈监听、连接选项、UDP 默认对端、向量消息、半关闭、错误消费、
backlog 和期限回收已交付。真实用户态保护用户复制、共享 OFD、取消和资源耗尽，
1166 条 ABI 与固定 Linux 一致。原 iperf/netperf 的22项受控结果、连续脚本的
实际失败和代表负载分布见[网络记录](learning/network-ownership.md#原版网络应用交付2026-10-02)。
命名 AF_UNIX 与 SCM_RIGHTS 未实施，按真实需求另行设计；普通 sendmsg/recvmsg
通过不能推出 ancillary 或 fd 传递完成。后续仅由本文当前队列选择，不重复列出
已完成接口任务。

### N3 网卡与真实服务

- [x] VirtIO-net legacy/modern实际数据路径、ARP/IPv4与隔离宿主双向TCP/HTTP；内容、多个连接、半关闭、UDP压力及停止回收已验收，限制见[网卡模块](modules/riscv-virtio-net.md)。
- [ ] VF2/LS2K 后端分别核对 DMA/cache、MDIO、checksum、IRQ、路由和链路故障；内存 loopback 不算实网卡完成。
- [ ] DNS/TLS 属于用户态时独立验证其随机、时间、文件/证书和网络依赖；TCP 通不等于 HTTPS 可用。

**交付证据**：固定客户端/服务输入、双方日志与实际外部报文，分别报告 loopback、QEMU 网卡、每块实板；所有权和失败场景与性能分开。

## L：LoongArch 与实板支线

**入口与资料**：`arch/riscv/`、`arch/loongarch/`、架构头与 Makefile，LA首阶段入口为 `kernel-la`。先读 `references/README.md` 与清单中的 LoongArch 手册/文档、Linux、QEMU、VF2/2K1000LA 资料，记录具体 commit/tag/文档版本或 SHA-256。

### L0 架构依赖盘点

- [x] 列出通用 MM/调度对 RV 头、satp、SFENCE.VMA、trap frame、页大小和寄存器布局的直接依赖，按实际消费者提取架构操作；不复制 `arch/riscv/mm.c` 中通用 VMA/文件页策略。
- [x] 保留架构 MMU/context/trap 与平台 DTB/MMIO/DMA 的区分，新增接口由第二实现验证，不预建空泛 HAL。

L0–L1于2026-10-06通过首阶段验收：构建期IRQ/MMU/task/timer绑定、共用MM/uaccess/ELF策略、
QEMU全部RAM bank与16KiB/三级用户页表、两个内核/用户任务的真实timer抢占及回收。
512MiB/1GiB各有9组同LA ELF的固定Linux对照，另有构造/缺页OOM与回收门禁；
RV完整架构、真实用户程序、glibc、1344条ABI和SQLite回归通过。范围见[LA首阶段](modules/loongarch-boot.md)。

### L1 最小启动与用户态

- [x] 串口→trap→timer→物理页→TLB/页表→高地址映射→一个真实 U-mode exit，每步有独立启动/故障/回收证据。
- [x] 延续已选 LA64 16 KiB/三级页表配置，页大小是架构构建期事实；不把目标配置描述为硬件唯一能力。只生成 `kernel-la` 不算用户态通过。

### L2 ABI 与映像

首阶段已覆盖内存ELF段/BSS/栈/auxv、整数寄存器、基本syscall与最小fork/COW计算探针。
statx/clone子TID、PCI根盘及LP64S静态musl已通过第二阶段验收；整数signal handler、
sigreturn/同步故障/等待重启和静态pthread/TLS已双侧验收，含真实musl取消、
非PI robust、线程组生命周期及任务/栈创建OOM回滚；原BusyBox ash非交互trap/wait通过。
用户已选择原版LP64D musl与LA标量FPU路线，动态libc/解释器/初始及late DSO TLS
和FR/FCC/FCSR/信号/clone/exec子集已双侧验收。LSX/LASX状态与扩展帧、关闭CPU扩展
及clone标量继承/上半部初始化已按固定Linux验收；本轮指定原程序和环境矩阵
已完成，范围外的更广程序仍未验收；已选统一VirtIO框架、内嵌SIMD状态及原版glibc2.42。
PGDH内核栈窗口/guard/NX及可信异常栈已验收。SIMD接入后GNU启动缺失AT_RANDOM的
SIGSEGV已定位到LA漏接QEMU DTB种子；复用不计熵的早期随机材料策略后，固定原版
共用net/Ethernet和LA现代PCI真实TAP已在双侧两种RAM验证，原BusyBox HTTP、
共享块/RNG/net IRQ及九类构造/reset失败回到基线。无metadata checksum的空索引目录误分类已由真实宿主/LA反例修复。AF_UNIX发送者缓存模型及sendfile datagram批次已按用户选择的Linux路线接入，
实际双侧等待/取消/关闭及资源验收通过。共用ns16550与LA TTY/termios2/作业控制、PTY、原BusyBox交互/script和录制重启已双侧两种RAM验收，UART启动失败/fatal与owner通过；LA pipe实际持有16页与容量一致，双侧满环/wrap/关闭通过；LS7A RTC/环境已双侧两种RAM验证，派生QEMU的原Linux告警另有证据；完整ABI1366条已双架构匹配，BusyBox/libc-test229个共同ID已双架构通过（LA两种RAM）；原SQLite DELETE/WAL、多进程、静态/动态CLI与独立重启内容也已双侧两种RAM通过。
最终RV完整架构、userland/GNU/ABI/栈、统一驱动/TTY、SQLite/NBD全恢复与双盘隔离，以及LA架构/平台失败回收均通过，一次整体审查的页边界验收问题已集中修复并完成最终回归，运行产物清理完成；不由设备通过声明所有应用等价。
glibc2.42五形态在Linux/BoarOS两种RAM均通过退出及根owner门禁。真实PCI RNG已接入并
完成正常/缺失/延迟/在途停止、构造OOM/IRQ失败及回收；启动种子不会提前发布random
ready。本轮指定原程序及单核平台矩阵已验证，整体审查与集中修复完成，最终清理完成。
共用VirtIO transport/split queue已迁入block，三个transport的真实块与LA根owner
门禁、RV可睡眠I/O四组合已通过。RNG/net迁移及LA PCI、UART/RTC验收完成；
本轮单核QEMU指定矩阵已完成；GNU版本差异、客体原生开发、完整Harness及范围外能力继续单列，不声明所有RV应用在LA验收。

- [x] ELF 段对齐、BSS 尾页、auxv、用户栈、stat/signal 结构及 clone 寄存器逐项核对；不能只换汇编入口却保留 RV ABI 编码。
- [x] 同一用户源码分别编译 RV/LA ELF，每架构内部用同一 ELF 对照 Linux 与 BoarOS；共享测试语义，隔离寄存器/页表差异，不拿 RV ELF 验证 LA。
- [x] LA `PROT_EXEC` 的数据读取已按冷/驻留状态独立核对：LA PTE 使用固定 Linux
  的非 NONE 可读权限，请求 VMA 保持原值；双侧验证冷页 fault、驻留读取、取指
  物化、fork、uaccess 与改权。资料见[LA学习记录](learning/loongarch-bringup.md)。

### L3 扩大真实用户空间

2026-10-06 已批准 PCI→VirtIO块→ext4根盘→静态musl 阶段。共用块队列/owner/
超时/reset 核心共用，RV MMIO 与 LA PCI 独立负责 transport/IRQ。该阶段已通过两种RAM的
PCI共享INTx、ext4读写/只读、原BusyBox七个applet及musl组合ABI的固定Linux对照，
故障/OOM/页堆与BAR claim回收、真实写I/O失败owner保留，以及RV存储/SQLite回归。
通用statx新增22条，当前RV ABI矩阵1366条匹配；完整比赛用户环境仍阻塞。

- [x] 本轮指定静态 musl→动态 musl/DSO/TLS→fork/COW/信号→共享映射→glibc→真实应用矩阵，逐层保留错误与资源回收结果。
- [x] 本轮RV/LA同口径功能矩阵已验收；后续继续记录缺能力/阻塞，不让新平台回退到固定输出或修改过的用户程序。

### L4 两块实板

- [ ] 每块板分别验证固件交接/DTB→RAM 保留区→timer→块设备→根盘读写→shell→IRQ→网络→SMP；QEMU、VF2、2K1000LA 结果分列。
- [ ] 记录 DMA 地址/缓存一致性、设备限速/超时、烧录校验、固件/DTB/镜像哈希与可回退启动配置；先确认镜像和网段，不把环境错误先归因 ABI。
- [ ] 每板最小交付是可重复启动和真实根文件系统读写，之后单独标记网络、多核、复杂应用及性能；未拿到设备或固件输入时明确阻塞。

## P7：多核真实编译与性能

**依赖**：P5/P6；实板性能另需 L4。代码优化落到被测模块，先保留基准和成本解释。

### P7a 可比的负载与测量

- [ ] 固定源码、工具链、镜像副本初态、核数、内存、日志、计时起止、冷/热缓存；明确数据写到 RAM、介质缓存还是已经同步。
- [ ] 真实 C/Rust `-j1/-j2/-j4/-j8` 全部构建成功且产物运行正确，再报告多次中位数/范围和失败。Linux/BoarOS 保持相同口径，不跨 ISA/机器/持久性模型直接比快慢。
- [ ] 观察加速曲线中的串行部分和竞争，不要求所有负载核数翻倍性能翻倍；QEMU 的功能证据与板上吞吐分开。

### P7b 测量驱动的优化候选

候选统一维护在上方“按证据触发的性能候选”，此处不另列执行顺序。
多核扩展仍依赖P6正确性，不用增加hart掩盖已确认的单核浪费。

## P8：系统交付与现场复现

### P8a 离线交付包

- [ ] 新目录或新机器按固定输入离线构建，校验产物/镜像身份，不依赖开发目录残留；保留依赖许可证与版本。
- [ ] 提供内核 commit、编译器/依赖、磁盘/DTB/固件哈希、平台启动命令、已支持/未支持范围、失败复现、回归日志与恢复/回退方式。
- [ ] 验证目录 rootfs 重新制盘、整镜像校验、串口抓日志、双网卡网段/路由和最小应用探针。真实设备操作遵循人的发布/烧录授权。

### P8b 组合与现场演练

- [x] main 提供只读 PID 1 path/argv/envp 构建配置，默认 `/init`，独立生成依赖；真实用户栈与交替重建见[根启动](modules/riscv-root-boot.md#pid-1-构建配置)。比赛配置留在评测分支。

- [ ] 从未知组合脚本/服务逐层定位设备→根盘→用户态→应用的首个失败，形成最小复现修通用机制；不能识别脚本名称输出答案。
- [ ] 默认引导、交互模式和评测 runner 分开，由同一内核能力支撑；保留一个已验证可回退版本。
- [ ] 数据库恢复、网络服务、多核编译、两个 QEMU 架构和每块板分别给验收结果，不能合成“全面支持 Linux”。他人能据列出的输入和命令复现才收口。

## 实施前需要确认的路线

以下是可行候选与调查方向，不是批准记录。实施任务时继续补全固定资料、接口、失败路径与 owner 的证据，再由维护者确认。明显违反契约的“fsync 空返回”“只共享已有 PTE”“用本地关中断代替跨核同步”不列作候选。

| 取舍 / 当前证据 | 可行候选与正确性、演进、复杂度、成本 |
|---|---|
| P1b VFS：原 fs context 借用单根 mount、cwd 字符串；OFD 已独立 | 已选择并完成②：最小 mount+路径对象/后端操作。路径持有 mount、inode 与父链引用，ext4 字符节点按 `rdev` 分派；共享可改名目录项、普通/NOREPLACE rename、linkat、tmpfs 与独立第二盘已完成；新后端仍需独立设计。 |
| P4a 共享匿名 | 已选择并交付②：专用对象按索引惰性发布页，保持稀疏分配并提供稳定身份；急切分配会改变 lazy/OOM 成本。后续已迁移到统一 memory_object，匿名跨 MM futex 与共享文件页已交付；共享文件 futex 仍待设计。 |
| P3b 持久化 | 用户已选择并交付逐 inode dirty/error、定向写回和真实 flush；共享事务可提交关联元数据，不主动全量写回无关文件。 |
| P3d 恢复 | journal/replay 与持久 orphan 已启用并验收；故障模型、限制和复现命令见 VFS 模块。 |
| P6 SMP：当前 SIE 串行化，缺远端 TLB 确认 | ① 进程态对象先用粗粒度可睡眠锁、IRQ/队列另设短锁，验证较少但并行有限；② MM/OFD/cache/队列对象锁直接演进，锁顺序/取消成本更高。先盘点消费者和睡眠边界再选，临时启动大锁有退出条件。 |
| 单 hart ASID：当前 ASID 0，换根前后各一次全局 fence | 已调查并推迟到实板：固定 QEMU 无 ASID 化 TLB（satp 变化与任意 sfence.vma 均全刷、翻译不使用 ASID），收益不可观测，且收窄 fence 的正确性缺陷会被 QEMU 全刷掩盖；在 ASID 标签 TLB 的硬件（L4）上实现并验证 ASIDLEN 与复用顺序。 |
| P6g 栈 guard：连续物理栈、canary/高水位 | 已选择①并交付：独立内核栈窗口（12 KiB 槽＝4 KiB 未映射 guard＋8 KiB 栈），运行期插/删叶、空表释放；见已交付表。direct-map 别名与 idle/boot 栈的边界见[调度模块](modules/kernel-scheduler.md)。 |
| N1 协议栈与分配 owner | 已确认 BoarOS 持有 fd/OFD、ABI、等待与缓冲队列，固定官方 lwIP 2.2.1 raw API/NO_SYS；协议和 pbuf 静态有界池，socket/OFD/请求由 kernel_heap 持有。IPv4/IPv6双栈loopback和socketpair已验收；QEMU VirtIO-net与静态IPv4宿主应用已交付，实板另核对；命名AF_UNIX按需设计。 |
| N4 网络执行资格与预算 | 已确认并实现短 syscall + 统一后台服务；raw 调用持单 hart 执行资格、不睡眠、不重入，批次之间开放中断并调度。poll 不推进协议。SMP 的每 hart 状态和锁仍另设计，不把当前资格当作多核同步。 |

单 hart 已交付 OTHER/FIFO/RR 与全局实时带宽；后续调度改动以公平性/负载和实际消费者证据比较策略。第二架构按连续小里程碑推进；不等待 RV “全部完成”，也不复制整套通用内核。

## 每个任务的统一验收与现有命令

| 维度 | 提交前应有的证据 |
|---|---|
| 触发与契约 | 独立最小输入、声明支持范围、固定参考路径与版本；正常返回、errno、部分成功与 fault 可观察 |
| 失败前后 | 同输入的真实失败与修复结果，Linux 参考有效；未改上游答案、未宽泛归一化错误 |
| 所有权与安全 | 对象由谁持有、睡眠期间谁 pin、发布/失败/退出谁释放；真实 I/O 错误 owner 与分配器 fatal 不变量分开 |
| 并发 | 单 hart 临界区与跨 hart 同步分列；等待登记、可见性、发布/回收顺序和竞争探针 |
| 资源与错误 | OOM/超时/真实 I/O 返回所属层状态；合法释放完成即返回，非法释放/引用损坏 fatal，不能建立无 owner 重试链 |
| 性能 | 记录成本边界；无数据时称候选瓶颈。风格建议不混入 ABI/安全/并发结论 |
| 验证与文档 | 最窄→真实 U-mode→相关架构/全量；已知缺能力写阻塞。检查 README、modules、learning；TODO 更新具体完成条目 |

现有入口如下，子任务按依赖选择；上文拟新增文件不代表已经有同名 Make 目标：

```sh
make test-riscv
make test-userland-riscv
make test-diff-abi-riscv
make test-lwext4-host
make test-stack-usage
make test-program-inventory-host test-diff-abi-host
make inventory-userland-riscv
```

`inventory-userland-riscv` 默认成功只说明清单生成成功。当前229项共同清单已在RV和LA两种RAM完整通过，历史228项失败保留原记录；严格模式失败不是自动产生的新回归；`--case` 与 `--require-pass` 只严格判定本次选择集合，未选项目保留历史结果或 `not-run`，选择集合写入状态供恢复报告解释。完整Harness与更广程序、客体原生开发环境仍需另行验收，按实际缺能力保留阻塞原因；
本轮动态musl/DSO TLS、完整FPU/LSX/LASX状态、线程、终端/网络/RTC及指定程序矩阵已经独立验收。

## 范围与交付边界

暂不进主线：NUMA、swap、透明大页、完整 namespace/cgroup、seccomp/eBPF/ftrace、Linux 内核模块 ABI、复杂可加载框架和 io_uring。它们可另立任务，当前失败无需先完成这些能力。不因未来可能有用引入第三方框架或 Agent 编排平台。

`final-2025` 沿用只是规划假设；比赛事实只据本地固定规则/Harness，不能预测未来赛题。许可证和正式发布策略仍待决定。正常开发沿当前分支；提交围绕可说明/可验证的问题，AI 贡献按 CONTRIBUTING 添加 trailer；不提交会话材料、JSONL、秘密或运行镜像，发布权限遵循 [AGENTS.md](../AGENTS.md)。
