# 接收请求与单 hart 规模成本

2026-09-27，在 `11f7e44` 基线复现 UDP 普通 read/readv 的数据丢失：用户容量 8192，报文为 257、1500、4096 或 8192 字节时只返回 256，后续报文仍可读取，说明剩余部分已被消费。新增 42 组大小、入口、容量组合在旧实现上有 8 组与固定 Linux 不同；recvfrom 的循环复制和用户容量不足时的截断本来正确。内部临时缓冲大小不能作为 datagram 的用户可见容量。

## 契约、owner 与参考

接收拆为 reserve/copy/finish。socket 和任务登记同一个栈请求，reservation 临时持有调用者的 OFD pin；同 socket 的另一个读者不能越过队首。请求持有一页 scratch，分配发生在 reserve 之前，ENOMEM 不消费 UDP。每次按偏移复制，UDP 完成一次请求只消费一个报文；用户复制 fault 丢弃当前 UDP。TCP 每个队列片段完成后提交，当前片段 fault 不消费，先前已经提交的字节仍作为部分成功返回。TCP 分段由协议决定，测试验证字节守恒，不能要求 Linux 与 lwIP 返回相同的偶然短读长度。

正常返回先 finish reservation、再释放 scratch；退出清理先取消 reservation 并释放 pin，再释放 scratch，最后处理文件表和任务页。非法物理页释放仍 fatal，没有新增跨层重试 owner。文件和 TCP 写入同样使用请求页；文件后端与用户访问层负责页边界，TCP 额外按用户页边界和协议容量推进。已取得接收进展时，不为填满用户缓冲继续等待。

固定 Linux 依据为本地 `references/linux/net/ipv4/udp.c`、`net/ipv4/tcp.c`、`fs/read_write.c`，commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e`。协议实现为本地 `third_party/lwip/src/core/tcp_out.c` 等，官方 `STABLE-2_2_1_RELEASE`、commit `77dcd25a72509eb83f72b033d219b1d40cd8eb95`。来源与恢复方式见 `references/README.md`、`references/sources.tsv`。

MM 的 resident 链表继续负责遍历和生命周期，动态哈希只负责按完整虚拟页地址查找。首次 fault 和 fork 在发布 PTE/alias 之前准备索引容量；扩容失败保留旧表，不留下半发布映射。forget、截断、固定替换与销毁同步摘除索引。物理页引用和 private COW source 没有迁移到索引。共享页首次写和写回 rearm 改用三层叶 PTE 定位，检查计数与物理 owner，保留 PROT_NONE/COW 状态机，执行地址级本地 `sfence.vma`。非当前 MM 仍依赖现有切换刷新，不引入延迟失效或跨 hart 保证。

## 实测成本与检错能力

`make test-scale-riscv` 使用真实 files、MM、Sv39、页缓存、ext4 和 lwIP 路径，计数为操作次数，不以 QEMU 墙钟倍数宣称加速。输出数值为十六进制，下表换算为十进制。

| 负载 / 计数 | 修改前红测 | 修改后 | 门槛 |
|---|---:|---:|---:|
| 对齐 1 MiB 文件写入：复制分块 | 16,384 | 256 | ≤512 |
| 同次写入：用户页解析 | 16,384 | 256 | ≤512 |
| 错位 8192 字节 TCP 写入：协议提交 / 用户页解析 | 未作同负载旧版实测 | 3 / 3 | 各≤8，成功字节=8192 |
| 16 MiB resident 首次写：查找探测 | 8,390,656 | 6,210 | 与下行联合比较 |
| 64 MiB resident 首次写：查找探测 | 134,225,920 | 24,519 | 增长≤6倍；实测3.95倍 |
| 16 / 64 MiB 每页首次写：叶路径访问 | 全表扫描 | 12,288 / 49,152 | 每页≤6；实测3 |
| 同负载：地址失效 / 全局失效 | 0 / 每页一次全局 | 每页一次地址 / 0 | 不随无关映射增长 |

旧 MM 在查找增长门槛失败；只加入哈希后，单页改权门槛仍失败，因此两个优化有独立检错能力。单页入口新增的 aggregate 状态损坏测例也先在缺少检查时失败，再修复。新建、fork、move/destroy 后诊断计数保持定义明确的初始状态。

规模 fixture 顺序建立文件驻留页，再用互质步长遍历首次写，覆盖哈希碰撞与扩容；前 128 页扫描元数据分配失败位置，失败时验证无 PTE 发布，并重试。另覆盖 fork 后销毁、完整内容回读、scratch ENOMEM 后 UDP 仍完整排队，以及物理页回收至基线。既有 MM/VMA/files-partial 测试继续覆盖稀疏/替换/unmap、COW、PROT_NONE、截断 SIGBUS、写回失败后再次写和 fork 元数据 OOM。

该 boot fixture 没有调度任务，仅对空等待队列通知使用测试替身，且遇到非空队列即 trap；它不证明等待/唤醒时序。真实 U-mode pthread 测试另验证共享 socket 双读、阻塞读 close/fd 复用、线程组退出及池背压恢复，并检查 root heap 回收。TCP 差分累计传输 1 MiB，交替使用错位 write/writev，检查完整内容及不受 256 字节内部上限限制。UDP 覆盖 0/255/256/257/1500/4096/8192、read/readv/recvfrom、完整/不足容量、空 iovec、后继报文和跨页 fault。

## 收口复现

最终内核 SHA-256 为 `32eb2cfd37f9fe2470d5d0898ab10b876a2f1fdb4e559f11f3da5fc40f339fa8`，本机 QEMU 11.1.1。RISC-V 全套、真实 static/pthread userland、548 条固定 Linux 差分（其中 socket 118 条）、glibc 2.44 五种形态、SQLite DELETE 冒烟与多进程 WAL、离线 GCC 五阶段和栈检查通过。编译器栈报告覆盖 1308 个函数，最大 1952 字节；真实 userland 的最低剩余栈分别为 4936/6064 字节。lwIP、record-lock、allocator-release、block、NBD、lwext4 宿主入口和执行器 27＋18 项通过。

最终内核还通过固定 Linux 同一 ELF 的 DELETE/WAL 正常恢复，以及两个完整 NBD 故障矩阵。DELETE 小事务为 147 个事件（100 WRITE、47 FLUSH），覆盖 441 个断电组合、100 个写失败点、47 个 flush 失败点；WAL 为 42 个事件（26 WRITE、16 FLUSH），覆盖 126 个断电组合、26 个写失败点、16 个 flush 失败点。每个位置均执行两次恢复与 ext4 检查；故障模型仍限于 512 字节原子写、未 flush 写丢失或重排，不外推实板保证。

最终内核的 228 项清单全部完成，227 pass、1 `busybox.official` upstream-failure，无既有通过项回退；suite identity SHA-256 为 `d445f0be23db2522fb0d471196c07469b5cdc9092071a5776093f4c4e4dc9c69`。BusyBox 包装脚本的既有 df/dmesg/free/hwclock 缺口继续单列，不将其算成新增回归。

差分与应用的固定 Linux Image SHA-256 为 `f199c416620394d4ed0f86deb556700b8fc4ebf64261686f25ec2e93e70e4425`；完整程序清单使用不同配置的 Image `7ca338ec75e681cc68c5d946b3ae633fc0088fd78569b7847528105a9de6c8ec`。SQLite archive SHA-256 为 `1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d`，恢复 ELF 为 `179be6d2e5ab52c908d4e0547225e7999d9e404ffd05c12f404e9f170fdca5e0`，NBD 服务为 `b838e11a094c0c34114dbca310a8a75c158a442b3130dfc440c7da6a43092e6f`。runner 每次输出并核验本次输入身份，运行日志与镜像不作为长期证据。

```sh
make test-scale-riscv test-files-riscv test-files-partial-write-riscv \
  test-mm-riscv test-vma-riscv test-sv39-riscv
make test-riscv test-userland-riscv test-diff-abi-riscv test-glibc-riscv \
  test-sqlite-rollback-riscv test-sqlite-wal-riscv test-offline-c-riscv test-stack-usage
make test-lwip-host test-record-lock-host test-allocator-release-host \
  test-block-host test-nbd-host test-lwext4-host
make test-sqlite-recovery-riscv test-sqlite-wal-recovery-riscv \
  test-sqlite-recovery-matrix-riscv test-sqlite-wal-recovery-matrix-riscv
python3 -B tests/program-inventory/run.py --reuse-builds --jobs 2 --output build/scale-inventory
python3 -B tests/prune-build.py
make prune-build
```

PR CI 加入规模、SQLite DELETE/WAL 和离线 GCC 入口。SQLite/musl 源按固定清单恢复，Alpine 工具链由 runner 校验固定输入；缺失或校验失败不能跳过报绿。恢复 CI 独立支持手动与每周一北京时间 02:00（UTC 周日 18:00），显式 bash pipefail 保留 make 的失败状态，失败产物上传。Linux 缓存由身份键定位，runner 仍检查输入。工作流已做本地解析与对应入口验证，尚未执行托管 GitHub Actions。glibc 仍是固定本机输入的严格本地验收，可移植供应另列待办。

本次规模阶段当时未包含可睡眠 I/O、后台写回或多盘；这些机制现已在后续阶段交付，见[可睡眠存储](sleepable-storage.md)及[VFS/ext4](../modules/vfs-ext4.md)。范围写回索引、事务合并、共享文件 futex、SMP 和第二架构仍未交付；后续优先顺序只在[路线](../goals.md)维护。

AF_UNIX DGRAM 的成本边界为每条消息最多 64 KiB 连续暂存与一次队列提交，不把用户页或 iovec 当作消息边界。零消息收一字节预算，截断接收和 fault 丢弃返还整包预算；否则只返还复制字节会在重复短接收后泄漏队列容量。`make test-scale-riscv` 覆盖两处分配 OOM、100 条零消息、满队列、重复截断和接收 fault；`make test-userland-riscv` 用真实 pthread 验证尚有少量 POLLOUT 空间时写者仍等待整条预算，并检查取消不发布消息。这里验证语义和内存上界，没有吞吐测量结论。

## C0 窗口验收（2026-09-30）

成本测量的默认关闭聚合窗口已接入，契约见[成本观测模块](../modules/kernel-cost.md)。
以 `main@c9b6ca6` 的原默认 ELF 作红测，真实 U-mode 在缺少控制节点处 ENOENT；
观测构建的三个独立启动副本各完成四个有效窗口，另验证控制组退出产生 incomplete。
覆盖完整/错误命令、坏指针、重复开始、其他组结束、开始前后代已阻塞操作的在途结束、
恢复后结束和 epoch 复用。宿主核心检查溢出、零/最高桶、取消、延后激活/结束与抑制；
五项报告测试刻意删除/重复/交换 owner 对应 lane/破坏单位、计数与 histogram，均拒绝。

可重建命令：`make test-cost-riscv COST_CASE=contract`；
`make COST_DIAGNOSTICS=1 test-context-riscv test-trap-return-riscv`。
本阶段默认关闭 ELF 与原生产 ELF 的 `.text` 二进制逐字节一致，text/data/bss 分别
376832/52/502880 字节，`nm` 无 kernel_cost 或聚合存储符号；观测任务新增 48 字节，
聚合主体 5320 字节，DTB timebase 10 MHz。固定参考继续是
`references/linux@f4cdf7ca9a1fdcca413157df19753f388a5a224e` 与
`references/qemu@84f07211cc5b4fc6a371559bf8a5de4fb068e648`；实际 QEMU 为 11.1.1。
这些是该 C0 阶段的接口与检错证据，当时 C1–C6 尚未测量；最终计数、扰动和候选见下方成本基线。

2026-09-30 的最终 C0–C6 测量、观测扰动、消费者阻塞及优化候选见[成本基线](cost-baseline.md)，
该页保留结果与重建命令，原消费者输出由执行器生成。这是主线成本诊断，未重跑评测分支原judge。

## 追加增长规模门禁（2026-10-05）

基线 `5687377` 每个增长片段都会从 inode 缓存页链头部扫描。直接在旧循环加入
默认关闭的 COST 计数后，真实 RV64 1 MiB/4 KiB 追加得到 32896 次访问、0 次尾页处理，
新增门禁因此失败。固定文件大小而只计 usercopy 分块无法发现这个问题。

增长改为旧 EOF 非页对齐时的一次哈希查询；缩小仍走独立范围撤销。
`make test-cache-growth-riscv` 使用真实 VFS/页缓存、128 MiB 受管池和 QEMU 11.1.1，
每组确认全部文件页仍驻留并逐页核对内容，结果如下：

| 文件大小 | 4 KiB/64 KiB 请求的增长访问 | 1 KiB 请求的增长访问/尾页数 |
|---|---:|---:|
| 1 MiB | 0 | 768 / 768 |
| 4 MiB | 0 | 3072 / 3072 |
| 16 MiB | 0 | 12288 / 12288 |
| 64 MiB | 0 | 49152 / 49152 |

此表是算法成本，不是吞吐。4 KiB/64 KiB 追加不需要旧尾页；1 KiB 每页三次尾部增长，
总工作随页数线性增加。哈希扩容/探测和实际别名数量另计，不宣称任意冲突及任意别名下
严格常数耗时。真实 VFS、文件、部分写、静态 musl/动态 pthread 回归通过，包含
稀疏洞、截断再增长与私有 COW 尾页；COST 宿主和旧 schema 解析测试通过。


## 冷页覆盖与脏范围（2026-10-06）

冷页完整覆盖的生产函数反例先在旧实现运行：已存在且已同步的页被缓存失效后覆盖，
`kernel_vfs_node_pread` 包装计数为 1，触发门禁；当前为 0。稳定内核输入完整覆盖
才走新路径，loading 保留至初始化、脏组织和长度更新完成；部分页仍读取旧内容，
1 字节覆盖后检查其余 4095 字节未变。省去的是页缓存层预读，不是 ext4 的必要 undo、
块内合并和元数据读取。

每个 inode 保留全页链用于失效，另有脏页链与计数。首次标脏、共享映射变脏、写回、
再次变脏、截断和回收走同一索引维护；索引自身不持额外页引用。范围写回按页数/脏页数
选择较小集合，在不可睡眠的捕获区间分配并 pin，之后排序和 I/O 可以等待。
完整写回且代次相同才摘脏链，旧写回不清除新修改。快照页仍独立持有，只复制需要的区间。

`tests/riscv/scale_main.c` 的普通构建测试 1 MiB，COST 构建保留 64 MiB / 16384 页，
统计确认这些页没有被回收。范围规模使用真实缓存的稀疏零页，冷页覆盖另有第 0 页
真实已同步内容。它不是密集文件吞吐测量。最初把 64 MiB 全部写入后逐页同步的准备
放入 120 秒 fixture，超时盘上已推进至 18,759,680 字节；改为与范围索引契约直接相关的
稀疏准备，并让 runner 在后续超时时保存已捕获输出。没有将原超时算作通过。

| 已测操作 | 候选访问 | 快照复制 | 其他约束 |
|---|---:|---:|---|
| 16384 个常驻页中，一个脏页的页范围 | 1 | 1 字节 | 其余缓存页不扫描 |
| 整文件中仅 3 个脏页 | 3 | 3 字节 | 实际后端提交按偏移递增 |
| 16 个脏页中同步一个页范围 | 1 | 1 字节 | 剩余 15 页仍脏，之后全部写回 |

VFS、文件、部分写、共享/private mmap、1/4/16/64 MiB 增长、真实 userland、COST 和
栈门禁通过；四组 io-sleep 保留并发加载/写回、快照不变、映射再脏、inode owner 与错误回收。
生产内核 SHA-256 `56970dc9d644c26b42f37d903bd7be4805f93bb4571a9888d89c3923b98d2440`，
QEMU 11.1.1。完整 lwext4 恢复宿主目标通过，SQLite 3.53.4 原生 Unix VFS 的
DELETE/WAL EXTRA/FULL、热日志、已确认提交及两次重启完整性通过：

| SQLite 模式 | 断电切点 none/odd/reverse | WRITE 候选 / 实际注入 | FLUSH 候选 / 实际注入 |
|---|---|---:|---:|
| DELETE | 57 / 55 / 60（172 次） | 40 / 38 | 20 / 19 |
| WAL | 28 / 28 / 26（82 次） | 18 / 16 | 10 / 10 |

DELETE 的 WRITE 36/38、FLUSH 20，以及 WAL 的 WRITE 17/18 在对应执行中未达到
注入序号，runner 明确记作已提交后的切断与重启检查；不能算真实故障注入通过。
切点由各自运行日志测得，不借用历史 441 次矩阵的包络。恢复用相同程序 SHA-256
`510fc4c7b8e72a2224603b053bc9d5967f295e20a5085500038a4dd4936436f5`，
NBD server 为 `22a8c979a85a153c28c7b7fdeac7aa73f06d440b44c5e90cda7590db8dd452ab`。
这些是正确性和成本结果，当前没有匹配发布构建的吞吐结论。

```sh
make all test-scale-riscv test-cache-growth-riscv
make test-vfs-riscv test-files-riscv test-files-partial-write-riscv
make test-io-sleep-riscv test-userland-riscv test-stack-usage
make test-lwext4-host test-lwext4-recovery-host
make test-sqlite-recovery-matrix-riscv test-sqlite-wal-recovery-matrix-riscv
```


## 八请求批量读的设备边界（2026-10-06）

`kernel_block_read_batch` 在既有八槽容量内增加读发布/收割分离，不扩队列。
旧回退路径的实际驱动探针 max-inflight=1，等待八项的门禁超时；当前为 8。
宿主实际驱动覆盖 legacy/modern × 七种完成模式，包括单项错误、另一项仍在途、
超时 reset 与跨扇区失败的 505 字节成功前缀；ASan/UBSan 通过。真实 QEMU 11.1.1
四种 transport/cache 组合均在释放前收到八个 NBD READ，反序释放七个后仍保留
最后一个 DMA owner，取消/唤醒不能提前返回，全部完成后逐字节核对输出。
顺序设备回退、全参数预检与可写目标重叠拒绝由通用块宿主测试覆盖。

```sh
make test-block-host test-block-riscv
python3 -B tests/host/virtio_block_diagnostics.py
make test-io-sleep-riscv
```

这是底层并发和生命周期验收，既不表示 ext4 已用批量读，也不是预读吞吐收益。
VFS/ext4/页缓存接入、预读和连续写回实验继续作为独立阶段交付。


## ext4 缓存版本内的批量读（2026-10-06）

新增内部 `kernel_vfs_node_pread_batch` 与 lwext4 `ext4_fpread_batch`，保持 caller 的
inode/read owner，无共享 file offset 副作用。固定输入为仓库中的 lwext4
`58bcf89a121b72d4fb66334f1693d3b30e4cb9c5` 加已记录本地补丁；不绕过 bcache 读取
journal pending 的磁盘旧版本。本批的新 loading 先发布并完成，再等其他批次的
loading；单块失败只释放对应引用，其他项保留独立结果。

旧标量 `ext4_fpread` 对照在同一 fixture 中得到 width=0、batch calls=0，新门禁失败。
当前 8 个 4 KiB 页在 1/4/8 KiB ext4 上分别得到 4×8、1×8、1×4 个物理块请求批次；
8 KiB 情形同块双页只加载一次。extent 与传统映射共六组合通过，检查洞、EOF、
非对齐片段、首个失败块前的 17 字节成功前缀、其他 span 完成、失败后重试与
journal pending 缓存的新内容；ASan/UBSan 通过。实际 bcache 模型另覆盖重叠加载、
重复块、16 个分配失败位置的引用回收，以及无批量设备的标量回退。

真实 `test-io-sleep-riscv` 使用八页 `/batch-data`，同一次 VFS batch 在任何响应释放
前产生八个 NBD READ。四个 legacy/modern × writeback/writethrough 组合全部通过，
先释放七个仍保持最后一个 DMA owner，之后检查全部页内容、完成量与最终无在途。
本轮 fixture 内核 SHA-256 为
`681421ab44460fec278a3a813fb9ce5679e376d34491443f7a4418cd26332b70`，QEMU 11.1.1。
该证据到 VFS 内部接口为止；普通 demand read 的页缓存预读调度尚待接入，未测吞吐。

```sh
make test-lwext4-cache-host test-lwext4-batch-read-host test-lwext4-host
BATCH_TEST_CFLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' sh tests/lwext4-batch-read-host.sh
make test-io-sleep-riscv
```
