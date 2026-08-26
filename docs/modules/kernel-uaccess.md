# 用户内存访问模块

本文描述内核向当前已映射用户地址空间复制数据的稳定边界。页表格式和 MM 生命周期分别见 [RISC-V Sv39 分页模块](riscv-sv39.md)与[内核 MM 模块](kernel-mm.md)，首个真实消费者见[系统调用解码模块](kernel-syscall.md)。

## 接口与职责

公共声明位于 `include/kernel/uaccess.h`，当前 RISC-V 实现在 `arch/riscv/uaccess.c`：

```c
enum kernel_uaccess_status kernel_copy_to_user(
    const struct kernel_mm *mm,
    uint64_t user_destination,
    const void *kernel_source,
    size_t size,
    size_t *bytes_copied);
```

构建期直接选择架构实现，不使用运行期 vtable。syscall 层从当前 task 借用只读 MM 后调用该接口，不依赖 Sv39 PTE 位或物理地址布局。当前只提供已经由 `uname(160)` 使用的复制方向；没有为接口对称而提前增加 `copy_from_user`。

## 范围、权限与部分复制

非零请求首先验证整个半开区间 `[user_destination, user_destination + size)` 位于 Sv39 低半区 `[0, RISCV_SV39_USER_LIMIT)`；整数溢出或越过上限在写入前返回 `KERNEL_UACCESS_STATUS_FAULT`，`bytes_copied=0`。零长度请求不检查用户地址或源地址，直接成功并报告 0。

合法范围按 4 KiB 基页边界切分。每个片段先经 `kernel_mm_lookup()` 查询物理地址和通用权限，只有同时具有 `KERNEL_MM_USER|KERNEL_MM_WRITE` 才通过物理页分配器的已绑定 direct-map 访问函数写入。未映射或权限不足返回 `FAULT`；MM 状态、页表结构或已映射物理页无法解析返回 `STATE`，不能伪装成用户 `EFAULT`。

复制不是事务。若前面的完整片段已经写入，后续页未映射或不可写，函数保留已复制的连续前缀并精确报告 `bytes_copied`；这与 Linux usercopy 允许部分修改目标缓冲区的内部语义一致。`uname` 只把 `FAULT` 转成用户可见的 `-EFAULT`，内核状态错误仍由 syscall/Trap 边界判为 fatal。

## 并发与性能边界

当前单 hart 内核没有运行期 `unmap/mprotect`、COW 或按需缺页，运行中任务拥有的 MM 在每个 lookup 与 direct-map 写入之间保持稳定。timer 可以抢占 syscall，但被抢占任务及其 MM 不会在调用栈恢复前被 reaper 释放。接入 SMP 或并发映射修改时，必须在 uaccess 内部增加 MM 读锁、页固定或等价的读侧协议，并与缺页、COW 和 TLB shootdown 协调；公共 syscall ABI 不需要因此改变。

当前每个涉及的基页进行一次三级软件页表查询和一次物理页解析，随后执行页内线性字节复制；路径不分配内存，不切换 `satp`，不执行 `SFENCE.VMA`，也不修改 `sstatus.SUM`。这适合当前固定 390 字节、至多跨两页的 `uname`。大块 `read/write` 出现后，应在真实 QEMU/开发板工作负载上比较软件遍历与 RISC-V SUM+异常表快路径，再决定阈值或替换策略；当前结果不能推导大缓冲区吞吐。

QEMU `virt` 与 VisionFive 2 共享这套 Sv39 实现，板级 RAM/MMIO 差异已由启动内存和页表建立隔离。LoongArch 后续为相同公共接口提供 16 KiB/三级页表实现，并使用自己的用户地址范围与硬件访问机制。

## 验证与限制

```sh
make test-uaccess-riscv
make test-syscall-riscv
make test-user-riscv
make test-riscv
```

聚焦测试覆盖合法同页/跨页复制、零长度、整体用户范围、只读页、跨入未映射页后的精确前缀、非法内核参数、已释放 MM 和物理页访问失败。真实 U-mode 测试验证 `uname` 的跨页 Linux ABI、三类 `-EFAULT`、timer 抢占、共享 MM 和最终资源回收。

当前没有 `copy_from_user`、用户字符串复制、缺页调入、COW、运行期 unmap、SUM/异常表快路径或 SMP 映射稳定协议。
