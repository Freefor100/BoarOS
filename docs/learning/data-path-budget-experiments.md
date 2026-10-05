# 数据路径预算实验的输入与验收

## 已交付工具与当前证据（2026-10-06）

本页区分可运行候选、功能烟测、诊断与正式性能。当前框架及以下功能验收已完成，
正式匹配性能结果仍待后续记录；没有据此修改生产默认，也不承诺吞吐倍数。

TCP 候选为 `w8/16/32-p1/2/4-m1/2/4` 共 27 组：窗口与发送缓冲为 8/16/32 MSS，
segment/pbuf 池及协议堆各自为 1/2/4 倍。PCB 数不变，32 MSS=46720 B，不启用窗口扩大。
存储为 `ra0/1/2/4/8-wb1/2/4/8` 共 20 组，预读/连续写回生产默认 0/1。配置校验
拒绝其他数值；候选由独立 BUILD_DIR 构建，不覆盖根 kernel-rv。

`tests/network-budget-experiment.py`、`tests/io-budget-experiment.py` 与共享
`tests/budget_identity.py` 把 family/profile、实际预处理宏、编译命令、编译器摘要及
源码身份绑定进 ELF 的非 ALLOC section，再记录最终 kernel SHA-256。运行同时核对
相邻 identity.json、ELF 内身份和真实文件摘要；canonical 标签必须匹配实际 profile，
`current` 只作显示别名。参数候选须来自同一保守源码集合（含未跟踪、非忽略输入），
整个 build 批次期间不得变动。运行时树身份与构建身份分开；固定旧基线和固定 Linux
明确作为例外，不猜测任意内核的配置。烟测可显式使用 unmanaged 普通标签，但不能
升格成正式性能；formal run 必须使用 managed 候选。

原基线为 `5687377c58dc96adfa1f72f1636a319bbd25b419`，本地复用缓存
`build/data-path-baselines/5687377/kernel-rv`，固定 SHA-256
`b962c890989d268282b55d49a4fa9b51afbaf7008dab2fc47e717388a6fea2f4`。
两工具支持 `--baseline` 指定同一摘要的文件，缺失或不匹配直接失败；不拿新构建冒充
该历史输入。可重建工作负载和内核源码均在 Git，缓存内核只是跨实验复用输入。

正式 release 至少三次独立启动，按 case/重复/variant 交错。每个 case 共用相同
ELF、初始镜像、QEMU、RAM、设备配置与缓存准备；每次启动复制磁盘。原基线自动加入，
Linux 只可通过 `--linux-kernel` 的固定源码/配置身份核验加入，保留 linux 标签。
TCP 与 I/O 的正式/诊断运行都取得 `build/cost/measurement.lock`；冲突先于构建或
启动客体被拒绝。该锁只约束合作工具，测量期间仍须停止其他编译/QEMU负载。
失败组标 incomplete，成功子集可保留诊断数据，但只有 status=measured 能用于比较。

## 负载与统计口径

网络 `all` 是 60 个明确 case：loopback/TAP、blocking/nonblocking、bulk/RR/mixed、
单/五/近池连接及方向。mixed 加一条独立控制连接，近池为 loopback 13 bulk+1 control、
TAP 27 bulk+1 control，留四个 active PCB 槽；这是 fixture 配额，不是协议优先级预留。
接收侧逐字节核对并确认全部连接完成，保存每连接起止时间和有效字节。准备阶段测
16 次 echo RTT；Jain 指标只描述有限完成速率，不作为长期无饥饿证明。
控制 tail 的主集合按 RR **发起时 bulk 尚活跃**选择，跨 bulk 结束的慢回复仍计入；
全部样本和 fully-contained 辅助集合另存，保留样本数，不按完成时点筛掉最慢请求。

TAP 可选 `--tap-delay-ms 0/1/10`，只在隔离 namespace 的 host→guest egress 设置
netem，另报告实际 RTT。本宿主 1/10 ms 均返回 `Specified qdisc kind is unknown`，
在 QEMU 启动前明确记 unsupported。没有加载宿主模块或用另一种 relay 混作同一测量后端。
固定 ABI Linux 未启用 NETDEVICES，TAP 配置实测 ENODEV 后，另用既有
`tests/network-linux.config` 从固定 `references/linux@f4cdf7ca9a1fdcca413157df19753f388a5a224e`
构建网卡参考 Image（SHA-256 `7fdeac1a0821f821e02c15c35cd2c4003b66268a65b8ef591520094c096f442e`），
同 ELF loopback/TAP 功能已通过；原 ABI 参考保留。

存储 case 为 `backend:operation:completion:files:size:request`：ext4/tmpfs，
append/overwrite/cold-read/hot-read，1/4 个文件，每文件 1/4/16/64 MiB，请求 1/4/64 KiB。
overwrite 使用镜像已有文件且不 truncate；cold-read 是新 guest 首次读取预置文件，
hot-read 先完整验证读取一次。tmpfs read 是已建立的内存后备，拒绝 cold-disk 标签。
多文件由单任务逐请求轮转，不冒充并发调度公平性。cache 窗口没有显式 sync，fsync/
fdatasync 窗口在全部数据请求后每文件调用一次。setup、最后读回、cache 模式收尾同步
与结果文件持久化在主窗口外；模式生成、读校验及五个文件大小检查在主窗口内。
新 guest 缓存不表示冷宿主存储；tmpfs 同步成功不表示持久介质保证。

结果在传输/成本窗口结束后写客体文件并 fsync，退出后复制磁盘，只在读出副本重放
journal 再提取，保存原盘/读出盘/结果摘要。这样不依赖可能双向交错的串口记录。
COST 诊断另保存完整成本快照、32 位协议计数和协议池高水位；MEM_AFTER 仅为时点。
managed/heap peak 从初始化累计到查询前，排除未受管的内核映像/固件/MMIO，heap
为子集不可相加；不宣称 workload 独占峰值。release 与诊断扰动单独比较。

TX 诊断补充软件收割 DONE→FREE 和同槽 FREE→下一次 publish 的 count/sum/max，
单位由 `tx-clock-hz` 转换。范围是设备生命期，含准备和收口；后者也含正常空闲，
不能单独作为进展缺陷证据。复制 TX 的完成直接归还计零，首次使用和 reset-only
撤销不伪造完成时间。受控时钟测试先在缺记录实现上失败，补齐后两传输及 COST
开/关的实际驱动模型、worker 进展模型和结果解析测试通过。

## 已完成的功能验证

27 组 TCP 宏及非法覆盖、宿主工作负载与结果拒绝测试通过。框架审查先复现了错标签/
同 hash 假参数汇总、linux 标签绕过、完成筛选丢慢 RR、缺测量锁等问题，再加入身份、
边界长 RR 和锁冲突门禁。宿主测试为 network 10 组、I/O 6 组；它们不是性能结果。
实际 managed 构建还纠正了裸机 GCC 预处理缺 freestanding/kernel include 的问题。

此前同 RV64 ELF 网络近池 BoarOS/Linux 四启动、两次诊断通过；I/O 单 MiB 的
BoarOS/Linux 18 次 release 功能启动和两次诊断通过。新增 managed 身份与内存峰值后，
又验证两个 TCP mixed（loopback/TAP）及两个 RA8/WB8（追加 fsync/冷读）诊断启动，
均读出 peak>=current 和合法 COST 快照。所有记录为 functional-smoke-not-performance，
不生成性能分布，也没有把参数列表中的 64 MiB 全矩阵称为已运行。

最终 COST 契约复核曾捕获 `observer.journal_wait...` 一行被关机 network final
诊断插入，原始记录保留为失败。原因是 `fflush` 后 UART 队列尚未排空；沿已有
metadata workload 的边界，在 contract 正常退出前显式 `tcdrain`。该操作位于
全部观测窗口之后；不修改 COST 状态机，也不让解析器跳过坏行。修复后三次独立
启动、每次四个诊断窗口通过，原失败记录不计为成功副本。

```sh
python3 -B tests/network-budget-config.py
python3 -B tests/network-budget-selftest.py
python3 -B tests/io-budget-selftest.py
python3 -B tests/network-budget-experiment.py build --profiles all --jobs 4
python3 -B tests/io-budget-experiment.py build --profiles all --jobs 4
python3 -B tests/network-budget-experiment.py run --variant current=build/network-budget/kernels/w8-p1-m1/kernel-rv --cases loopback:blocking:mixed:5:tx,tap:nonblocking:mixed:5:rx --repeat 3
python3 -B tests/io-budget-experiment.py run --variant current=build/io-budget/kernels/ra0-wb1/kernel-rv --cases ext4:append:fsync:1:16M:64K,ext4:cold-read:cache:4:4M:4K --repeat 3 --timeout 600
```

`make prune-build` 保留身份绑定候选的 kernels/ 编译缓存及固定基线，清理 runs、镜像、
日志与汇总；核实后的永久结论和可重建命令在本页，不把被忽略的输出当作永久档案。


## 最终正确性复核

生产源码冻结于 `faf8e6395a7d56cd92e5fdb460fd9532066d2f26`。默认 `test-riscv`、
musl、glibc 2.44 五种 ELF 形态、1,344 条固定 Linux ABI 差分、epoll 同 ELF、
实际网络 owner ASan、VirtIO/worker/重组/块层/分配器抢占与 COST 宿主门禁通过。
I/O 暂扣四组合通过；双盘收口竞态修复后四组故障/重启、四组 FIFO/RR 和额外
30 次触发配置故障/重启通过。上述两个测试收口修复与原失败见各模块 learning。

1/4/16/64 MiB × 1/4/64 KiB 的 COST 规模门禁通过；64 MiB 对齐追加增长访问为零，
1 KiB 追加仅处理 49,152 次实际尾页。16,384 页缓存中的小范围写回只访问一个候选。
默认及 RA8/WB8、TCP32MSS/池4倍/堆4倍的编译栈门禁通过，最大单帧 3,152 B；
这不是整个调用链的栈上界，动态栈仍由各真实客体检查。

lwext4 完整恢复与阶段七后默认/RA8WB8 的 SQLite DELETE/WAL 完整矩阵均通过，
精确切点、实际故障命中和未到达项见[最终恢复](record-lock-sqlite-recovery.md#数据路径最终恢复与宿主收口2026-10-06)。
完整比赛 Harness 仍阻塞于缺少 `kernel-la`：固定
`references/oscomp-autotest@d1bb3a3c4b27274e196a2648518525c1a304e339/kernel/run.py`
要求启动 LoongArch 内核，当前 `make -n kernel-la` 无目标；上述 RV64 验收不算完整比赛通过。
