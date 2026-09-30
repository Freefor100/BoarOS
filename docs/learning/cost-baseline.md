# 成本基线与归因

## C1 写入（2026-09-30）

这是 C1 的三个独立启动副本；观测开/关均使用同一 ELF，串行运行，modern/writeback。每组同步前先 fsync，映射写先缺页并清脏，准备不在窗口内。时间为客体单调时间 min/median/max；QEMU 墙钟没有及格线。

| 窗口 | staging/整包 heap bytes | usercopy bytes | snapshot bytes | device requests | 观测开 ns（min/median/max） | 开/关中位比 |
|---|---:|---:|---:|---:|---|---:|
| file-hot-1 | 524288 | 128 | 0 | 860 | 1610768400/1654373800/1672399300 | 1.160 |
| file-hot-4096 | 524288 | 524288 | 0 | 1408 | 2011865300/2143235300/2248346800 | 1.137 |
| file-cold-4096 | 524288 | 524288 | 0 | 800 | 2399803600/2413680800/2568807400 | 1.059 |
| file-1m-aligned | 4096 | 1048576 | 0 | 11 | 17842600/18062600/18682400 | 1.111 |
| file-1m-misaligned | 4096 | 1048576 | 0 | 11 | 18417400/19489200/28264700 | 1.099 |
| tcp-1m-aligned | 200704 | 1242836 | 0 | 0 | 36193700/38914900/41432900 | 1.151 |
| dgram-64k | 65576 | 65536 | 0 | 0 | 1163300/1164400/1222500 | 0.985 |
| file-sync-0-every-1 | 524288 | 524288 | 524288 | 1675 | 2929888500/3159719900/3479765500 | 1.000 |
| file-sync-0-every-16 | 524288 | 524288 | 32768 | 1176 | 2272709700/2319037900/2515045300 | 1.166 |
| file-sync-0-every-128 | 524288 | 524288 | 4096 | 1155 | 2238107300/2460893600/2537608900 | 1.213 |
| file-sync-2-every-1 | 0 | 0 | 524288 | 384 | 1149507400/1169509200/1179363200 | 1.026 |
| file-extend-sync | 0 | 0 | 524288 | 2728 | 5173509900/5429364700/5461939700 | 1.000 |

1 字节热写的 128 次调用分配 128 个临时 staging 页，累计容量 524288 字节，接受 128 字节；累计容量不是同时驻留峰值。单次 1 MiB 文件写仍只用一个 staging 页，aligned/misaligned 的独立解析计数分别为 256/512，内容、offset、大小一致。TCP 允许短写，调用者续写完成 1 MiB；重试会复制已 staging 但未接受的尾部，表中复制量按实际调用核对。DGRAM 的容量栏为整包 heap owner 的实际请求字节（含 packet 元数据），64 KiB 接收完整，65537 字节返回 EMSGSIZE。

每次/每16次/结束时 fsync 的快照复制分别为 524288/32768/4096 字节。它反映同一热页反复脏化和快照的次数；设备事务/flush 也不同，不能单凭它推断整体吞吐收益。所有设备读写字节暂归 unknown；backend_accepted 仅表示能证明的逻辑文件数据，事务内的真实扇区仍可能是 data/metadata/journal，未按扇区号分类。

36 窗口覆盖 cold/hot 七尺寸各128次，文件/TCP两种对齐1MiB、DGRAM上限与拒绝、4KiB同步九组合、O_SYNC/O_DSYNC、随机、扩展接受/最终同步及EFAULT前缀/首字节。开/关都检查实际内容、offset/size、errno、退出状态与最终 heap-live=0。观测版 scale 的独立 linker wrapper、原 MM 页解析统计及原设备统计再次核对复制和请求；原 scale 门槛仍通过。OOM/短写/EIO/取消的组合边界由既有 partial-write/scale/io-sleep 回归保护，四组合压力观测在 C2 接入。

固定输入：

- on: kernel `8e6d5479b10b9aa28cc6912dd2de234ec5603327ffcd61407a942e31ac396476`；ELF `c017c3f88956bd816b398396899b5ab34fbcb90bc85853a68bd119d0cacf1da6`；source tree `1d614473ab8a4f6b908f93b4256aec0eee93dd3b`；source 内容 `cd22630c4e67b532405d6533bd5f1cd6f73756a3d911e0162dc0523bb4bd886a`。
- off: kernel `0569288a81768a824eba4e88e30706c0f398457c5f6cfac603b9790855eb249a`；ELF `c017c3f88956bd816b398396899b5ab34fbcb90bc85853a68bd119d0cacf1da6`；source tree `1d614473ab8a4f6b908f93b4256aec0eee93dd3b`；source 内容 `f3953866040d46d0cdfe64f4ec09e1ef67da9080684cfcd7ae3ada90ce046db0`。
- QEMU emulator version 11.1.1，binary SHA-256 `a1cfcceb6c688f9b0a290d512211ed08cf465b92b26a04cfb032280a53625718`；timebase 10000000 Hz。

可重建：`make COST_DIAGNOSTICS=1 all`；`python3 -B tests/cost-riscv.py --case write`；`make all`；`python3 -B tests/cost-riscv.py --case write --off --kernel kernel-rv`，按此顺序串行。`python3 -B tests/cost-summary.py <on/results.json> <off/results.json>` 验证三个独立副本和完整 schema，并生成全部非零指标的 min/median/max。执行器互斥拒绝并行成本运行。原始可重建镜像/日志在核对后清理，本文件保留结论与命令。

本阶段尚不选择优化；C2–C6 继续分解锁、扫描、IRQ-off 与真实消费者，再比较候选。

## C2 锁等待（2026-09-30）

单盘与双盘各三个独立启动，每启动13个窗口，所有任务先握手确认 blocked；
四种 legacy/modern × writeback/writethrough 的低内存 fixture 各三次启动通过，
每次包含 pressure-io 和 timeout-cancel 两个完成窗口，既有 owner/页/堆基线保持。
32个等待者的计数在三个副本一致（双盘 backend rank30 的7/9/9除外）：

| 对照 | rank10 阻塞 | rank15 阻塞 | rank15 重阻塞 | rank30 阻塞（单盘） |
|---|---:|---:|---:|---:|
| 同 OFD | 528 | 0 | 0 | 0 |
| 独立 OFD，同 inode | 0 | 528 | 496 | 0 |
| 不同 inode | 0 | 256 | 225 | 8 |

这是该握手负载下 wake_all 的可重复重阻塞成本；不能由此推断一般吞吐、无饥饿保证或双盘收益。
同OFD的offset串行挡在rank15之前；独立OFD可见inode整次写门闩；不同inode仍有各自门闩及
共享后端的等待。向量追加、重叠定位写、同步尾部、截断和取消的内容、offset/大小、状态均通过。
热路径仅增加默认关闭的观测，不改变门闩或唤醒策略。

固定 kernel SHA-256：`5e9d2fb7c098c5795b98f0ae1fc8f5d0b4ed4550e2d721d5d3efff54a3ef6673`。
可重建：`make COST_DIAGNOSTICS=1 all`；串行执行
`python3 -B tests/cost-riscv.py --case locking` 和同命令追加 `--two-disks`；
`python3 -B tests/io-sleep-riscv.py --cost-output <result.json> --transport modern --cache writeback`，
对四配置分别重复三次。报告由 `tests/cost-summary.py` 检查完整三副本和 schema。

## C3 外围扫描（2026-09-30）

三个启动各10窗口，目标中间页先降权再恢复。16/64/256无关VMA下（无文件驻留）
两次改权的 query比较为70/96/128，merge比较与recount访问均为44/140/524，
PTE访问恒为6；有16/64MiB文件驻留页时 resident访问为8192/32768，PTE仍为6。
这些计数在三副本相同；总成本包含外围数组移动、合并和驻留扫描，不能由PTE局部常数推断整体常数。
权限拒绝/洞失败窗口未进入PTE修改，内容可继续写；scale在16/64MiB真实映射中
用原MM统计核对PTE/TLB并以页数独立核对resident扫描，OOM/fork/split契约沿用原回归。
可重建：`make test-cost-riscv COST_CASE=mprotect`；`make COST_DIAGNOSTICS=1 test-scale-riscv`；
`make test-vma-riscv test-scale-riscv test-diff-abi-riscv`。范围索引仍留待全部测量后选择。

## C4 deadline（2026-09-30）

三个启动各8窗口，正常窗口每次严格4个timeout，提前唤醒/TERM/KILL窗口为0，
最后重新建立同期限4任务检查复用。期限在所有fork/blocked握手之后通过共享页发布，
准备时间不会耗掉期限。每轮deadline访问保留总数/样本数/最大值，窗口包括最终排空，
平均值会受排空过程影响；固定集合的最大值及重复分布另随最终C6报告保存。
源码 `validate_queue_shape` 是头尾检查，诊断分别记录实际shape/thread检查次数，
不把O(1)校验伪称全队列扫描。timer耗时止于调度切换前，deadline_to_run包括到期IRQ延迟和就绪等待。
QEMU timebase为10000000Hz，到期偏差不是实板/硬实时保证。
可重建：`make test-cost-riscv COST_CASE=deadline`；
`make test-scheduler-cases-riscv test-io-sleep-riscv test-sched-policy-host test-sched-bandwidth-riscv`。

固定blocked集合每轮最大访问数（三副本）：0→7,7,7；32→39,39,39；128→135,135,135；256→263,263,263。包含正常系统worker和控制线程，未按任务名称过滤。
