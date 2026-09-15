# 内存管理学习总结

本文整理开发启动期内存管理和分页时需要掌握的知识、BoarOS 已经确定的选择及理由、平台能力依据和可复用的验证经验。当前接口、限制和测试契约以 [DTB 与启动内存布局模块](../modules/dtb-memory.md)、[物理页分配模块](../modules/physical-pages.md)、[内核堆模块](../modules/kernel-heap.md)、[RISC-V Sv39 分页模块](../modules/riscv-sv39.md)和[内核 MM 模块](../modules/kernel-mm.md)为准。

## 从物理内存到虚拟地址

物理地址标识平台物理地址空间中的位置，其中既可能是 RAM，也可能是 ROM 或 MMIO 设备。虚拟地址是分页开启后软件使用的地址，需要由 MMU 根据页表翻译成物理地址。

页是虚拟地址空间的固定大小单位，页框是物理内存中同样大小、同样对齐的单位。口语中常把二者都称为“页”，但页表项实际描述的是虚拟页如何映射到物理页框。

若页大小为 `2^n` 字节，地址低 `n` 位就是页内偏移，页和页框起始地址的低 `n` 位必须为零。向下对齐可以直接清除低 `n` 位；向上对齐需要先加 `2^n - 1`，所以必须先检查整数溢出。

内存范围通常写成半开区间 `[base, base + size)`。这种表示让区间长度等于两端之差，相邻区间可以无重叠地拼接，也便于表示空区间。计算任何 `base + size` 前都必须检查是否超过整数上限。

## 启动期怎样找到可用 RAM？

内核不能根据“机器有多少内存”直接使用整段 RAM。固件、内核镜像、设备树、启动栈和平台保留区都可能已经占用其中一部分。

设备树 DTB 中与启动内存相关的内容主要有：

- `/memory` 节点的 `reg` 属性描述 RAM 区间。
- memory reservation block 描述固件预留的物理区间。
- `/reserved-memory` 的静态子节点描述操作系统不得作为普通 RAM 使用的区间。
- DTB header 的 `totalsize` 给出 DTB 自身占用长度。

DTB 使用大端编码，并用父节点的 `#address-cells` 和 `#size-cells` 决定地址和长度各由多少个 32 位 cell 组成。解析器不能把可能未对齐的 DTB 字节直接转换成 C 结构，而应逐字段读取、检查边界并组合整数。

得到 RAM 和保留区后，启动期通常执行以下区间运算：

1. 检查所有区间长度和端点是否合法。
2. 把保留区裁剪到 RAM 范围内。
3. 按起始地址排序。
4. 合并重叠或相邻的保留区。
5. 从 RAM 中逐段减去保留区，得到字节粒度的可用区间。
6. 将可用区间首尾向内对齐到完整页框。

向内对齐会丢弃首尾不足一页的碎片，但可以保证返回的页框不会覆盖保留字节或越过 RAM 边界。

### BoarOS 的应用方式

BoarOS 使用固件传入的 DTB 发现 RAM，而不是把 QEMU 或某块开发板观察到的 RAM 大小写成常量。启动布局在 DTB 保留区之外继续排除内核 ELF 和 DTB 自身，再把剩余字节区间交给物理页分配器。

这样做的原因是 QEMU 的 `-m` 会改变 RAM 大小和 DTB 放置位置，真实开发板的固件保留区也可能不同。DTB 解析与区间相减属于通用机制；固件交接方式和设备地址图属于平台事实。

## 物理页分配器负责什么？

物理页分配器跟踪哪些 RAM 页框空闲、哪些已经发放。它返回页对齐的物理地址，不负责选择虚拟地址，也不自动建立页表映射。

基本语义通常包括：

- 初始化：接收已经排除保留区的物理内存范围。
- 分配：取出一个或多个空闲页框，并更新所有权状态。
- 释放：归还已经分配的页框；未分配地址、非对齐地址和重复释放都属于错误。
- 查询：报告总页数、空闲页数或其他统计信息。

常见实现包括空闲链表、位图和伙伴系统。空闲链表可以把链指针存进空闲页本身；位图便于快速查询单页状态；伙伴系统适合连续多页分配和相邻空闲块合并。无论采用哪一种，都必须明确页框所有权、失败时状态是否变化、分配内容是否清零，以及并发访问如何同步。

### 为什么启动分配器和运行期分配器分成两种模式？

BoarOS 使用同一个物理页对象承载两种有明确迁移点的算法。分页启动期只有“可用区间
游标 + 已释放页侵入式链表”：初始化只记录区间，首次分配按地址递增，不需要先访问
任意物理页内容。这一点很重要，因为最终 direct map 尚未激活，内核还没有稳定方式
把任意 PA 当作 C 指针使用。

最终 Sv39 根和高半区生效后，内核绑定 direct-map 访问函数，并单向 finalize 为
buddy。迁移不是重新分配或复制已经交给页表的页面，而是重建所有权描述：启动期
已发放页标为 allocated，回收链与未发放尾部标为空闲，metadata 自身标为 internal。
这样页表、scheduler 和 MM 可以一直使用同一单页 API，不会把“启动期地址能否解引用”
扩散为每个消费者的分支。

### Buddy split 和 coalesce 怎样工作？

order N 块含 `2^N` 个连续基础页，物理起点还必须按整块大小对齐。每个 order 有一条
空闲链；请求 order N 时，分配器向上寻找第一个非空 order，取下一块并逐级对半 split，
每次把暂时不用的上半块放入低一级链。释放时，大小相同且地址只相差该块大小的另一
半就是 buddy，可由物理页号对相应 bit 做异或得到：

```text
buddy_pfn = pfn XOR (1 << order)
```

若 buddy 也空闲且仍在同一个可用 RAM 区间，就把两者摘链、取低地址作为高一级块，
继续向上合并。BoarOS 在 metadata 中保存双向链索引，因此已知 buddy head 可以 O(1)
摘链；split/coalesce 只随 order 数增长，不再像启动回收链那样扫描所有空闲页。

逐页 metadata 让错误语义也更精确：allocated head 记录原 order 和引用数，allocated tail
不能单独释放，free head/tail 能识别重复释放，internal 页永远不能返回给调用者。代价是
finalize 需要 O(页数) 初始化，并永久占用每页 16 字节；4 KiB 页下约占 RAM 的
0.391%，16 GiB 理论完整 RAM 为 64 MiB。分配/释放大块还要更新本次块内的逐页状态，
但常用 order 0 只触碰一条记录。

共享物理页和共享虚拟地址空间不是一回事。COW 与只读文件缓存只让不同 PTE owner 持有
同一 order-0 页的独立引用；每次 release 先减引用，末引用才归还 buddy。高阶连续块仍保持
单 owner，避免任一 tail 单独存活。内存压力回收则发生在首次分配失败的慢路径：分配器调用
一个已注册回收器并只重试一次，同时抑制回调中的递归回收。它让页缓存可回收，但不把 LRU、
文件或 I/O 知识塞进通用 buddy 接口。

metadata 从可用 RAM 自托管，而不是编进固定数组或预留固定 heap。这样 512 MiB、
1 GiB、16 GiB 乃至不同开发板容量都按实际 RAM 缩放；当前实现要求某个未发放 range
tail 能连续容纳 metadata，这是用单段 RAM 目标换取简单索引和良好局部性的明确边界。

页大小由架构构建目标确定：RISC-V 使用 4 KiB，即 `BOAROS_PAGE_SHIFT=12`；LoongArch 基线使用 16 KiB，即 `BOAROS_PAGE_SHIFT=14`。分配算法可以共享页大小概念，但物理地址怎样转换成内核可访问地址仍受各架构地址模式约束。

### 物理页之上为什么还需要内核堆？

物理页分配器只能高效表达整页或二次幂连续页所有权；文件系统对象、路径、缓存和设备状态通常只有几十到几百字节。若每个小对象独占一页，内部碎片和 TLB/cache 压力会很大；若各子系统各自切割页面，又会重复实现对齐、回收和错误检查。因此需要由物理页供给、返回普通内核虚拟地址的动态堆。

小对象分配常见方法包括固定 size class、slab/slub 和通用边界标记堆。size class 把请求向上归入少量槽大小，同一页只保存同类对象，分配和释放不需要遍历可变长度块；代价是请求大小与槽大小之间存在内部碎片。边界标记更灵活，但拆分、合并和空闲结构更复杂，也更容易把不可控线性扫描放进热路径。

BoarOS 当前对不超过 2048 字节的对象使用 16 至 2048 字节的二次幂 class，一页 slab 内用空闲槽链和 bitmap 记录状态；slab 变空就立即归还 buddy。更大的对象直接按所需页数向上取整为 buddy order，避免额外复制和固定容量 arena。这样 heap 容量随可用 RAM 扩展，且大对象仍保持自然对齐；代价是大请求会因二次幂 order 产生外部可见的页级浪费，当前也不提供跨页非连续的 `vmalloc`。

`calloc` 必须在乘法前检查溢出并清零完整结果；`realloc` 在原 class 或原 buddy order 仍能容纳新长度时可以原地返回，否则先分配、复制两者较小长度，再释放旧对象。任何失败都不能改变旧对象所有权。统计中的 live bytes、当前/峰值页数和分配次数既用于发现泄漏，也为以后判断 per-CPU cache、延迟回收或更细 size class 是否值得提供基线。

当前堆服务单 hart 启动、可写/只读 ext4、文件表、线程和用户映像等生产路径，没有锁。SMP 到来时必须先用锁保护全局 slab/buddy 交接，再根据目标开发板上的争用与 cache miss 数据决定是否加入 per-CPU magazine；per-CPU cache 会减少锁竞争，但也会增加跨 CPU 回收和空闲页滞留，不能仅凭“通常更快”提前加入。

## 多级页表怎样翻译地址？

多级页表把一个巨大的单层数组拆成按需分配的树。虚拟地址被分为多级虚拟页号和页内偏移，每一级虚拟页号选择当前页表中的一个表项。

```text
根页表
  -> 用最高级虚拟页号选择表项
  -> 非叶子表项指向下一级页表
  -> 继续使用下一级虚拟页号
  -> 叶子表项给出物理页号和权限
  -> 物理页号 + 原页内偏移 = 物理地址
```

多级结构只为实际使用的虚拟地址范围分配下级页表。代价是一次 TLB miss 可能触发多次内存访问，因此 CPU 使用 TLB 缓存最近的翻译结果。

页表还承担权限控制。内核通常区分可执行代码、只读数据、可写数据和栈、用户页面、内核页面及 MMIO。把数据页设为可执行或把只读页设为可写都会削弱隔离。

## RISC-V 有哪些分页模式？

RV64 的 `satp.MODE` 定义了以下标准模式：

| 模式 | MODE | 虚拟地址位数 | 页表层级 | 基页大小 |
|---|---:|---:|---:|---:|
| Bare | 0 | — | 无页表翻译 | — |
| Sv39 | 8 | 39 | 3 | 4 KiB |
| Sv48 | 9 | 48 | 4 | 4 KiB |
| Sv57 | 10 | 57 | 5 | 4 KiB |

实现不必支持全部模式；向 `satp` 写入不支持的 MODE 时，整个写入不生效。规范要求支持 Sv48 的实现同时支持 Sv39，支持 Sv57 的实现同时支持 Sv48，因此也间接支持 Sv39。层级越多，虚拟地址空间越大，但页表容量和 TLB miss 时的遍历成本也更高。

Sv32 是 RV32 的分页模式，不是 RV64 上与 Sv39 并列的可选模式。Sv64 目前仍是为未来标准保留的编码。

## Sv39 的地址和页表项

Sv39 使用 4 KiB 基页和三级页表。每张页表正好占一个 4 KiB 页框，包含 512 个 64 位页表项。

| 位 | 名称 | 用途 |
|---|---|---|
| `63:39` | 符号扩展 | 必须全部等于 bit 38，否则地址不合法 |
| `38:30` | `VPN[2]` | 根页表索引 |
| `29:21` | `VPN[1]` | 第二级索引 |
| `20:12` | `VPN[0]` | 第三级索引 |
| `11:0` | page offset | 4 KiB 页内偏移 |

每级索引都是 9 位，所以能选择 `2^9 = 512` 个页表项。`satp` 同时保存 MODE、ASID 和根页表的物理页号 PPN；根页表必须按 4 KiB 对齐，因此地址低 12 位不需要存入 PPN。

Sv39 页表项的常用低位标志为：

| 标志 | 含义 |
|---|---|
| `V` | 表项有效 |
| `R` | 可读 |
| `W` | 可写 |
| `X` | 可执行 |
| `U` | U-mode 可访问 |
| `G` | 全局映射 |
| `A` | 已访问 |
| `D` | 已写入 |

`R/W/X` 全为零时，有效表项指向下一级页表；其中任一为一时，表项是叶子映射。`W=1` 且 `R=0` 是保留组合。若在最高级或第二级提前遇到叶子项，就形成 1 GiB 或 2 MiB 大页；大页的虚拟地址和物理地址都必须按大页大小对齐。第三级叶子项形成普通 4 KiB 映射。

Sv39 所说的“三级”是三次索引，不是只允许一种叶子大小：

| 硬件层级 | 常用软件名称 | 使用的索引 | 叶子覆盖范围 |
|---|---|---|---:|
| Level 2 | 根页表 / PGD | `VPN[2]` | 1 GiB |
| Level 1 | PMD | `VPN[1]` | 2 MiB |
| Level 0 | PTE 页 | `VPN[0]` | 4 KiB |

走到 Level 0 才得到 4 KiB 叶子；在 Level 1 提前结束就是 2 MiB 叶子，在 Level 2 提前结束就是 1 GiB 叶子。中间级表项只保存下一级页表的物理页号，不能带叶子的 R/W/X 组合。

### BoarOS 当前为什么组合 2 MiB 和 4 KiB？

BoarOS 的 Sv39 建表器在虚拟地址、物理地址和剩余长度都按 2 MiB 对齐时优先建立 2 MiB 叶子，其余边缘和权限边界使用 4 KiB 叶子。这样既能给 text、rodata、data 设置不同权限，又不会为大片连续 RAM 逐页建立数百万个 PTE。

以完全对齐的 16 GiB 映射为例：全部使用 2 MiB 叶子需要 8192 个叶子项、16 张 Level 1 表和 1 张根表，共 17 个 4 KiB 页表页。若全部使用 4 KiB 叶子，则需要 4,194,304 个叶子项、8192 张 Level 0 表、16 张 Level 1 表和 1 张根表，共 8209 个页表页，约 32.06 MiB。大页还会减少相同范围所需的 TLB 项数。

当前不建立 1 GiB 叶子。它可以在以后作为 Level 2 叶子加入，并不要求改变三级结构；但 1 GiB 同时要求 VA/PA 对齐，而且无法在叶子内部表达内核段权限差异。当前 2 MiB 已把 16 GiB 建表开销降到 17 页，先保留更简单的冲突和权限模型。

## `satp`、TLB 和 `SFENCE.VMA`

TLB 缓存虚拟页到物理页框的翻译和权限。内存中的页表项改变后，TLB 里的旧结果不会因为普通 store 自动失效。

RISC-V 使用 `SFENCE.VMA` 同步页表更新与后续地址翻译。它可以刷新全部地址空间，也可以按虚拟地址和 ASID 缩小范围。写入页表、切换根页表或复用 ASID 时，必须按照特权规范安排 `satp` 更新和 `SFENCE.VMA`。

内核最终页表与运行期用户页表的所有权不同。BoarOS 的每个用户地址空间拥有独立根表、低半区用户叶子页和为它们分配的下级页表；根表高半区条目只借用稳定的内核映射，不拥有也不释放对应页表。用户映射固定使用 4 KiB 叶子并要求 `U` 权限，防止一个大叶子无意覆盖不同权限或归属的对象。

当前全部地址空间使用 ASID 0，因此切换根表前后执行全局 `SFENCE.VMA`。这比 ASID 定向刷新开销更高，但没有 ASID 分配、复用代际和跨 hart shootdown 的隐藏状态，适合当前单 hart 基线。销毁地址空间时按页表树后序回收：先释放归它所有的用户叶子页和下级页表，最后释放根页；借用的内核高半区不参与递归释放。

## 地址空间为什么需要独立生命周期？

可调度任务和虚拟地址空间不是一一对应关系。Linux `clone` 的 `CLONE_VM` 决定新任务与调用者共享同一个内存描述对象；同一线程组的线程通常共享 MM，而 `fork` 产生的新进程拥有独立 MM（页内容可以再由 COW 延迟复制）。`execve` 则为调用任务准备并提交新的地址空间。若把页表树直接嵌进任务，引用共享、最后一个使用者回收和 exec 替换都会迫使 scheduler 理解具体页表格式。

BoarOS 因此把 `kernel_mm` 定义为引用计数的拥有型句柄：`acquire` 得到同一地址空间的另一个独立引用，`move` 只转移引用，`release` 在末引用时才销毁页表树。RISC-V 后端用独立记录页保存引用计数和 Sv39 owner；通用任务层只看到统一权限与生命周期，LoongArch 后端以后可在不改变 scheduler 接口的前提下使用自己的 16 KiB/三级页表对象。构建时直接选择后端而不是每次运行期经 vtable 分派，避免在当前只有单一架构实现时引入间接调用。

引用计数回答“还有多少 owner”，不能单独解决并发。当前单 hart scheduler 用关中断串行化 acquire/release 和末引用回收；SMP 下多个 hart 可能同时取得或释放引用，必须使用原子计数或 MM 锁，并把最后释放与页表修改、TLB shootdown 纳入同一生命周期协议。

地址空间释放按固定的后序顺序完成：先撤销叶子、刷新 TLB，再释放物理页和下级页表，最后释放根表与 MM record。合法 owner 的释放不会返回可重试状态；非法页、引用或分配器元数据直接触发 fatal。只有文件来源的真实 VFS/I/O 清理错误由其 owner 保留。测试应覆盖释放顺序、非法释放 fatal 和最终空闲页基线，而不是伪造 allocator 释放失败。

“访问函数有错误返回”不自动证明 teardown 可以重试。BoarOS production 的 runtime page access 是固定 direct-map 地址转换；finalized allocator 又先确认目标仍是 allocated page。它没有 pager、设备 I/O 或异步修复者，页表 owner 也不会在两次 release 之间重新建立映射。因此合法页表 backing 无法 resolve 只能归为 kernel invariant violation。测试 wrapper 可以制造一次失败再恢复，但这属于仅测试可达的 producer；若据此保留 cleanup 状态，反而会把已经部分释放的页表树暴露给第二次销毁。正确 regression 应验证 fail-stop，而不是验证人工恢复。

## VMA 和 PTE 为什么都需要？

页表回答的是“此刻这个虚拟页有没有硬件翻译、落在哪个物理页、权限是什么”。它不足以表达一个尚未驻留但已经合法的逻辑区间：按需分页的匿名堆、只提交了栈顶的向下栈、file-backed mapping 都可能没有某个地址的 PTE，却仍要求 fault handler 知道它可以怎样补页。VMA（virtual memory area）补充这一层，保存半开区间、逻辑权限、匿名或文件 backing 和用途；fault 路径先查 VMA 决定合法性和补页策略，再把驻留结果写入 PTE。

因此“VMA 存在”与“PTE 存在”是两个不同的不变量：

```text
VMA 不存在 + PTE 不存在  -> 无效地址，fault
VMA 存在   + PTE 不存在  -> 合法性由 VMA 的 fault policy 决定
VMA 存在   + PTE 存在    -> 当前可由硬件翻译
VMA 不存在 + active PTE 存在 -> 内核错误；撤销必须先让 PTE/TLB 失效再收紧 VMA
```

BoarOS 为 VMA 显式记录 fault policy，而不是从 role 猜测行为。栈、`brk` heap 和 private-anonymous mmap 使用 `DEMAND_ZERO`：栈 VMA 覆盖低半区顶端完整 8 MiB reserve，初始 PTE 只覆盖参数栈和 64 KiB headroom；heap VMA 只覆盖当前 program break 向上对齐的范围；mmap 先保留地址区间。只读普通文件的 private mmap 使用 `FILE_PRIVATE`，VMA 保存文件页对齐 offset 并借用由 MM 独立持有的 OFD。ELF `PT_LOAD` 使用专用 `ELF_PRIVATE`：完整文件页从 page cache 取得并以 COW 发布，文件/BSS 边界页和纯 BSS 页由 fault 路径私有分配、清零和填充；MM 对 source 保留引用。其余合法页由真实 U-mode page fault 或当前 MM 的 uaccess 首次访问时提交。reserve 下方的一页 guard 没有 VMA，因此不会因“靠近栈”被隐式扩展。

一次当前用户任务的页故障按互斥状态分类：

```text
不是当前活动 MM / MM 状态损坏 --------------------> 内核 fatal
地址不在 VMA，或访问不满足 VMA 权限 --------------> 用户访问故障
PTE 已存在且权限也允许，却仍收到页故障 ------------> 内核地址空间错误
PTE 不存在 + RESIDENT_REQUIRED ---------------------> 用户访问故障
PTE 不存在 + 匿名 DEMAND_ZERO
  +-- 页分配、清零和映射成功 ----------------------> 单页 SFENCE.VMA；可执行页另 FENCE.I；原指令重试
  +-- 物理页耗尽 ----------------------------------> 任务因资源原因退出，wait status 9
  +-- 已分配页无法访问且回滚释放完成 --------------> 返回原始故障
  +-- 其他页表/分配器状态错误 ----------------------> 内核 fatal
PTE 不存在 + FILE_PRIVATE
  +-- 页起点 >= 文件大小 --------------------------> SIGBUS，wait status 7
  +-- read/execute fault ---------------------------> 共享缓存页，建立 COW PTE
  +-- write fault ----------------------------------> 缓存命中则复制，否则直接读入私有页
present COW PTE + write fault
  +-- 物理引用数 == 1 -----------------------------> 原地恢复 W
  +-- 物理引用数 > 1 ------------------------------> 复制一页、替换 PTE、释放旧引用
```

同一个 MM fault 入口被硬件 U-mode 和内核 uaccess 共用，但两者的失败边界不同：硬件用户缺页时物理页耗尽属于任务资源耗尽，可生成资源退出；syscall 的 uaccess 在复制过程中遇到 `NO_MEMORY`、未映射或权限错误，则把这次复制报告为 `FAULT`，保留已复制前缀，由 syscall 按自身 ABI 转成 `-EFAULT` 或部分成功。页表损坏、已映射页无法解析等真正的内核状态错误仍不能伪装成用户指针错误。

页故障是同步异常，`sepc` 指向需要重试的原指令。补页成功后不能像 `ecall` 一样把 `sepc` 前移；更新 PTE 后还要执行针对该虚拟页的本地 `SFENCE.VMA`，带执行权限的页还要用 `FENCE.I` 排序此前写入的指令字节，再由 `sret` 重执行 load/store/fetch。当前所有用户地址空间使用 ASID 0，且只有单 hart，所以本地失效足够；SMP 下必须把远端正在运行同一 MM 的 hart 纳入 shootdown，并让每个执行 hart 完成自己的指令同步。

demand-zero 把未触碰的栈/heap 页物理内存和清零成本推迟到首次访问，file-private mapping 则把文件 I/O 推迟到首次触页。代价是首次触页需要 trap、VMA 二分查找、软件页表查询、可能的页分配/清零或文件 I/O、PTE 写入和 TLB 失效；驻留后的普通访问仍由硬件翻译，不增加软件热路径。缓存命中的文件读页无需复制，write-first miss 直接构造私有页，避免无用缓存页。当前解析器为确认“确实没有 PTE”先 lookup，再由映射函数走一次叶表路径，属于 cold fault path 的重复遍历；若开发板计数显示缺页延迟重要，可在不改变 VMA/MM 接口的前提下合并 walker，但不能据 QEMU 正确性结果宣称性能收益。

当前 VMA 集合用按起始地址排序的连续数组：查找二分为 `O(log n)`，插入为 `O(n)`，相邻且属性相同的区间合并。对于 ELF source、栈和少量早期匿名区间，这比树节点、旋转和更多分配更小、更容易验证，且不在当前调度热路径上。真实 `mmap` 工作负载若显示大量频繁插入/删除，才应在保持 VMA 语义不变的前提下换成平衡树或区间树；没有测量不能把“树一定更快”当成结论。

## mmap、munmap 和 mprotect 怎样协作？

`mmap` 分配的是虚拟地址区间，不等于立刻分配每个物理页。anonymous-private mapping 没有文件 backing；首次读取应看到零，首次写入只影响本进程。普通地址参数只是 hint，内核可以在冲突时另选空洞；`MAP_FIXED_NOREPLACE` 要求精确地址且冲突失败，`MAP_FIXED` 则要求精确地址并破坏性替换旧映射。BoarOS 当前先尝试对齐 hint，再从每个 MM 独立的随机或无种子确定性 mmap ceiling 以下 top-down 选择空洞。`MAP_STACK` 暂不改变 VMA 增长模型，`MAP_NORESERVE` 在没有 commit accounting 时与普通匿名映射等价。

file-private mapping 把页对齐文件 offset 与虚拟区间对应，读页可以和 page cache 共享，写入必须通过 COW 与文件和其他映射隔离。包含 EOF 的尾页先保留有效文件字节并把页内余部补零，下一整个页才产生 `SIGBUS`。fd 是可关闭的进程槽，不能承担映射生命周期；MM 对同一 OFD 只持有一个来源引用，直到最后一个相关 VMA 被 munmap、fixed replace 或 MM 销毁；重复映射不增加历史引用，fork 子 MM 取得自己的一份。

`munmap` 的 Linux 语义允许区间包含洞：已经映射的部分被撤销，原本未映射的部分不构成错误。`mprotect` 不同，它要求整个非空区间都有 VMA，遇到洞返回 `ENOMEM`。两者都可能在起止边界拆分 VMA；若先改 PTE 后才发现 descriptor 扩容失败，会出现硬件状态已经提交而逻辑状态无法提交的问题。BoarOS 因此先校验并预留确实需要的 descriptor 容量，再修改页表/TLB，最后用不分配的 commit 完成拆分、删除、改权和相邻合并。

RISC-V 叶子 PTE 的 `V=1` 才可供硬件翻译，但 `V=0` 时 RSW 两位仍可由 supervisor 保存软件状态。BoarOS 同时区分硬件有效性和软件所有权：

```text
active exclusive: V=1，RSW=00，PPN 和 R/W/X/U 供硬件使用
active COW:       V=1，RSW=01，去掉 W，共享 PPN 等待写 fault
protected excl.:  V=0，RSW=10，PPN/内容仍属于 PROT_NONE 映射
protected COW:    V=0，RSW=11，同时保留 PROT_NONE 与共享属性
```

`PROT_NONE` 使用 RSW bit 9 的 protected PTE，保留 resident 页内容和 exclusive/COW 属性；它与 unmap 的瞬时 invalid PTE 不同，后者只存在于“失效 PTE → `SFENCE.VMA` → 释放页”窗口，函数返回前会被清零。Fork 先让子页表取得所有物理引用，最后才无分配地提交父 COW PTE；失败时父权限不变。RISC-V 规范保留 W=1、R=0 的叶子编码，项目因此把仅写请求规范化为 RW。增加执行权限前还需要 `FENCE.I` 让先前数据写入对后续取指可见，改 PTE 后再 `SFENCE.VMA` 失效旧地址翻译。

VMA 属于 MM 而不是 task 或单张页表。fork 必须复制其逻辑布局，文件 VMA 还要求子 MM 取得独立 OFD 来源引用。最后一个 MM owner 必须按 VMA metadata、文件来源、驻留页引用/页表的顺序释放，才能避免 metadata 指向已经丢失的状态。页和堆释放遵循 fail-stop 契约；文件来源的真实 VFS/I/O 错误才由对应 owner 延后处理。

需要延后的 owner 必须比触发错误的栈帧活得更久。BoarOS 的根启动把尚未发布的 file、MM、fs 与设备 owner 移入持久 `riscv_root_boot`；只有真实 ext4/block I/O 清理错误才进入有限重试，持续失败会报告并停止。错误码不能替代仍然存在的资源 owner，但 allocator 释放错误应直接进入 fatal，而不是被包装成 cleanup。

## `brk` 怎样形成匿名 heap？

program break 是进程数据段高端之后的一个**字节地址**。Linux raw `brk` syscall 返回调整后的 break；若请求不能满足，则返回原值。常见 libc `brk()` 再把这个结果转换成 0/-1 并设置 `errno`，不能把 libc 包装层的返回约定写进内核 syscall ABI。

ELF 映像的初始 break 应覆盖所有装载段在内存中的末端，因此计算的是最高 `PT_LOAD.p_vaddr + p_memsz`，其中 `p_memsz` 已包含 BSS；program header 不保证按地址排序。BoarOS 把这个最高末端向上按 4 KiB 对齐并叠加独立随机（或无种子确定性）偏移作为 start/current break，把每 MM 的 mmap ceiling 和随机选择的 RX vDSO 作为增长上界约束，VDSO 由独立 VMA 保护。当前 break 仍保存用户请求的精确字节值，只有 VMA/PTE 范围使用 `page_end(break)`：同页内调整不需要改页表，跨页增长才增加 VMA，跨页缩小才撤销整页。

增长只建立 RW anonymous `DEMAND_ZERO` heap VMA，不立即分配数据页。这样申请一大片地址空间但只访问少量页面时，不会预先消耗所有物理页；代价由首次触页 fault 承担。相邻 heap VMA 属性相同会合并，所以反复小幅增长不会为每次 syscall 保留一个描述符。fork 复制调用时的精确 break、VMA 和已驻留页，父子随后独立调整；exec 则从新 ELF 重新计算，不继承旧 heap 高水位。

缩小的关键不是“把 PTE 清零”这么简单，而是所有权与 TLB 顺序：先把 active PTE 暂时改为硬件无效，执行 `SFENCE.VMA`，再归还物理页并清零 PTE，最后收紧 VMA。若先释放页框再失效 TLB，用户可能通过旧 TLB 翻译访问已经分配给别处的内存；若先删 VMA 却保留 active PTE，则形成逻辑无效但硬件仍可访问的地址。用户可以先对 heap 打洞或局部改权，因此 brk shrink 复用通用范围删除，而不假定 heap 永远由单个连续 VMA 表示。

物理页释放是 fail-stop 操作。`riscv_sv39_user_unmap_owned_range()` 在 `SFENCE.VMA` 后立即释放 owner，返回时不保留 PPN、retired PTE、计数或 reclaim API；分配器检测到非法释放、引用或 metadata 直接触发 fatal。`PROT_NONE` 的 protected PTE 仍保留内容，但不属于 unmap 的回收状态。

当前 shrink 保留变空的中间页表直至 MM 销毁。对连续高水位，每覆盖 2 MiB 虚拟跨度最多保留一张 4 KiB Level 0 表，比例约 `4 KiB / 2 MiB = 0.195%`；若程序每隔 2 MiB 只触碰一页，页表相对实际数据页的开销会显著更高。立即回收中间表可降低长寿命进程 shrink 后的占用，但需要把表页撤销和 SMP TLB shootdown 纳入同一事务；当前先保留页表层级，待基准或目标工作负载证明需要时在不改变 raw `brk` ABI 的前提下优化。

## 地址空间激活与高半区 Direct Map

切换分页前还要保证当前代码、栈、异常入口、页表页，以及马上访问的 UART 等 MMIO 在新地址空间中都有有效映射。缺少其中任何一项，都可能让 CPU 在分页生效后的第一条取指或访存时产生异常。

若最终地址空间不准备保留低 RAM 映射，第一次启用分页仍需要处理“写 `satp` 后下一条指令还从哪里取”的过渡问题。BoarOS 先激活一张只覆盖内核低/高别名和 UART 的专用页表，迁移 PC、栈、`gp` 与 `stvec` 后，再从高半区激活最终页表。最终页表只包含高半区内核、RAM Direct Map 和平台 MMIO；低地址内核物理别名由真实 load page fault 测试证明已经失效。两阶段切换把硬件必需的过渡映射限制在启动边界内；启动迁移不依赖后来为用户 heap shrink 增加的运行期范围撤销。

平台 MMIO 也要考虑运行期地址空间共享。BoarOS 的 QEMU UART 在过渡表中使用物理低地址，最终表则把它映射到 direct-map 窗口之后的 supervisor-only 高半区；最终根生效后驱动只切换一次访问基址。用户根借用该高半区映射，所以 trap 诊断不必临时换回内核根，而缺少 U 位仍阻止用户直接访问 UART。把设备继续留在低半区会与每个进程私有的用户树冲突，也会让用户根下的 fatal 日志递归页故障。

高半区内核映射和物理内存 direct map 是两个不同用途的区域。前者让内核代码和静态数据使用稳定的高虚拟地址，通常只覆盖内核镜像并保留严格段权限；后者让内核按固定偏移访问大量物理页框。实现了高半区内核并不自动解决“物理页地址如何解引用”，因此删除恒等映射前必须先建立 direct map 或等价的物理地址转换约定，并把 DTB、页表页和 MMIO 等低地址使用点逐一迁移。

### BoarOS 怎样安排 RISC-V direct map？

BoarOS 为 Sv39 高半区的低 128 GiB 预留物理内存 direct map：

```text
direct VA = 0xffffffc000000000 + PA
PA        = direct VA - 0xffffffc000000000
```

这使物理地址范围 `[0, 128 GiB)` 对应虚拟范围
`[0xffffffc000000000, 0xffffffe000000000)`。页表只为 DTB 实际报告的 RAM 建立叶子，
窗口中没有 RAM 的洞不会消耗下级页表。当前 QEMU 和 VisionFive 2 的 RAM 物理端点
远低于 128 GiB；若未来目标越过该上限，必须重新评估 Sv39 虚拟地址布局，而不能
静默截断 RAM。

另一种常见做法是把第一段 RAM 的起点映射到固定虚拟基址，即
`VA = base + (PA - ram_base)`；Linux RISC-V 的线性映射采用运行时 PA/VA 偏移。
BoarOS 当前选择直接加固定基址，因为它不需要保存 `ram_base`，同一 PA 在不同板上
得到同一 VA，并且固定偏移保持 2 MiB 对齐关系，便于继续使用大页。代价是窗口容量
由最高物理地址而不是 RAM 总容量决定。

direct map 不是“所有 RAM 都可写可执行”的别名。BoarOS 将普通 RAM 映射为 RW、NX，
内核 text 和 rodata 在 direct map 中只读且 NX；内核 text 只有正式高半区别名可执行。
这样不会通过 direct alias 绕过内核镜像的只读和不可执行约束。MMIO 也不属于 RAM
direct map，必须由平台代码单独映射。

函数指针也带有地址空间。分页前用 PC 相对指令取得的函数地址会落在当前低物理
执行别名，即使同一函数的 ELF 符号位于高半区。若把这个指针保存到分页后继续调用，
代码仍会暗中依赖 identity。BoarOS 因此让物理页分配器先保持未绑定状态，只进行
不解引用页内容的顺序分配；控制流进入高半区后再一次性绑定 direct-map 访问函数。
启动测试同时要求保存的访问函数地址确实位于高半区。

## 为什么 RISC-V 统一使用 Sv39/4 KiB？

本地资料给出的能力交集是：

- 固定的 QEMU v11.1 `virt` 默认使用 RV64 基础 CPU；该 CPU 支持到 Sv57，因此也支持 Sv48 和 Sv39。
- VisionFive 2 的 JH7110 使用 SiFive U74 应用核；JH7110 数据手册和 U74 Core Complex 手册都明确标出 Sv39 支持。
- RISC-V 规范规定 Sv39 使用 4 KiB 基页和三级页表。

因此 BoarOS 的 RISC-V 侧固定为 Sv39/4 KiB，在 QEMU `virt` 和 VisionFive 2 之间复用同一套架构分页代码。这样只需要维护一种页表层级、地址拆分和 `satp.MODE`，减少实现、测试和板级迁移路径。Sv39 已足以覆盖当前内核和目标开发板的地址空间；代价是不利用 QEMU 可提供的更大 Sv48/Sv57 虚拟地址空间。

这里复用的是 RISC-V 页表机制，不是整套平台代码。QEMU 与 VisionFive 2 的固件流程、RAM/MMIO 布局、UART、中断控制器和设备仍然需要各自的平台适配。

### Linux 的做法提供了什么参照？

当前本地 Linux RISC-V 源码默认从 Sv57 尝试，根据命令行、DTB 的 `mmu-type` 和 `satp` 写入读回结果逐级退到 Sv48 或 Sv39。它把内核映像映射与物理内存线性映射分开，并在严格内核权限配置下区分可执行代码、只读数据和普通可写内存。

Linux 建立线性映射时同样按对齐和剩余长度选择较大叶子；在 Sv39 的三级配置中，其通用建表层折叠了 PUD，因而线性映射最大选择 2 MiB PMD 叶子，而不是直接使用 1 GiB 根叶子。BoarOS 固定 Sv39 且先实现 2 MiB/4 KiB，是对当前两块 RISC-V 目标硬件交集和早期实现范围的主动收窄，不是 RISC-V 硬件只能这样设置。

## LoongArch 为什么是另一套分页实现？

LoongArch 不使用 RISC-V 的 `satp` 和 PTE 格式。它通过 `PRCFG2.PSAVL` 表示 CPU 支持的页大小，通过 `STLBPS.PS` 选择 STLB 页大小，并用 `PWCL`、`PWCH` 配置多级页表的索引位置和宽度。16 KiB 页对应 `PS=14`。

LoongArch64 Linux 默认选择 16 KiB/三级页表，该布局支持最多 47 位应用虚拟地址。BoarOS 的 LoongArch 基线也固定为 16 KiB/三级页表，以减少 LoongArch 内部的布局组合；物理页分配器使用 `BOAROS_PAGE_SHIFT=14`，但建表、TLB refill、直接映射窗口和地址转换必须由 LoongArch 架构代码实现。

2K1000LA 集成 LA264 处理器核。实际硬件支持的页大小应以 `PRCFG2.PSAVL` 读回值为准；采用 16 KiB 时 bit 14 必须可用。这个检查比根据“都是三级页表”推断兼容性更可靠。

## 验证和调试经验

- 区间算法要覆盖相邻、重叠、完全包含、越界裁剪、整数溢出和相减后为空等情况。
- 页分配测试除非对齐边缘、耗尽和失败状态不变外，还要覆盖 bootstrap→buddy 导入、自然对齐 split、不同释放顺序的多级 coalesce，以及 allocated-head/tail、free、internal、越界和 wrong-order 的完整状态分区。
- 分页切换应分阶段验证：先检查页表内存内容和 PTE 编码，再启用 MMU；切换后立即输出一个最小标记，可以区分“建表错误”和“后续子系统错误”。
- 只验证 store page fault 不足以证明权限正确：应先从目标页成功读取，再用明确的汇编 store 触发故障，并核对 `scause` 和 `stval`。
- 范围映射必须说明失败是否回滚。BoarOS 启动建表采用部分提交：中途 OOM 或冲突时保留已经写入的页表，但整个失败页表不得激活；测试同时检查计数和已写 PTE。
- 只把“不允许继续使用”写进注释并不能维持不变量；页表对象用 `UNINITIALIZED/BUILDING/FAILED/ACTIVE` 状态机约束初始化、建表和激活，错误路径由接口本身拒绝，而不是依赖调用者记住约定。
- 独立 page-table walker 可以从已生成的树反向计算 PA、叶子大小和权限，再与建表请求比较。它与建表器采用相反的数据流，比重复检查几个 PTE 常量更容易发现索引、边界或叶子层级错误。
- 缺页测试要把“逻辑合法”和“当前驻留”分开：既检查 VMA/PTE 组合与权限，又要用真实 U-mode 深栈 load/store 证明 trap 后原指令重试；OOM 应耗尽真实分配器，合法回滚应完成页释放，非法 allocator release 则由 fatal-path 测试验证，最后比较页数/heap 基线。
- QEMU 能验证架构机制和 `virt` 平台路径，但不能替代开发板上的固件交接、DTB、MMIO 和真实 TLB 行为验证。
- 分页开启后，分配器托管的 RAM 必须存在可访问的连续内核映射，否则 bootstrap 回收节点和 buddy metadata 都无法安全访问；BoarOS 已由 Direct Map 承担最终地址空间的页内容访问，低 RAM 只存在于首次 `satp` 切换使用的专用过渡页表，高半区内核映射、Direct Map 和过渡别名的职责不能混为一谈。

当前聚焦验证入口为：

```sh
make test-dtb-riscv
make test-page-riscv
make test-sv39-riscv
make test-sv39-fault-riscv
make test-vma-riscv
make test-brk-riscv
make test-mmap-riscv
make test-demand-page-riscv
make test-user-riscv
make test-high-half-trap-riscv
make test-no-identity-riscv
make test-riscv
```

Sv39 建表测试覆盖精确 PTE、2 MiB/4 KiB 选择、16 GiB 规模、边界拒绝和部分提交；权限故障测试覆盖 MMU 生效后的只读保护；VMA/demand-page 测试覆盖策略边界、真实页耗尽、回滚 owner、U-mode 深栈/heap/mmap 补页、protected owner、区间编辑和完整回收。`test-brk-riscv` 覆盖精确 raw 返回与 fork/exec heap 生命周期，`test-mmap-riscv` 覆盖真实 U-mode anonymous mmap/mprotect/munmap。完整启动测试再验证 512 MiB、1 GiB 和 16 GiB RAM 下的 `satp`、buddy metadata 与页表页精确计数和高半区执行上下文；高半区 trap 测试验证硬件实际使用迁移后的 `stvec`，no-identity 测试验证最终页表真实拒绝低 RAM load。开发板到手后还必须补充同类硬件验证和 fault/TLB 性能测量，不能把 QEMU 结果直接等同于板级兼容。

## 资料依据

执行 `make references` 后可在本地核对：

- `references/riscv/riscv-privileged-20260120.pdf`：`satp` MODE、Sv39/Sv48/Sv57、PTE 与 `SFENCE.VMA`。
- `references/qemu/hw/riscv/virt.c`、`references/qemu/target/riscv/cpu.c`：`virt` 默认 CPU 与支持的最大分页模式。
- `references/linux/arch/riscv/mm/init.c`、`references/linux/arch/riscv/include/asm/pgtable-64.h`：Linux 的模式探测、地址空间和线性映射叶子选择。
- `references/visionfive2/jh7110-datasheet-v1.67.pdf`、`references/visionfive2/sifive-u74-core-complex-21G3.pdf`：U74 与 Sv39 能力。
- `references/loongarch-documentation/docs/LoongArch-Vol1-EN/`：`PSAVL`、`STLBPS`、`PWCL/PWCH` 和多级页表结构。
- `git -C references/linux show HEAD:arch/loongarch/Kconfig`：LoongArch Linux 的页大小与页表层级组合。

资料的固定版本、上游地址和恢复方式见 [本地参考资料](../../references/README.md)。
