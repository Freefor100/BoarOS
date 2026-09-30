# RISC-V VirtIO MMIO 块设备模块

本文描述 QEMU `virt` 上当前同步读写块设备路径。DTB 节点来源见[DTB 与启动内存布局模块](dtb-memory.md)，上层文件语义见[VFS 与 ext4 模块](vfs-ext4.md)。

## 发现与 transport

`dtb_read_boot_info()` 收集启用的 `compatible = "virtio,mmio"` 节点，翻译父总线 `ranges` 后按物理地址排序并拒绝重叠。最终 Sv39 页表只映射实际发现的 MMIO 范围。`arch/riscv/virtio_mmio_block.c` 依次探测这些 transport：非 block device 跳过，所有成功初始化的 block device 均登记，首个成为当前根设备，其余可通过设备节点挂载；已经表明自己是 block 但 transport/feature 不受支持时准确失败，不继续扫描磁盘内容。

当前同时接受 VirtIO MMIO version 1 legacy 和 version 2 modern。modern 路径要求协商 `VIRTIO_F_VERSION_1` 并检查设备回显 `FEATURES_OK`；legacy 路径使用传统 feature 寄存器和状态序列，不伪造 modern feature。驱动在两种 transport 下均探测 `VIRTIO_BLK_F_RO`（bit 5）：若设备提供只读标志（如 QEMU `readonly=on`），驱动接受该 feature 并设置 `block.write = 0`；若设备可写，则安装 `virtio_block_write`。设备状态按各自规范推进，失败和销毁会复位设备后再回收队列页。QEMU 默认 legacy 和显式 `-global virtio-mmio.force-legacy=false` 的 modern 配置都由同一驱动验证。

## 请求和 DMA

通用 `kernel_block_device` 暴露容量、逻辑块大小和精确字节范围的同步读写接口（`kernel_block_read_at`/`kernel_block_write_at`），统一检查空参数、越界和整数溢出。两种 transport 共用最多 32 个描述符的 split queue，每请求占三描述符，最多八个在途槽；按设备 QueueNumMax 向下取可用容量，至少一个槽。modern 分配连续 8 KiB，legacy 分配连续 16 KiB 并以 4 KiB 对齐 used ring。每槽独立持有 header/status、512 字节 bounce、任务 owner、完成状态与等待队列；同步调用拥有槽位直到完成确认，不能在 DMA 期间复用。

`kernel_block_flush()` 表示设备持久化边界。块设备显式声明 UNKNOWN、WRITETHROUGH 或 WRITEBACK；未知能力且没有 flush 回调返回 UNSUPPORTED，只有声明 write-through 的设备可以省略 flush。VirtIO 协商 `VIRTIO_BLK_F_FLUSH`（bit 9），发送独立的 type 4 请求，只有 header/status 两个描述符，不带数据。未协商 CONFIG_WCE，因此 FLUSH 存在表示 writeback，缺失表示 write-through；驱动不修改缓存模式。该规则依据固定 Linux `references/linux/drivers/block/virtio_blk.c` 的 `virtblk_get_cache_mode()`（`f4cdf7ca9a1fdcca413157df19753f388a5a224e`）与 QEMU `references/qemu/hw/block/virtio-blk.c`（v11.1.0，`84f07211cc5b4fc6a371559bf8a5de4fb068e648`）。设备错误和超时保持所属状态，flush 请求单独计数。

此后端限定于 QEMU 同步 MMIO reset：写零后必须读回零，才能把借用的 DMA buffer 还给调用方或释放队列；违反该平台契约触发 fatal，普通请求超时在成功 reset 后仍返回 TIMEOUT。异步 reset 的实板需要独立的 DMA owner/完成协议，不能直接套用此检查。块 flush 不代表上层已实现 fsync、日志或崩溃恢复。

扇区对齐且位于 direct map 的目标缓冲区直接作为 DMA 地址，连续多扇区合并成一个请求；写入请求使用 `VIRTIO_BLOCK_REQUEST_OUT` 且数据描述符不设 `VIRTQ_DESC_WRITE`。非整扇区写入通过 bounce buffer 执行读-改-写（Read-Modify-Write）：先读出包含该范围的完整扇区，将修改字节合并到 bounce buffer，再将该扇区写回磁盘。启动阶段显式轮询；生产调度启用后，PLIC IRQ 先确认设备中断，再读取 used ring 并唤醒任务，使确认期间的新完成保留通知。轮询和期限唤醒都先收割已发布完成，再判定一秒超时；期限唤醒只做一次收割，运行期不连续轮询。运行期满槽和设备等待均不可中断地睡眠，idle/IRQ 提交会 fatal，不能退回忙等。超时采用 DTB timebase 的一秒期限，先停止提交并 reset，确认 DMA 停止后统一完成未完成请求；设备保持失败态，不自动重试写。IRQ 与超时在同一短关中断区仲裁，完成、唤醒和槽归还各一次。

flush 先阻止新请求并排空此前逻辑调用，再发送 FLUSH，完成后才放行后续请求；无 FLUSH 特性的 write-through 同样经过软件排空屏障。统计记录提交数、实际最大已发布在途数、IRQ、完成等待睡眠/唤醒、队列等待、运行期轮询及错误；不把已预留但未发布槽计入在途。

PLIC 路由由 `dtb_read_irq_info()` 根据 CPU interrupt-controller phandle、启动 hart 的 supervisor cause 9 和 VirtIO interrupt-parent 关联；不硬编码 IRQ/context 编号。外部中断执行 claim、设备分派/ack、complete；IRQ 不分配或做文件清理。块/VFS 同步接口不变，设备内部允许多个调用同时等待。实板、IOMMU、非一致 DMA 和 SMP 仍不在此边界内。

## 验证

```sh
make test-dtb-riscv
make test-block-riscv
make test-block-host
make test-io-sleep-riscv
```

测试使用真实 QEMU raw disk，分别以默认 legacy 和显式 modern 配置覆盖 feature/status negotiation、批量 direct DMA、非对齐 bounce、容量边界、超时/错误统计和队列页回收。默认无块设备的生产内核仍可进入 timer idle；挂入两种 transport 的根盘时由根启动测试继续消费本模块。

块测试将两个 transport 分别与 writeback/writethrough 组合，检查协商结果、flush 完成与独立统计。host 测试检查未知能力拒绝和错误传播；`tests/host/block_registry.c` 检查稳定设备号查找、重复登记拒绝、跨设备独立 claim、同设备重复 claim 拒绝及 claim 释放后注销；`tests/host/block_fault.c` 提供以文件为稳定镜像、以内存为易失缓存的 512 字节原子写模型，可选择持久化任意事件、丢弃未同步写、注入 write/flush 失败。模型自测不是文件系统恢复验收。

请求、扇区与 direct/bounce 计数提供当前结构成本基线；QEMU 功能测试不能证明 VisionFive 2 上的吞吐、延迟、cache coherency 或 CPU 忙等成本。开发板接入后需要用相同镜像分别记录冷启动读量、周期、吞吐和 CPU 占用，再决定请求合并深度、队列并行度及 IRQ 唤醒优先级。

`test-io-sleep-riscv` 使用 NBD 控制握手暂扣和乱序释放响应：两个不同文件冷读必须先形成两个请求，期间计算与无关缓存命中完成；八槽满队列后验证逆序完成、flush 前后顺序及超时/reset。legacy/modern × writeback/writethrough 四种配置均运行。禁用 QEMU 请求合并，避免两个相邻 guest 请求合成一个 NBD 命令掩盖门槛；不靠宿主 sleep 猜时序。失败保留 guest/server 日志和镜像。

RT 组合进展由 `make test-multi-disk-rt-riscv`（调用 `tests/multi-disk-io-riscv.py --rt-load`）单独验证：默认全局 RT 预算下，持续 FIFO/RR 子任务存在时，普通父任务与两台真实 NBD 的 READ/WRITE/FLUSH 均可完成，随后检查子任务、动态挂载、根页/堆/栈回收和两盘持久字节。该模式使用同一个 `tests/userland/multi_disk_io.c` 的显式 `/rt-load` fixture 分支，原错误隔离模式不变；详见[可睡眠存储](../learning/sleepable-storage.md#默认-rt-带宽下的存储进展)。

成本诊断的请求带提交时标量 epoch/lane，正常 IRQ 和 timeout/reset 均按该身份记账；registry 磁盘归属与 unknown 字节见[成本观测](kernel-cost.md)，现有设备统计保持原契约。
