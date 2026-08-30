# 虚拟内存区域（VMA）模块

本文描述用户地址空间的逻辑区间元数据。页表格式与硬件失效见 [RISC-V Sv39 分页模块](riscv-sv39.md)，MM owner 生命周期见 [内核 MM 模块](kernel-mm.md)，背景知识见[内存管理学习总结](../learning/memory-management.md)。

## 范围与入口

| 文件 | 当前职责 |
|---|---|
| `include/kernel/vma.h`、`mm/vma.c` | 定义 VMA 描述符、按地址排序的集合及其创建、复制、查询、销毁 |
| `include/kernel/mm.h`、`arch/riscv/mm.c` | 让 RISC-V MM record 拥有 VMA 集合，并提供受 MM 生命周期约束的匿名插入、查询与缺页解析 |
| `arch/riscv/user_elf.c` | 为已经物化的静态 ELF 和完整栈保留区登记第一个真实消费者 |
| `tests/riscv/vma_cases.c`、`tests/vma-riscv.sh` | 覆盖合并、冲突、fork 复制和 VMA 清理重试 |

一个 VMA 用半开区间 `[start, end)`、R/W/X 权限、kind、role、fault policy 和可选 backing 表示一段**逻辑有效**的用户虚拟地址。PTE 只表示其中当前已经驻留的 4 KiB 页；没有 PTE 不等于没有 VMA。`RESIDENT_REQUIRED` 要求合法页已经由装载路径物化，`DEMAND_ZERO` 允许硬件用户缺页按 VMA 权限建立匿名零页。例如栈 VMA 覆盖完整 8 MiB reserve，但初期只有靠近栈顶的一小段有 PTE；其余页在用户 load/store 首次访问时分配。guard 页不属于栈 VMA，访问它继续按未映射处理。

当前公开的 MM 入口为：

```c
enum kernel_mm_status kernel_mm_vma_enable(
    struct kernel_mm *mm,
    struct kernel_heap *heap);

enum kernel_mm_status kernel_mm_vma_insert_anon(
    struct kernel_mm *mm,
    uint64_t start,
    uint64_t end,
    uint32_t permissions,
    enum kernel_vma_role role,
    enum kernel_vma_fault_policy fault_policy);

enum kernel_mm_status kernel_mm_vma_lookup(
    const struct kernel_mm *mm,
    uint64_t virtual_address,
    struct kernel_vma *vma);
```

启用只能在一个 LIVE owner 的 MM 上成功一次；插入同样要求未共享的 LIVE MM。范围必须是 Sv39 用户低半区内、4 KiB 对齐、非空的区间，权限至少有一个 R/W/X，且不能形成 W&&!R。`lookup` 对合法但不属于任何 VMA 的地址返回 `KERNEL_MM_STATUS_NOT_MAPPED`。当前只有匿名 VMA 进入生产路径；`FILE_PRIVATE` 描述符字段尚没有公开创建入口或文件页引用生命周期，不能把它当作已经支持的 file-backed mapping。

## 排序、合并与生命周期

集合是由内核堆拥有的动态排序数组，以起始地址二分定位。插入拒绝任何重叠，并合并权限、kind、role、fault policy 和 backing 都相同的相邻区间；file-private VMA 还必须保持连续 file offset。不同 fault policy 的相邻区间不能合并，否则会丢失缺页语义边界。当前静态 ELF、栈和后续少量匿名区间的数量很小，因此查找为 `O(log n)`、插入移动为 `O(n)` 的紧凑数组比预建树和额外节点分配更合适。若真实 `mmap` 工作负载证明确有大量、频繁变化的 VMA，再在同一 MM 语义下替换为平衡树或区间树。

MM record 拥有 VMA 集合，而不是 task 或 Sv39 page-table object。集合借用启用时传入的 kernel heap；该 heap 必须在 MM 及其全部 cleanup retry 之后才结束生命周期。`kernel_mm_fork()` 在成功深复制用户页之后复制 VMA 描述符，因此子进程的逻辑地址布局与父进程独立但初始相同；当前匿名描述符不持有外部 backing。若该复制失败且内部回收成功，输出目标恢复为可复用的 `EMPTY`；若仍有资源无法回收，输出才成为精确的 `CLEANUP` owner。最后一个 MM owner 的清理顺序固定为：

```text
VMA metadata -> Sv39 private leaves/page tables -> MM record page
```

VMA metadata 释放失败使 MM 停在 `KERNEL_MM_CLEANUP_VMAS`；重试仍从该阶段开始，不能提前销毁 PTE 或记录页。没有 VMA 的既有 MM 在 Sv39 销毁尚未发生任何提交时仍保持 LIVE，从而保留原有的普通失败语义。

## 静态 ELF 消费者

静态 `ET_EXEC` 仍先急切分配和填充 Sv39 用户页。空间转入 `kernel_mm` 后，`riscv_user_elf_register_static_vmas()` 用同一仍有效的 read source 重新校验 header/layout，以 `RESIDENT_REQUIRED` 登记按页合并最终权限的 ELF 匿名 VMA，并核对每个已有 PTE 的 R/W/X/U 权限。它再以 `DEMAND_ZERO` 登记完整栈 reserve 的 RW VMA，guard 继续不登记。根启动和 `execve` 都在关闭可执行文件前完成这一步；任何登记失败都使新 MM 只可走清理路径，不能发布给任务。

这是从急切静态装载到 file-backed demand paging 的受限桥梁：当前只读根文件系统和 exec 事务都保证 source 在两次读取期间仍有效。将来引入可写文件、file-private backing 或文件按需缺页时，装载器应一次解析并让 VMA 持有明确的文件/backing 引用，再删除这次重新解析，而不是把两次读取当作通用并发一致性协议。

## 验证与限制

```sh
make test-vma-riscv
make test-user-elf-cases-riscv
make test-mm-riscv
make test-root-init-riscv
make test-demand-page-riscv
make test-exec-riscv
make test-root-boot-cleanup-riscv
```

聚焦 VMA 测试检查相邻合并、不同 fault policy 的边界、孔洞查询、重叠拒绝、fork 描述符复制、demand-zero 零页与权限、真实页耗尽、映射回滚失败、fork metadata 分配失败后目标恢复 `EMPTY`、完整回收，以及注入 heap release 失败后 `CLEANUP_VMAS` 的重试。ELF 用例检查共享 ELF 页的权限并集、完整栈 reserve 和 guard；真实 ext4 `/init -> stage2 -> stage3` 链在 U-mode 深栈 load/store 中实际触发缺页，并覆盖根启动、两次 `execve` 与最终资源基线。OOM 版本还验证子任务以 wait status 9 退出且资源回到基线。

当前只有匿名栈使用 demand-zero；没有 `mmap`、`munmap`、`mprotect`、COW、file-backed 缺页/VMA 引用或 SMP 并发修改。VMA 操作和缺页解析由当前单 hart、关中断的任务生命周期串行化；接入 SMP 前必须为 VMA 查改、fork、最终清理和远端 TLB shootdown 定义锁与引用协议。
