# 内核 MM 模块

本文描述任务地址空间的通用所有权接口、共享 MM 策略、RISC-V Sv39 与 LA64 后端。页表格式、映射规则和硬件切换见 [RISC-V Sv39 分页模块](riscv-sv39.md)，地址空间与任务身份分离的背景见[内存管理学习总结](../learning/memory-management.md)。

## 范围与入口

| 文件 | 当前职责 |
|---|---|
| `include/kernel/mm.h` | 定义跨架构 MM 句柄、权限、状态、引用和 VMA 入口 |
| `mm/mm.c`、`include/kernel/mm_backend.h` | MM 引用、VMA、后备来源、缺页与驻留策略；记录页拥有构建期选定的页表后端 |
| `include/arch/mmu.h`、`arch/riscv/mm.c` | 构建期 MMU 操作绑定与既有 RV 创建/统计接口的薄适配 |
| `arch/loongarch/mmu.c`、`tlb_refill.S` | 16 KiB/三级用户页表、COW 所有权、INVTLB 与软件 refill；LA 验收见[首阶段](loongarch-boot.md) |
| `tests/riscv/mm_cases.c`、`tests/mm-riscv.sh`、`tests/mm-fatal-riscv.sh` | 验证创建、共享引用、移动、查询、页表回收和 resolution invariant fatal |
| `tests/riscv/vma_cases.c`、`tests/vma-riscv.sh` | 验证 VMA 集成后的 fork、缺页解析、OOM 与所有权回收 |

稳定接口为：

```c
enum kernel_mm_status riscv_kernel_mm_create(
    struct kernel_mm *mm,
    struct riscv_sv39_user_space *space);

enum kernel_mm_status kernel_mm_acquire(
    struct kernel_mm *destination,
    const struct kernel_mm *source);

enum kernel_mm_status kernel_mm_fork(
    struct kernel_mm *destination,
    struct kernel_mm *source);

enum kernel_mm_status kernel_mm_move(
    struct kernel_mm *destination,
    struct kernel_mm *source);

enum kernel_mm_status kernel_mm_lookup(
    const struct kernel_mm *mm,
    uint64_t virtual_address,
    struct kernel_mm_mapping *mapping);

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

enum kernel_mm_status kernel_mm_brk_initialize(
    struct kernel_mm *mm,
    uint64_t start,
    uint64_t limit);

enum kernel_mm_status kernel_mm_brk(
    struct kernel_mm *mm,
    uint64_t requested,
    uint64_t *result);

enum kernel_mm_status kernel_mm_mmap_anonymous(
    struct kernel_mm *mm,
    uint64_t hint,
    uint64_t length,
    uint32_t permissions,
    uint32_t flags,
    uint64_t *address);

enum kernel_mm_status kernel_mm_mmap_file_private(
    struct kernel_mm *mm,
    struct kernel_open_file_description **file,
    uint64_t hint,
    uint64_t length,
    uint64_t file_offset,
    uint32_t permissions,
    uint32_t flags,
    uint64_t *address);

enum kernel_mm_status kernel_mm_munmap(
    struct kernel_mm *mm,
    uint64_t address,
    uint64_t length);

enum kernel_mm_status kernel_mm_mprotect(
    struct kernel_mm *mm,
    uint64_t address,
    uint64_t length,
    uint32_t permissions);

enum kernel_mm_status kernel_mm_resolve_user_fault(
    struct kernel_mm *mm,
    uint64_t virtual_address,
    uint32_t access);

enum kernel_mm_status kernel_mm_release(struct kernel_mm *mm);
```

`riscv_kernel_mm_satp()` 是 RISC-V scheduler 创建任务时使用的架构入口。通用层不暴露 Sv39 对象，也没有运行期 vtable；RISC-V 构建通过 `include/arch/mmu.h` 直接绑定 Sv39 操作；通用策略位于 `mm/mm.c`。LoongArch 构建以后为同一后端契约提供 16 KiB/三级页表实现。这样任务和 syscall 层依赖 MM 语义，而不依赖页表格式，调用热路径也没有间接分派。


`kernel_mm_pin_user_page()`只对resident且已许可的基页成功，在rank15元数据保护内
取得实际物理引用；`NOT_MAPPED/ACCESS`要求调用者fault后重查。输出token零初始化，
成功才发布；同一I/O owner在复制完成后调用`kernel_mm_unpin_user_page()`。
调用者保持LIVE MM句柄，末引用释放时仍有pin属于非法生命周期并fatal。
原`kernel_mm_lookup()`仍只返回快照，不能将其物理指针借用当成pin。
`make test-mm-pin-host`链接生产MM/VMA/Sv39 walker，只替换硬件CSR/fence；
文件/SysV等范围外入口一旦被调用就中止。反例先证明旧lookup只有PTE引用，
新实现检查撤映射、耗尽/复用、权限、复制前缀、token错误和最终allocator审计。
此入口不替代硬件TLB或两核验收。

## 句柄、引用与状态

`struct kernel_mm` 是一个可移动的拥有型引用，不是地址空间本体。RISC-V 后端用 `record_page_address` 指向一张物理记录页；记录页保存引用计数、清理阶段和唯一的 `riscv_sv39_user_space`。多个 LIVE 句柄可以指向同一记录页：

- `acquire` 增加引用计数并发布一个新的独立 owner；vfork 与线程 clone 用它共享地址空间，任一线程释放自己的引用不撤销其他 owner。共享 MM 的 VMA/PTE 变更对同组立即可见；当前单 hart 关闭 SIE 串行化，并执行本地 TLB 失效，不等同于 SMP 同步；
- `fork` 创建独立地址空间，克隆 VMA/文件来源并让父子暂时共享用户物理页，逻辑 MM 和后续写入仍独立；
- `move` 转移一个 owner，不改变引用计数；
- `release` 消耗一个 owner，非末引用只减计数，末引用才销毁页表树和记录页；
- `lookup` 把架构权限翻译为 `KERNEL_MM_READ/WRITE/EXECUTE/USER`，不让通用调用者依赖 RISC-V PTE 位值。

LA 的有效 PTE 权限按固定 Linux `arch/loongarch/mm/cache.c::protection_map`：
非 PROT_NONE 用户页可读，EXEC 独立控制 NX。请求的 VMA 权限保持原值；因此
未驻留的纯 EXEC 页数据读取被 VMA 拒绝，页经合法写入或取指驻留后可由 PTE
读取。uaccess 同样先看实际 PTE，冷页才走原通用 fault 权限检查。不能统一把
VMA 的 EXEC 改写成 READ，也不能只修改用户硬件权限而使复制仍误报 EFAULT。
`make test-permissions-loongarch` 用同 ELF 验证两种 RAM 下的冷/驻留页、取指
物化、mprotect、fork、uaccess 和撤执行权限；PROT_NONE、COW及回收契约保留。

输入目标必须是全零 `EMPTY` 句柄。成功移动后源进入 `MOVED`，成功释放后进入 `RELEASED`；二者都不再拥有资源。分配器释放属于 fail-stop 契约，不能由调用者按错误码重试；只有文件/OFD 等真实 I/O owner 的清理状态会交给上层继续处理。

RISC-V 创建先分配并解析记录页，最后才把 LIVE Sv39 空间移入记录；成功后输入空间成为 MOVED。记录页及页表的合法释放完成即结束；若同时持有 VFS/file-source owner，则仅保留对应的 I/O cleanup 状态。

## Program break 与匿名 heap

`kernel_mm_brk_initialize()` 只用于尚未发布、只有一个 owner 且已经启用 VMA 的新映像。它记录页对齐的起始 break、当前精确 break 和页对齐上界；RISC-V ELF image 把最高非空 `PT_LOAD` 的 `p_vaddr + p_memsz` 向上对齐作为起点，并将独立布局计算出的 mmap ceiling/vDSO 作为约束。每次 exec 因而得到由新 ELF 独立计算的 break，而 fork 复制父进程调用时的精确值。

`kernel_mm_brk()` 实现 raw Linux syscall 所需的返回语义：参数 0 查询当前值；落在起点以下、上界以上、溢出或因 VMA 冲突/metadata OOM 无法增长时，MM 保持不变并把原 break 写入结果，而不是产生负 errno。成功结果保留字节粒度；只有 VMA 和 PTE 操作向上按 4 KiB 对齐。

跨页增长只把新增 `[old_page_end, new_page_end)` 登记为 RW anonymous `DEMAND_ZERO` heap VMA，不提前分配数据页。首次 U-mode load/store 复用普通匿名 fault 路径分配零页。跨页缩小通过通用区间编辑撤销目标范围，因此能容忍用户先用 `munmap` 打洞、用 `mprotect` 分段或用 fixed mmap 替换局部区域；它不再依赖一个从 break 起点连续延伸的阶段性 heap VMA。缩小先让范围内用户 PTE 失效并刷新本 hart TLB，再无分配地提交 VMA 删除和精确 break；物理页释放完成后该操作才返回。

## 匿名与文件映射

`kernel_mm_mmap_anonymous()` 实现 private demand-zero 与 `MAP_SHARED|MAP_ANONYMOUS`。非 fixed 请求优先使用空闲的页对齐 hint，否则在每个 MM 的随机 mmap ceiling 以下、栈 guard 之外 top-down 选址；`FIXED_NOREPLACE` 只检查冲突，`FIXED` 则撤销旧页和 VMA 后替换。长度向上按 4 KiB 对齐，返回地址只在成功时写入。RISC-V 的 W&&!R PTE 编码保留，因此仅写保护被规范化为 RW。

共享匿名 mmap 在提交 VMA 前建立独立对象；MM registry 对每个仍使用的对象持一份引用，VMA 只借用并保存连续字节偏移。对象以稀疏页索引保存已产生的零页，每个页槽持一个物理引用，每个 PTE 另持一个。fork 先为子 MM 取得对象引用，再克隆页表；已驻留共享页在父子保持可写共享，未驻留页由任一方按对象和索引首次发布。`PROT_NONE`/恢复保留共享属性，不借用 COW 标记。撤映射、fixed replace 与 MM 销毁在最后使用它的 VMA 消失时释放本 MM 的对象引用；对象末引用释放所有页槽。仍有对象引用时，局部撤映射不截断对象内容；首版没有 swap 回收。

`kernel_mm_validate_file_private_mapping()` 复用真实映射的规范化、范围、选址和 fixed-noreplace 冲突检查，但不分配来源节点、不编辑 VMA，也不预留返回的区间；当前单 hart syscall 在该检查与真实提交之间不会调度。`kernel_mm_mmap_file_private()` 接收调用者已经 pin 的可读普通文件 OFD、页对齐文件偏移和同一组选址/权限参数；传入 `KERNEL_MM_MAP_SHARED` 时建立 `FILE_SHARED` VMA。syscall 层先完成无副作用校验，使零长度、非法 fixed 地址、offset 溢出和既有映射冲突保持原 errno 优先级；校验通过后才检查 OFD 可读性，以及共享可写映射的 OFD 写权限，不符返回 `-EACCES` 并释放本次临时 pin。成功时 MM 消耗 pin，失败时仍由调用者持有。MM 对每个不同 OFD 只建一个来源节点，并由该节点持有一份来源引用；重复 mmap 不累积历史引用，VMA backing 借用同一对象。VMA 提交后若来源已存在，只递减一个由调用者刚取得且必然不是末引用的临时 pin；新来源则直接转移 pin，因此系统调用不会出现“返回错误但映射已生效”。关闭 fd 不影响映射；fork 为子 MM 建立独立来源节点并取得一份引用。`munmap`/fixed replace 在提交 VMA 后的冷路径释放已经没有 VMA 使用的来源节点，真实 VFS/OFD 清理错误由其 owner 状态向上转交。页故障查到 VMA 后直接取得 backing，不在 fault 热路径遍历 fd 表或来源链。

文件 VMA 不预分配数据页。`FILE_PRIVATE` read/execute 首次缺页从挂载页缓存取得共享页并建立 COW PTE；首次写若缓存已命中则复制缓存页，未命中则直接把文件内容读入新私有页。缓存命中的写时 COW 例程自身完成该页的 `SFENCE.VMA`/必要 `FENCE.I`，外层缺页路径不重复刷新；其他新填充页由外层统一刷新。文件最后一页的有效内容之后补零；故障页起点已经不小于文件大小时返回 `BUS_FAULT`。普通 read/write 与后续私有缺页共用 inode 页缓存；已私有化的 COW 页继续隔离。

`FILE_SHARED` 的每个驻留 PTE 持缓存物理页引用，MM 持对应 OFD 来源引用；同一 inode 页在独立 open、独立 MM 和多个 VA 中维持同一数据源。MM 分配驻留记录后安装只读共享 PTE，再把记录挂入缓存页的反向索引；失败撤销 PTE，未发布的记录由 MM 回收。写故障在缓存项标记当前有效字节脏、推进 generation 后将本 PTE 升为可写。缓存写回前按反向索引重新保护别名，写回中再次写入会重新故障并改变 generation，旧写回不能清除新脏状态。MM 拥有记录，缓存借用链接，PTE 持物理页；fork 预留子记录并在页表共享成功后挂入索引，munmap/fixed replace/退出无分配地摘除。只读 fd 建立的共享 VMA 保存不可升级的写资格，`mprotect` 请求写返回 `EACCES`，合法权限变化重新建立首次写追踪。

`make test-files-partial-write-riscv` 对共享映射注入 flush EIO、重试后再次写 fault；在 ext4 写回已复制旧值、页缓存比较 generation 前，通过第二个驻留 VA 别名触发真实用户写 fault，下一次同步与驱逐重读必须看到新值。测试还扫描 fork metadata 分配失败、固定地址替换分配失败时保留原映射、别名和 OFD，最后检查页资源基线。该确定性交错仍在单 hart 上运行，第二个 hart 的真实并发、跨核 TLB 与发布协议由 P6 验证。

`kernel_mm_msync()` 按固定 Linux `references/linux/mm/msync.c`（commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e`）检查 flags、对齐和范围；长度零成功，空洞在处理已映射片段后返回 `ENOMEM`。`MS_SYNC` 对共享文件 VMA 的对应 inode 范围写回，再同步元数据并传递块 flush，错误沿 inode/OFD 序列传播；`MS_ASYNC` 不提交 I/O。映射保持 OFD/node 引用，所以关闭 fd 或 unlink 后仍能同步。

ELF image 使用独立的 `kernel_elf64_source`，不把 `PT_LOAD` 当作普通 file-private mmap。source 在 exec 时一次解析 program headers，并将页区间规范化为 `ELF_PRIVATE` VMA 的 `backing_offset`。完整文件页可以共享 page cache；文件/BSS 边界页、多个段贡献页和 BSS 页由 fault 路径私有分配、清零并精确填充。source 引用由 MM 记录，重复映射不增加历史引用，fork 子 MM 获取独立引用，最后一个相关 VMA 消失才 release。可执行页发布后执行本地 `SFENCE.VMA` 和必要 `FENCE.I`，覆盖先读后取指。source 的 OFD/I/O 清理错误仍由 source owner 保留，堆和物理页释放不建立重试状态。

MM 还单独持有主 ELF 的执行 OFD，供 `/proc/<pid>/exe` 查询。它在新映像全部构造成功后取得，exec 失败不改变旧 MM；线程共享同一 MM，fork 子 MM 另取引用，末次 MM 清理在 ELF source 仍持有 OFD 时先释放该引用。`PT_INTERP` 只决定实际入口，不改变 exe 所指主文件；查询返回带引用的路径，调用者不能在任务退出后借用 MM 指针。

`kernel_mm_proc_memory_snapshot()` 无分配读取 VMA 集合在成功编辑时更新的覆盖字节数，以及 Sv39 有效/受保护页计数，供 proc 的 VmSize/VmRSS 使用。受保护页仍拥有物理页，故计入驻留；未发生缺页的虚拟映射只计入 VmSize。数值是瞬时 MM 统计，不把页缓存全局占用误计为该进程 RSS。

`kernel_mm_munmap()` 采用 Linux 洞语义：输入范围中没有 VMA 或只覆盖部分 VMA 仍可成功；resident、`PROT_NONE` 和待释放页都由 Sv39 owner 状态处理。`kernel_mm_mprotect()` 要求整个范围无洞覆盖，先准备 VMA 拆分容量，再原地修改已有 PTE 权限，最后提交 metadata；长度 0 对齐地址直接成功。`PROT_NONE` 不释放物理页，恢复权限后仍看到原内容。

## 硬件用户缺页解析

`kernel_mm_resolve_user_fault()` 的 `access` 必须恰为 READ、WRITE、EXECUTE 之一。RISC-V 后端先检查用户范围、VMA、逻辑权限和现有 PTE；VMA 外地址、`PROT_NONE`/权限冲突和仅驻留策略 VMA 的空洞返回 `NOT_MAPPED`。`ELF_PRIVATE/ELF` VMA 再按 source 区间二分取得 FILE、COMPOSITE 或 ZERO backing；只有确认需要提交 anonymous/file/ELF 页或解析 present COW 后，才要求目标 MM 是本 hart 当前 `satp`，从而既不为非法软件访问分配，也能让 uaccess 对合法未驻留页复用同一解析器。

匿名成功路径按 4 KiB 对齐故障地址，分配并清零一页，以 VMA 的完整 R/W/X 权限建立 U-mode PTE。文件路径则按上述缓存/私有策略取得页面。两者成功后都执行针对该虚拟页、ASID 0 的本地 `SFENCE.VMA`；VMA 带执行权限时还执行本 hart 的 `FENCE.I`，保证新填充的指令字节对后续取指可见。该规则覆盖匿名清零、文件填充和先读后执行的 file-private fault；COW 复制和恢复执行权限的页表路径也各自同步指令缓存。dispatcher 保持 `sepc` 不变，`sret` 后硬件重试原 load/store/fetch。`NOT_MAPPED` 在 Trap 边界终止为 `SIGSEGV(11)`，文件整页越过 EOF 的 `BUS_FAULT` 终止为 `SIGBUS(7)`；uaccess 把二者都转换为 `EFAULT`。物理页耗尽精确返回 `NO_MEMORY`；合法 owner 的页释放完成即返回，非法页表或分配器状态直接 fatal。PTE 已存在却仍产生允许权限的页故障或非活动 MM 都是内核状态错误。

scheduler 的 U-mode 硬件页故障和当前任务的 uaccess 都可进入该入口。uaccess 只在 `kernel_mm_lookup()` 报未驻留后尝试解析；合法页成功提交后重查 PTE，VMA 外或权限不符仍按用户 fault 处理。

## fork 与 COW

`kernel_mm_fork()` 先克隆所有会分配的 VMA、共享匿名对象引用和文件来源 metadata，最后才调用 Sv39 两阶段 fork。子页表先取得每个 active/`PROT_NONE` 4 KiB 页的物理引用；共享匿名页保持原权限，其他页以 COW 形态建立。全部构造成功后再把父进程私有可写叶子去掉 W 并标记 COW，随后刷新父地址空间 TLB。metadata 或子页表构造失败时父 PTE 不变；父提交阶段不再分配。无效或已撤销的页不进入子地址空间，内核高半区仍只借用稳定映射。

写 fault 只接受软件明确标记的 present COW 页，不能把真正只读页误当成可写。若物理引用数大于 1，就分配并复制新页、把当前 PTE 改回 VMA 权限并释放旧引用；若只剩一个引用，则原地恢复写权限而不复制。`mprotect(PROT_NONE)` 使用不同 RSW 编码保存 exclusive/COW 属性，恢复后仍能正确决定是否复制。uaccess 写入也进入同一 resolver，因此内核代用户写内存不会绕过 COW。

创建失败时，目标句柄要么仍为 EMPTY，要么仍持有尚未发布的完整 owner；分配器释放不失败，调用者不按释放错误建立重试状态。Fork 仍需遍历已提交页并构造子页表；共享页分类在子构造和父提交时对每个驻留页查询 VMA，成本为 `O(P log V)`（P 为驻留页数，V 为 VMA 数），另有 metadata 和页表遍历，但不在 fork 时复制每页内容或预留同量数据内存。额外数据页成本只落在之后真正写入的页。当前 clone 仍在单 hart 关中断临界区内完成 walker 和 metadata 分配，大地址空间的最坏中断延迟尚需开发板测量。私有匿名并发 fork 已接入 gate、原子页引用和同步远端失效；文件/外部后备的 fork 仍不开放跨核。

## 末引用回收

末引用只能在其 `satp` 已不活动时释放。有 VMA 时回收按四个阶段执行：

```text
LIVE(last ref)
  -> release VMA metadata
  -> release file/ELF-source registry
  -> release user-leaf references and destroy page tables
  -> release MM record page
  -> RELEASED
```

`CLEANUP_VMAS`、`CLEANUP_FILE_SOURCES` 和 `CLEANUP_ELF_SOURCES` 只表示仍由 MM 持有的文件/OFD/source owner 需要完成真实 VFS/I/O 清理；页表树、VMA metadata、记录页和物理页的合法释放完成即推进流程，不产生 allocator retry。所有 CLEANUP 句柄都只能 move 或由所属上层继续处理，不能 acquire、lookup 或生成 `satp`。非末引用释放不触碰 VMA、文件来源或页表树。

页表页在 production 中只经 finalized physical-page allocator 和固定 direct map 解析。合法且仍归 MM owner 所有的页表页没有异步缺页、I/O 或其他能让失败条件稍后消失的 producer；解析失败只可能表示页所有权、allocator metadata 或 direct-map 不变量已经破坏。因此 Sv39 teardown 直接触发 fatal trap，不保留 `CLEANUP_SPACE`，也不允许下一次 `release` 把部分释放的树伪装成恢复成功。VFS/OFD 的真实外部 I/O owner 仍使用各自 cleanup 状态。

用户退出路径先切到内核根页表，再在仍有效的任务内核栈上调用 `kernel_mm_release()`；只有真实 VFS/OFD cleanup 仍有 owner 时才交给后续回收上下文处理。因此不会销毁硬件当前仍在使用的用户根。MM 完成后才允许任务成为 zombie，zombie 只保留身份、亲缘、wait status 和任务页。

## 并发与性能边界

MM 句柄各自持有一个实际引用，引用和 user 计数使用原子操作；句柄本身不能被其他执行者并发 move/release。匿名并发域的 VMA 操作由 rank25 可睡眠 gate 串行化，页表、pin 许可及活动 CPU 由 rank15 raw 保护，页引用可嵌套 physical rank20。最后引用要求 pin、活动 CPU 和退休批次均归零，资源销毁仍由 CPU0 执行。文件、外部后备及进程业务继续遵守原单CPU边界。

Scheduler 在用户任务创建时解析 MM、验证入口与栈权限，并缓存目标 `satp`。tick 和 current 校验仍使用任务的 context 缓存；实际地址空间切换在调度域外调用 `kernel_mm_activate()`，登记活动 CPU、切根并失效后才清旧记录的活动位，不增减 MM 引用，也不遍历用户页表。并发域同 SATP 也做完整本地失效及取指同步。地址空间切换的当前主要成本仍是 ASID 0 下的 `satp`/全局 `SFENCE.VMA`。demand-zero 或文件首次触页增加 VMA 二分查找、页表查询、页分配/I/O、PTE 写入和一次单页本地 fence；缓存命中的读 fault 不复制文件页，写 fault 至多复制一页。驻留后的普通用户访存不经过软件 fault 路径。`brk` 不是普通访存热路径；增长的 metadata 操作为排序数组插入，缩小需要遍历覆盖范围中已经存在的页表分支并执行一次全局本地 fence。默认单CPU路径的缩小不回收变空的中间页表，它们由 MM 销毁统一释放；匿名并发域在确认失效后归还空表。默认路径避免在 unmap 提交中再引入页表页回滚状态，但会保留约为连续高水位虚拟跨度 0.2% 的 Level 0 表容量，稀疏触页时相对实际数据页的比例可能更高。尚无开发板 fork/fault/unmap 延迟和 TLB/cache 计数，不能从 QEMU 正确性测试推断硬件性能。

## 验证与限制

```sh
make test-mm-riscv
make test-vma-riscv
make test-brk-riscv
make test-mmap-riscv
make test-demand-page-riscv
make test-user-riscv
make test-diff-abi-riscv
make test-sqlite-wal-riscv
make test-riscv
```

MM 聚焦测试覆盖创建失败原子性、共享引用、移动、COW fork 的父子共享/写隔离/末引用原地恢复、`PROT_NONE` COW 属性，以及正常页表回收和物理页基线；VMA 聚焦测试另覆盖共享匿名页的 fork 前驻留、双方首次 fault 顺序、权限拆分、部分撤映射、替换、退出和 OOM 回滚。同一 MM target 还运行独立 fatal kernel，注入一次页表 backing 无法解析并确认只产生一个 fatal 结果、不会返回 retry/success 路径。文件测试覆盖 cache hit/miss、write-first、尾页补零、整页越 EOF、fd 关闭后 fault、fork 后 OFD 来源和最终回收。syscall 聚焦测试验证校验错误先于不可读 OFD 的 `EACCES`，两类拒绝都释放临时 pin 且不进入 MM 提交；`test-userland-riscv` 用真实 musl mmap 覆盖匿名共享的双向可见、fd 忽略、fixed 冲突，以及原有私有 mmap。`test-mmap-riscv` 与真实 ext4 `/init` 从 U-mode 完成匿名/文件私有 mmap、COW、SIGBUS、mprotect/munmap 生命周期；`test-diff-abi-riscv` 与 `test-sqlite-wal-riscv` 另验证文件共享 mmap、`msync` 和独立进程 WAL。

已接入构建期绑定的RV64 Sv39/4 KiB与LA64 16 KiB/三级页表后端；共用策略仅开放受控私有/共享匿名并发域；文件/外部后备仍限CPU0。普通 fork 使用独立 MM，私有页 COW、共享匿名与共享文件页保持共享；线程 clone/vfork 共享同一 MM record。匿名映射和 ELF image 使用每 MM 的 ASLR mmap ceiling（无任何随机材料时确定性降级；仅 DTB 混种不提供可信安全保证），但没有 commit accounting。栈软限制约束后续未驻留栈页的填充，已存在的 PTE 保留；新 exec 在固定容量 VMA 内按当前软限制建立初始栈。可读普通文件支持 MAP_PRIVATE/MAP_SHARED 与 `msync`；brk 尚未接入 RLIMIT_DATA。文件表和信号表不属于 MM。private futex 使用 MM 创建时分配的单调身份号与用户地址，避免 MM record 页回收后重用身份；共享匿名 futex 使用后备对象与连续字节偏移，等待者持有对象引用。共享文件 futex 尚无后备 key。

## 驻留文件映射与截断

MM record 拥有按 VFS node 去重的稳定关联记录，以及每个驻留文件页的地址、物理页
身份和私有化状态。VFS 只借用关联；OFD 来源持有 node 的生命周期。共享 MM 不重复
登记，fork 在页表共享提交前复制来源/关联/驻留元数据，成功后登记新 MM。mmap 在
VMA/PTE 修改前预留关联；失败不消耗调用者 OFD。末个 VMA 消失或销毁时先解除关联，
再释放 OFD；文件 fault/msync 在途操作 pin 推迟最后来源释放，直至睡眠操作完成。真实 close 错误仍保留来源 owner。不得登记可移动的 VMA 数组元素或 MM handle 地址。

首次文件 fault 在发布 PTE/alias 前预留驻留记录和地址哈希容量；COW 成功后更新私有标志及物理地址。
缓存命中的 write-first fault 若 COW 物理分配失败，必须撤销临时 cache PTE 后返回，
不能留下无来源记录的 PTE。fork 后私有页面即使再次带 COW 标志，仍保持私有来源。
munmap/fixed replace 清除对应驻留记录，mprotect/PROT_NONE 保留来源。

VFS 向下截断按实际 live size（包括部分生效后报错）同步通知相关 MM。扫描每个
驻留记录并查询当前 VMA 与文件偏移，撤销整页起点不小于新 EOF 的 PTE，保留 VMA；
非对齐尾页只清零文件来源页的后缀，保留 private 页内容。当前与非当前 MM 使用
相同的 PTE 失效、全局本 hart SFENCE.VMA、物理引用释放顺序；尾页代码修改执行
FENCE.I。当前单 hart syscall 不可调度区保证登记和通知稳定，不代表 SMP 协议。

关联只扫描相关 MM；每个 MM 以动态虚拟页地址哈希查询驻留记录，保留链表负责遍历。哈希碰撞比较完整地址，装载因子不超过 1，按二倍扩容；分配成功后无分配地重建并替换桶，失败保留旧索引。fork 提前准备全部桶，unmap/fixed replace/截断同步摘除索引与链表；最后 MM 释放桶。查找为平均常数成本，截断仍遍历驻留链，每页 VMA 查找 O(log VMA 数)。元数据随驻留规模增长；不在 tick 热路径扫描。
`test-files-riscv` 扫描准备阶段分配失败与 fork 回滚，并注入缓存命中后的 COW
物理分配失败；`test-userland-riscv` 与 `test-diff-abi-riscv` 验证真实驻留/COW、
PROT_NONE、非当前 MM、尾页、关闭 fd、unlink、O_TRUNC 与重新增长。

`riscv_kernel_mm_get_statistics` 提供驻留查找探测、改权页表访问及地址/全局失效计数。`make test-scale-riscv` 在 16/64 MiB 文件全部读驻留后按置换顺序首次写入，验证探测增长不超过 6 倍，并注入前 128 页准备阶段的元数据分配失败。首次写开放权限和写回 rearm 使用单页 PTE 操作；引用与脏代次协议保持不变。

调度器在任务发布及新映像提交时调用 `kernel_mm_add_user`，在退出或旧映像退休前调用 `kernel_mm_remove_user`。活跃使用者与 MM 资源引用分开：清理任务暂存的旧 MM 不影响 CHILD_CLEARTID 条件；fork 创建零使用者的新 MM，acquire/move 不隐式注册任务。计数下溢、溢出属于内核 owner 错误。

活跃使用者在可睡眠的清 TID 复制/唤醒结束后才注销；其他任务同时退出时仍须看到正在退出但尚未完成 mm_release 的使用者。`test-mm-riscv` 通过用户复制挂起点的确定性交错检查两个用户字均清零及最终物理页归还；真实 U-mode 的生命周期另由 tid.* 差分覆盖。

## 统一内存后备对象

共享匿名与其 futex key 使用 `kernel_memory_object` 的稳定对象身份。对象拥有每个稀疏驻留页的一份引用，get/find 成功返回额外页引用；find 遇空洞不分配。对象锁 rank 35 位于 inode 30 之后；无磁盘 I/O。truncate 摘除越界的对象页引用并清零保留尾页，调用者须先撤销受影响 PTE 并持有对象；匿名缺页通过 prepare/finish 归还失败引用或记录 PTE 接纳，准备计数归零后才撤销未接纳页。tmpfs 已接入同一对象及按实例共享的页预算；普通文件的 inode/OFD 与 MM 登记仍保留，内存数据页不进入磁盘缓存。内存文件缺页使用 prepare/finish：每次准备持有实际来源及额外页引用，接纳后转给 PTE 或作为复制来源归还；最后失败者仅撤销未被接纳且身份匹配的后备页。磁盘来源仍沿原页缓存协议。文件配额耗尽为 BUS_FAULT，普通分配耗尽仍为 NO_MEMORY。详见 [tmpfs](tmpfs.md)。

缺页解析在合法 VMA 上权限不足返回 ACCESS，地址无映射返回 NOT_MAPPED；trap 层据此提供 SEGV_ACCERR/MAPERR，uaccess 将两者统一映射为用户复制 FAULT。这个区分不把 syscall 坏指针变成异步故障信号。

默认关闭的 C3 成本观测记录 VMA 查询比较、coverage/权限检查、prepare/commit 时长、
数组移动/编辑、全集合合并/字节重计、file_residents 遍历、实际 PTE 访问和地址/全局 TLB 失效。
计数入口在实际循环或修改点，不改变 prepare 失败原子性；单页 PTE 路径与原 MM 统计独立核对。
`make test-cost-riscv COST_CASE=mprotect` 使用固定一页目标和16/64/256无关VMA、
0/16/64MiB文件驻留页，三个启动副本；权限拒绝和洞窗口要求PTE访问为0。
OOM/split/fork 由 vma/scale/ABI 组合回归继续保护。


## 匿名 MM 的并发与回收

自持同一 MM pin 的调用者只尝试取得 operation gate；忙时返回 `CONFLICT`，原 pin、映射和未发布的输出保持。新 pin 许可已关闭也不等待；缺页准备后的重取 gate 使用同一规则，冲突归还候选及 COW 临时引用后返回，不在保留 pin 时自旋重试。调用者归还原 pin 后可重试，无争用的自持 pin 撤映射仍有效。零长度 mprotect 不进入 gate。

`kernel_mm_enable_concurrency()`只用于 CPU0 私有构造完成的记录：独占引用、无 ELF/SysV/未开放外部后备、无 allocator 压力 hook。受控 tmpfs 来源须核对真实 inode、同 heap/allocator 和实例页预算；受控ext4来源须核对真实inode/挂载及独立缓存的heap/allocator绑定，登记状态由CPU0在发布前准备。共享匿名对象须经check_domain确认属于同heap/allocator且无budget；发布后允许MAP_SHARED匿名创建。成功后才能共享句柄和通过 `kernel_thread_create_joinable_mm()` 绑定受控任务。普通用户任务仍限 CPU0；LA 仍为单CPU。CPU0 负责文件构造、修改和最终释放；CPU1 只执行已绑定的映射操作，控制者保留独立 OFD/挂载/缓存 owner，命名空间运行期间不修改。内部开关不开放系统压力或普通文件业务跨核。

每个复制片段在 MM raw 下完成 PTE/权限检查及真实物理 acquire，复制在锁外；同一 task/I/O owner 归还 pin。pin 固定页存活，不固定映射身份或内容。`munmap` 可先撤销映射，旧片段靠 pin 完成；`mprotect` 关闭新许可并排空旧片段，fork 关闭写许可并排空旧写 pin。已持有需要排空的 pin 时不能等待自己，非法 owner fatal；零长度及无效参数不进入 drain。精确复制前缀保持。

共享匿名fault在gate下取得实际对象引用，放保护后prepare页/准备缺表，恢复后重查VMA代次、backing、偏移和权限；prepare返回的额外页引用成功转给PTE，finish记录接纳；失败finish归还该引用并结束准备计数，只有最后失败者撤销未接纳的匹配页。另一MM撤PTE不清接纳历史，最后归还临时对象引用。并发fork按VMA分类，只把私有叶提交成COW，共享及受保护共享叶保持共享。单侧降权只作用于本MM，不保证另一MM的共享数据内容不变。

fault 先在 gate/raw 下取得 VMA generation 和 PTE/COW 快照，释放保护后准备数据页及至多两张缺表，清零或复制也在锁外。恢复后重新查 VMA、权限及 PTE cookie；过期候选和竞争 loser 归还全部私有 owner，再重试。fork 的子树和 metadata 完成全部可失败准备后，才把父叶提交为 COW；同步父目标 hart 后发布子 LIVE，不在失败构造时修改父权限。

撤叶及空表将 owner 转给固定容量为16的退休批次。MM raw 内清 PTE/摘父项、增加代次并采样 active mask；raw 外完成本地 SFENCE 和同步 SBI RFENCE，再释放数据及表页。非叶修改使用全范围失效。取指变化另完成本地/远端 FENCE.I。目标采样后首次进入的 CPU 在同一保护下先登记、切根并完整本地失效；离开者清活动位发生在旧翻译停止之后。固件返回错误 fatal，保留真实 owner，不建立释放重试链。

`kernel_mm_proc_memory_snapshot()`在 gate 下固定 VMA 数组、raw 下取得页表计数；statistics 使用 raw 快照。匿名 `msync` 保留既有区间和 errno，在 gate 下读取；自持 pin 的 gate 忙冲突在此 int 接口对应 `-EAGAIN`，不伪报资源不足。并发文件域只接受经域校验的tmpfs及受控ext4映射，拒绝 SysV/未开放外部后备创建与 shmdt。共享 fd/offset、SysV、一般设备和系统压力并发尚未开放，依赖见[路线图](../goals.md#p6c-tlb-完成与页回收)。

```sh
make test-mm-pin-host test-mm-concurrency-host
make test-memory-object-concurrency-host test-mm-shared-concurrency-host
make test-mm-smp-riscv
make test-mm-smp-userland-riscv INIT_CONFIG=config/init.json
```

host 链接真实 MM/VMA/Sv39/allocator/uaccess/等待实现，只替换硬件及调度后端；2/4 actor 模型与两逻辑 CPU 活动集合模型分列。真实 MTTCG 双RAM验证见[两核模块](riscv-smp.md)。固定资料、准备/提交理由及资源证据见[内存学习说明](../learning/memory-management.md#匿名并发的许可与失效完成)。


## 受控 tmpfs 文件映射

文件缺页在 MM gate25 下取得临时 OFD owner，再在 gate 外、inode I/O30 读保护下取得代次、尺寸及 rank35 后备准备。私有复制、缺表和驻留候选在 raw 外完成；重取 gate 和 inode 读保护、完成最后可睡眠的驻留扩容后，核对 VMA 代次、真实来源、偏移、权限、inode 代次、EOF 与 PTE，才提交；普通单CPU记录也不能依赖睡眠前的校验。共享叶使用同一后备页，私有初始别名只读且标记 COW，没有短暂可写发布。首次共享缺页只发布、收费一份后备；共享 fork/写缺页不复制数据页。既有驻留 hash、扩容及范围扫描复杂度保持。

截断的 rank15 写操作门保留。校验后在 inode 写保护下关闭旧代次，释放 I/O 再逐登记取得一个 MM gate，撤销整页 EOF 外的 PTE/驻留并确认 SFENCE/RFENCE；最后在 inode 写保护下截断后备、处理尾页、提交尺寸/时间并开放代次。关闭后不新增分配或磁盘 I/O。固件失败保留数据、表、来源及退休 owner 后 fatal，不进入后备截断。旧 pin 仅保证存活，私有 COW 尾页保留自身内容，共享后备尾零；重新增长不暴露已截断内容。

登记由 MM 拥有，来源 OFD 保持 inode；有借用的游标覆盖同步回调和退休后继。停止登记在 gate 内摘链，drain 在 gate、inode I/O 和 raw 外完成，之后才释放登记/MM存储。首次登记状态 OOM 在 VMA 发布前返回。并发 fork 私有准备及 inode 读保护在父提交前完成，父私有叶失效确认后才发布完整 child 登记；关闭的代次返回 CONFLICT，不发布半成品 child。

```sh
make test-file-mapping-concurrency-host test-mm-file-concurrency-host
make test-mm-file-smp-riscv
make test-mm-file-smp-userland-riscv INIT_CONFIG=config/init.json
```

宿主链接实际 VFS/tmpfs/OFD/object/MM，4/16 KiB 的登记与来源 owner 模型、Sv39 4 KiB 的跨 MM 模型分别报告。真实 RV 使用独立 `build/riscv-mm-file-smp-test/`、MTTCG 两核及一核退化；默认用户和 LA 仍单 CPU。执行器身份和自然退出 owner 检查沿用 [RV SMP](riscv-smp.md)。

## 受控 ext4 文件映射

来源准备在MM gate下取得实际OFD引用，锁外由CPU0缓存worker执行冷读；恢复gate和
inode读保护后重查VMA/文件代次、EOF、权限及PTE，过期候选只归还本次取得的owner。
共享叶与其他MM使用同一缓存页，初始RO；私有叶保持只读后备别名/COW，fork按VMA分类。
磁盘可写pin附带按页写借用，快照关闭后不能新增许可；等待重开先释放gate和inode保护。
fault成功后pin若遇快照临时撤W，重新解析；真正的VMA撤权或EOF仍按用户故障结束。

CPU0范围同步每批固定最多16份OFD与范围，释放gate后执行，以免写回回调等待同一gate。
别名注销先摘链，再在gate/raw/inode外drain，忙gate的后台重试登记持实际alias借用，
由gate释放代次与通知交接保证寿命。数据、页表与来源仍在RFENCE确认后才归还。
缓存写回/截断的交接见[VFS](vfs-ext4.md#后台写回与水位回收)，真实两核及独立重启
见[RV SMP](riscv-smp.md)。`make test-mm-disk-concurrency-host`链接真实Sv39、VFS/OFD、
lwext4及镜像读写；宿主模型不证明硬件TLB或用户线程跨核ABI。
