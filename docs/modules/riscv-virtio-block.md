# RISC-V VirtIO MMIO 块设备模块

本文描述 QEMU `virt` 上同步读写与批量读写块设备路径。DTB 节点来源见[DTB 与启动内存布局模块](dtb-memory.md)，上层文件语义见[VFS 与 ext4 模块](vfs-ext4.md)。

## 共用块核心与 RV 适配

`drivers/virtio/block.c` 和 `include/kernel/virtio_block.h` 拥有请求、八槽、批量/部分
读写、FLUSH门闩、超时、DMA借用与统计；feature/status、配置、split ring及在途
descriptor/token由[共用VirtIO框架](virtio-framework.md)提供。transport提供
语义寄存器访问、先于used快照的IRQ acknowledge，以及IRQ owner登记/摘除。
MMIO 与 PCI 的总线偏移、访问宽度和 read-to-clear 差异留在各自实现；IRQ/DMA
CPU 屏障由构建期架构操作提供，不是运行期架构 vtable。

RV `arch/riscv/virtio_mmio_block.c` 负责PLIC绑定，MMIO身份/版本和寄存器映射由共用adapter实现；
旧类型和入口保留薄适配。`riscv_virtio_mmio_block_base()` 仅暴露已有 MMIO 资源
用于 DTB IRQ 匹配。启动 fixture 可在物理别名执行，函数指针必须从实际 PC 初始化，
不能直接使用含高地址常量的静态回调表；生产内核仍在最终地址空间构造设备。

`make test-virtio-block-host` 用相同真实块核心分别经 MMIO 和独立语义 transport
驱动 wire 模型，覆盖反序完成、索引回绕、超时/reset、八槽与 DMA owner。
新 transport 的测试不构成真实 PCI 验收。`test-block-riscv`、`test-root-init-riscv`、
四组合 `test-io-sleep-riscv` 与栈门禁保护既有 RV 路径。

## 发现与 transport

`dtb_read_boot_info()` 收集启用的 `compatible = "virtio,mmio"` 节点，翻译父总线 `ranges` 后按物理地址排序并拒绝重叠。最终 Sv39 页表只映射实际发现的 MMIO 范围。`arch/riscv/virtio_mmio_block.c` 依次探测这些 transport：非 block device 跳过，所有成功初始化的 block device 均登记，首个成为当前根设备，其余可通过设备节点挂载；已经表明自己是 block 但 transport/feature 不受支持时准确失败，不继续扫描磁盘内容。

当前同时接受 VirtIO MMIO version 1 legacy 和 version 2 modern。modern 路径要求协商 `VIRTIO_F_VERSION_1` 并检查设备回显 `FEATURES_OK`；legacy 路径使用传统 feature 寄存器和状态序列，不伪造 modern feature。驱动在两种 transport 下均探测 `VIRTIO_BLK_F_RO`（bit 5）：若设备提供只读标志（如 QEMU `readonly=on`），驱动接受该 feature 并设置 `block.write = block.write_batch = 0`；若设备可写，则安装单次和批量写回调。设备状态按各自规范推进，失败和销毁会复位设备后再回收队列页。QEMU 默认 legacy 和显式 `-global virtio-mmio.force-legacy=false` 的 modern 配置都由同一驱动验证。

## 请求和 DMA

通用 `kernel_block_device` 暴露容量、逻辑块大小和精确字节范围的同步读写接口（`kernel_block_read_at`/`kernel_block_write_at`），统一检查空参数、越界和整数溢出。两种 transport 共用最多 32 个描述符的 split queue，每请求占三描述符，最多八个在途槽；按设备 QueueNumMax 向下取可用容量，至少一个槽。modern 分配连续两个架构页（RV为8KiB，LA为32KiB）；当前RV legacy分配四个4KiB页（16KiB），used ring按协议4KiB对齐。每槽独立持有 header/status、512 字节 bounce、任务 owner、完成状态与等待队列；同步调用拥有槽位直到完成确认，不能在 DMA 期间复用。

`kernel_block_write_batch(device, spans, count)` 接受最多八个 `kernel_block_span { offset, buffer, size }`。设备与逻辑块大小必须有效，至少具有单写或批量写回调；只读设备返回 UNSUPPORTED。通用层在任何写入前检查所有 span 的容量、指针范围和非空字节区间重叠；非空 span 之间重叠返回 INVALID。有效可写设备上的 `count == 0` 可以传空 spans，成功且不调用驱动；空 span 可以传空 buffer，但 offset 仍须不超过容量，全部为空也不调用驱动。可选 `device.write_batch` 一次接收原 span 数组，可能含空项；没有回调时跳过空项、按输入顺序调用单写，遇到首个错误即停止。它保留实际失败状态，不承诺整批原子性或自动 flush；已完成的写不会回滚，调用方须保持所有非空 buffer 有效直到函数返回。

`kernel_block_read_batch(device, spans, count)` 同样最多八项，在发布前检查全部参数。
读 span 提供可写 buffer、已完成字节前缀和独立状态；磁盘范围可以重叠，但目标内存
必须彼此及与描述符数组分离。预检失败不发布任何请求，未执行项保留 NOT_SUBMITTED。
没有批量回调时按顺序执行全部标量读，保存每项结果；返回输入顺序中首个错误。
VirtIO 批量读支持 direct DMA 与逐扇区 bounce；一个 span 出错不会取消其他独立 span，
失败 span 只承诺其 completed 前缀。调用返回前必须排空所有已发布 DMA，或确认 reset。
读取消同样不能提前归还目标内存；成功完成的部分不会因别的请求超时而改写为失败。

VirtIO 将发布、等待和收割分开。同一 batch 按当前空闲槽发布多个 span，支持乱序完成，并在其他调用释放槽时继续补发；不要求先凑齐整批槽位。每个请求保留独立的 I/O context、期限、状态、DMA buffer 和成本身份。batch 只占一个逻辑调用 owner，`active` 统计逻辑调用数，`inflight` 统计已经发布的请求数；释放某个槽不会提前释放整批 owner。不同字节区间仍可能落在同一物理扇区，因此本批内涉及同扇区的 span 按输入顺序串行推进读改写，其他 span 可并发；不会因两个独立 bounce 快照覆盖相邻字节。

`kernel_block_flush()` 表示设备持久化边界。块设备显式声明 UNKNOWN、WRITETHROUGH 或 WRITEBACK；未知能力且没有 flush 回调返回 UNSUPPORTED，只有声明 write-through 的设备可以省略 flush。VirtIO 协商 `VIRTIO_BLK_F_FLUSH`（bit 9），发送独立的 type 4 请求，只有 header/status 两个描述符，不带数据。未协商 CONFIG_WCE，因此 FLUSH 存在表示 writeback，缺失表示 write-through；驱动不修改缓存模式。该规则依据固定 Linux `references/linux/drivers/block/virtio_blk.c` 的 `virtblk_get_cache_mode()`（`f4cdf7ca9a1fdcca413157df19753f388a5a224e`）与 QEMU `references/qemu/hw/block/virtio-blk.c`（v11.1.0，`84f07211cc5b4fc6a371559bf8a5de4fb068e648`）。设备错误和超时保持所属状态，flush 请求单独计数。

此后端限定于 QEMU 同步 MMIO reset：写零后必须读回零，才能把借用的 DMA buffer 还给调用方或释放队列；违反该平台契约触发 fatal，普通请求超时在成功 reset 后仍返回 TIMEOUT。异步 reset 的实板需要独立的 DMA owner/完成协议，不能直接套用此检查。块 flush 不代表上层已实现 fsync、日志或崩溃恢复。

reset 停止 DMA 的依据是固定 `references/qemu/hw/virtio/virtio-mmio.c` 的 `virtio_mmio_soft_reset()`/STATUS 写入路径，以及 `references/qemu/hw/block/virtio-blk.c` 的 `virtio_blk_reset()` 中 `blk_drain()`；版本为 v11.1.0，commit `84f07211cc5b4fc6a371559bf8a5de4fb068e648`。

扇区对齐且位于 direct map 的目标缓冲区直接作为 DMA 地址，连续多扇区合并成一个请求；写入请求使用 `VIRTIO_BLOCK_REQUEST_OUT` 且数据描述符不设 `VIRTQ_DESC_WRITE`。非整扇区写入通过 bounce buffer 执行读-改-写（Read-Modify-Write）：先读出包含该范围的完整扇区，将修改字节合并到 bounce buffer，再将该扇区写回磁盘。启动阶段显式轮询；生产调度启用后，PLIC IRQ 先确认设备中断，再读取 used ring 并唤醒任务，使确认期间的新完成保留通知。轮询和期限唤醒都先收割已发布完成，再判定请求超时；期限唤醒只做一次收割，运行期不连续轮询。批量等待登记后若发现DMA已发布而IRQ尚未收割的used项，先经既有完成校验/收割路径更新request状态，再结束登记并继续；仅跳过park不能证明软件owner已经完成。没有新完成时仍阻塞，不建立运行期轮询。运行期满槽和设备等待均不可中断地睡眠，idle/IRQ 提交会 fatal，不能退回忙等。超时采用 DTB timebase 的30秒有限期限，从请求发布时计算，先停止提交并 reset，确认 DMA 停止后统一完成未完成请求；设备保持失败态，不自动重试写。IRQ 与超时在同一短关中断区仲裁，完成、唤醒和槽归还各一次。期限用于识别停滞，不是正常I/O延迟上限；合法FLUSH可能等待宿主整个后备文件落盘，一秒会误判。固定Linux v7.2的 `references/linux/block/blk-mq.c` 同样采用30秒默认块队列期限；BoarOS仍在超时后冻结设备，不承诺Linux的恢复策略。

flush 先阻止新逻辑调用并排空此前逻辑调用，再发送 FLUSH，完成后才放行后续调用；已入场的 batch 在 flush 等待期间仍可补发余下 span，避免屏障与未发布项互锁。无 FLUSH 特性的 write-through 同样经过软件排空屏障。batch 收到一个错误后停止发布所有剩余项，包含已读入 bounce 但尚未写出的 RMW；已发布 DMA 继续收割，或在期限到达时 reset 并确认 DMA 停止，最后才返回实际失败。取消和信号不会中断内部等待或提前归还 buffer。统计记录提交数、实际最大已发布在途数、IRQ、完成等待睡眠/唤醒、队列等待、运行期轮询及错误；不把已预留但未发布槽计入在途。另有时间加权计量：在途深度积分（inflight-ticks，单位 深度·tick）、忙时、总span、队列等待与服务时间，销毁时打印一行最终统计供真实窗口取证。

PLIC 路由由 `dtb_read_irq_info()` 根据 CPU interrupt-controller phandle、启动 hart 的 supervisor cause 9 和 VirtIO interrupt-parent 关联；不硬编码 IRQ/context 编号。外部中断执行 claim、设备分派/ack、complete；IRQ 不分配或做文件清理。块/VFS 同步接口不变，设备内部允许多个调用同时等待。实板、IOMMU、非一致 DMA 和 SMP 仍不在此边界内。

完成队列的冷路径诊断在 reset 前输出 `block queue fault`：used 快照与消费差值超过槽容量为 `used-overflow`；完成项的描述符 head 未按三描述符槽对齐、超出槽范围或 length 为零为 `used-element`；指向非 submitted 槽为 `slot-state`；设备 status 超出 OK/IOERR/UNSUPP 为 `device-status`。输出包含 transport context/device status、中断快照、transport、队列虚拟/物理地址及容量，used、consumed、avail、入口观察到的完成数，逻辑调用/在途数和 reserved/published/complete 槽数；每个固定槽记录 state、owner/completion 指针值、status、type、字节数、扇区与期限。只读取驱动持有的 transport、队列和最多八个固定槽，不追 owner/completion 或设备提供的描述符地址，不分配、不增加重试。used 是触发校验的快照，consumed 包含已经取出的非法项，avail/status 是打印期间的标量；它不是跨 DMA 的原子快照。正常完成和普通 IOERR/UNSUPP 不输出这段诊断，reset、返回状态和 owner 回收契约不变。重复完成若发生在本次合法完成已收割之后，设备仍失败，但不会覆写已完成请求的真实结果。

纯超时也在reset前输出同一快照，原因为`timeout`，不附带不存在的非法完成项。
这覆盖设备停止推进、used仍然合法的路径：固定QEMU的`virtio_error()`会将设备置为
broken，legacy又不会发布modern的NEEDS_RESET状态，原先只有一行超时信息就reset会
丢掉关键现场。新增输出仍返回TIMEOUT，且reset确认之前保留所有DMA owner；它用于
定位停滞的设备与请求，不会仅凭超时判断是哪种设备故障或内存破坏。

split ring 使用协议规定的自然对齐布局，available/used 的 16 位 index 必须以单次半字指令访问，不能使用使编译器拆成字节访问的 `packed`。例如 `0x00ff → 0x0100` 的字节写可能先暴露 `0x0000`；设备会把反向跳变当作大量待处理 head，重复消费并触发 `Virtqueue size exceeded`。单 hart 关中断不能阻止设备并行访问，写后的 fence 也不能消除已暴露的中间值。既有发布/收割屏障仍保留。布局与对齐由编译断言保护，RV64 访问宽度由聚焦测试核对；固定依据为 `references/linux/include/uapi/linux/virtio_ring.h` 和 QEMU v11.1.0 的 `references/qemu/hw/virtio/virtio.c`。

## 验证

```sh
make test-dtb-riscv
make test-block-riscv
make test-block-host
python3 -B tests/host/virtio_block_diagnostics.py
make test-io-sleep-riscv
# 聚焦一个代表配置；不替代默认四组合验收。
BLOCK_TEST_TRANSPORT=modern BLOCK_TEST_CACHE=writeback make test-block-riscv
python3 -B tests/io-sleep-riscv.py --kernel build/riscv/tests/kernel-io-sleep-rv --transport modern
```

测试使用真实 QEMU raw disk，分别以默认 legacy 和显式 modern 配置覆盖 feature/status negotiation、批量 direct DMA、非对齐 bounce、容量边界、超时/错误统计和队列页回收。默认无块设备的生产内核仍可进入 timer idle；挂入两种 transport 的根盘时由根启动测试继续消费本模块。

块测试将两个 transport 分别与 writeback/writethrough 组合，检查协商结果、flush 完成与独立统计，并覆盖单批八个 direct DMA、非对齐相邻字节/跨扇区的 RMW 回读及只读拒绝。host 测试检查未知能力拒绝和错误传播，以及 batch 的整批预检、零项、重叠、八项边界、顺序回退和可选回调；`tests/host/block_registry.c` 检查稳定设备号查找、重复登记拒绝、跨设备独立 claim、同设备重复 claim 拒绝及 claim 释放后注销；`tests/host/block_fault.c` 提供以文件为稳定镜像、以内存为易失缓存的 512 字节原子写模型，可选择持久化任意事件、丢弃未同步写、注入 write/flush 失败。模型自测不是文件系统恢复验收。

`virtio_block_diagnostics.py` 仅在临时副本中将 RISC-V fence/time 指令替换为宿主 fence/受控时钟，编译实际驱动其余代码。legacy/modern 的 literal split-ring 反例检查上述四种错误分类、reset 前字段、非法指针值不被追踪、真实返回状态、一次发布及最终队列回收；正常完成与普通设备错误检查无诊断输出。它不验证真实 DMA 顺序、中断时序或 QEMU 完成行为。

无完成反例验证timeout快照先于reset、返回值仍为TIMEOUT；独立设备侧计数模型在每种
transport执行65544次请求，覆盖八槽复用、反序完成、重复IRQ及16位索引绕回，检查
已发布head不重复、在途不超过八条且最终归零。它没有重现历史QEMU告警，不能证明
所有设备交错都安全。真实NBD的八槽超时用例另检查reset前已记录全部在途槽和队列计数。

请求、扇区与 direct/bounce 计数提供当前结构成本基线；QEMU 功能测试不能证明 VisionFive 2 上的吞吐、延迟、cache coherency 或 CPU 忙等成本。开发板接入后需要用相同镜像分别记录冷启动读量、周期、吞吐和 CPU 占用，再决定请求合并深度、队列并行度及 IRQ 唤醒优先级。

`test-io-sleep-riscv` 使用 NBD 控制握手暂扣和乱序释放响应：两个不同文件冷读必须先形成两个请求，期间计算与无关缓存命中完成；八槽满队列后验证逆序完成、flush 前后顺序及超时/reset。legacy/modern × writeback/writethrough 四种配置均运行。禁用 QEMU 请求合并，避免两个相邻 guest 请求合成一个 NBD 命令掩盖门槛；不靠宿主 sleep 猜时序。失败保留 guest/server 日志和镜像。

batch 因果用例先要求同一个调用在任何响应释放前发布八项。随后用两个外部 writer 占槽：writeback 保持本批六个响应全部暂扣，释放外部响应后要求出现至少一项补发，再逆序释放本批已发布项并验证最后尾项出现在 guest FLUSH 前；writethrough 逆序释放首批八项，随后逐项释放两条补发请求，检查完整批次、屏障和持久字节，但不单独要求外部槽先于本批槽完成。

writethrough 的响应波次依据固定 QEMU v11.1.0（`references/qemu`，commit `84f07211cc5b4fc6a371559bf8a5de4fb068e648`）：`block/block-backend.c` 的 `blk_co_do_pwritev_part()` 在写缓存关闭时加 FUA，`block/io.c` 的 `bdrv_driver_pwritev()` 在 backend 不支持 FUA 时以 `bdrv_co_flush()` 模拟。当前 NBD fixture 仅声明 SEND_FLUSH，故外部 WRITE 回复不是外部 VirtIO 写完成的充分握手；这些 backend FLUSH 回复即时释放。

错误用例使第二个已发布 batch 写先失败，保留另一个 DMA，验证取消仍不能提前返回，再释放该 DMA 并回读确认未发布项未写入；正常单写与 batch 混合的八槽超时由 reset 收口，队列页和任务栈最终回收。运行期轮询计数仍为零。

RT 组合进展由 `make test-multi-disk-rt-riscv`（调用 `tests/multi-disk-io-riscv.py --rt-load`）单独验证：默认全局 RT 预算下，持续 FIFO/RR 子任务存在时，普通父任务与两台真实 NBD 的 READ/WRITE/FLUSH 均可完成，随后检查子任务、动态挂载、根页/堆/栈回收和两盘持久字节。该模式使用同一个 `tests/userland/multi_disk_io.c` 的显式 `/rt-load` fixture 分支，原错误隔离模式不变；详见[可睡眠存储](../learning/sleepable-storage.md#默认-rt-带宽下的存储进展)。

双盘故障 runner 在确认隔离检查点后，通过 NBD `cut` 收到两盘的明确回执，再终止
QEMU，避免直接 kill 截断健康盘的响应。诱导断线后的原始日志保留；进展和内核错误
判断截至受控 cut 前，冷启动读回和 fsck 仍必须通过，服务器非零退出不被忽略。
失败产物另含 guest/两盘退出码及 cut 请求/回执状态，详见[收口竞态](../learning/sleepable-storage.md#双盘受控收口2026-10-06)。

成本诊断的请求带提交时标量 epoch/lane，正常 IRQ 和 timeout/reset 均按该身份记账；registry 磁盘归属与 unknown 字节见[成本观测](kernel-cost.md)，现有设备统计保持原契约。

2026-10-01 最终四组合 io-sleep 与真实日志/消费者路径均达到最大八个已发布请求，运行期轮询为零；这不表示串行读或阶段间 FLUSH 也能保持八槽并行。消费者定点诊断的 ready/resume 关联只覆盖实际唤醒 owner 的请求，batch 会合并唤醒，不能把这一子集与旧所有请求样本直接比较平均延迟。存储流水线的恢复通过、Parent 收益未达标与完整输入身份见[最终验收](../learning/cost-baseline.md#s9-存储流水线验收2026-10-01)。

批量读的旧驱动顺序回退在生产函数宿主模型中只能发布一个请求，新增“暂扣至八项”
门禁超时；新驱动同时发布八项。两种 transport 的反序完成、单项错误、错误时另项
仍在途、超时 reset、非对齐、多片段和 505 字节成功前缀由同一实际驱动模型验证，
含 ASan/UBSan；这些是受控设备边界证据。真实 `test-io-sleep-riscv` 四组合额外要求
单次调用先产生八个 NBD READ，释放七个后取消/唤醒仍不得返回，最后一个回复后才
检查完整数据、独立状态和 owner 回收。此阶段尚未证明 ext4/页缓存能产生批量读。

以上“此阶段”指块层接口单独交付时的证据边界；后续 VFS/ext4、页缓存预读已接入并通过单次八 READ 实际暂扣门禁及两种传输的组合回归。发布吞吐候选、默认/最大候选恢复结果和未测平台见[数据路径最终报告](../learning/data-path-budget-experiments.md)。
