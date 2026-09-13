# RISC-V VirtIO MMIO 块设备模块

本文描述 QEMU `virt` 上当前只读块设备路径。DTB 节点来源见[DTB 与启动内存布局模块](dtb-memory.md)，上层文件语义见[VFS 与只读 ext4 模块](vfs-ext4.md)。

## 发现与 transport

`dtb_read_boot_info()` 收集启用的 `compatible = "virtio,mmio"` 节点，翻译父总线 `ranges` 后按物理地址排序并拒绝重叠。最终 Sv39 页表只映射实际发现的 MMIO 范围。`arch/riscv/virtio_mmio_block.c` 依次探测这些 transport：非 block device 跳过，首个成功初始化的 block device 成为当前根设备；已经表明自己是 block 但 transport/feature 不受支持时准确失败，不继续扫描磁盘内容。

当前同时接受 VirtIO MMIO version 1 legacy 和 version 2 modern。modern 路径要求协商 `VIRTIO_F_VERSION_1` 并检查设备回显 `FEATURES_OK`；legacy 路径使用传统 feature 寄存器和状态序列，不伪造 modern feature。驱动在两种 transport 下均探测 `VIRTIO_BLK_F_RO`（bit 5）：若设备提供只读标志（如 QEMU `readonly=on`），驱动接受该 feature 并设置 `block.write = 0` 保持纯只读块设备；若设备可写，则安装 `virtio_block_write`。设备状态按各自规范推进，失败和销毁会复位设备后再回收队列页。QEMU 默认 legacy 和显式 `-global virtio-mmio.force-legacy=false` 的 modern 配置都由同一驱动验证。

## 请求和 DMA

通用 `kernel_block_device` 暴露容量、逻辑块大小和精确字节范围的同步读写接口（`kernel_block_read_at`/`kernel_block_write_at`），统一检查空参数、越界和整数溢出。两种 transport 共用 size 8 split queue、三描述符请求链、状态字和 512 字节 bounce buffer；modern 队列占用一张 4 KiB 页，legacy 采用 4 KiB `GuestPageSize`/`QueueAlign`、连续对齐的 16 KiB 分配，并把 used ring 放在下一对齐边界。当前同一时刻只有一个请求。

扇区对齐且位于 direct map 的目标缓冲区直接作为 DMA 地址，连续多扇区合并成一个请求；写入请求使用 `VIRTIO_BLOCK_REQUEST_OUT` 且数据描述符不设 `VIRTQ_DESC_WRITE`。非整扇区写入通过 bounce buffer 执行读-改-写（Read-Modify-Write）：先读出包含该范围的完整扇区，将修改字节合并到 bounce buffer，再将该扇区写回磁盘。完成路径轮询 used ring，以 DTB timebase 的一秒为上限；超时后先复位设备，不能在设备仍可能 DMA 时归还队列页。统计区分请求、direct/bounce 请求、读写扇区、超时和设备错误。

该轮询路径是外部中断、等待队列和阻塞唤醒尚未建立时的必要边界，不属于 VFS 接口。以后可在设备后端改为 IRQ + sleep/wake，而不改变 block/VFS/ext4 调用者。开发板的 SD/eMMC/PCI transport、IOMMU 和非一致 DMA 也位于此边界之外。

## 验证

```sh
make test-dtb-riscv
make test-block-riscv
```

测试使用真实 QEMU raw disk，分别以默认 legacy 和显式 modern 配置覆盖 feature/status negotiation、批量 direct DMA、非对齐 bounce、容量边界、超时/错误统计和队列页回收。默认无块设备的生产内核仍可进入 timer idle；挂入两种 transport 的根盘时由根启动测试继续消费本模块。

请求、扇区与 direct/bounce 计数提供当前结构成本基线；QEMU 功能测试不能证明 VisionFive 2 上的吞吐、延迟、cache coherency 或 CPU 忙等成本。开发板接入后需要用相同镜像分别记录冷启动读量、周期、吞吐和 CPU 占用，再决定请求合并深度、队列并行度及 IRQ 唤醒优先级。
