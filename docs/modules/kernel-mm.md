# 内核 MM 模块

本文描述任务地址空间的通用所有权接口和当前 RISC-V Sv39 后端。页表格式、映射规则和硬件切换见 [RISC-V Sv39 分页模块](riscv-sv39.md)，地址空间与任务身份分离的背景见[内存管理学习总结](../learning/memory-management.md)。

## 范围与入口

| 文件 | 当前职责 |
|---|---|
| `include/kernel/mm.h` | 定义跨架构 MM 句柄、权限、状态、引用和 VMA 入口 |
| `include/arch/riscv/mm.h`、`arch/riscv/mm.c` | 用一张记录页封装 Sv39 用户地址空间与可选 VMA 集合，并实现当前构建所选的通用 MM 操作 |
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

`riscv_kernel_mm_satp()` 是 RISC-V scheduler 创建任务时使用的架构入口。通用层不暴露 Sv39 对象，也没有运行期 vtable；RISC-V 构建直接链接 RISC-V 实现，LoongArch 构建以后为同一通用接口提供 16 KiB/三级页表后端。这样任务和 syscall 层依赖 MM 语义，而不依赖页表格式，调用热路径也没有间接分派。

## 句柄、引用与状态

`struct kernel_mm` 是一个可移动的拥有型引用，不是地址空间本体。RISC-V 后端用 `record_page_address` 指向一张物理记录页；记录页保存引用计数、清理阶段和唯一的 `riscv_sv39_user_space`。多个 LIVE 句柄可以指向同一记录页：

- `acquire` 增加引用计数并发布一个新的独立 owner；vfork 与线程 clone 用它共享地址空间，任一线程释放自己的引用不撤销其他 owner。共享 MM 的 VMA/PTE 变更对同组立即可见；当前单 hart 关闭 SIE 串行化，并执行本地 TLB 失效，不等同于 SMP 同步；
- `fork` 创建独立地址空间，克隆 VMA/文件来源并让父子暂时共享用户物理页，逻辑 MM 和后续写入仍独立；
- `move` 转移一个 owner，不改变引用计数；
- `release` 消耗一个 owner，非末引用只减计数，末引用才销毁页表树和记录页；
- `lookup` 把架构权限翻译为 `KERNEL_MM_READ/WRITE/EXECUTE/USER`，不让通用调用者依赖 RISC-V PTE 位值。

输入目标必须是全零 `EMPTY` 句柄。成功移动后源进入 `MOVED`，成功释放后进入 `RELEASED`；二者都不再拥有资源。分配器释放属于 fail-stop 契约，不能由调用者按错误码重试；只有文件/OFD 等真实 I/O owner 的清理状态会交给上层继续处理。

RISC-V 创建先分配并解析记录页，最后才把 LIVE Sv39 空间移入记录；成功后输入空间成为 MOVED。记录页及页表的合法释放完成即结束；若同时持有 VFS/file-source owner，则仅保留对应的 I/O cleanup 状态。

## Program break 与匿名 heap

`kernel_mm_brk_initialize()` 只用于尚未发布、只有一个 owner 且已经启用 VMA 的新映像。它记录页对齐的起始 break、当前精确 break 和页对齐上界；RISC-V ELF image 把最高非空 `PT_LOAD` 的 `p_vaddr + p_memsz` 向上对齐作为起点，并将独立布局计算出的 mmap ceiling/vDSO 作为约束。每次 exec 因而得到由新 ELF 独立计算的 break，而 fork 复制父进程调用时的精确值。

`kernel_mm_brk()` 实现 raw Linux syscall 所需的返回语义：参数 0 查询当前值；落在起点以下、上界以上、溢出或因 VMA 冲突/metadata OOM 无法增长时，MM 保持不变并把原 break 写入结果，而不是产生负 errno。成功结果保留字节粒度；只有 VMA 和 PTE 操作向上按 4 KiB 对齐。

跨页增长只把新增 `[old_page_end, new_page_end)` 登记为 RW anonymous `DEMAND_ZERO` heap VMA，不提前分配数据页。首次 U-mode load/store 复用普通匿名 fault 路径分配零页。跨页缩小通过通用区间编辑撤销目标范围，因此能容忍用户先用 `munmap` 打洞、用 `mprotect` 分段或用 fixed mmap 替换局部区域；它不再依赖一个从 break 起点连续延伸的阶段性 heap VMA。缩小先让范围内用户 PTE 失效并刷新本 hart TLB，再无分配地提交 VMA 删除和精确 break；物理页释放完成后该操作才返回。

## 匿名与文件私有映射

`kernel_mm_mmap_anonymous()` 当前实现 anonymous-private demand-zero 映射。非 fixed 请求优先使用空闲的页对齐 hint，否则在每个 MM 的随机 mmap ceiling 以下、栈 guard 之外 top-down 选址；`FIXED_NOREPLACE` 只检查冲突，`FIXED` 则撤销旧页和 VMA 后替换。长度向上按 4 KiB 对齐，返回地址只在成功时写入。RISC-V 的 W&&!R PTE 编码保留，因此仅写保护被规范化为 RW。

`kernel_mm_validate_file_private_mapping()` 复用真实映射的规范化、范围、选址和 fixed-noreplace 冲突检查，但不分配来源节点、不编辑 VMA，也不预留返回的区间；当前单 hart syscall 在该检查与真实提交之间不会调度。`kernel_mm_mmap_file_private()` 接收调用者已经 pin 的可读普通文件 OFD、页对齐文件偏移和同一组选址/权限参数。syscall 层先完成无副作用校验，使零长度、非法 fixed 地址、offset 溢出和既有映射冲突保持原 errno 优先级；校验通过后才拒绝访问模式为 `O_WRONLY` 的 OFD 并返回 `-EACCES`，同时释放本次临时 pin。该可读要求不随请求的 `PROT_*` 组合改变。成功时 MM 消耗 pin，失败时仍由调用者持有。MM 对每个不同 OFD 只建一个来源节点，并由该节点持有一份来源引用；重复 mmap 不累积历史引用，VMA backing 借用同一对象。VMA 提交后若来源已存在，只递减一个由调用者刚取得且必然不是末引用的临时 pin；新来源则直接转移 pin，因此系统调用不会出现“返回错误但映射已生效”。关闭 fd 不影响映射；fork 为子 MM 建立独立来源节点并取得一份引用。`munmap`/fixed replace 在提交 VMA 后的冷路径释放已经没有 VMA 使用的来源节点，真实 VFS/OFD 清理错误由其 owner 状态向上转交。页故障查到 VMA 后直接取得 backing，不在 fault 热路径遍历 fd 表或来源链。

文件 VMA 不预分配数据页。read/execute 首次缺页从挂载页缓存取得共享页并建立 COW PTE；首次写若缓存已命中则复制缓存页，未命中则直接把文件内容读入新私有页，避免先创建缓存页再立即复制。缓存命中的写时 COW 例程自身完成该页的 `SFENCE.VMA`/必要 `FENCE.I`，外层缺页路径不重复刷新；其他新填充页由外层统一刷新。文件最后一页的有效内容之后补零；故障页起点已经不小于文件大小时返回 `BUS_FAULT`。可写根上的 write/truncate 会先完成介质更新再精确失效 node 页缓存；只读根则由块设备能力拒绝修改，因此两种挂载都使用创建时 OFD 持有的稳定 node/size。

ELF image 使用独立的 `kernel_elf64_source`，不把 `PT_LOAD` 当作普通 file-private mmap。source 在 exec 时一次解析 program headers，并将页区间规范化为 `ELF_PRIVATE` VMA 的 `backing_offset`。完整文件页可以共享 page cache；文件/BSS 边界页、多个段贡献页和 BSS 页由 fault 路径私有分配、清零并精确填充。source 引用由 MM 记录，重复映射不增加历史引用，fork 子 MM 获取独立引用，最后一个相关 VMA 消失才 release。可执行页发布后执行本地 `SFENCE.VMA` 和必要 `FENCE.I`，覆盖先读后取指。source 的 OFD/I/O 清理错误仍由 source owner 保留，堆和物理页释放不建立重试状态。

`kernel_mm_munmap()` 采用 Linux 洞语义：输入范围中没有 VMA 或只覆盖部分 VMA 仍可成功；resident、`PROT_NONE` 和待释放页都由 Sv39 owner 状态处理。`kernel_mm_mprotect()` 要求整个范围无洞覆盖，先准备 VMA 拆分容量，再原地修改已有 PTE 权限，最后提交 metadata；长度 0 对齐地址直接成功。`PROT_NONE` 不释放物理页，恢复权限后仍看到原内容。

## 硬件用户缺页解析

`kernel_mm_resolve_user_fault()` 的 `access` 必须恰为 READ、WRITE、EXECUTE 之一。RISC-V 后端先检查用户范围、VMA、逻辑权限和现有 PTE；VMA 外地址、`PROT_NONE`/权限冲突和仅驻留策略 VMA 的空洞返回 `NOT_MAPPED`。`ELF_PRIVATE/ELF` VMA 再按 source 区间二分取得 FILE、COMPOSITE 或 ZERO backing；只有确认需要提交 anonymous/file/ELF 页或解析 present COW 后，才要求目标 MM 是本 hart 当前 `satp`，从而既不为非法软件访问分配，也能让 uaccess 对合法未驻留页复用同一解析器。

匿名成功路径按 4 KiB 对齐故障地址，分配并清零一页，以 VMA 的完整 R/W/X 权限建立 U-mode PTE。文件路径则按上述缓存/私有策略取得页面。两者成功后都执行针对该虚拟页、ASID 0 的本地 `SFENCE.VMA`；VMA 带执行权限时还执行本 hart 的 `FENCE.I`，保证新填充的指令字节对后续取指可见。该规则覆盖匿名清零、文件填充和先读后执行的 file-private fault；COW 复制和恢复执行权限的页表路径也各自同步指令缓存。dispatcher 保持 `sepc` 不变，`sret` 后硬件重试原 load/store/fetch。`NOT_MAPPED` 在 Trap 边界终止为 `SIGSEGV(11)`，文件整页越过 EOF 的 `BUS_FAULT` 终止为 `SIGBUS(7)`；uaccess 把二者都转换为 `EFAULT`。物理页耗尽精确返回 `NO_MEMORY`；合法 owner 的页释放完成即返回，非法页表或分配器状态直接 fatal。PTE 已存在却仍产生允许权限的页故障或非活动 MM 都是内核状态错误。

scheduler 的 U-mode 硬件页故障和当前任务的 uaccess 都可进入该入口。uaccess 只在 `kernel_mm_lookup()` 报未驻留后尝试解析；合法页成功提交后重查 PTE，VMA 外或权限不符仍按用户 fault 处理。

## fork 与 COW

`kernel_mm_fork()` 先克隆所有会分配的 VMA 和文件来源 metadata，最后才调用 Sv39 两阶段 fork。子页表先取得每个 active/`PROT_NONE` 4 KiB 页的物理引用并以 COW 形态建立；全部构造成功后再把父进程原可写叶子去掉 W 并标记 COW，随后刷新父地址空间 TLB。metadata 或子页表构造失败时父 PTE 不变；父提交阶段不再分配。无效或已撤销的页不进入子地址空间，内核高半区仍只借用稳定映射。

写 fault 只接受软件明确标记的 present COW 页，不能把真正只读页误当成可写。若物理引用数大于 1，就分配并复制新页、把当前 PTE 改回 VMA 权限并释放旧引用；若只剩一个引用，则原地恢复写权限而不复制。`mprotect(PROT_NONE)` 使用不同 RSW 编码保存 exclusive/COW 属性，恢复后仍能正确决定是否复制。uaccess 写入也进入同一 resolver，因此内核代用户写内存不会绕过 COW。

创建失败时，目标句柄要么仍为 EMPTY，要么仍持有尚未发布的完整 owner；分配器释放不失败，调用者不按释放错误建立重试状态。Fork 仍需遍历已提交页和构造子页表，时间与驻留页数线性，但不再在 fork 时复制每页内容或预留同量数据内存；额外数据页成本只落在之后真正写入的页。当前 clone 仍在单 hart 关中断临界区内完成 walker 和 metadata 分配，大地址空间的最坏中断延迟尚需开发板测量。SMP 时还必须用 MM 锁、原子页引用和远端 TLB shootdown 保护父提交与并发 fault/unmap。

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

当前引用计数和记录内容由单 hart、关 SIE 的 scheduler 生命周期串行化，不是 SMP 原子操作。接入 SMP 时必须在 MM 引用和末引用判定处加入锁或原子协议，并与页表修改、TLB shootdown 协调；公共句柄接口不需要因此改变。

Scheduler 在用户任务创建时解析 MM、验证入口与栈权限，并缓存目标 `satp`。tick、current 校验和 context switch 直接读取任务前缀中的缓存，不解析记录页、不增减 MM 引用，也不遍历用户页表。地址空间切换的当前主要成本仍是 ASID 0 下的 `satp`/全局 `SFENCE.VMA`。demand-zero 或文件首次触页增加 VMA 二分查找、页表查询、页分配/I/O、PTE 写入和一次单页本地 fence；缓存命中的读 fault 不复制文件页，写 fault 至多复制一页。驻留后的普通用户访存不经过软件 fault 路径。`brk` 不是普通访存热路径；增长的 metadata 操作为排序数组插入，缩小需要遍历覆盖范围中已经存在的页表分支并执行一次全局本地 fence。当前缩小不回收变空的中间页表，它们由 MM 销毁统一释放；这避免在 unmap 提交中再引入页表页回滚状态，但会保留约为连续高水位虚拟跨度 0.2% 的 Level 0 表容量，稀疏触页时相对实际数据页的比例可能更高。尚无开发板 fork/fault/unmap 延迟和 TLB/cache 计数，不能从 QEMU 正确性测试推断硬件性能。

## 验证与限制

```sh
make test-mm-riscv
make test-vma-riscv
make test-brk-riscv
make test-mmap-riscv
make test-demand-page-riscv
make test-user-riscv
make test-riscv
```

MM 聚焦测试覆盖创建失败原子性、共享引用、移动、COW fork 的父子共享/写隔离/末引用原地恢复、`PROT_NONE` COW 属性，以及正常页表回收和物理页基线；同一 target 还运行独立 fatal kernel，注入一次页表 backing 无法解析并确认只产生一个 fatal 结果、不会返回 retry/success 路径。文件测试覆盖 cache hit/miss、write-first、尾页补零、整页越 EOF、fd 关闭后 fault、fork 后 OFD 来源和最终回收。syscall 聚焦测试验证校验错误先于不可读 OFD 的 `EACCES`，两类拒绝都释放临时 pin 且不进入 MM 提交；`test-userland-riscv` 用真实 musl mmap 覆盖零长度、非法 fixed 地址、原 fd 与 dup。`test-mmap-riscv` 与真实 ext4 `/init` 从 U-mode 完成匿名/文件私有 mmap、COW、SIGBUS、mprotect/munmap 生命周期。

当前只有 RISC-V 后端；映射仍由 Sv39/4 KiB 用户页实现。普通 fork 使用独立 MM+COW，线程 clone/vfork 共享同一 MM record；匿名映射和 ELF image 使用每 MM 的 ASLR mmap ceiling（无可信种子时确定性降级），但没有 commit accounting。栈软限制约束后续未驻留栈页的填充，已存在的 PTE 保留；新 exec 在固定容量 VMA 内按当前软限制建立初始栈。可读普通文件支持 MAP_PRIVATE，尚无 MAP_SHARED、写回或 truncate 并发；brk 尚未接入 RLIMIT_DATA。文件表和信号表不属于 MM。futex 当前以 MM 身份与用户地址为 key，共享文件映射落地前不提供跨 MM futex 语义。

## 驻留文件映射与截断

MM record 拥有按 VFS node 去重的稳定关联记录，以及每个驻留文件页的地址、物理页
身份和私有化状态。VFS 只借用关联；OFD 来源持有 node 的生命周期。共享 MM 不重复
登记，fork 在页表共享提交前复制来源/关联/驻留元数据，成功后登记新 MM。mmap 在
VMA/PTE 修改前预留关联；失败不消耗调用者 OFD。末个 VMA 消失或销毁时先解除关联，
再释放 OFD。不得登记可移动的 VMA 数组元素或 MM handle 地址。

首次文件 fault 在发布 PTE 前预留驻留记录；COW 成功后更新私有标志及物理地址。
缓存命中的 write-first fault 若 COW 物理分配失败，必须撤销临时 cache PTE 后返回，
不能留下无来源记录的 PTE。fork 后私有页面即使再次带 COW 标志，仍保持私有来源。
munmap/fixed replace 清除对应驻留记录，mprotect/PROT_NONE 保留来源。

VFS 向下截断按实际 live size（包括部分生效后报错）同步通知相关 MM。扫描每个
驻留记录并查询当前 VMA 与文件偏移，撤销整页起点不小于新 EOF 的 PTE，保留 VMA；
非对齐尾页只清零文件来源页的后缀，保留 private 页内容。当前与非当前 MM 使用
相同的 PTE 失效、全局本 hart SFENCE.VMA、物理引用释放顺序；尾页代码修改执行
FENCE.I。当前单 hart syscall 不可调度区保证登记和通知稳定，不代表 SMP 协议。

关联只扫描相关 MM；每个 MM 的驻留记录为链表，COW 查找 O(驻留文件页数)，截断
每页 VMA 查找 O(log VMA 数)。元数据随驻留规模增长；不在 tick 热路径扫描。
`test-files-riscv` 扫描准备阶段分配失败与 fork 回滚，并注入缓存命中后的 COW
物理分配失败；`test-userland-riscv` 与 `test-diff-abi-riscv` 验证真实驻留/COW、
PROT_NONE、非当前 MM、尾页、关闭 fd、unlink、O_TRUNC 与重新增长。
