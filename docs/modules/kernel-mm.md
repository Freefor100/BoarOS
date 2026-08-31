# 内核 MM 模块

本文描述任务地址空间的通用所有权接口和当前 RISC-V Sv39 后端。页表格式、映射规则和硬件切换见 [RISC-V Sv39 分页模块](riscv-sv39.md)，地址空间与任务身份分离的背景见[内存管理学习总结](../learning/memory-management.md)。

## 范围与入口

| 文件 | 当前职责 |
|---|---|
| `include/kernel/mm.h` | 定义跨架构 MM 句柄、权限、状态、引用和 VMA 入口 |
| `include/arch/riscv/mm.h`、`arch/riscv/mm.c` | 用一张记录页封装 Sv39 用户地址空间与可选 VMA 集合，并实现当前构建所选的通用 MM 操作 |
| `tests/riscv/mm_cases.c`、`tests/mm-riscv.sh` | 验证创建、共享引用、移动、查询和既有页表回收语义 |
| `tests/riscv/vma_cases.c`、`tests/vma-riscv.sh` | 验证 VMA 集成后的 fork、缺页解析、OOM 与 VMA 清理重试 |

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
    const struct kernel_mm *source);

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

`riscv_kernel_mm_satp()` 是 RISC-V scheduler 创建任务时使用的架构入口。通用层不暴露 Sv39 对象，也没有运行期 vtable；RISC-V 构建直接链接 RISC-V 实现，LoongArch 构建以后为同一通用接口提供 16 KiB/三级页表后端。这样任务和 syscall 层依赖 MM 语义，而不依赖页表格式，调用热路径也没有间接分派。

## 句柄、引用与状态

`struct kernel_mm` 是一个可移动的拥有型引用，不是地址空间本体。RISC-V 后端用 `record_page_address` 指向一张物理记录页；记录页保存引用计数、清理阶段和唯一的 `riscv_sv39_user_space`。多个 LIVE 句柄可以指向同一记录页：

- `acquire` 增加引用计数并发布一个新的独立 owner；
- `fork` 创建独立地址空间，复制用户映射、页内容、权限和已启用的 VMA 描述符，不共享用户叶子页；
- `move` 转移一个 owner，不改变引用计数；
- `release` 消耗一个 owner，非末引用只减计数，末引用才销毁页表树和记录页；
- `lookup` 把架构权限翻译为 `KERNEL_MM_READ/WRITE/EXECUTE/USER`，不让通用调用者依赖 RISC-V PTE 位值。

输入目标必须是全零 `EMPTY` 句柄。成功移动后源进入 `MOVED`，成功释放后进入 `RELEASED`；二者都不再拥有资源。失败时不会凭错误码暗示所有权，句柄状态仍精确说明调用者应重试还是继续持有。

RISC-V 创建先分配并解析记录页，最后才把 LIVE Sv39 空间移入记录；成功后输入空间成为 MOVED。记录页已经分配但无法立即释放时，输出成为 CLEANUP owner，从而不会遗失唯一物理地址。

## Program break 与匿名 heap

`kernel_mm_brk_initialize()` 只用于尚未发布、只有一个 owner 且已经启用 VMA 的新映像。它记录页对齐的起始 break、当前精确 break 和页对齐上界；RISC-V 静态 ELF 路径把最高非空 `PT_LOAD` 的 `p_vaddr + p_memsz` 向上对齐作为起点，把栈 guard 起点作为上界。每次 exec 因而得到由新 ELF 独立计算的 break，而 fork 复制父进程调用时的精确值。

`kernel_mm_brk()` 实现 raw Linux syscall 所需的返回语义：参数 0 查询当前值；落在起点以下、上界以上、溢出或因 VMA 冲突/metadata OOM 无法增长时，MM 保持不变并把原 break 写入结果，而不是产生负 errno。成功结果保留字节粒度；只有 VMA 和 PTE 操作向上按 4 KiB 对齐。

跨页增长只把新增 `[old_page_end, new_page_end)` 登记为 RW anonymous `DEMAND_ZERO` heap VMA，不提前分配数据页。首次 U-mode load/store 复用普通匿名 fault 路径分配零页。跨页缩小通过通用区间编辑撤销目标范围，因此能容忍用户先用 `munmap` 打洞、用 `mprotect` 分段或用 fixed mmap 替换局部区域；它不再依赖一个从 break 起点连续延伸的阶段性 heap VMA。缩小先让范围内用户 PTE 失效并刷新本 hart TLB，再无分配地提交 VMA 删除和精确 break。物理页释放失败不会恢复硬件映射：Sv39 以 invalid software-owned PTE 保留 owner，后续覆盖该范围的增长先重试释放，MM 最终销毁也会继续回收。

## 匿名映射与保护

`kernel_mm_mmap_anonymous()` 当前实现 anonymous-private demand-zero 映射。非 fixed 请求优先使用空闲的页对齐 hint，否则在 heap 上界/栈 guard 以下 top-down 选址；`FIXED_NOREPLACE` 只检查冲突，`FIXED` 则撤销旧页和 VMA 后替换。长度向上按 4 KiB 对齐，返回地址只在成功时写入。RISC-V 的 W&&!R PTE 编码保留，因此仅写保护被规范化为 RW。

`kernel_mm_munmap()` 采用 Linux 洞语义：输入范围中没有 VMA 或只覆盖部分 VMA 仍可成功；resident、`PROT_NONE` 和待释放页都由 Sv39 owner 状态处理。`kernel_mm_mprotect()` 要求整个范围无洞覆盖，先准备 VMA 拆分容量，再原地修改已有 PTE 权限，最后提交 metadata；长度 0 对齐地址直接成功。`PROT_NONE` 不释放物理页，恢复权限后仍看到原内容。

## 硬件用户缺页解析

`kernel_mm_resolve_user_fault()` 的 `access` 必须恰为 READ、WRITE、EXECUTE 之一。RISC-V 后端先检查用户范围、VMA、逻辑权限和现有 PTE；VMA 外地址、`PROT_NONE`/权限冲突和静态 ELF 的 `RESIDENT_REQUIRED` 空洞返回 `NOT_MAPPED`。只有确定需要为 anonymous `DEMAND_ZERO` VMA 建页时，才要求目标 MM 是本 hart 当前 `satp`，从而既不为非法软件访问分配，也能让 uaccess 对合法未驻留页复用同一解析器。

成功路径按 4 KiB 对齐故障地址，分配并清零一页，以 VMA 的完整 R/W/X 权限建立 U-mode PTE，然后执行针对该虚拟页、ASID 0 的本地 `SFENCE.VMA`。dispatcher 保持 `sepc` 不变，`sret` 后硬件重试原 load/store/fetch。物理页耗尽精确返回 `NO_MEMORY`；页已经分配但回滚释放失败返回 `CLEANUP_REQUIRED`；PTE 已存在却仍产生允许权限的页故障、页表损坏或非活动 MM 都是内核状态错误，不能降级成用户 `SIGSEGV`。

scheduler 的 U-mode 硬件页故障和当前任务的 uaccess 都可进入该入口。uaccess 只在 `kernel_mm_lookup()` 报未驻留后尝试解析；合法页成功提交后重查 PTE，VMA 外或权限不符仍按用户 fault 处理。

## fork 与后续 COW 边界

`kernel_mm_fork()` 当前使用 eager copy：遍历源 Sv39 用户树，为每个 active 或 `PROT_NONE` 4 KiB owner 分配新物理页并复制内容，同时重建相同 VA 和逻辑权限；protected 页在子空间中仍保持硬件不可访问。内核高半区根项仍只借用稳定内核映射，不复制也不参与子 MM 的释放。成功后父子地址空间初始字节、VMA 和精确 break 相同，但任一方后续写页、改保护或调整 break 都不会改变另一方；retired 页不进入子地址空间。源 MM 始终保持 LIVE。

创建失败时，目标句柄要么仍为 EMPTY，要么成为持有精确剩余 owner 的 CLEANUP；调用者必须按状态继续释放，不能只按错误码假定没有分配。测试同时核对映射、物理地址不等、双向写隔离、父先释放后子仍可用以及最终页数回到基线。

Eager copy 的时间和新增物理内存都与已提交用户页数线性相关，当前还在关中断的 clone 临界区内执行，因此大进程 fork 会增加中断延迟并产生明显复制开销。这个实现建立的是正确的普通进程语义；以后可在同一 `kernel_mm_fork()` 接口内改为 COW：父子叶子改成共享只读引用，写 fault 再复制，并补充页引用、PTE 原子更新、TLB shootdown 与失败回滚。Scheduler 和 syscall ABI 不需要因 COW 改变。

## 末引用回收

末引用只能在其 `satp` 已不活动时释放。有 VMA 时回收按三个阶段执行：

```text
LIVE(last ref)
  -> release VMA metadata
  -> destroy private user leaves and page tables
  -> release MM record page
  -> RELEASED
```

VMA metadata 释放失败时转入 `CLEANUP_VMAS`，后续调用只重试这项释放；成功后才进入 Sv39 回收。Sv39 销毁一旦发生部分提交，MM 转入 `CLEANUP_SPACE`；后续调用只继续地址空间回收。地址空间已经销毁而记录页释放失败时转入 `CLEANUP_RECORD`；后续调用只重试记录页。所有 CLEANUP 都可 move，不能 acquire、lookup 或生成 `satp`。没有 VMA 的既有 MM 若 Sv39 销毁未发生提交即失败，则仍保持 LIVE；非末引用释放不触碰 VMA 或页表树。

用户退出路径先切到内核根页表，再在仍有效的任务内核栈上调用 `kernel_mm_release()`；失败 owner 交给 idle reaper 在后续重试。因此不会销毁硬件当前仍在使用的用户根。MM 完成后才允许任务成为 zombie，zombie 只保留身份、亲缘、wait status 和任务页。

## 并发与性能边界

当前引用计数和记录内容由单 hart、关 SIE 的 scheduler 生命周期串行化，不是 SMP 原子操作。接入 SMP 时必须在 MM 引用和末引用判定处加入锁或原子协议，并与页表修改、TLB shootdown 协调；公共句柄接口不需要因此改变。

Scheduler 在用户任务创建时解析 MM、验证入口与栈权限，并缓存目标 `satp`。tick、current 校验和 context switch 直接读取任务前缀中的缓存，不解析记录页、不增减 MM 引用，也不遍历用户页表。地址空间切换的当前主要成本仍是 ASID 0 下的 `satp`/全局 `SFENCE.VMA`。demand-zero 只增加 heap/栈首次触页的 VMA 二分查找、页表查询、页分配/清零、PTE 写入和一次单页本地 fence；驻留后的普通用户访存不经过软件 fault 路径。`brk` 不是普通访存热路径；增长的 metadata 操作为排序数组插入，缩小需要遍历覆盖范围中已经存在的页表分支并执行一次全局本地 fence。当前缩小不回收变空的中间页表，它们由 MM 销毁统一释放；这避免在 unmap 提交中再引入页表页回滚状态，但会保留约为连续高水位虚拟跨度 0.2% 的 Level 0 表容量，稀疏触页时相对实际数据页的比例可能更高。尚无开发板 fault/unmap 延迟和 TLB 计数，不能从 QEMU 正确性测试推断硬件性能。

## 验证与限制

```sh
make test-mm-riscv
make test-vma-riscv
make test-brk-riscv
make test-mmap-riscv
make test-demand-page-riscv
make test-user-riscv
make test-user-elf-riscv
make test-riscv
```

MM 聚焦测试覆盖创建失败原子性、共享引用、移动、eager fork 的内容/权限复制与写隔离，以及页表部分回收、记录页访问/释放失败后的阶段化重试。VMA 聚焦测试还覆盖精确 break、匿名映射区间编辑、打洞后的 brk、`PROT_NONE` fork、uaccess 补页、retired owner 和 metadata 失败。`test-mmap-riscv` 汇总 Sv39/VMA/syscall 并让真实 ext4 `/init` ELF 在 U-mode 完成 mmap/mprotect/munmap 生命周期。

当前只有 RISC-V 后端；映射仍由 Sv39/4 KiB 用户页实现。普通 clone 已使用独立 MM，但尚无 COW 或 `CLONE_VM` 共享进程；匿名映射没有 commit accounting/ASLR，file-backed/shared mmap 尚未实现；`brk` 上界目前只由地址布局约束，尚未接入 `RLIMIT_DATA`。文件表、信号处理表和其他进程资源不属于 MM。
