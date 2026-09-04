# 用户内存访问模块

本文描述内核与当前已映射用户地址空间之间复制数据的稳定边界。页表格式和 MM 生命周期分别见 [RISC-V Sv39 分页模块](riscv-sv39.md)与[内核 MM 模块](kernel-mm.md)，消费者见[系统调用解码模块](kernel-syscall.md)和[进程文件资源模块](kernel-files.md)。

## 接口与职责

公共声明位于 `include/kernel/uaccess.h`，当前 RISC-V 实现在 `arch/riscv/uaccess.c`：

```c
enum kernel_uaccess_status kernel_copy_to_user(
    struct kernel_mm *mm,
    uint64_t user_destination,
    const void *kernel_source,
    size_t size,
    size_t *bytes_copied);

enum kernel_uaccess_status kernel_copy_from_user(
    struct kernel_mm *mm,
    void *kernel_destination,
    uint64_t user_source,
    size_t size,
    size_t *bytes_copied);

enum kernel_uaccess_status kernel_copy_string_from_user(
    struct kernel_mm *mm,
    char *kernel_destination,
    uint64_t user_source,
    size_t capacity,
    size_t *string_length);
```

构建期直接选择架构实现，不使用运行期 vtable。syscall、文件层和 exec 准备阶段从当前 task 借用 mutable MM 后调用这些接口，因为合法的 demand-zero 页可能在复制中首次提交。调用者不依赖 Sv39 PTE 位或物理地址布局。`uname(160)` 使用写入用户空间方向，路径和 exec 字符串使用有界字符串读取，exec 的指针向量使用固定长度读侧接口。公共 `kernel_user_range_check()` 在不访问页表的情况下校验整个用户半开区间。

## 范围、权限与部分复制

非零固定长度请求首先验证整个半开区间位于 Sv39 低半区 `[0, RISCV_SV39_USER_LIMIT)`；整数溢出或越过上限在复制前返回 `KERNEL_UACCESS_STATUS_FAULT`，`bytes_copied=0`。零长度请求不检查用户地址或内核 buffer，直接成功并报告 0。

合法范围按 4 KiB 基页边界切分。每个片段先经 `kernel_mm_lookup()` 查询物理地址和通用权限：若 PTE 尚未驻留，uaccess 用实际读/写方向调用 `kernel_mm_resolve_user_fault()`；anonymous `DEMAND_ZERO`、file-private 和 COW 页都复用该解析器，成功后重查 PTE。写入用户空间要求 `USER|WRITE`，读取用户空间要求 `USER|READ`，随后通过物理页分配器已绑定的 direct-map 访问函数复制。VMA 外、`PROT_NONE`、权限不足或文件整页越过 EOF 返回 `FAULT`；MM 状态、页表结构、分配失败或已映射物理页无法解析返回 `STATE`，不能伪装成普通用户 `EFAULT`。

固定长度复制不是事务。若前面的完整片段已经复制，后续页未映射或权限不足，函数保留已复制的连续前缀并精确报告 `bytes_copied`；这与 Linux usercopy 允许部分修改目标缓冲区的内部语义一致。文件 `read` 依据这个前缀提交 open-file offset；`uname` 和文件层只把 `FAULT` 转成用户可见的 `-EFAULT`，内核状态错误仍由 syscall/Trap 边界判为 fatal。

字符串接口最多读取 `capacity` 字节，成功时把 NUL 一并复制并用 `string_length` 返回不含 NUL 的长度。容量内无 NUL 返回 `TOO_LONG`；到达用户上限、未映射或不可读页返回 `FAULT`，同时保留并报告已复制前缀。它不接受容量 0，因而不会把未验证指针当作空字符串。

## 并发与性能边界

当前 uaccess 只对 scheduler 当前、已经激活的 MM 提交新页；VMA/PTE 检查会在真正分配前拒绝非法范围。单 hart、关中断 syscall 路径保证 lookup、fault-in/COW、物理页解析和复制之间没有并发 unmap/mprotect。接入 SMP 或共享 MM 时，必须一起定义 MM 读锁、页固定、原子页引用、分配失败、部分复制和 TLB shootdown 协议；公共 syscall ABI 不需要因此改变。

驻留页每个基页进行一次三级软件页表查询和一次物理页解析，随后执行页内线性字节复制；首次提交的页额外承担 VMA 二分查找、页分配/清零、PTE 写入和单页 `SFENCE.VMA`。uaccess 不切换 `satp`，也不修改 `sstatus.SUM`。文件 `read` 已以 4 KiB staging chunk 使用该路径，但当前只用结构成本和 QEMU 正确性测试约束，尚未取得开发板吞吐、TLB miss 或 cache 数据。应在真实工作负载上比较软件遍历与 RISC-V SUM+异常表快路径，再决定阈值或替换策略。

QEMU `virt` 与 VisionFive 2 共享这套 Sv39 实现，板级 RAM/MMIO 差异已由启动内存和页表建立隔离。LoongArch 后续为相同公共接口提供 16 KiB/三级页表实现，并使用自己的用户地址范围与硬件访问机制。

## 验证与限制

```sh
make test-uaccess-riscv
make test-syscall-riscv
make test-user-riscv
make test-riscv
```

聚焦测试分别覆盖 copy-to、copy-from 和字符串复制的合法同页/跨页、零长度、整体用户范围、读写权限、容量内缺少 NUL、跨入未映射页后的精确前缀、非法内核参数、已释放 MM 和物理页访问失败。VMA 测试让 copy-to 首次提交匿名 mmap 页；真实 U-mode 测试验证 `uname` 的跨页 Linux ABI；文件测试验证跨页路径、超过一页的读缓冲区、`-EFAULT` 与 offset 提交。

当前没有 SUM/异常表快路径、SMP 映射稳定协议或面向原子用户结构读取的序列化辅助接口。硬件用户 fault 的页分配耗尽会终止任务；syscall 内 uaccess 遇到同类耗尽仍分类为内核侧 `STATE`，尚未形成可返回的进程 OOM 协议。
