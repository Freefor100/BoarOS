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
四组合 `test-io-sleep-riscv` 与栈检查覆盖既有 RV 路径。

## 发现与 transport

`dtb_read_boot_info()` 收集启用的 `compatible = "virtio,mmio"` 节点，翻译父总线 `ranges` 后按物理地址排序并拒绝重叠。最终 Sv39 页表只映射实际发现的 MMIO 范围。`arch/riscv/virtio_mmio_block.c` 依次探测这些 transport：非 block device 跳过，所有成功初始化的 block device 均登记，首个成为当前根设备，其余可通过设备节点挂载；已经表明自己是 block 但 transport/feature 不受支持时准确失败，不继续扫描磁盘内容。

当前同时接受 VirtIO MMIO version 1 legacy 和 version 2 modern。modern 路径要求协商 `VIRTIO_F_VERSION_1` 并检查设备回显 `FEATURES_OK`；legacy 路径使用传统 feature 寄存器和状态序列，不伪造 modern feature。驱动在两种 transport 下均探测 `VIRTIO_BLK_F_RO`（bit 5）：若设备提供只读标志（如 QEMU `readonly=on`），驱动接受该 feature 并设置 `block.write = block.write_batch = 0`；若设备可写，则安装单次和批量写回调。设备状态按各自规范推进，失败和销毁会复位设备后再回收队列页。QEMU 默认 legacy 和显式 `-global virtio-mmio.force-legacy=false` 的 modern 配置都由同一驱动验证。

## 请求和 DMA

轮询先检查共享 used index，没有进展时不访问中断状态寄存器；真实 IRQ 总会先确认中断再收割。一次收割向每个实际完成的请求或批次通知一次，并通知描述符等待者。请求槽等原调用者消费结果后才归还，届时通知槽位等待者。

通用 `kernel_block_device` 暴露容量、逻辑块大小和精确字节范围的同步读写接口（`kernel_block_read_at`/`kernel_block_write_at`），统一检查空参数、越界和整数溢出。每个主分区持有不可变的父盘引用、起始偏移和容量，自身保留软块大小与只读设置。有效只读状态包含父盘软件只读和硬件能力。通用入口先按分区容量校验，再平移一次物理偏移；整盘的可睡眠 `io_lock` 将整盘及全部分区的单写、批量写、journal phase、字节 RMW 和 flush 串行化；批量内部仍可同时持有多个驱动请求。字节 RMW 在同一资格内读取扇区在设备容量以内的完整范围、修改指定字节并写回，因此文件系统的对齐写不能插入 RMW 的读写之间。容量末尾不足一个逻辑扇区时，只读取和写回实际剩余字节；loop 的容量仍按 Linux 的 512 字节单位取整。两种 transport 共用最多 64 个描述符的 split queue，最多八个在途请求槽；按设备 QueueNumMax 向下取可用容量，至少一个槽。请求槽只持有 header/status/bounce，head 与数据描述符从队列空闲项选择，不再与槽号固定对应。普通请求使用 header、一个或多个数据描述符、status；modern 的请求区从 2048 字节开始，避开扩大后的 descriptor/available/used 区域，编译断言检查它们不重叠。FLUSH 只用两个描述符。modern 分配连续两个架构页（RV为8KiB，LA为32KiB）；当前RV legacy分配四个4KiB页（16KiB），used ring按协议4KiB对齐。每槽独立持有 header/status、512 字节 bounce、任务 owner、完成状态与等待队列；同步调用拥有槽位直到完成确认，不能在 DMA 期间复用。

`kernel_block_write_batch(device, spans, count)` 接受最多 32 个 `kernel_block_span { offset, buffer, size }`。设备与逻辑块大小必须有效，至少具有单写或批量写回调；缺少写回调返回 UNSUPPORTED；设备被设为只读后，非空写返回 READ_ONLY。通用层在任何写入前检查所有 span 的容量、指针范围和非空字节区间重叠；非空 span 之间重叠返回 INVALID。有效可写设备上的 `count == 0` 可以传空 spans，成功且不调用驱动；空 span 可以传空 buffer，但 offset 仍须不超过容量，全部为空也不调用驱动。可选 `device.write_batch` 一次接收原 span 数组，可能含空项；没有回调时跳过空项、按输入顺序调用单写，遇到首个错误即停止。它保留实际失败状态，不承诺整批原子性或自动 flush；已完成的写不会回滚，调用方须保持所有非空 buffer 有效直到函数返回。

journal 内部的 `kernel_block_write_phase()` 接收独立 context 和可重新遍历的稳定范围游标，分区偏移由 context 转换而不修改生产者节点或拆散阶段，先检查整阶段的容量、对齐和 RAM 边界，再持续填充预留工作区。任何完成成员都可立即接纳后续范围，不等待原 32 项窗口全部排空；错误后停止新发布并等待已有 DMA 结束。范围不重叠由 journal 的块所有权保证，公共批量接口的检查保持原语义。连续的大范围先拆成不超过 128 KiB 的活动片段，同一 512 KiB 来源可同时形成四个请求；实际链仍受设备段数、段长和队列容量限制。阶段内待发布成员按磁盘位置串联，SG 规划沿链前进，描述符与完成成员按位图选择。

`kernel_block_read_batch(device, spans, count, completion)` 同样最多 32 项，在发布前检查全部参数。
读 span 提供可写 buffer、已完成字节前缀和独立状态；磁盘范围可以重叠，但目标内存
必须彼此及与描述符数组分离。预检失败不发布任何请求，未执行项保留 NOT_SUBMITTED。
没有批量回调时按顺序执行全部标量读，保存每项结果；返回输入顺序中首个错误。
VirtIO 批量读支持 direct DMA 与逐扇区 bounce；一个 span 出错不会取消其他独立 span，
失败 span 只承诺其 completed 前缀。调用返回前必须排空所有已发布 DMA，或确认 reset。
读取消同样不能提前归还目标内存；成功完成的部分不会因别的请求超时而改写为失败。

批量驱动的成员进度与临时 SG 段表由本次调用独占的工作槽维护。每槽 2048 字节，设备初始化按实际请求槽数保留工作区并紧凑放入普通架构页；八槽设备在 RV 上额外保留四个 4 KiB 页，在 16 KiB 页架构上保留一页。这些元数据不保存数据内容，也不参与 DMA。工作槽耗尽时在 workspaces 队列等待归还，不重新分配页，保证脏页回收在空闲页耗尽时仍能提交批量 I/O。调用消费全部完成或确认 reset 后才归还工作槽，销毁时统一释放保留页；初始化中途分配失败也先 reset 再回收已有页。单项和多项调用共用预留工作槽，不在单项分支保留完整 SG 段表。VFS 读取按实际页数准备描述，跨窗口复用；文件系统块适配的单项描述使用标量，多项按实际数量准备。批量描述不能沿调用链反复叠放满容量数组。固定 COST 写负载同时检查完成后的任务栈余量。

VirtIO 将发布、通知、等待和收割分开。同一 batch 将磁盘连续、扇区对齐且能直接 DMA 的多个 span 组成一个 SG 请求，各 span 保留独立的数据描述符，无需搬到连续内存；32 个独立 4 KiB 页可组成一个 128 KiB 请求，其完整链为 header、32 个数据描述符、status，共 34 项。设备 QueueNumMax 较小时向下选择队列容量并拆成多条请求；协商 SIZE_MAX/SEG_MAX 时读取对应上限，将每个数据描述符按整扇区截断，并限制每条链的数据段数。一个 span 可由多段描述符承载，其 completed 只在所属请求完成后推进。每个请求不超过 128 KiB，并受空闲描述符限制；请求槽维持八个，队列扩大不增加请求槽或 bounce。磁盘不连续的 span 在同一轮发布多个请求，然后只通知队列一次。请求可以乱序完成，并在其他调用释放槽或描述符后继续补发；不要求先凑齐整批资源。一个 SG 请求的成员共用设备完成状态，读失败时各成员保留此前已确认的 completed 前缀。每个请求保留独立的 I/O context、期限、状态、DMA buffer 和成本身份。batch 只占一个逻辑调用 owner，`active` 统计逻辑调用数，共用队列的 `inflight` 是已经发布的请求数的唯一计数；释放某个槽不会提前释放整批 owner。不同字节区间仍可能落在同一物理扇区，因此本批内涉及同扇区的 span 按输入顺序串行推进读改写，其他 span 可并发；不会因两个独立 bounce 快照覆盖相邻字节。

## 用户态块设备

块设备节点按真实 `st_rdev` 查找登记设备，普通打开建立独立的 block OFD，保留设备节点路径与设备引用，不进入普通文件页缓存。read/write/readv/writev 支持非扇区对齐的字节范围；pread/pwrite 不修改共享偏移，dup/fork 别名共同持有同一偏移和设备打开引用。读取容量末尾返回 0，跨末尾写返回可容纳的前缀，末尾开始的非空写返回 ENOSPC；lseek 仅允许设备容量以内的位置。普通打开可以与挂载或独占打开并存，`O_EXCL` 与挂载 claim 竞争同一个独占 owner。设备、挂载、OFD 和请求引用全部释放后才能注销。

块设备 ioctl 支持 `BLKGETSIZE`（512 字节扇区数）、`BLKGETSIZE64`（字节数）、`BLKSSZGET`、`BLKROGET/SET`、`BLKBSZGET/SET` 和 `BLKFLSBUF`。软件块大小是设备共享状态，只接受不小于逻辑扇区、介于 512 和架构页大小之间的 2 的幂；设置时需要已有独占资格或临时 claim。只读状态下仍允许以 `O_WRONLY/O_RDWR` 打开；新旧 OFD 的实际写入均返回 EPERM。fsync、fdatasync、`O_SYNC/O_DSYNC` 和 `BLKFLSBUF` 调用同一真实设备 flush；`BLKFLSBUF` 先持有挂载根路径并同步该设备上的文件系统，将缓存的文件数据和元数据交给设备。错误如实返回。没有裸设备缓存需要清除。mmap、`O_DIRECT`、discard、secure discard 和 zeroout 当前明确不支持。

`kernel_block_flush()` 表示设备持久化边界。块设备显式声明 UNKNOWN、WRITETHROUGH 或 WRITEBACK；未知能力且没有 flush 回调返回 UNSUPPORTED，只有声明 write-through 的设备可以省略 flush。VirtIO 协商 `VIRTIO_BLK_F_FLUSH`（bit 9），发送独立的 type 4 请求，只有 header/status 两个描述符，不带数据。未协商 CONFIG_WCE，因此 FLUSH 存在表示 writeback，缺失表示 write-through；驱动不修改缓存模式。该规则依据固定 Linux `references/linux/drivers/block/virtio_blk.c` 的 `virtblk_get_cache_mode()`（`f4cdf7ca9a1fdcca413157df19753f388a5a224e`）与 QEMU `references/qemu/hw/block/virtio-blk.c`（v11.1.0，`84f07211cc5b4fc6a371559bf8a5de4fb068e648`）。设备错误和超时保持所属状态，flush 请求单独计数。

此后端限定于 QEMU 同步 MMIO reset：写零后必须读回零，才能把借用的 DMA buffer 还给调用方或释放队列；违反该平台契约触发 fatal，普通请求超时在成功 reset 后仍返回 TIMEOUT。异步 reset 的实板需要独立的 DMA owner/完成协议，不能直接套用此检查。块 flush 不代表上层已实现 fsync、日志或崩溃恢复。

reset 停止 DMA 的依据是固定 `references/qemu/hw/virtio/virtio-mmio.c` 的 `virtio_mmio_soft_reset()`/STATUS 写入路径，以及 `references/qemu/hw/block/virtio-blk.c` 的 `virtio_blk_reset()` 中 `blk_drain()`；版本为 v11.1.0，commit `84f07211cc5b4fc6a371559bf8a5de4fb068e648`。

扇区对齐且位于 direct map 的目标缓冲区直接作为 DMA 地址，连续多扇区合并成一个请求；写入请求使用 `VIRTIO_BLOCK_REQUEST_OUT` 且数据描述符不设 `VIRTQ_DESC_WRITE`。非整扇区写入通过 bounce buffer 执行读-改-写（Read-Modify-Write）：先读出包含该范围的完整扇区，将修改字节合并到 bounce buffer，再将该扇区写回磁盘。启动阶段显式轮询；生产调度启用后，PLIC IRQ 先确认设备中断，再读取 used ring 并唤醒任务，使确认期间的新完成保留通知。轮询和期限唤醒都先收割已发布完成，再判定请求超时；期限唤醒只做一次收割，运行期不连续轮询。单次和批量调用在等待前都先收割 used；登记等待期间若发现 DMA 已完成而 IRQ 尚未处理，也经同一完成校验路径更新 request 状态，再决定是否 park。设备已完成的调用不增加 sleeps；没有新完成时仍阻塞，不建立运行期轮询。运行期满槽和设备等待均不可中断地睡眠，idle/IRQ 提交会 fatal，不能退回忙等。超时采用 DTB timebase 的30秒有限期限，从请求发布时计算，先停止提交并 reset，确认 DMA 停止后统一完成未完成请求；设备保持失败态，不自动重试写。IRQ 与超时在同一短关中断区仲裁，完成、唤醒和槽归还各一次。期限用于识别停滞，不是正常I/O延迟上限；合法FLUSH可能等待宿主整个后备文件落盘，一秒会误判。固定Linux v7.2的 `references/linux/block/blk-mq.c` 同样采用30秒默认块队列期限；BoarOS仍在超时后冻结设备，不承诺Linux的恢复策略。

完成通知与资源归还分开：每个请求或批次等待自己的完成队列；DMA 完成只归还描述符并通知完成 owner，请求结果消费后才归还请求槽。普通调用入场、flush 等待逻辑调用排空、描述符、请求槽和批量工作区分别等待对应条件。只有 active 降到零才通知 flush，屏障结束后才放行普通调用。仍有在途请求又需要补发的批次，同时登记自己的完成和缺少的资源，不等无关的设备广播。故障确认 reset 后通知所有终止条件，也通知同轮先完成但尚未消费的 owner。

批量读在调用任务即将继续等待时报告已完成 span；整批排空直接返回结果，不再逐项补发通知。同一 SG 链内的页一起完成。该 span 的全部 DMA 返回后才报告，回调后不再改写其 payload。描述、回调 context 和整批 buffer 的借用仍持续到同步调用返回。单个 span 的错误或提前可用不会释放其他在途资源。

flush 先阻止新逻辑调用并排空此前逻辑调用，再发送 FLUSH，完成后才放行后续调用；已入场的 batch 在 flush 等待期间仍可补发余下 span，避免屏障与未发布项互锁。无 FLUSH 特性的 write-through 同样经过软件排空屏障。batch 收到一个错误后停止发布所有剩余项，包含已读入 bounce 但尚未写出的 RMW；已发布 DMA 继续收割，或在期限到达时 reset 并确认 DMA 停止，最后才返回实际失败。取消和信号不会中断内部等待或提前归还 buffer。普通构建记录提交数、实际最大已发布在途数、IRQ、完成等待睡眠/唤醒、队列等待及错误，不把已预留但未发布槽计入在途。仅 `BOAROS_COST_DIAGNOSTICS=1` 累计在途深度积分（inflight-ticks，单位 深度·tick）、忙时、总时间、队列等待与服务时间，销毁时将这些时间写入最终统计；普通构建不为这类统计读取时钟，相关字段保持零。请求期限仍在所有构建中正常检查。

PLIC 路由由 `dtb_read_irq_info()` 根据 CPU interrupt-controller phandle、启动 hart 的 supervisor cause 9 和 VirtIO interrupt-parent 关联；不硬编码 IRQ/context 编号。外部中断执行 claim、设备分派/ack、complete；IRQ 不分配或做文件清理。块/VFS 同步接口不变，设备内部允许多个调用同时等待。实板、IOMMU、非一致 DMA 和 SMP 仍不在此边界内。

完成队列的冷路径诊断在 reset 前输出 `block queue fault`：used 快照与消费差值超过槽容量为 `used-overflow`；完成项的描述符 head 超出队列范围、指向数据/status 描述符为 `used-element`；legacy 的零长度完成同样被拒绝；指向未发布 head、重复 head 或非 submitted 请求为 `slot-state`；设备 status 超出 OK/IOERR/UNSUPP 为 `device-status`。输出包含 transport context/device status、中断快照、transport、队列虚拟/物理地址及容量，used、consumed、avail、入口观察到的完成数，逻辑调用/在途数和 reserved/published/complete 槽数；每个固定槽记录 state、owner/completion 指针值、status、type、字节数、扇区与期限。只读取驱动持有的 transport、队列和最多八个固定槽，不追 owner/completion 或设备提供的描述符地址，不分配、不增加重试。used 是触发校验的快照，consumed 包含已经取出的非法项，avail/status 是打印期间的标量；它不是跨 DMA 的原子快照。modern IN 只有 used length 覆盖全部数据和末尾 status、且 status 成功时才交付数据；零长和其他短完成直接使该请求返回 I/O 错误，不读取未覆盖的 status，不补零，也不因此复位整台设备。合法 used 已结束本次 DMA，完成 owner 照常归还，后续请求仍可执行。legacy 保留现有长度兼容范围。正常完成、短完成和普通 IOERR/UNSUPP 不输出这段诊断，其他损坏的 reset 与 owner 回收契约不变。重复完成若发生在本次合法完成已收割之后，设备仍失败，但不会覆写已完成请求的真实结果。

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

块测试将两个 transport 分别与 writeback/writethrough 组合，检查协商结果、flush 完成与独立统计，并覆盖单批八个 direct DMA、32 个物理不连续 4 KiB 页组成的 128 KiB SG 读写（每方向恰好一条请求、页间 canary 与资源归还）、非对齐相邻字节/跨扇区的 RMW 回读及只读拒绝。host 测试检查未知能力拒绝和错误传播，以及 batch 的整批预检、零项、重叠、32 项边界、顺序回退和可选回调；`tests/host/block_registry.c` 检查稳定设备号查找、重复登记拒绝、跨设备独立 claim、同设备重复 claim 拒绝及 claim 释放后注销；`tests/host/block_file_cases.h` 通过真实 files/OFD、MM 用户复制和内核睡眠锁检查裸设备字节/向量/定位 I/O、dup/fork 共享偏移、容量边界、ioctl、只读、flush 错误及最终引用归还；`tests/host/block_fault.c` 提供以文件为稳定镜像、以内存为易失缓存的 512 字节原子写模型，可选择持久化任意事件、丢弃未同步写、注入 write/flush 失败。模型自测不是文件系统恢复验收。

`virtio_block_diagnostics.py` 仅在临时副本中将 RISC-V fence/time 指令替换为宿主 fence/受控时钟，编译实际驱动其余代码。legacy/modern 的 literal split-ring 反例检查上述四种错误分类、reset 前字段、非法指针值不被追踪、真实返回状态、一次发布及最终队列回收；正常完成与普通设备错误检查无诊断输出。它不验证真实 DMA 顺序、中断时序或 QEMU 完成行为。

无完成反例验证timeout快照先于reset、返回值仍为TIMEOUT；独立设备侧计数模型在每种
transport执行65544次请求，覆盖八槽复用、反序完成、重复IRQ及16位索引绕回，检查
已发布head不重复、在途不超过八条且最终归零。它没有重现历史QEMU告警，不能证明
所有设备交错都安全。真实NBD的八槽超时用例另检查reset前已记录全部在途槽和队列计数。

请求、扇区与 direct/bounce 计数提供当前结构计数；QEMU 功能测试不能证明 VisionFive 2 上的吞吐、延迟、cache coherency 或 CPU 忙等成本。开发板接入后需要用相同镜像分别记录冷启动读量、周期、吞吐和 CPU 占用，再决定请求合并深度、队列并行度及 IRQ 唤醒优先级。

`test-io-sleep-riscv` 使用 NBD 控制握手暂扣和乱序释放响应：两个不同文件冷读必须先形成两个请求，期间计算与无关缓存命中完成；八槽满队列后验证逆序完成、flush 前后顺序及超时/reset。legacy/modern × writeback/writethrough 四种配置均运行。禁用 QEMU 请求合并，避免两个相邻 guest 请求合成一个 NBD 命令掩盖门槛；不靠宿主 sleep 猜时序。失败保留 guest/server 日志和镜像。

batch 因果用例先要求同一个调用在任何响应释放前发布八项。随后用两个外部 writer 占槽：writeback 保持本批六个响应全部暂扣，释放外部响应后要求出现至少一项补发，再逆序释放本批已发布项并验证最后尾项出现在 guest FLUSH 前；writethrough 逆序释放首批八项，随后逐项释放两条补发请求，检查完整批次、屏障和持久字节，但不单独要求外部槽先于本批槽完成。

writethrough 的响应波次依据固定 QEMU v11.1.0（`references/qemu`，commit `84f07211cc5b4fc6a371559bf8a5de4fb068e648`）：`block/block-backend.c` 的 `blk_co_do_pwritev_part()` 在写缓存关闭时加 FUA，`block/io.c` 的 `bdrv_driver_pwritev()` 在 backend 不支持 FUA 时以 `bdrv_co_flush()` 模拟。当前 NBD fixture 仅声明 SEND_FLUSH，故外部 WRITE 回复不是外部 VirtIO 写完成的充分握手；这些 backend FLUSH 回复即时释放。

错误用例使第二个已发布 batch 写先失败，保留另一个 DMA，验证取消仍不能提前返回，再释放该 DMA 并回读确认未发布项未写入；正常单写与 batch 混合的八槽超时由 reset 结束，队列页和任务栈最终回收。等待前有限收割 used，没有完成时仍阻塞等待。

RT 组合进展由 `make test-multi-disk-rt-riscv`（调用 `tests/multi-disk-io-riscv.py --rt-load`）单独验证：默认全局 RT 预算下，持续 FIFO/RR 子任务存在时，普通父任务与两台真实 NBD 的 READ/WRITE/FLUSH 均可完成，随后检查子任务、动态挂载、根页/堆/栈回收和两盘持久字节。该模式使用同一个 `tests/userland/multi_disk_io.c` 的显式 `/rt-load` fixture 分支，原错误隔离模式不变；详见[可睡眠存储](../learning/sleepable-storage.md#默认-rt-带宽下的存储进展)。

双盘故障 runner 在确认隔离检查点后，通过 NBD `cut` 收到两盘的明确回执，再终止
QEMU，避免直接 kill 截断健康盘的响应。诱导断线后的原始日志保留；进展和内核错误
判断截至受控 cut 前，冷启动读回和 fsck 仍必须通过，服务器非零退出不被忽略。
失败产物另含 guest/两盘退出码及 cut 请求/回执状态，详见[收口竞态](../learning/sleepable-storage.md#双盘受控收口2026-10-06)。

成本诊断的请求带提交时标量 epoch/lane，正常 IRQ 和 timeout/reset 均按该身份记账；registry 磁盘归属与 unknown 字节见[成本观测](kernel-cost.md)。

2026-10-01 最终四组合 io-sleep 与真实日志/消费者路径均达到最大八个已发布请求；这不表示串行读或阶段间 FLUSH 也能保持八槽并行。消费者定点诊断的 ready/resume 关联只覆盖实际唤醒 owner 的请求，batch 会合并唤醒，不能把这一子集与旧所有请求样本直接比较平均延迟。存储流水线的恢复通过、Parent 收益未达标与完整输入身份见[最终验收](../learning/cost-baseline.md#s9-存储流水线验收2026-10-01)。

批量读的旧驱动顺序回退在生产函数宿主模型中只能发布一个请求，新增“暂扣至八项”
测试超时；新驱动同时发布八项。两种 transport 的反序完成、单项错误、错误时另项
仍在途、超时 reset、非对齐、多片段和 505 字节成功前缀由同一实际驱动模型验证，
这些是受控设备边界证据。新增 SG 模型检查 4/8/32/64 项队列、八段及 32 段内容与边界；32 个 4 KiB 页之间保留 4 KiB 空隙和 canary，检查一个 128 KiB 请求、一次通知、无 bounce，以及受 SIZE_MAX=1024、SEG_MAX=3 限制时的拆分、空闲描述符有洞时仍保留其他链、一次通知多个请求，以及 scalar/batch 的 DMA 先于 IRQ 零 park；设备在通知时发布 used，不靠驱动时钟推进完成。真实 `test-io-sleep-riscv` 四组合额外要求
单次调用先产生八个 NBD READ，释放七个后取消/唤醒仍不得返回，最后一个回复后才
检查完整数据、独立状态和 owner 回收。文件批量读的八个连续页则必须形成一个 32 KiB SG 请求，暂扣该响应期间取消不能使批量读提前返回。

VFS/ext4 和页缓存预读通过现有 batch span 接口提供独立目标页，连续磁盘窗口由块驱动形成 SG；预读暂扣用例按完整窗口字节数验证一个请求。发布吞吐候选、默认/最大候选恢复结果和未测平台见[数据路径最终报告](../learning/data-path-budget-experiments.md)。


IRQ 启用后的读取 fixture 检查两个真实任务的内容、两个新增请求和扇区完成量，
并要求没有新增错误或超时；同时报告 interrupts 和 sleeps。used 在等待前完成时，
收割会确认设备中断，PLIC 可以一次也未进入，因此不能要求快请求必须产生 IRQ。
真实延迟完成、睡眠及唤醒由上面的 NBD 用例检查。2026-10-10 的 case47 旧记录
缺少 IRQ 等计数，不能据此唯一归因；原内核和输出保留。
