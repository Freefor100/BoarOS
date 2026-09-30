# 虚拟内存区域（VMA）模块

本文描述用户地址空间的逻辑区间元数据。页表格式与硬件失效见 [RISC-V Sv39 分页模块](riscv-sv39.md)，MM owner 生命周期见 [内核 MM 模块](kernel-mm.md)，原理见[内存管理学习总结](../learning/memory-management.md)。

## 范围与入口

| 文件 | 当前职责 |
|---|---|
| `include/kernel/vma.h`、`mm/vma.c` | VMA 描述符、排序集合、查找/选址以及准备—提交式区间编辑 |
| `include/kernel/mm.h`、`arch/riscv/mm.c` | 匿名/文件私有/文件共享 `mmap`、`munmap`、`mprotect`、`msync`、`brk` 和缺页解析 |
| `arch/riscv/elf_image.c`、`arch/riscv/mm.c` | 登记 source-backed ELF、栈 reserve、随机 mmap ceiling 与初始 heap 布局 |
| `tests/riscv/vma_cases.c`、`tests/vma-riscv.sh` | 区间语义、失败原子性、fork、缺页与 1/64/1024 VMA 查找基线 |

一个 VMA 用半开区间 `[start, end)`、R/W/X 权限、kind、role、fault policy 和可选 backing 表示一段逻辑有效的用户地址。PTE 只表示其中已经驻留的 4 KiB 页；没有 PTE 不等于没有 VMA。`ELF_PRIVATE/ELF` 用不可变 ELF source 和 `backing_offset` 描述专用按需装载，`DEMAND_ZERO` 用于栈、heap 和私有匿名 mmap，`ANON_SHARED` 用共享匿名对象与页偏移描述跨 fork 的后备页；`FILE_PRIVATE` 与 `FILE_SHARED` 都用 OFD backing 和文件偏移，所有 VMA 另记 `maximum_permissions`，与当前 R/W/X 分开；共享文件只读 fd 和 `SHM_RDONLY` 附件不保留 WRITE 上限。guard 和被 `munmap` 的洞不属于任何 VMA。

当前生产 MM 入口除静态登记/查询外还包括：

```c
enum kernel_mm_status kernel_mm_mmap_anonymous(
    struct kernel_mm *mm, uint64_t hint, uint64_t length,
    uint32_t permissions, uint32_t flags, uint64_t *address);

enum kernel_mm_status kernel_mm_mmap_file_private(
    struct kernel_mm *mm,
    struct kernel_open_file_description **file,
    uint64_t hint, uint64_t length, uint64_t file_offset,
    uint32_t permissions, uint32_t flags, uint64_t *address);

enum kernel_mm_status kernel_mm_munmap(
    struct kernel_mm *mm, uint64_t address, uint64_t length);

enum kernel_mm_status kernel_mm_mprotect(
    struct kernel_mm *mm, uint64_t address, uint64_t length,
    uint32_t permissions);

int kernel_mm_msync(struct kernel_mm *mm, uint64_t address,
                    uint64_t length, uint32_t flags);

enum kernel_mm_status kernel_mm_shmat(
    struct kernel_mm *mm, struct kernel_shm_segment *segment,
    uint64_t hint, uint32_t permissions, uint32_t flags, uint64_t *address);

enum kernel_mm_status kernel_mm_shmdt(
    struct kernel_mm *mm, uint64_t address);
```

文件 VMA 要求非空 backing、与 kind 匹配的 fault policy、页对齐 offset，并保证 `offset + VMA length` 不溢出；私有匿名 VMA 禁止 backing 和非零 offset，共享匿名 VMA 则要求非空对象、`ANON_SHARED` fault policy 和页对齐连续 offset。SysV 共享内存 VMA（`KERNEL_VMA_KIND_SYSV_SHM`）借用 `kernel_memory_object` 后备并拥有稳定 `shm_attachment` 片段引用，禁止与其他 VMA 合并，在 munmap 与 release 时自动触发段附加计数递减；详见 [SysV 共享内存模块](sysv-shm.md)。集合只借用 backing，MM 的独立 registry 持有 OFD 或共享匿名对象引用；SysV attachment 的片段引用由通用集合插入、分裂、clone、编辑提交和销毁统一维护，不再依赖架构层重叠扫描。`kernel_vma_set_backing_in_use()` 只用于 munmap/fixed replace/销毁后的冷清理，不进入缺页查找热路径。

## 排序、选址与合并

集合是 MM record 通过 kernel heap 拥有的动态排序数组。按地址查找使用二分搜索；插入、拆分、删除和合并可能移动后缀。相邻 VMA 只有在权限、kind、role、fault policy、backing、权限上限以及连续后备偏移全部一致时才合并（SysV SHM 显式禁止合并），因而不会跨越缺页策略或资源生命周期边界。

非 fixed mmap 先尝试页对齐 hint；冲突时在每个 MM 的随机 mmap ceiling 以下、避开栈 guard 和 vDSO 的用户区间内 top-down 查找空洞。无任何随机材料时 ceiling 使用确定性布局。`MAP_FIXED_NOREPLACE` 在任意重叠时返回冲突且不改输出；`MAP_FIXED` 删除范围内所有旧 VMA/PTE 后放入新的匿名或文件映射。vDSO VMA 由 ELF image 独立选择并受普通用户映射边界保护。RISC-V 不能编码 W&&!R 用户叶子，因此 MM 把仅写请求规范化为 RW；`PROT_NONE` 用零权限 VMA 表示。

排序数组的查找成本为 `O(log n)`，编辑成本最坏为 `O(n)`，内存连续且每个 VMA 不需要独立节点分配。`make test-vma-riscv` 在 QEMU `virt` 单 hart 上预热后分别对 1、64、1024 个间隔 VMA 重复 256 次 lookup，并输出平均 `rdcycle` 读数；脚本只要求三组基线存在，不把 QEMU 周期值当成开发板性能结论。若真实 mmap 密集工作负载显示数组移动或锁持有时间成为瓶颈，可在保持当前 MM 语义的前提下换成平衡树/Maple Tree 类结构。

## 区间编辑事务

任意 `munmap`、fixed replace 和 `mprotect` 都可能在区间两端拆分 VMA。集合因此使用两阶段内部协议：

```text
prepare: 校验范围/覆盖条件，只为确实需要的拆分或插入预留 descriptor 容量
page table commit: 修改 PTE、刷新 TLB，然后完成物理页释放
VMA commit: 不再分配，按 generation 验证 prepared edit 后拆分/删除/改权/合并
```

`prepare` 不改变逻辑集合；相同集合发生任何成功编辑后，旧 edit 因 generation 不匹配而返回 `STATE`。`munmap` 允许范围包含洞，且纯洞删除不需要扩容；`mprotect` 要求整个范围由一个或多个相邻 VMA 无洞覆盖，否则在修改 PTE 前返回 `NOT_MAPPED`。`mprotect` 还先逐段检查权限上限，超限在修改 PTE 前返回 `ACCESS`（用户态 `EACCES`）；降权后仍可恢复上限内的权限，split/fork 保留上限。这保证 metadata OOM 不会发生在硬件映射已经改变之后。当前单 hart、关中断路径使 PTE 与 VMA 提交不被并发观察；SMP 时必须用 MM 写锁和远端 TLB shootdown 扩展同一不变量。

## `brk`、fork 与回收

raw `brk` 保存精确字节地址，只在跨页时编辑 VMA。增长增加 RW anonymous `HEAP/DEMAND_ZERO` 区间；冲突或 metadata OOM 时返回旧 break。缩小使用通用范围删除，因此即使用户先 `munmap` heap、`mprotect` 其中一段，或用 fixed mmap 替换局部区域，收缩仍能撤销 `[page_end(new), page_end(old))` 中的全部映射，而不依赖“只有一个连续 heap VMA”的阶段假设。撤销顺序为 PTE 失效、`SFENCE.VMA`、物理页释放和 VMA 提交；合法释放完成即结束，分配器不变量错误进入 fatal。

`kernel_mm_fork()` 克隆整套 VMA 描述符、共享匿名对象引用与文件来源；已驻留共享匿名页和共享文件页保持同一物理页，其他页按私有 COW 规则克隆。子 MM 的共享文件别名记录在页表克隆成功后挂入缓存反向索引。父子随后独立编辑区间，共享匿名对象在两个 MM 中各由 registry 引用维持，文件 VMA backing 则各由独立的 file-source owner 维持。最后一个 MM owner 的回收先解除 node–MM 关联并清理驻留来源记录，再释放 VMA metadata、后备对象与 file-source registry、Sv39 页引用/页表和 MM record；只有 file-source 的真实 VFS/I/O 清理错误需要保留 owner。

## 验证与限制

```sh
make test-vma-riscv
make test-mmap-riscv
make test-brk-riscv
make test-root-init-riscv
make test-diff-abi-riscv
make test-sqlite-wal-riscv
make test-riscv
```

聚焦测试覆盖相邻合并、孔洞、冲突、两端拆分、hint/top-down、fixed replace/noreplace、文件 backing/offset 合并边界、`PROT_NONE` 内容保持、W→RW、mprotect 全覆盖、munmap 洞语义、打洞后的 brk、fork COW、metadata OOM 和资源基线。真实 ext4 `/init` ELF 从 U-mode 调用匿名与文件私有 mmap/mprotect/munmap，验证写隔离、EOF/SIGBUS、errno 与最终回收；非法分配器释放由 fatal-path 测试覆盖。

当前实现共享匿名、普通文件 private/shared mapping 和 `msync`，没有 `MAP_POPULATE`、内存承诺 accounting、VMA 数量上限或 SMP 并发修改。ELF source-backed demand paging 和 RISC-V Sv39 ASLR 已接入；没有任何随机材料时沿用确定性布局；仅有 DTB 材料时的随机布局不构成可信安全保证。`MAP_STACK` 目前只是兼容性标志，不改变增长方向；`MAP_NORESERVE` 与全局尚无 commit accounting 的当前策略等价。

## 文件缺页跨 I/O 等待

文件/ELF source 登记有独立在途 fault 引用。缺页先保存 VMA generation 和 source owner，在发布任何 PTE 前完成文件 I/O；返回后重新验证 VMA 版本、大小、页状态及同地址 PTE。并发 unmap/固定替换不能发布旧映射；另一线程已满足同一缺页时视为成功并执行本地失效。截断由 inode 锁与发布阶段互斥，元数据分配只做干净回收，避免发布中途递归存储等待。source 清理等待在途 fault，清理发生等待后重新定位 registry 链接，不能使用过期前驱。`tests/riscv/files_main.c` 的确定性交错覆盖 I/O 期间 unmap 和另一线程先发布同一页。
