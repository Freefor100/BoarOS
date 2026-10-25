# 内核 MM 模块

本文描述任务地址空间的通用所有权接口、共享 MM 策略、RISC-V Sv39 与 LA64 后端。页表格式、映射规则和硬件切换见 [RISC-V Sv39 分页模块](riscv-sv39.md)，地址空间与任务身份分离的背景见[内存管理学习总结](../learning/memory-management.md)。

普通 MM、受控任务绑定的 MM 和 `USER_SMP` 使用同一修改协议：operation gate 串行映射编辑及缺页提交，MM raw 保护 PTE、resident 索引和 pin 许可。匿名、COW、ELF 与文件缺页按真实后备类型准备候选，在锁外完成分配和 I/O，恢复保护后重查映射再发布；不再按对象资格选择两套 mapper。匿名缺页和驻留用户复制可在发起 CPU 完成，ext4 普通文件冷页在发起 CPU 完成缓存装入与请求提交，日志和后台写回由 CPU0 推进。非叶变更同步全范围，退休页在本地与远端 TLB 确认后才归还。实际执行边界见[RV 两核](riscv-smp.md#真实用户态与cpu0业务交接)。

## 锁页、MEMLOCK 与驻留查询

`mlock`、`mlock2`、`munlock`、`mlockall`、`munlockall` 和 `MAP_LOCKED` 使用同一份 VMA 属性。`LOCKED` 表示锁定范围，`ONFAULT` 表示保留按需装入；MM 保存未来映射策略。VMA 分裂、合并、替换、搬移和删除同时维护锁定虚拟字节数，合并必须具有相同锁定策略。`/proc/<pid>/status` 的 `VmLck` 包含尚未触页的 ONFAULT 范围。

接纳使用实际调用线程组的 MEMLOCK 软限额，按架构页取整，`CAP_IPC_LOCK` 豁免。锁定重叠范围只计一次；下调限额保留已有范围。mmap（含固定替换）、brk、新增栈范围、mremap 扩大和 SysV attach 在修改前检查额度。mmap 按 Linux 将本次完整长度加入旧锁定用量后检查，额度拒绝保留固定替换的旧映射；brk 拒绝返回旧 break，锁定映射的 mremap 扩大超限返回 EAGAIN。mprotect 保留策略。

立即锁定先提交 VMA 属性，再释放 MM 修改资格，复用匿名、COW 和文件缺页路径逐页预装。每页在同一次 MM 资格内检查当前锁定策略、选择访问类型并分派到既有缺页后端，不再释放后重复取得资格。普通用户缺页保留原调用过程。分配、COW 复制和文件 I/O 仍在锁外完成，恢复后重新核对 VMA 代次及来源；冲突时重新检查该页策略。PROT_NONE 范围保留属性而不装入，普通 mlock 返回 ENOMEM，ONFAULT 登记成功；纯执行权限范围沿执行缺页路径装入。普通 mlock 预装缺页失败转为 ENOMEM，物理内存不足转为 EAGAIN，已提交属性和已驻留前缀保留；mmap 和 mlockall 的预装按 Linux 采用尽力完成规则。`MCL_FUTURE` 的重复设置替换未来策略，munlockall 同时清除当前和未来策略。

当前没有 swap；PTE 已持有真实物理引用，文件映射的 cache alias 也参与回收判断，因此锁页复用现有持有关系。munlock 解除策略和计费，显式 unmap、文件截断和打洞继续执行原失效及回收协议。fork 创建的新 MM 清除锁定属性和未来策略；CLONE_VM/vfork 共享原 MM；成功 exec 使用新 MM，失败 exec 保留原状态。

mincore 每页输出最低位的驻留状态，查询本身不装入页面。MM raw 内查询已有页面归属，包含 PROT_NONE 页面；没有 PTE 时查询内存对象或已完成装入的文件缓存。一次最多处理 16 页，任务 I/O 工作区保存结果，用户输出在 MM 保护外完成。文件来源借用覆盖锁外权限和缓存查询，恢复 MM 资格后核对 VMA 代次；截断、解除映射和最终关闭沿原来源退休协议等待借用结束。文件所有者、CAP_FOWNER 或有当前写权限的调用者可见真实状态，其余调用者按 Linux 获得全 1；匿名映射直接返回真实状态。VMA 空洞返回 ENOMEM，之前完整输出的前缀保留。

直接检查使用 `tests/workloads/resource/memlock.c` 和 `tests/mm-resource-limits-host.sh`；前者与固定 Linux 运行相同 ELF，后者检查映射接纳、计数、部分修改和物理分配失败。通用 MAP_GROWSDOWN 仍在后续范围。


## madvise 与共享段锁定

RV64/LA64 的 madvise(233) 支持 NORMAL、RANDOM、SEQUENTIAL、WILLNEED、DONTNEED，以及 DONTFORK/DOFORK、WIPEONFORK/KEEPONFORK，参数中的 advice 按 Linux 32 位 int 解释。NORMAL/RANDOM/SEQUENTIAL 验证映射范围并沿用当前缓存策略；它们是性能建议。未接入的扩展模式返回 EINVAL。

DONTNEED 在 MM 修改资格内撤销范围的 PTE 和文件驻留登记，保留 VMA、保护位、AS/DATA 用量和后备对象。私有匿名页重新访问得到零，私有文件 COW 页面重新取文件内容；共享匿名、SysV 和共享文件的数据保存在真实后备中。复用现有有界退页过程，页和页表在活动 CPU 完成 TLB 失效后释放。范围中的空洞最终返回 ENOMEM，空洞两侧的有效映射仍执行；遇到锁定 VMA 返回 EINVAL，保留此前范围已经完成的变化。当前 VDSO 缺少特殊缺页重装路径，DONTNEED 对它返回 EINVAL；Linux VDSO 可重装，这一差异保留为缺口。

WILLNEED 使用已有文件缓存装入过程，单次建议至多准备16个数据页，不安装用户 PTE、不改变OFD offset。文件/ELF来源借用覆盖锁外I/O，最后一个映射或原FD关闭后，来源在建议结束时归还。返回0表示接纳建议；装入失败采用 Linux 忽略 fadvise 返回值的规则，实际读取/缺页继续处理其实际I/O结果。无swap时私有匿名、纯BSS和无文件身份的ELF来源返回EBADF，共享内存没有swap-in工作。

DONTFORK 使指定范围不进入新建子 MM，DOFORK 恢复继承；WIPEONFORK 保留子进程的私有匿名范围，但不继承父物理页，首次访问从零后备装入，KEEPONFORK 清除此策略。纯 ELF BSS 按实际零后备处理，文件及共享映射拒绝 WIPEONFORK。策略随 VMA 分裂、合并、mprotect、mremap 和后续 fork 保留；锁定与 fork 策略共用原 flags 字段、分别按位修改，munlockall 只清除锁位。

fork 构造时直接筛选子 VMA，子 MM 只借用仍被继承范围使用的文件、ELF、共享匿名来源及驻留记录。两架构页表准备明确区分 PRIVATE、SHARED、SKIP；DONTFORK 和 WIPEONFORK 不取得父页引用，只有实际进入子页表的私有叶才使父页转 COW。子构造失败由已有清理归还临时页表、来源和附件，父 COW 提交在准备成功后执行；共享 MM 的线程和 vfork 沿原共享地址空间规则。

SHM_LOCK/UNLOCK 在既有CPU0共享内存控制路径执行。段保存首次LOCK时调用者真实UID的账户引用，按段尺寸向上取整的页数计费，以当前调用者MEMLOCK软限额接纳，CAP_IPC_LOCK豁免。无该capability时，euid须为owner或creator；LOCK零限额返回EPERM，超额返回ENOMEM。权限与零限额检查先于重复LOCK判断；重复操作沿用已有计费。IPC_SET保留锁定状态，IPC_STAT和`/proc/sysvipc/shm`显示SHM_LOCKED。

LOCK保留按需装页，当前memory_object已经持有已驻留页。UID或owner改变后，UNLOCK或RMID后的最后attachment销毁向原账户归还。共享段的UID额度与普通mlock的MM虚拟字节、VmLck分别统计。

共享内存查询与槽/代次规则见[SysV SHM](sysv-shm.md)。直接消费者为`tests/workloads/resource/madvise.c`、`shm_lock.c`，宿主入口为`tests/mm-resource-limits-host.sh`、`tests/shm-lock-host.sh`和文件I/O并发检查。RV固定Linux参考关闭swap；madvise的LA对照使用独立的无swap参考缓存，与BoarOS保持相同条件：

```sh
python3 tests/loongarch/prepare.py --component linux --profile platform-noswap
```

原`platform`参考缓存继续供其他消费者使用，配置和镜像身份分别检查。

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

`struct kernel_mm` 是一个可移动的拥有型引用，不是地址空间本体。句柄直接持有内核记录指针，`record_page_address` 只用于归还记录页。记录保存引用计数、清理阶段和架构页表；创建、fork 的成功与清理分支、acquire、move 和最终 release 共同维护指针与所有权。正常访问不再从物理句柄重新解析记录，也不重复审计整份 VMA/来源/清理状态。多个 LIVE 句柄可以指向同一记录：

- `acquire` 增加引用计数并发布一个新的独立 owner；vfork 与线程 clone 用它共享地址空间，任一线程释放自己的引用不撤销其他 owner。共享 MM 的 VMA/PTE 使用同一 operation gate 与 metadata raw；发布变更后同步活动 CPU 的翻译，确认完成才回收旧页，具体见下文并发协议；
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

`kernel_mm_munmap()` 采用 Linux 洞语义：输入范围中没有 VMA 或只覆盖部分 VMA 仍可成功；resident、`PROT_NONE` 和待释放页都由 Sv39 owner 状态处理。`kernel_mm_mprotect()` 在 MM gate 内由 VMA 层一次顺序遍历合并覆盖、权限和资源限制检查，以局部 DATA 用量投影判定可成功的前缀，同时预留首尾拆分空间；MM 随后一次修改该前缀的已有 PTE、提交 metadata 并完成 TLB 同步。遍历只在起点查询数组下标，不复制整份 VMA，也不再将已检查的前缀交回通用保护接口重验。遇洞、权限或限额失败保留成功前缀，首段拆分 OOM 不修改权限，末段拆分 OOM 仍提交此前前缀。同权限段无需修改。长度 0 对齐地址直接成功。`PROT_NONE` 不释放物理页，恢复权限后仍看到原内容。

AS/DATA 限额属于当前调用者的线程组，MM 只保存 VMA 用量；`CLONE_VM` 的独立线程组共享用量但分别受自身限制约束。VMA 原有统计遍历同时维护总虚拟字节与私有可写、非栈的 DATA 字节；拆分、保护、替换、搬移和 fork 使用同一统计来源。mmap、shmat、brk、mremap、mprotect 和栈增长均在已有 MM 操作锁内检查并修改用量。当前执行者保持组身份存活，接纳按字段原子读取限额；跨任务查询及候选 exec 仍在调度保护内取得快照。mprotect 仅在原非 DATA 区间变为 DATA 时读取并检查限额，降低权限和同分类修改沿原接纳规则处理。降低限制保留当前映射，后续增长检查当前限额；DATA 软限制为零时，映射扩张采用硬限制，brk 的精确字节检查仍使用零软限制。

`brk` 先检查主 ELF 的 `end_data-start_data` 加 `requested-start_brk`，包括同页调整，再对新增页检查 AS/DATA。`MAP_FIXED` 以全部覆盖字节抵扣扩张量，在撤映射前检查限额；mremap 扩张同样先检查限额，因此此类失败保留固定目标。mprotect 使用固定 Linux 的新旧属性检查，并逐段保留成功前缀。初始栈和 exec 的限制检查见 [用户 ELF](user-elf.md)。

## `mremap` 区间调整与搬移

`kernel_mm_mremap()` 返回零或负 Linux errno，成功才写入新地址。普通 mmap 建立的私有/共享匿名及普通文件映射支持缩小、原地扩大、`MREMAP_MAYMOVE` 自动选址和 `MREMAP_FIXED|MREMAP_MAYMOVE` 固定搬移；源可以是单个 VMA 的子范围。非固定缩小沿用 munmap 尾部语义，等长不移动直接保留地址；原地扩大要求旧范围结束于源 VMA 末端，新增区间无映射。新增页继续按原后备缺页，不预分配整段数据内存。

入口在 MM gate 下分别检查固定目标和实际缩小旧尾；任一区间含 SysV 附件时先结束 operation、转入 CPU0 服务再重取 gate 并重查，避免远端关闭附件目录引用，也不把两区间之间的无关映射纳入迁移判断。搬移在共同 MM gate 下准备 VMA 两端拆分及插入容量，关闭新用户页 pin 并排空已有 pin，再按驻留页预留目标页表。两架构后端转移原始叶 PTE，保留物理引用、COW、`PROT_NONE` 和访问权限，数据页不复制，RSS 不变；同一次 raw 修改迁移文件 resident 的地址哈希与链表登记。整个区间的叶转移后同步活动 CPU 的新旧翻译，确认完成再通过普通退休协议回收源空表；VMA 提交无需分配。准备阶段 OOM 归还未发布候选，已经发布的空目标表也经失效确认后回收，源叶仍保留。

VMA 保留后备对象、文件权限上限和源子范围偏移。共享匿名对象本身是无固定长度的稀疏页索引，扩大不更换对象，fork 两侧扩大到同一对象偏移仍看到同一页。文件来源 OFD、node–MM 登记和缓存 alias 在搬移期间继续由原 owner 持有；缓存回调取得 MM gate 后读取 resident 的当前地址，因此搬移前已借出的旧地址回调仍保护新地址。截断回调同样在取得 gate 后按当前 VMA/offset 处理 EOF，旧地址被其他映射复用不改变新映射。

参数校验顺序依据固定的 [`mm/mremap.c`](../../references/linux/mm/mremap.c)。固定目标先撤销，随后页表准备失败不会恢复该目标；固定缩小也可先撤源尾部，接口不承诺整次失败无副作用。合法的 `MREMAP_DONTUNMAP`、`old_size=0` 共享复制、特殊映射搬移，以及跨多个连续 VMA 的等长固定搬移返回 `EOPNOTSUPP`；私有映射零旧长度、缺失 MAYMOVE、重叠固定区间等仍先返回对应参数错误。跨 VMA 扩大及包含洞的跨 VMA 搬移返回 `EFAULT`。

`sh tests/mm-remap-host.sh` 检查物理身份、COW/保护页、子范围、共享对象、文件偏移/EOF、准备 OOM、pin 排空及延迟 TLB 确认后的 owner 归还；文件截断用明确握手覆盖回调等待期间的再次搬移和旧 VA 复用。`CACHE_HOST_BLOCKS=4096 sh tests/mm-disk-concurrency-host.sh mremap` 使用真实 ext4 与页缓存检查搬移前借出的 alias 回调、搬移后写入和同步，以及资源归还。宿主 MM 用 RV64/4 KiB 后端；LA64/16 KiB 使用真实用户差分验证，不能由修改宿主页大小宏替代。

## 硬件用户缺页解析

`kernel_mm_resolve_user_fault()` 的 `access` 必须恰为 READ、WRITE、EXECUTE 之一。通用层先检查用户范围、VMA、逻辑权限和现有 PTE；地址无 VMA 返回 `NOT_MAPPED`，合法 VMA 的权限不足返回 `ACCESS`。`ELF_PRIVATE/ELF` VMA 按 source 区间二分取得 FILE、COMPOSITE 或 ZERO backing，再按实际缺失的资源准备候选。硬件缺页与 uaccess 使用同一解析器，不为非法软件访问分配页面。

匿名成功路径按架构页大小对齐故障地址，分配并清零一页，以 VMA 权限建立用户 PTE。文件路径按上述缓存/私有策略取得页面。实际提交者根据本次 PTE 变化登记翻译与指令代次：已有表内的单页发布按页同步，非叶结构变化使用全范围同步；可执行内容填充、COW 和执行权限发布还同步取指。RISC-V 对应 `SFENCE.VMA`、必要的远端 RFENCE 与 `FENCE.I`，不能由后端每次改单页时隐含执行。dispatcher 保持 `sepc` 不变，`sret` 后硬件重试原 load/store/fetch。`NOT_MAPPED/ACCESS` 在 Trap 边界形成 `SIGSEGV` 的 MAPERR/ACCERR，文件整页越过 EOF 的 `BUS_FAULT` 形成 `SIGBUS`；uaccess 将这些用户访问错误转换为 `EFAULT`。物理页耗尽精确返回 `NO_MEMORY`，非法页表或分配器状态直接 fatal。

scheduler 的 U-mode 硬件页故障和当前任务的 uaccess 都可进入该入口。uaccess 在 pin 遇到未驻留或需要写故障处理的页面时解析；成功后重新 pin，VMA 外或权限不符仍按用户访问错误处理。

并发 fault 可以在候选准备期间由另一任务先完成同一地址的映射或权限升级。恢复 gate 后，若 VMA 仍允许且 PTE 已满足请求，resolver 直接成功；实际提交者负责 SFENCE/RFENCE，迟到或 resumed resolver 不重复同步。映射代次、来源、EOF 或权限已经变化时丢弃本次候选并重新判断，不能把过期候选提交给新 VMA。固定资料 [`mm/memory.c`](../../references/linux/mm/memory.c) 的 `fix_spurious_fault()` 用于核对该语义；真实匿名及文件同页竞争见[RV 两核](riscv-smp.md#用户态原程序与压力组合)。

## fork 与 COW

`kernel_mm_fork()` 先克隆所有会分配的 VMA、共享匿名对象引用和文件来源 metadata，最后才调用 Sv39 两阶段 fork。子页表先取得每个 active/`PROT_NONE` 4 KiB 页的物理引用；共享匿名页保持原权限，其他页以 COW 形态建立。全部构造成功后再把父进程私有可写叶子去掉 W 并标记 COW，随后刷新父地址空间 TLB。metadata 或子页表构造失败时父 PTE 不变；父提交阶段不再分配。无效或已撤销的页不进入子地址空间，内核高半区仍只借用稳定映射。

写 fault 只接受软件明确标记的 present COW 页，不能把真正只读页误当成可写。若物理引用数大于 1，就分配并复制新页、把当前 PTE 改回 VMA 权限并释放旧引用；若只剩一个引用，则原地恢复写权限而不复制。`mprotect(PROT_NONE)` 使用不同 RSW 编码保存 exclusive/COW 属性，恢复后仍能正确决定是否复制。uaccess 写入也进入同一 resolver，因此内核代用户写内存不会绕过 COW。

创建失败时，目标句柄要么仍为 EMPTY，要么仍持有尚未发布的完整 owner；分配器释放不失败，调用者不按释放错误建立重试状态。Fork 仍需遍历已提交页并构造子页表；共享页分类在子构造和父提交时对每个驻留页查询 VMA，成本为 `O(P log V)`（P 为驻留页数，V 为 VMA 数），另有 metadata 和页表遍历，但不在 fork 时复制每页内容或预留同量数据内存。额外数据页成本只落在之后真正写入的页。fork 在 gate 下构造私有 child，分配和复制在 MM raw 外完成，父私有叶的 COW 发布在 raw 内完成，确认远端失效后再归还旧引用。文件登记与实际来源沿同一协议迁移。

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

MM、页表及用户 pin 已持有实际物理页生命周期时，通过 `physical_page_map_owned()` 直接调用绑定的映射函数，不再取得分配器锁、查询 buddy 所有权树。页表页没有异步缺页、I/O 或其他能让映射失败条件稍后消失的 producer；映射失败表示内部不变量损坏。因此 Sv39 teardown 直接触发 fatal trap，不保留 `CLEANUP_SPACE`，也不允许下一次 `release` 把部分释放的树伪装成恢复成功。VFS/OFD 的真实外部 I/O owner 仍使用各自 cleanup 状态。

用户退出路径先切到内核根页表，再在仍有效的任务内核栈上调用 `kernel_mm_release()`；只有真实 VFS/OFD cleanup 仍有 owner 时才交给后续回收上下文处理。因此不会销毁硬件当前仍在使用的用户根。MM 完成后才允许任务成为 zombie，zombie 只保留身份、亲缘、wait status 和任务页。

## 并发与性能边界

MM 句柄各自持有一个实际引用，引用和 user 计数使用原子操作；句柄本身不能被其他执行者并发 move/release。所有活 MM 的映射编辑共用 rank25 gate，页表、pin、resident 哈希和活动 CPU 共用 rank15 raw，页引用可嵌套 physical rank20。哈希候选在 raw 外分配，重排、发布和摘除在 raw 内完成，旧存储在 raw 外释放。普通驻留匿名页复制只取得 MM pin 及物理引用所需的四次实际锁，不取得 operation gate，也不因同一 MM 存在文件映射而增加额外保护。末次释放要求 pin、活动 CPU 和退休批次归零。

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

已接入构建期绑定的RV64 Sv39/4 KiB与LA64 16 KiB/三级页表后端；共用策略处理私有/共享匿名、ELF、SysV 及 tmpfs/ext4 映射；SysV attachment 和未并行化后端的实际业务仍在 CPU0 边界处理。普通 fork 使用独立 MM，私有页 COW、共享匿名与共享文件页保持共享；线程 clone/vfork 共享同一 MM record。匿名映射和 ELF image 使用每 MM 的 ASLR mmap ceiling（无任何随机材料时确定性降级；仅 DTB 混种不提供可信安全保证），但没有 commit accounting。栈 VMA 随实际向下增长更新，VmSize 不计尚未扩展的最大栈窗口。可读普通文件支持 MAP_PRIVATE/MAP_SHARED 与 `msync`。文件表和信号表不属于 MM。private futex 使用 MM 创建时分配的单调身份号与用户地址，避免 MM record 页回收后重用身份；共享映射的 futex 身份与引用由 MM 一并解析，具体规则见[调度模块](kernel-scheduler.md#等待与-futex)。

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

VFS 向下截断按实际 live size（包括部分生效后报错）同步通知相关 MM。保留 VMA，
撤销文件内页起点不小于新 EOF 的 PTE；非对齐尾页只清零文件来源页的后缀，保留
private 页内容。范围摘除复用下面的有界遍历，剩余 resident 链只检查保留尾页。
登记与来源借用维持跨等待寿命；PTE 变更在 MM raw 内发布，锁外确认本地及远端
翻译失效后才归还旧页，尾页可执行内容改变时另推进指令同步。

关联只扫描相关 MM；每个 MM 以动态虚拟页地址哈希查询驻留记录，保留链表负责遍历。哈希碰撞比较完整地址，装载因子不超过 1，按二倍扩容；分配成功后无分配地重建并替换桶，失败保留旧索引。fork 提前准备全部桶，unmap/fixed replace/截断同步摘除索引与链表；最后 MM 释放桶。查找为平均常数成本。munmap/fixed replace 与截断使用有界区间摘除：每次 raw 最多检查 16 个页表项，实际数据/空表 owner 交给同一个 16 项退休批次。每个涉及的叶表只扫描一次；范围外的叶只用于判断表是否仍被使用，缺失子树直接跳过。一个 middle 范围结束后有界检查空表，跨同步仅保存 VA、下标和占用标记，不保存 PTE 指针。后端在清 PTE 前把 VA/物理身份交给 MM，由 MM 解绑 resident 的哈希和活动链；alias 清理和实际页引用释放均在 raw 外完成。mprotect 仍以有结束地址的遍历访问驻留叶，包括 PROT_NONE。mprotect 对仍共享缓存的叶保留写保护，不再先开放写权限后全表 rearm。完整链表用于销毁与 fork；截断先按 VMA 形成 EOF 外的连续区间并复用同一有界摘除；剩余驻留链只处理保留尾页。元数据随驻留规模增长；不在 tick 热路径扫描。
`test-files-riscv` 扫描准备阶段分配失败与 fork 回滚，并注入缓存命中后的 COW
物理分配失败；`test-userland-riscv` 与 `test-diff-abi-riscv` 验证真实驻留/COW、
PROT_NONE、非当前 MM、尾页、关闭 fd、unlink、O_TRUNC 与重新增长。

`riscv_kernel_mm_get_statistics` 提供诊断快照。驻留探测、准备页、pin 获取/等待、退休批次和 fault 重试等纯统计只在 `COST_DIAGNOSTICS=1` 更新；pin 数、页引用、活动 CPU 与 TLB 代次始终参与正确性。测试用链接包装观察实际 pin、锁与解析调用。驻留匿名页的 1、64、4096 字节双向复制均由每次 9 次锁获取、5 次受检查地址解析降为 4 次锁获取、0 次受检查解析；这四次仍保护 MM pin 与物理页引用。`make test-scale-riscv` 在 16/64 MiB 文件全部读驻留后按置换顺序首次写入，验证探测增长不超过 6 倍，并注入前 128 页准备阶段的元数据分配失败。首次写开放权限、写回 rearm、mprotect 和未发布 fork child 使用纯单页 PTE 修改，不在后端执行 SFENCE 或指令同步。已发布 MM 在 raw 内登记翻译/指令代次，raw 外按实际范围统一同步；未发布 child 由首次激活完成同步。单页降权/恢复合计只访问两个相关 resident，不随其他驻留页增长。VMA 权限编辑后以一次读写游标压缩相邻可合并项，被合并的 attachment 引用各归还一次，不再逐次左移后缀。宿主用巨大稀疏范围检查遍历跳过空子树并遵守结束地址。引用与脏代次协议保持不变。

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

`kernel_mm_check_task_domain()` 仅在创建受控内核任务时检查实际消费者、来源和 heap/allocator 关系，不修改 MM 的同步方式，也不关闭系统分配器的压力挂钩。普通用户 MM 无需启用远端或并发资格。受控测试仍可使用自己的资源和交错，匿名、文件 pin 与映射修改使用相同生产协议。

每个复制片段在 MM raw 下完成 PTE/权限检查及真实物理 acquire，复制在锁外；同一 task/I/O owner 归还 pin。pin 固定页存活，不固定映射身份或内容。`munmap` 可先撤销映射，旧片段靠 pin 完成；`mprotect` 关闭新许可并排空旧片段，fork 关闭写许可并排空旧写 pin。已持有需要排空的 pin 时不能等待自己，非法 owner fatal；零长度及无效参数不进入 drain。精确复制前缀保持。

`kernel_copy_to_user()` 用一个局部 cursor 完成单次复制，结束前归还最后一个 pin；普通文件读在本批已取得的缓存页内保留当前用户页 pin，跨来源页或 iovec 片段不重复解析同一目标页，单页请求也共用这一批次路径。换目标页时先归还再处理缺页，整批结束或出错时归还，下一次存储等待不携带用户 pin。该游标只延长现有片段借用，不缓存映射身份，撤映射、撤权限和共享页写回仍遵守原来的 pin 排空规则。普通与受控读取共用这一复制核心和缺页解析；成本与吞吐分别见 [VFS/ext4](vfs-ext4.md#当前成本边界)。

驻留 pin 在 MM raw 内检查当前 PTE、访问许可并取得实际物理引用。futex 的登记与 pin 顺序见[调度模块](kernel-scheduler.md#等待与-futex)。共享磁盘写 pin 还取得该 entry 的写借用；普通匿名复制不因此普遍增加 gate。缺页或 COW 使用同一个 resolver，成功后重取 pin；没有 resident-only 复制、重试或本地读取专用接口。fork 和降权关闭相应新许可，等待旧写入者归还，pin 结束仅需 raw，不等待 gate。

文件与 ELF 的来源节点持有实际 OFD/ELF owner；缺页准备期间借用该节点，最后 VMA 消失也不能提前关闭来源。映射登记在 gate 内停止和摘除，gate、inode 与 raw 外排空回调，再释放来源。最终关闭暂时需要 CPU0 或真实 I/O 清理时，同一退休节点继续持有 owner；用户返回准备在 offset、pin 等调用资格已经归还后排空它，不复活已退休来源，也不建立第二组引用。

active CPU 位图与 TLB 代次在 MM raw 内维护。撤映射、降权、COW 和截断先发布翻译变更并固定退休 owner，raw 外完成本地 SFENCE 与同步 SBI RFENCE，确认后才归还数据和表页。新加入者在相同保护下登记；换根由架构入口完成一次全量失效，同根仅在加入或翻译代次变化时补做失效。CPU 记录实际观察到的翻译代次，相同 MM、相同代次的再次激活不清空 TLB。可执行页的填充、COW、执行权限发布及文件尾清零另推进指令代次；首次进入和未观察到该代次的 CPU 执行指令同步，不把普通 PTE 修改一概当作代码变化。离开者在旧翻译停止后清活动位。固件失败保留实际资源并 fatal。

`kernel_mm_proc_memory_snapshot()`在 gate 下固定 VMA 数组、raw 下取得页表计数；statistics 使用 raw 快照。匿名 `msync` 保留既有区间和 errno，在 gate 下读取；自持 pin 的 gate 忙冲突在此 int 接口对应 `-EAGAIN`，不伪报资源不足。共享文件映射遵守同一来源与 alias 协议。SysV 的 attachment 修改在实际业务边界进入 CPU0，匿名范围编辑不迁移。设备、网络和命名空间仍在各自串行边界执行。

```sh
make test-mm-pin-host test-mm-concurrency-host
make test-memory-object-concurrency-host test-mm-shared-concurrency-host
make test-mm-smp-riscv
make test-mm-smp-userland-riscv INIT_CONFIG=config/init.json
```

host 链接真实 MM/VMA/Sv39/allocator/uaccess/等待实现，只替换硬件及调度后端；2/4 actor 模型与两逻辑 CPU 活动集合模型分列。真实 MTTCG 双RAM验证见[两核模块](riscv-smp.md)。固定资料、准备/提交理由及资源证据见[内存学习说明](../learning/memory-management.md#匿名并发的许可与失效完成)。


## tmpfs 文件映射

文件缺页在 MM gate25 下借用已持有 OFD owner 的来源节点，再在 gate 外、inode I/O30 读保护下取得代次、尺寸及 rank35 后备准备。私有复制、缺表和驻留候选在 raw 外完成；重取 gate 和 inode 读保护、完成最后可睡眠的驻留扩容后，核对 VMA 代次、真实来源、偏移、权限、inode 代次、EOF 与 PTE，才提交；普通单CPU记录也不能依赖睡眠前的校验。重取 gate 后还须重新查询 PTE 和缺失的非叶层级：相邻文件的最后一个 resident 可被截断并回收共用页表，而不改变本 VMA 代次。若缺表增加，保留已准备的数据页、来源和表页引用，释放 gate/inode 后只补空的 prepared 槽，再重复上述校验；普通 backend INVALID 仍是地址空间错误。同步范围由本次实际发布的非叶决定，已有表内的单页发布只做该页同步。共享叶使用同一后备页，私有初始别名只读且标记 COW，没有短暂可写发布。首次共享缺页只发布、收费一份后备；共享 fork/写缺页不复制数据页。既有驻留 hash、扩容及范围扫描复杂度保持。

截断的 rank15 写操作门保留。校验后在 inode 写保护下关闭旧代次，释放 I/O 再逐登记取得一个 MM gate，撤销整页 EOF 外的 PTE/驻留并确认 SFENCE/RFENCE；最后在 inode 写保护下截断后备、处理尾页、提交尺寸/时间并开放代次。关闭后不新增分配或磁盘 I/O。固件失败保留数据、表、来源及退休 owner 后 fatal，不进入后备截断。旧 pin 仅保证存活，私有 COW 尾页保留自身内容，共享后备尾零；重新增长不暴露已截断内容。

登记由 MM 拥有，来源 OFD 保持 inode；有借用的游标覆盖同步回调和退休后继。停止登记在 gate 内摘链，drain 在 gate、inode I/O 和 raw 外完成，之后才释放登记/MM存储。首次登记状态 OOM 在 VMA 发布前返回。并发 fork 私有准备及 inode 读保护在父提交前完成，父私有叶失效确认后才发布完整 child 登记；关闭的代次返回 CONFLICT，不发布半成品 child。

```sh
make test-file-mapping-concurrency-host test-mm-file-concurrency-host
make test-mm-file-smp-riscv
make test-mm-file-smp-userland-riscv INIT_CONFIG=config/init.json
```

宿主链接实际 VFS/tmpfs/OFD/object/MM，4/16 KiB 的登记与来源 owner 模型、Sv39 4 KiB 的跨 MM 模型分别报告。真实 RV 使用独立 `build/riscv-mm-file-smp-test/`、MTTCG 两核及一核退化；默认用户和 LA 仍单 CPU。执行器身份和自然退出 owner 检查沿用 [RV SMP](riscv-smp.md)。

## ext4 文件映射

来源准备在 MM gate 下取得实际来源借用，锁外在调用 CPU 准备冷页与描述，CPU0 缓存 worker 执行后端读取；恢复 gate 和
inode读保护后重查VMA/文件代次、EOF、权限及PTE，过期候选只归还本次取得的owner。
共享叶与其他MM使用同一缓存页，初始RO；私有叶保持只读后备别名/COW，fork按VMA分类。
磁盘可写pin附带按页写借用，快照关闭后不能新增许可；等待重开先释放gate和inode保护。
fault成功后pin若遇快照临时撤W，重新解析；真正的VMA撤权或EOF仍按用户故障结束。

范围同步每批固定最多16份OFD与范围，释放gate后执行，以免写回回调等待同一gate。
别名注销先摘链，再在gate/raw/inode外drain，忙gate的后台重试登记持实际alias借用，
由gate释放代次与通知交接保证寿命。数据、页表与来源仍在RFENCE确认后才归还。
缓存写回/截断的交接见[VFS](vfs-ext4.md#后台写回与水位回收)，真实两核及独立重启
见[RV SMP](riscv-smp.md)。`make test-mm-disk-concurrency-host`链接真实Sv39、VFS/OFD、
lwext4及镜像读写；宿主模型不证明硬件TLB或用户线程跨核ABI。

用户 fault 统计由 `kernel_mm_resolve_user_fault()` 的共用完成出口负责，后备实现只返回解析结果；成功一次计一次，失败/内部重试不计，服务任务处理其他 MM 不计到服务者。
`make test-fault-count-riscv test-fault-count-loongarch TEST_MEMORIES="512M 1G"` 使用真实 tmpfs 映射和 proc stat 对照共享首次读取、私有 COW 的计数增长及失败不增长，不比较两系统偶然的累计数值。
