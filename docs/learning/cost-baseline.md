# 成本基线与归因

## C0–C6 成本测量已验收（2026-10-01）

本轮沿 `main@c9b6ca6` 实施默认关闭的聚合观测，没有修改 deadline、唤醒、驻留索引或持久化机制。
最终矩阵包含 **20 配置、60 次串行独立启动、339 个完成的观测窗口**：七种 case 的开/关各三次启动、
双盘锁负载三次、固定 Linux 原消费者三次，以及四种 transport/cache 的 I/O 压力与取消各三次。
“完成矩阵”仅表示预设运行列表已执行并保存真实状态，不表示原消费者全部通过，也不是比赛评分。
该历史矩阵的消费者曾有启动拒绝及预算截断；本次另补兼容配置开/关与固定Linux各三启动、48窗口，取得原旧glibc实际I/O并确认可用负载自然完成。续测与原judge成绩见本文末节，保留双侧原向量版本排除及完整评测预算/kernel-la限制。
下文 C1–C5 独立阶段的旧内核/时间表保留为历史证据；本节及归档是最终统一源码的结果。

[完整测量归档](cost-measurements.json)保留每次启动的冻结身份、全部指标、直方图及消费者原输出。
零指标压缩后重建完整 schema 并核对 SHA-256 seal，三个副本、epoch、完整阶段集合、
缺项、溢出、incomplete 和不可能的直方图都严格验证。`tests/cost-summary.py` 输出每项
min/median/max、每副本样本数/最大值、p50/p95/p99 桶区间和运行时间的未覆盖余量。
报告解码在测量结束后增加“所选测试不可用”的检错；没有改动冻结的 kernel、ELF、输入或计数。

### 固定输入与测量边界

- 生产者 commit `75ca4bd0f48bb820d925e34df66dd3c175cfce63`，tree `d4f785d8896474ebca3b6733a13f874e30e26d53`，完整源码内容 SHA-256 `b6136ca9f1313f0650a599568b3f826633fe919997ca8ef3edd918ae6dd77870`。归档逐启动保存来源；后续报告解码与文档不冒充测量时源码。
- 观测内核 SHA-256 `d9e9496039cb8e84be97737be8982b5d5541b357286b201e4c827184083834cc`；I/O fixture 内核 `d135119f3eb4c15304e60c1e9aa8cd97474fb1fa15963e6ae191ab08b9db5c19`，其窗口明确为 `mode=fixture`。
- Linux 参考 `references/linux@f4cdf7ca9a1fdcca413157df19753f388a5a224e`，Image SHA-256 `01d60a8ae733f56aa94cf11b4805da1fe876cac09d2ef81e7e7397568ee1f668`。
- QEMU 参考 `references/qemu` v11.1.0、commit `84f07211cc5b4fc6a371559bf8a5de4fb068e648`；实际 emulator 11.1.1，binary SHA-256 `a1cfcceb6c688f9b0a290d512211ed08cf465b92b26a04cfb032280a53625718`。
- 实际 firmware `/usr/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin` SHA-256 `894e2aef99590fc07ec6c60ab00282b8bc5d5d5bb2a1d0c6ada0c52df24274c0`；kernel/ELF/fixture/DTB/firmware/工具和完整 preboot manifest 的哈希均在归档。DTB timebase 10 MHz，tick 分辨率 100 ns。
- 聚合及声明的桥接预留共 60947 字节；每任务诊断标量 48 字节，加原内嵌 backend guard 新增 16 字节，总计 64 字节。默认 `.text`（364544 字节，SHA-256 `fb3c83264024c6f7997fc79516da46efca4d1f86b7716ea1ab7ef1c0fc352c19`）与 `c9b6ca6` 逐字节一致，`nm` 无 cost 符号；不要求含 DWARF 的完整 ELF 哈希相同。

### 实际成本、重复分布与观测扰动

| 受控窗口 | 最终观测开时间 ns（min/median/max） | 观测关时间 ns（min/median/max） | 开/关中位比 |
|---|---|---|---:|
| 128 次 1 B 热文件写 | 1757103300 / 1920461400 / 2002153600 | 1657609800 / 1807153100 / 1956258600 | 1.063 |
| 128 次 4 KiB 热文件写 | 2598556700 / 2767236700 / 3079399000 | 2506428900 / 2566341000 / 2718094000 | 1.078 |
| 单次对齐 1 MiB 文件写 | 19936700 / 26913200 / 38310300 | 16857800 / 35808300 / 40493800 | 0.752 |
| 单页改权，256 VMA、64 MiB 驻留 | 15329600 / 15915800 / 21527800 | 358700 / 389100 / 395100 | 40.904 |
| 热缓存 1 MiB 复制窗口 | 59222000 / 60122100 / 62164900 | 32579700 / 32940100 / 33082400 | 1.825 |
| 1 MiB 改权窗口 | 1873800 / 1884300 / 2303800 | 668800 / 675500 / 713400 | 2.789 |

latency-copy 窗口包含八次该尺寸的热 tmpfs pwrite，latency-protect 与单页扫描窗口均包含降权和恢复两次 mprotect；表中是整窗口时间，不是单次调用时间。
关闭观测的结果、内容、offset/大小、errno、wait status 与资源基线由同一负载检查。
0.752 是三启动测量离散，不能解释成观测带来性能收益；三次样本不足以做置信区间。
逐 resident 节点计数/计时带来约 40.9 倍扰动，因此观测时间不能作为生产改权延迟。
曲线同时显示默认构建时间与实际访问数；默认时间有非单调点，不能声称实测时间严格线性。

![最终三启动成本曲线](cost-curves.png)

- 128 次 1 B 热写接受 128 B，staging 累计请求 524288 B（4096 倍），不是同时驻留峰值；单次 1 MiB 文件写只申请一个 staging 页，对齐/错位解析次数为 256/512，三个副本相同。
- 128 次 4 KiB 热写每次/每16次/结束时 fsync 的 snapshot 复制分别为 524288/32768/4096 B，设备请求为 1675/1176/1155，三个副本相同；未知来源的提交写字节为 2514944/1736704/1708032 B，不能据此选择事务合并。
- 32 等待者、独立 OFD 同 inode 的 rank15 阻塞/重阻塞为 528/496；不同 inode 为 256/225；同 OFD 先被 rank10 串行，rank15 重阻塞为 0。三副本一致，单/双盘负载分别保存，不据此宣称双盘吞吐收益。
- 单页两次改权、64 MiB 无关驻留数据会访问 32768 个 resident 项，而 PTE 访问恒为 6；无文件驻留的单变量对照中，16/64/256 个无关 VMA 的 merge/recount 访问为 44/140/524，三副本一致。独立原 MM 统计和实际页数检出漏计扫描。
- 固定四个期限任务，增加 0/32/128/256 个无期限 blocked 后，整轮最大访问为 7/39/135/263（三副本一致，前台/后台取最大，含实际系统任务）。期限到运行的分位只报告桶区间。
- 1 MiB 热复制的最大采样 IRQ-off 为 75694/80345/76792 ticks，即 7.5694/8.0345/7.6792 ms；独立唤醒到运行最大值为 145677/81627/72065 ticks。均为观测构建的采样段；U/S 入口 7/6 条、sret 尾 11 条、C enable 尾至少 9 条及编译器恢复、C disable 的 CSR 到首 rdtime 前缀仍是盲区，不构成硬件上界或硬实时保证。

运行余量定义为窗口跨度减去前台/后台 run_ticks 与 idle_ticks（切换时结算的互斥 hart 时间）；
全部 339 窗口均非负。1 B 热写三副本余量为 55197/57370/61852 ticks。
observer_ticks 仅更新体，已包含在运行时间内，未覆盖读时钟、入口和恢复的全部成本。
阻塞/就绪等待按任务累积，多个任务可重叠并超过窗口跨度；操作、锁、设备的嵌套耗时不能相加。
未覆盖余量包括边界和记账间隙，不能全归为某个 syscall 或磁盘。空契约窗口另保留，不能用于估计负载主成本。

### 历史 main 的原 iozone：180秒诊断与缺口

原输入为 `references/oscomp-autotest/sdcard-rv.img`，SHA-256
`f419468678d342133546add2f8459ea09aeba987ba968e28753d6ee656996b8b`。
原 iozone 3.506 的 musl/glibc ELF SHA-256 分别为
`019cd6e219263f41b3c1d62024ea9ce38e613541bf2a1d9b7e2aa6506187ecd6`、
`984c8ad474072011f38d52a98d422c581b2277d3e2d03f90557f45a8046c66de`。
原脚本、loader、libc 哈希也在归档；每启动复制原盘，安装原解释器路径和逐 libc 的库路径，未修改 ELF 或 uname。
在 **main 的 QEMU RV64 根盘**逐命令运行 `/musl`、`/glibc` 的原八组参数，同时在固定 Linux 上运行同一输入。
这没有执行 `oscomp-rv-compat` 的整套启动脚本或原 judge，不能叫本轮评测/评分通过。

开启/关闭观测是 COST_DIAGNOSTICS=1/0 的两个内核构建，程序 ELF 相同；下表的完成数是具体命令完成所选测试，
不是采样工具完成数。窗口 state=complete 只表示采样正常结束且数据可校验，窗口内的命令仍可能失败或被超时终止。

每组参数为 `./iozone -a -r 1k -s 4m`，以及
`./iozone -t 4 -i A -i B -r 1k -s 1m`，其中 (A,B) 顺序为
(0,1)、(0,2)、(0,3)、(0,5)、(6,7)、(9,10)、(11,12)。每命令诊断预算 180000 客体 ms；
续测执行器支持 `--consumer-timeout-ms`，默认保持180000；新预算同时封存到启动输入、fixture和逐命令记录，
单独保留续测证据。预算增加只能用于观察是否自然结束，不能替代退出状态和所选测试完成检查。
超时终止整个进程组并回收，真实 wait status 和原输出不丢弃。

| libc / 命令索引 | main 观测开（三启动） | main 观测关（三启动） | 固定 Linux（三启动） |
|---|---|---|---|
| musl 0：自动 | timeout / timeout / timeout | timeout / timeout / timeout | 所选测试完成 ×3 |
| musl 1：(0,1) | timeout ×3 | 所选测试完成 ×3 | 所选测试完成 ×3 |
| musl 2：(0,2) | timeout ×3 | timeout ×3 | 所选测试完成 ×3 |
| musl 3：(0,3) | 所选测试完成 ×3 | 所选测试完成 ×3 | 所选测试完成 ×3 |
| musl 4：(0,5) | timeout ×3 | 所选测试完成 ×3 | 所选测试完成 ×3 |
| musl 5：(6,7) | 所选测试完成 ×3 | 所选测试完成 ×3 | 所选测试完成 ×3 |
| musl 6：(9,10) | 所选测试完成 ×3 | 所选测试完成 ×3 | 所选测试完成 ×3 |
| musl 7：(11,12) | 所选测试不可用 ×3；进程一次退出0，两次timeout | 所选测试不可用 ×3；进程退出0 ×3 | 所选测试不可用 ×3；进程退出0 ×3 |
| 原 glibc 0–7 | 全部启动前拒绝 ×3 | 全部启动前拒绝 ×3 | 0–6 所选测试完成 ×3；7 所选测试不可用 ×3 |

原 glibc 输出 `FATAL: kernel too old`，wait status 32512（exit 127），未进入 I/O 测试。
main 报 `0.1.0-boaros-dev`；已核对 `oscomp-rv-compat@3dcfe77` 的
`kernel/syscall/dispatch.c` 报 `4.15.0`，该历史测量时尚未合入本轮全部 main 改动，也未重跑评测。
main uname 未改，不借版本字符串宣称 Linux 能力；兼容分支后来单向合入并取得实际 glibc I/O 和原judge结果，见本文末节。
默认门禁中的 glibc **2.44** 静态/动态/PIE 是另一固定输入，其通过不能替代原旧 glibc 的结果。

命令只有无 timeout、wait=0、有完成 marker、未报告所选测试不可用四条件同时满足才记“所选测试完成”。
main 每启动完成数为观测开 3/16、关 5/16，Linux 为 14/16；原脚本 exit 0 不是逐命令验收。
(11,12) 原 ELF 不支持所选测试，不能宣称原消费者验证了向量 I/O；真实 writev ABI 由受控负载及 R7 回归另行保护。
截断预算改变开/关的完成集合，终止后文件/缓存状态会影响后续命令，不能给这些消费者计算完整工作负载的开销比或优化收益。

实际完成的 musl (6,7) 窗口前台接受均为 4194304 B、4096 次文件写，staging 累计 16777216 B；
设备请求为 36731/36677/38289，unknown 提交写字节为 59052032/59037696/61233152 B，
逻辑接受量的约 14.08/14.08/14.60 倍。(9,10) 同样接受 4 MiB，设备请求为 61431/60249/61431，
unknown 提交写为 96206848/93786112/96206848 B，约 22.94/22.36/22.94 倍。
这证明请求放大存在；所有原始磁盘字节仍归 unknown（包括提交后失败的请求），不能称为最终持久字节、
也不能将放大直接归给 journal/metadata。受控同步扫证明 snapshot 频度会改变成本，但尚未解释消费者所有放大来源。
(6,7) 的前台任务累计 blocked 为 3982315812/3813156622/4451903321 ticks，
窗口跨度为 851133193/810227900/949076173 ticks；多任务等待重叠，不能相除当 CPU 占比。
其 hart 记账未覆盖余量为 3067475/2993392/3504041 ticks，约 0.36–0.37%；不能把这部分或 unknown 字节强行归因。

### 可供下一轮确认的三个候选

| 候选及证据 | 正确性/演进约束 | 实施复杂度与成本 |
|---|---|---|
| resident 范围索引：32768 无关项访问，目标 PTE 仅6次 | 必须保持 fault 来源 pin、split/fork/unmap 和 OOM prepare/commit 原子性；额外索引不得成为 owner | 增加空间、更新及合并维护；计数证明扫描可省，实际延迟收益需关闭观测验证 |
| 写门闩选择性唤醒：32 等待者重阻塞496次 | 保护取消、交接、读写者进展及公平性；不能把所有锁一律改 wake_one | 需 waiter 选择/取消协议和饥饿对照；存储后端等待仍可能主导 |
| deadline 索引：固定4期限，遍历最大7→263 | 同期限、提前唤醒、信号、取消和槽复用都需可靠删除/代次管理 | heap/tree 需要额外标量与更新；节省扫描是否抵过维护需新因果对照 |

本轮只比较候选。没有选择或实现优化，也没有实板或硬实时收益结论。
上述历史消费者缺口后来通过兼容分支独立续测关闭，不能由这些扫描/唤醒候选推导。

### 验证与重建

默认构建的完整 RV64、真实 musl/pthread、glibc 2.44、1091 条 ABI 差分、scale、
legacy/modern × writeback/writethrough io-sleep 和栈检查通过。
观测构建的全部成本 case、四组合 I/O fixture、独立包装器 scale、VMA/context/trap-return/栈通过。
真实 allocator 回收重入、取消 owner、零时钟 fixture 运行/IRQ 边界及 14 项报告检错均通过。
默认/观测分别检查 1674/1729 个函数，最大编译器栈 2368 B，assembly trap 288 B、reserve 1024 B；
消费者根退出最小栈余量 4520 B、heap-live=0。
SQLite DELETE/WAL 正常、NBD WRITE/FLUSH 错误，以及固定 Linux 对照的选定正常/hot/confirmed/失败恢复通过。
未修改写回、事务或队列机制，未以此声称重新运行两种完整恢复矩阵；后续机制改动仍须完整矩阵及双盘隔离。
最终独立只读审查未发现新的 P1/P2，另行核对归档、14项报告检错、成本数字和默认 `.text`；扩大 QEMU 门禁由本轮实际执行，审查未独立重跑。
完整比赛 Harness 继续因缺少 `kernel-la` 阻塞；未 push、发布或转换阶段。

先串行测量，保存每个 runner 打印的 `results.json` 路径为 ON/OFF/DUAL/LINUX。每个命令默认三个独立启动；
下面四个用户 runner 和十二个 fixture 全部串行，不与其他 QEMU 测量并行：

```sh
make COST_DIAGNOSTICS=1 all
python3 -B tests/cost-riscv.py --case all
make all
python3 -B tests/cost-riscv.py --case all --off --kernel kernel-rv
python3 -B tests/cost-riscv.py --case locking --two-disks
python3 -B tests/cost-riscv.py --case consumer --linux
make COST_DIAGNOSTICS=1 build/cost/riscv/tests/kernel-io-sleep-rv build/host/nbd-fault
# 四种配置各重复三次，文件序号只为收集，执行器按实际配置归组三副本。
for transport in legacy modern; do
  for cache in writeback writethrough; do
    for replica in 0 1 2; do
      if [ "$cache" = writethrough ]; then
        python3 -B tests/io-sleep-riscv.py --kernel build/cost/riscv/tests/kernel-io-sleep-rv --transport "$transport" --write-through --cost-output "build/io-${transport}-${cache}-${replica}.json"
      else
        python3 -B tests/io-sleep-riscv.py --kernel build/cost/riscv/tests/kernel-io-sleep-rv --transport "$transport" --cost-output "build/io-${transport}-${cache}-${replica}.json"
      fi
    done
  done
done
# 将这四个路径设为上面各次 runner 打印的真实 results.json，再生成新实验的归档。
python3 -B tests/cost-evidence.py "$ON" "$OFF" "$DUAL" "$LINUX" build/io-*.json --final --output build/cost-measurements.json
python3 -B tests/cost-evidence.py docs/learning/cost-measurements.json
python3 -B tests/cost-summary.py docs/learning/cost-measurements.json
python3 -B tests/plot-cost.py docs/learning/cost-measurements.json build/cost-curves.png
make test-cost-host
make COST_DIAGNOSTICS=1 test-vma-riscv test-scale-riscv test-context-riscv test-trap-return-riscv test-stack-usage
make test-riscv test-userland-riscv test-glibc-riscv test-diff-abi-riscv test-scale-riscv test-io-sleep-riscv test-stack-usage
make test-sqlite-rollback-riscv test-sqlite-wal-riscv test-sqlite-nbd-riscv test-sqlite-recovery-riscv test-sqlite-wal-recovery-riscv
```

图由 matplotlib 3.10.6 生成，只有关观测的时间曲线可用于估计默认路径延迟；访问数和重阻塞曲线是机制成本证据。
每次重建会产生新的内核/镜像身份，不要求随机 fixture 或测量时间与旧归档逐字节相同。

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

该 C1 阶段未选择优化；后续 C2–C6 的最终计数、消费者阻塞及候选见本文首节。

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
共享后端的等待。向量追加、重叠定位写、同步尾部和截断的内容、offset/大小、状态均通过；
U-mode取消窗口终止尚未放行的gate waiter，真正写中取消与延后terminate由四组合压力fixture提供。
热路径仅增加默认关闭的观测，不改变门闩或唤醒策略。

固定 kernel SHA-256：`5e9d2fb7c098c5795b98f0ae1fc8f5d0b4ed4550e2d721d5d3efff54a3ef6673`。
可重建：`make COST_DIAGNOSTICS=1 all`；串行执行
`python3 -B tests/cost-riscv.py --case locking` 和同命令追加 `--two-disks`；
`python3 -B tests/io-sleep-riscv.py --kernel build/cost/riscv/tests/kernel-io-sleep-rv --cost-output <result.json> --transport modern`
（writethrough追加 `--write-through`），
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

## C5 IRQ-off与唤醒延迟（2026-09-30）

热tmpfs 4KiB/64KiB/1MiB复制和单页/1MiB改权各配一个实际blocked后独立唤醒的子任务，
开/关各三个串行启动，内容、大小、返回值、退出和heap基线保持。每个窗口均实际采到IRQ-off及wake_to_run样本。
下表是该阶段前台最大连续区间ticks（三副本）与客体操作时间的开/关中位比。

| 窗口 | 最大连续IRQ-off ticks | 时间中位比 |
|---|---|---:|
| latency-copy-4096 | 4769/4504/4859 | 0.913 |
| latency-copy-65536 | 17853/19406/5077 | 2.786 |
| latency-copy-1048576 | 79169/75376/78680 | 1.830 |
| latency-protect-4096 | 6065/6169/6077 | 2.997 |
| latency-protect-1048576 | 9060/11774/11333 | 3.371 |

hart区间跨任务切换，不按任务各自重置。观测自身的SIE临界区属于observer lane；
时钟取time CSR。实际样本保留架构固定盲区：U/S入口为7/6条指令，sret尾为11条；
C enable helper在最后rdtime后至少9条指令（含CSR），不同调用点另有编译器恢复指令。
这些是明确未采样前后缀，区间为采样段而非硬件最长关中断上界，不能宣称实板/硬实时保证。
结束stamp延后到最后安全寄存器恢复点，下一次进入安全C路径才聚合，避免聚合开销混入固定sret尾。
独立假时钟测试验证嵌套、跨任务归属、控制抑制和epoch复用。
可重建：`make test-cost-riscv COST_CASE=latency`；默认构建同ELF以
`python3 -B tests/cost-riscv.py --case latency --off --kernel kernel-rv`对照。
trap/trap-return/context/user、四组合io-sleep、bandwidth、stack检查通过。

### 后续：时间更新与数据后端的独立隔离

2026-09-30 新增 `make test-lwext4-cost-host`：实际 lwext4 源码、32MiB journal镜像、
4KiB块、256B inode、GCC16.2.1和mke2fs1.47.4。打开并预热同一个4KiB文件后，
按公开接口执行128次纯mtime/ctime更新、128次4KiB热覆盖及128次组合操作。
独立block_fault在设备入口计数；三次新镜像的下列结果完全一致，时间戳、内容读回及fsck通过。

| 宿主fixture路径 | 文件逻辑字节 | block写请求 | flush请求 | 新commit数 |
|---|---:|---:|---:|---:|
| timestamp-only | 0 | 384 | 256 | 128 |
| backend-only | 524288 | 128 | 128 | 0 |
| timestamp-and-backend | 524288 | 512 | 384 | 128 |

这证明每次时间更新即使不写文件数据也引入提交和屏障，组合路径比数据后端单独调用多256次flush。
`fs/files/io.c` 的非零普通文件写在用户复制前调用modified，`fs/ext4_backend.c`转入同一touch接口；
该顺序还保护首字节EFAULT的mtime语义，不能直接删除。这里的backend-only是因果隔离对照，
不是允许关闭用户可观察时间更新的实现候选。
该fixture不经过VFS页缓存，既不把commit计数当稳定ABI门槛，也不据其请求比推算iozone总耗时或QEMU收益。
新增原消费者自己的成本窗口见下节；磁盘unknown和未解释等待继续保留。生产写回、事务与队列没有改动，
完整恢复矩阵门禁没有被本探针替代。

## C6 原消费者续测收口（2026-10-01，上海）

原程序和八组参数均未改。通用实现 `main@8d740bc` 已单向合入评测分支 `oscomp-rv-compat@f380c70`；
旧 glibc ELF 的 `.note.ABI-tag` 为 Linux 4.15.0，兼容分支沿用该版本适配。main 仍报自身版本，
因此这批旧 glibc 的实际 I/O 结果属于兼容配置，不能写成旧 glibc 直接在默认 main 通过。

新增[续测归档](cost-consumer-followup.json)包含 **9次串行独立启动、48个完成观测窗口**：
modern/writeback、512MiB、单hart，观测开/关及固定Linux各三个新fixture；每命令预算明确为900000ms。
各配置使用相同协调ELF，开/关源码内容哈希相同，输入在启动前封存。全部144次命令都自然退出0，
0–6组的126次实际I/O完成；7组的18次均报告所选测试不可用，与固定Linux完全一致。
每条保留原ELF、argv、cwd、wait status、原输出与真实耗时。原20配置归档不被覆盖。

未修改负载在延长预算后自然结束，旧180秒超时未支持永久卡死判断：默认关闭构建的自动模式仍需约400–420秒，
随机读写约206–230秒；开启构建还有多个约180–193秒的组合。前序取消会改变缓存/文件/journal状态，
所以不能将旧截断序列与本次完整序列混算开销。新门禁已拒绝旧九条消费者记录，
只有完整续测通过后才生成 `original_consumer_closure=true`，并再次解包验证全部seal。

### 耗时分布与观测开销

下表为客体秒的最小/中位/最大，比例为开/关中位数；7是回退普通写的程序成本，不是向量性能。

| libc/组 | 开启 min/median/max (s) | 关闭 min/median/max (s) | 中位数比例 |
|---|---:|---:|---:|
| musl 0 | 483.685 / 484.516 / 487.198 | 413.694 / 415.066 / 419.714 | 1.167 |
| musl 1 | 180.942 / 187.080 / 190.823 | 152.708 / 155.022 / 160.668 | 1.207 |
| musl 2 | 252.693 / 254.468 / 254.835 | 213.641 / 213.818 / 214.452 | 1.190 |
| musl 3 | 186.522 / 190.322 / 192.952 | 146.315 / 157.751 / 162.052 | 1.206 |
| musl 4 | 178.593 / 180.888 / 187.200 | 142.076 / 147.102 / 161.289 | 1.230 |
| musl 5 | 112.468 / 112.785 / 114.079 | 99.205 / 99.392 / 99.915 | 1.135 |
| musl 6 | 112.540 / 113.602 / 114.790 | 99.655 / 100.791 / 101.608 | 1.127 |
| musl 7 | 162.500 / 170.340 / 174.908 | 143.488 / 145.488 / 149.129 | 1.171 |
| glibc 0 | 466.567 / 477.141 / 486.378 | 400.408 / 409.248 / 409.308 | 1.166 |
| glibc 1 | 183.191 / 183.416 / 185.630 | 158.818 / 164.508 / 166.478 | 1.115 |
| glibc 2 | 257.003 / 258.039 / 273.250 | 206.498 / 223.802 / 230.188 | 1.153 |
| glibc 3 | 181.116 / 181.562 / 190.675 | 151.228 / 163.248 / 164.788 | 1.112 |
| glibc 4 | 179.530 / 180.562 / 180.977 | 147.708 / 148.018 / 156.665 | 1.220 |
| glibc 5 | 111.334 / 111.598 / 115.882 | 98.358 / 98.438 / 102.169 | 1.134 |
| glibc 6 | 110.987 / 111.836 / 112.953 | 97.238 / 98.708 / 101.682 | 1.133 |
| glibc 7 | 156.327 / 164.611 / 172.596 | 146.315 / 146.428 / 146.618 | 1.124 |

16个窗口的总耗时：开启3330.136/3377.907/3405.847秒，关闭2861.099/2887.986/2901.818秒
（min/median/max），中位数增加 **16.964%**；各组约11.2–23.0%。这是实际观测扰动，
不是只测聚合函数体的observer_ticks。Linux三个完整序列约169–171秒；不能用缓存接受速度宣称持久写入等速。

### 真正写入路径、主成本与余量

自动模式的六项写各接受4MiB，两个libc每次都有24576次1KiB写、25165824B接受量。
实际后端接受20972544B（五次4MiB加1KiB），快照复制20975616B（五次4MiB加一整页）：
重复record-rewrite被页缓存合并，不能笼统称没有写缓存。mtime/ctime仍在每次非零write前进入touch事务。
纯timestamp宿主隔离已测出128次提交/256次flush，说明该路径存在独立的提交/屏障成本；
不直接据此分配原消费者的磁盘字节或耗时。

| 自动模式三次观测 | musl | glibc |
|---|---:|---:|
| 设备请求 | 229097 / 229097 / 229097 | 247536 / 247112 / 246444 |
| flush请求 | 96907 / 96907 / 96907 | 106093 / 105881 / 105547 |
| unknown提交写字节 | 374540288 / 374540288 / 374540288 | 379222016 / 379113472 / 378942464 |
| 前台运行秒 | 28.569 / 27.924 / 27.755 | 29.129 / 30.728 / 28.571 |
| 设备区间累计秒 | 295.633 / 294.114 / 296.284 | 255.283 / 252.827 / 253.107 |
| 前台就绪等待累计秒 | 167.031 / 172.598 / 167.767 | 200.499 / 211.152 / 192.422 |

musl的BG idle为453.507/457.675/455.167秒；hart记账未覆盖余量为0.331%/0.327%/0.327%。

glibc的BG idle为446.331/453.925/436.357秒；hart记账未覆盖余量为0.352%/0.354%/0.350%。

设备区间、blocked、ready及锁持有/等待可能跨任务重叠，不能直接相加得到CPU占比。
现有证据支持大量提交/屏障、设备等待和就绪等待；不能把全部idle或unknown字节归因给journal。
IRQ-off仍按真实SIE和固定盲区记录；iozone的Time Resolution是其调用估算，不是DTB的100ns时钟分辨率。

### 原judge成绩与预算边界

独立原评测一次启动、原1GiB/设备/网络/RTC配置、原3600秒总预算、原脚本及22个judge/postwork，
实际内核 `68b98d8`（生产机制与本次续测相同；后续main改动仅测量工具/探针），得到RV单侧投影626。
原iozone：glibc **21.451673**，musl **21.668778**，各20项都有正值，脚本结束0且各八个完成marker。
judge取Max throughput per process而非Children总和；慢于内嵌基准但非零仍给1分，
所以20项正值不代表速度达标。原(11,12)回退后的initial writers/rewriters也被原judge计分，
不能算作pwritev/preadv通过。

该次总预算耗尽于lmbench-glibc，其后lmbench-musl、LTP×2、Lua×2、netperf×2均未到达；
总体626低于旧662，原因包含原iozone由快速失败变成真实运行、消耗后续预算，不能说整体评测已跑通或提速。
只记录RV投影，完整Harness仍缺kernel-la；评测分支模块保留原judge逐项及22组状态。

### 下一次优化的三个真实路线

| 候选 | 证据和预计能减少什么 | 正确性/演进约束 | 复杂度与下一门禁 |
|---|---|---|---|
| 将普通写时间更新纳入脏inode/sync边界 | 独立touch每128次产生256次flush；数据缓存合并后时间更新仍逐次提交 | stat即时可见、首字节EFAULT时间语义、O_SYNC/fsync、清理与异步错误owner都须保持 | 新dirty owner/错误观察协议；完整DELETE/WAL恢复和双盘隔离 |
| 合并/延迟checkpoint，保留ordered/log/commit屏障 | 自动模式约10万flush和约3.8亿unknown写B，除touch外仍有成本；当前尚未证明全部来源 | 已提交log、inode版本、revoke/orphan、空间和OOM回滚不能混淆 | 先加来源隔离再选batch边界；完整恢复矩阵，禁止直接删flush |
| 缩短I/O完成到运行的等待 | 自动模式前台ready累计约167–211秒，远大于CPU运行；尚含控制者timer等来源 | 需先区分设备、锁与控制者唤醒，保留取消、公平性及栈安全 | 先独立唤醒归因，再确认调度/唤醒方案；不能把现有累计数当收益预测 |

三个路线均未选择、未实施。既有resident/deadline/锁候选仍有效，但不能据扫描数量宣称能解决本次慢写。
本轮没有改变写回、事务、队列或持久化机制，不以成本收口替代完整恢复门禁。

### 重建与验收

固定资料仍为 `references/oscomp-autotest@d1bb3a3c4b27274e196a2648518525c1a304e339`、
`references/linux@f4cdf7ca9a1fdcca413157df19753f388a5a224e`；QEMU本地源码参考v11.1.0，
实际运行二进制11.1.1、SHA-256 `a1cfcceb6c688f9b0a290d512211ed08cf465b92b26a04cfb032280a53625718`。
所有kernel/ELF/fixture/firmware/DTB/工具身份与源码tree/内容哈希均在续测归档中，时钟10MHz。

```sh
# 在已单向合入main的oscomp-rv-compat执行；不要与其他QEMU测量并行
make all COST_DIAGNOSTICS=1
python3 -B tests/cost-riscv.py --case consumer --replicas 3 --consumer-timeout-ms 900000
make all COST_DIAGNOSTICS=0
python3 -B tests/cost-riscv.py --case consumer --replicas 3 --consumer-timeout-ms 900000 --off --kernel kernel-rv
python3 -B tests/cost-riscv.py --case consumer --replicas 3 --consumer-timeout-ms 900000 --linux
# 使用上述三次runner打印的results.json路径生成独立续测文档
python3 -B tests/iozone-closure.py --verify docs/learning/cost-consumer-followup.json
# 原judge是另一配置的单次全预算运行
python3 -B tests/oscomp/run.py
```

续测接受/解包再次核对九次启动、原依赖、原argv和真实完成；旧180秒矩阵及诚实记录的startup/timeout
仍可作为诊断数据，但不能通过消费者收口。18项报告检错及原依赖/预算损坏拒绝已执行。

### 默认关闭构建的代表吞吐

以下为三个独立启动的 Max throughput per process 中位数，单位为原输出的 kB/s，
不是四进程 Children 总和；Linux列为同一原ELF、argv和512MiB配置。
原命令未要求每次持久同步，Linux与BoarOS的默认持久化成本不同，不能把比值当作磁盘硬件速度。
全部逐项原输出在续测归档中；原judge使用另一份内嵌基准，不能由本表自行拼出正式分数。

| 操作/组 | musl BoarOS | musl Linux | glibc BoarOS | glibc Linux |
|---|---:|---:|---:|---:|
| initial writers / (0,1) | 19.98 | 148746.70 | 20.11 | 103996.26 |
| rewriters / (0,1) | 23.18 | 294337.47 | 19.68 | 289265.53 |
| readers / (0,1) | 10889.22 | 378418.31 | 17700.65 | 381662.31 |
| re-readers / (0,1) | 65331.82 | 352132.03 | 68381.61 | 377859.78 |
| random writers / (0,2) | 27.08 | 102259.89 | 26.43 | 95253.54 |
| fwriters / (6,7) | 21.76 | 125015.27 | 22.67 | 119263.92 |
| pwrite writers / (9,10) | 21.37 | 117285.53 | 21.58 | 129811.74 |

### 续测回归范围

`make test-userland-riscv test-glibc-riscv test-stack-usage` 在本次main收口重新通过：
真实musl静态/pthread退出栈最小余量4584/5152B；固定glibc2.44五种ELF形态双侧退出和marker正确；
1674个函数的编译器栈检查通过，最大2368B，assembly trap288B，reserve1024B。
原旧glibc测试由续测归档保护，不由2.44替代。18项报告检错和续测解包验收通过；
完整RV64/1091 ABI/scale/四组合io-sleep/选定SQLite错误恢复的既有结果见本节上方历史验收，
本次未改生产机制，不宣称再次运行全部恢复矩阵。

### 本次独立审查与修正

新的只读全改动审查核对实际串口、冻结内核、九启动计数、吞吐/耗时表和原22个judge，
数字一致；同时发现两项P2验收器缺口：仅marker也能通过严格完成判定，跨开/关源码不一致仍能封存。
先重现红测，再修严格门禁：自动模式检查完整13列正值；线程模式从原输出配对每项Children/Max结果，
拒绝缺少请求方法、重复、零值及NaN/Inf；开/关源码及各比较的预算/QEMU/firmware/timebase必须一致。
历史诊断分类保持原契约。两个回归由红转绿，全18项通过，新九启动与旧60启动归档均再次验证。
实际保存的输出/身份全部符合新规则，未修改数据或重启测量。没有未解决的Critical/Important发现；
扩大QEMU回归由实施者执行，审查没有重新跑长时QEMU矩阵。
