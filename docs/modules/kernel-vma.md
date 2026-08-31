# 虚拟内存区域（VMA）模块

本文描述用户地址空间的逻辑区间元数据。页表格式与硬件失效见 [RISC-V Sv39 分页模块](riscv-sv39.md)，MM owner 生命周期见 [内核 MM 模块](kernel-mm.md)，原理见[内存管理学习总结](../learning/memory-management.md)。

## 范围与入口

| 文件 | 当前职责 |
|---|---|
| `include/kernel/vma.h`、`mm/vma.c` | VMA 描述符、排序集合、查找/选址以及准备—提交式区间编辑 |
| `include/kernel/mm.h`、`arch/riscv/mm.c` | 匿名 `mmap`、`munmap`、`mprotect`、`brk` 和缺页解析 |
| `arch/riscv/user_elf.c` | 登记已物化的静态 ELF、栈 reserve 与初始 heap 布局 |
| `tests/riscv/vma_cases.c`、`tests/vma-riscv.sh` | 区间语义、失败原子性、fork、缺页与 1/64/1024 VMA 查找基线 |

一个 VMA 用半开区间 `[start, end)`、R/W/X 权限、kind、role、fault policy 和可选 backing 表示一段逻辑有效的用户地址。PTE 只表示其中已经驻留的 4 KiB 页；没有 PTE 不等于没有 VMA。`RESIDENT_REQUIRED` 用于已经物化的静态 ELF，`DEMAND_ZERO` 用于栈、heap 和匿名 mmap。guard 和被 `munmap` 的洞不属于任何 VMA。

当前生产 MM 入口除静态登记/查询外还包括：

```c
enum kernel_mm_status kernel_mm_mmap_anonymous(
    struct kernel_mm *mm, uint64_t hint, uint64_t length,
    uint32_t permissions, uint32_t flags, uint64_t *address);

enum kernel_mm_status kernel_mm_munmap(
    struct kernel_mm *mm, uint64_t address, uint64_t length);

enum kernel_mm_status kernel_mm_mprotect(
    struct kernel_mm *mm, uint64_t address, uint64_t length,
    uint32_t permissions);
```

当前只有 anonymous-private backing。`FILE_PRIVATE` 类型保留了文件偏移字段的不变量，但尚无公开创建入口和文件页引用生命周期，不能视为 file-backed mmap 已实现。

## 排序、选址与合并

集合是 MM record 通过 kernel heap 拥有的动态排序数组。按地址查找使用二分搜索；插入、拆分、删除和合并可能移动后缀。相邻 VMA 只有在权限、kind、role、fault policy、backing 以及连续文件偏移全部一致时才合并，因而不会跨越缺页策略或资源生命周期边界。

非 fixed mmap 先尝试页对齐 hint；冲突时在 `brk_limit`（当前也是栈 guard 起点）以下 top-down 查找空洞，不做 ASLR。`MAP_FIXED_NOREPLACE` 在任意重叠时返回冲突且不改输出；`MAP_FIXED` 删除范围内所有旧 VMA/PTE 后放入新匿名映射。RISC-V 不能编码 W&&!R 用户叶子，因此 MM 把仅写请求规范化为 RW；`PROT_NONE` 用零权限 VMA 表示。

排序数组的查找成本为 `O(log n)`，编辑成本最坏为 `O(n)`，内存连续且每个 VMA 不需要独立节点分配。`make test-vma-riscv` 在 QEMU `virt` 单 hart 上预热后分别对 1、64、1024 个间隔 VMA 重复 256 次 lookup，并输出平均 `rdcycle` 读数；脚本只要求三组基线存在，不把 QEMU 周期值当成开发板性能结论。若真实 mmap 密集工作负载显示数组移动或锁持有时间成为瓶颈，可在保持当前 MM 语义的前提下换成平衡树/Maple Tree 类结构。

## 区间编辑事务

任意 `munmap`、fixed replace 和 `mprotect` 都可能在区间两端拆分 VMA。集合因此使用两阶段内部协议：

```text
prepare: 校验范围/覆盖条件，只为确实需要的拆分或插入预留 descriptor 容量
page table commit: 修改 PTE、刷新 TLB，并保留失败时的物理页 owner
VMA commit: 不再分配，按 generation 验证 prepared edit 后拆分/删除/改权/合并
```

`prepare` 不改变逻辑集合；相同集合发生任何成功编辑后，旧 edit 因 generation 不匹配而返回 `STATE`。`munmap` 允许范围包含洞，且纯洞删除不需要扩容；`mprotect` 要求整个范围由一个或多个相邻 VMA 无洞覆盖，否则在修改 PTE 前返回 `NOT_MAPPED`。这保证 metadata OOM 不会发生在硬件映射已经改变之后。当前单 hart、关中断路径使 PTE 与 VMA 提交不被并发观察；SMP 时必须用 MM 写锁和远端 TLB shootdown 扩展同一不变量。

## `brk`、fork 与回收

raw `brk` 保存精确字节地址，只在跨页时编辑 VMA。增长增加 RW anonymous `HEAP/DEMAND_ZERO` 区间；冲突、metadata OOM 或 retired 页暂时无法释放时返回旧 break。缩小使用通用范围删除，因此即使用户先 `munmap` heap、`mprotect` 其中一段，或用 fixed mmap 替换局部区域，收缩仍能撤销 `[page_end(new), page_end(old))` 中的全部映射，而不依赖“只有一个连续 heap VMA”的阶段假设。

`kernel_mm_fork()` 深复制驻留用户页并克隆整套 VMA 描述符，父子随后独立编辑。最后一个 MM owner 的回收顺序固定为 VMA metadata、Sv39 私有页/页表、MM record；metadata release 失败停在可重试的 `KERNEL_MM_CLEANUP_VMAS`，不会提前丢失 PTE owner。

## 验证与限制

```sh
make test-vma-riscv
make test-mmap-riscv
make test-brk-riscv
make test-root-init-riscv
make test-riscv
```

聚焦测试覆盖相邻合并、孔洞、冲突、两端拆分、hint/top-down、fixed replace/noreplace、`PROT_NONE` 内容保持、W→RW、mprotect 全覆盖、munmap 洞语义、打洞后的 brk、fork 中受保护页复制、retired owner、metadata OOM/cleanup retry 和资源基线。真实 ext4 `/init` ELF 从 U-mode 调用 mmap/mprotect/munmap，实际读写 demand-zero 页并检查 errno 与最终回收。

当前没有 file-backed/shared mapping、COW、`MAP_POPULATE`、内存承诺 accounting、ASLR、VMA 数量上限或 SMP 并发修改。`MAP_STACK` 目前只是兼容性标志，不改变增长方向；`MAP_NORESERVE` 与全局尚无 commit accounting 的当前策略等价。
