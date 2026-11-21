# 物理页分配模块

本文描述物理页分配器从分页启动期到运行期 buddy 模式的稳定接口。它消费 `boot_memory_layout.usable[]`，不重新发现 RAM，也不解释 DTB。

## 入口与契约

| 文件 | 当前职责 |
|---|---|
| `include/kernel/page.h` | 从构建目标导出页大小和掩码 |
| `include/kernel/physical_page.h`、`kernel/physical_page.c` | 管理显式分配器对象、bootstrap→buddy 状态迁移、单页与整块共享引用、连续页所有权、压力回收入口和计数 |
| `tests/riscv/physical_page_cases.c` | 验证 bootstrap 兼容、buddy split/coalesce、所有权状态和耗尽语义 |
| `tests/riscv/physical_page_main.c`、`tests/riscv/page.sh` | 构建并运行独立的 QEMU 聚焦测试内核 |
| `tests/host/allocator_release.c` | 独立子进程验证非法释放 fatal 和各 guard 的失败诊断 |

共用实现按构建目标使用RV64的 `BOAROS_PAGE_SHIFT=12` 或LA64的14。
初始化把每个字节粒度可用区间
向内收缩到完整页，拒绝溢出、乱序或重叠输入；不足一页的碎片被忽略。失败不会
修改调用者已有的分配器状态。

初始化后处于 bootstrap 模式。`physical_page_allocate()` 返回单页，优先复用已释放页，
再从地址最低的未耗尽区间顺序分配。调用者可通过
`physical_page_allocator_bind_access()` 一次性绑定“物理地址到当前可访问指针”的
函数；空函数或未初始化对象属于非法输入，重复绑定返回
`PHYSICAL_PAGE_STATUS_STATE`。bootstrap 释放页的首 64 位属于回收链节点，重新分配后
内容不作保证。

绑定 direct-map 访问函数后，`physical_page_allocator_finalize()` 执行一次性状态迁移：

1. 从某个从未发放的连续区间尾部保留 metadata 页，并验证这段 PA 对应连续 VA。
2. 把 bootstrap 已发放且未释放的页导入为 order-0 allocated，把回收链和从未发放的
   尾部页导入为空闲候选，metadata 自身标为 internal。
3. 按物理地址自然对齐边界把每段连续空闲页划成 buddy 块，建立 order 0..31 的
   双向侵入式 free list；全部计数和链表验证通过后才发布 finalized 状态。

finalize 只允许成功一次；访问函数未绑定或重复调用返回
`PHYSICAL_PAGE_STATUS_STATE`，没有连续 metadata 空间返回
`PHYSICAL_PAGE_STATUS_EMPTY`，损坏的 bootstrap 链或计数返回
`PHYSICAL_PAGE_STATUS_INVALID`。失败不替换分配器对象；曾被选为 metadata scratch 的
未发放页内容没有保留承诺。

finalized 后，`physical_page_allocate_order(order)` 分配 `2^order` 个物理连续且按总
字节数自然对齐的页，`physical_page_release_order()` 要求地址、head 和原 order 完全
匹配。wrong-order、allocated tail、internal、越界、非对齐地址和已经释放的 head/tail
都是分配器不变量错误，直接触发 fatal trap；不会把释放失败变成可重试状态。现有
`physical_page_allocate/release` 签名保持不变，在 finalized 模式委托给 order 0，因此
Sv39 和 MM 的单页接口不需要识别分配器模式；scheduler 初始化要求 finalized allocator，为独立内核栈取得 order-1 连续页。分配耗尽仍返回 `EMPTY`，不产生新 owner 或修改输出；压力回收实际归还的页仍计入 `available_pages`。

释放的 fatal 分支先输出 guard 原因、调用者传入的物理地址和 requested order，再触发
原 fatal trap。wrong-order、非 allocated head、重复释放、可用页计数、buddy 元数据和
free-list 操作各有可区分的原因。只有 finalized 分配器的受检查页查找成功后，才输出
该页的索引、state、stored order、引用数和链索引；buddy/free-list 失败显示相关页的
metadata 地址，非法地址或 bootstrap 失败不读取 metadata，也不追踪损坏的链索引。
输出经 `kernel_console_putc()` 的同步 raw UART sink，不分配、不回收、不睡眠，不经过
内核日志的等待者唤醒；正常释放路径没有诊断输出。这些字段标识本次命中的不变量检查，
单独的 trap PC 或这类 guard 现场仍不能证明导致元数据损坏的上游根因。

finalized 的 order-0 页可用 `physical_page_acquire()` 增加 32 位引用，
`physical_page_release()` 只在末引用消失时把页归还 buddy；
`physical_page_reference_count()` 供缓存回收等调用者查询共享状态。COW使用`physical_page_cow_borrow()`，在同一次分配器保护内读取引用数，并且仅在原引用数大于1时取得复制来源的临时引用；引用数为1时由仍受MM保护的原PTE持有页面，直接恢复写权限。这样不再将引用数查询与来源借用分成两次查找和取锁。普通零阶页按 PFN 定位元数据，在分配器锁内检查活块头并增减引用；非末引用释放不查询 buddy 祖先，末次释放才进入完整树验证和合并。分配、释放与合并同步维护块头状态和引用数，旧尾记录不能获得活页资格。高阶块可用 `physical_page_acquire_order()` 取得整个 buddy 分配的引用，
`physical_page_cow_borrow_order()` 在一次保护内查询并按需保留整个复制来源。
两者都校验块头和确切 order；每次借用以同一块头和 order 归还，只有末引用消失
才归还全部物理页。单页 acquire/COW 接口仍只接受 order-0，不能取得高阶 tail 的
独立引用。`physical_page_allocate_contiguous()` 仍交付已拆成独立 order-0 owner 的
页面，与整块共享是两个不同契约。整块引用不增加每个尾页的元数据操作或引用计数。

MM 退休批次可携带整块 head/order 与普通页表页；混合批次使用
`physical_page_release_order_batch()` 在一次 allocator 保护内归还。普通基页批次
保留原有 release-batch 快路径。`finish_tlb_change()` 仍先完成本地/远端翻译同步，
再统一归还所有权，不能在同步前释放整块，也不能把一个大页展开成多条 tail 归还。

这不是 `MAP_HUGETLB` 用户接口完成。双架构大叶与整块 pin/退休消费者的阶段状态见
[显式大页](hugepages.md)；hstate 预留、VMA 与 syscall 接入仍未开放，不能以连续基本页替代。
引用达到 `UINT32_MAX`、对 free/internal/tail 操作或在 bootstrap 阶段 acquire 都是
不变量错误并触发 fatal trap；合法 acquire/release 只改变引用和最终可用页计数。
`physical_page_acquire_batch()` 和 `physical_page_release_batch()` 在一次分配器锁内分别取得或归还至多 16 个零阶引用。同一物理地址可出现多次，每项各归还一个引用；末引用仍执行与单页释放相同的祖先、块头和空闲链校验及合并。

`physical_page_allocate_batch()` 为实际批量使用者一次尝试至多 16 页，只交付请求范围内的独立零阶 owner。它在同一次分配器保护内复用已定位的 buddy 块，将适合当前剩余数量的大块直接拆成独立页；碎片不足时逐级尝试较小块，不要求整批物理连续。全部借用、块头、树和空闲链仍沿共同分配实现校验。返回实际完成前缀，未完成输出保持原样；不预留常驻空页。压力 owner 的借用在同一保护内取得，锁外合并通知一次；短前缀由调用者决定是否进入原单页回收和等待路径。

分配器还允许注册一个压力回收回调。一次 buddy 分配返回 `EMPTY` 时，若当前不在回调中，
分配器以所需页数调用干净页回收器，每次实际归还后重新申请目标 order；仍缺连续块且回收有进展时继续，不用空闲页总数代替物理相邻性。没有可归还页时保留 `EMPTY`。干净回收可按正向锁序等待元数据资格，不等待脏页或设备 I/O；只有可睡眠且无 worker 所需锁的普通任务路径，才可额外等一轮后台回收并重试。递归抑制防止回收器内部的堆/页分配再次进入自身。
同一分配器的磁盘缓存登记到共同 owner，干净回收按实例轮转；停止一个实例不注销其他实例。最后实例及等待者都释放引用后才销毁共同 owner。

`physical_page_resolve()` 为持有分配页的调用者提供受检查的物理地址访问。bootstrap
模式只能按发放历史检查；finalized 模式要求页首位于 allocated 权威块，允许连续块内的尾页页首，明确
拒绝 free 和 internal 页。两种模式都只在访问回调返回非空指针后写输出。

内部 `physical_page_map_owned()` 供已经持有分配页、页表或用户 pin 的调用者使用，直接调用绑定的映射函数，不再重复取得元数据锁或确认所有权。调用者必须覆盖整个访问期间的页寿命；分配、归还和首次接纳身份仍由各自边界负责。

`physical_page_total()` 保留所有对齐可用页的原始总数；
`physical_page_available()` 在 finalized 后排除 bootstrap owner 与 metadata；
`physical_page_metadata_pages()` 只在 finalized 后返回内部占用。metadata由每页16字节载荷、有序根目录和3-bit隐式树组成；只有活跃块头使用载荷。
令总页数P、根数R，当前字节预算为 `16*P + 32*R + ceil(3*(2*P-R)/8)`，最终按页取整。
4 KiB下主体约占RAM的0.409%，16 KiB约0.102%；根目录与页取整另计，不能继续沿用
旧表示的0.391%作为新资源基线。metadata来自未发放连续区间，全部标为internal。

页缓存借助现有的页元数据发布脏页和写回状态。状态由页缓存的短保护串行更新，
原有物理页引用保证更新期间不会发生释放或复用。读取方在分配器保护下核对实际块首，
避免读取旧块尾留下的状态。末次释放清除这些标志，后续分配不会继承旧页缓存的状态。
用户查询格式见[逐页查询](procfs.md#逐页查询)。

## 算法与限制

初始化只遍历可用区间，不扫描 RAM。bootstrap 单页顺序分配为 O(区间数)，释放时的
重复释放检测仍会扫描临时回收链；这条路径只服务最终页表建立前的硬件迁移。
finalize 初始化逐页 metadata，成本为 O(物理页数)，但每次启动只发生一次。

finalized分配搜索order入口，根目录按PA或平坦页索引二分；相关owner沿隐式树查找。
free/split/allocated/internal为权威状态，inactive节点不拥有页，编码5–7为损坏。
分裂先准备子状态再发布split，合并先摘链/退休子节点再发布free。无跨range合并；
旧尾载荷不参与owner判断，不能用一段旧尾的状态替代树验证。

受检查的resolve和allocation_order在页首载荷给出候选块头时，
验证该几何位置的活动ALLOCATED树节点和块头字段；正常块头查询为O(log R)，
连续块内部页或过期提示回退到完整祖先查找。合并先将子节点标为不活动，因此旧块尾
的记录不会被当成新的所有者。空闲链表已给出块的阶数，分配器按该阶数定位实际树节点，
检查节点为空闲状态、块头与阶数一致，并检查本次要改写的链表链接。生产配置在空闲
链表操作中不再逐层检查祖先是否处于分裂状态；`LOCK_DEBUG=1` 保留该检查。
最终释放仍沿祖先链确定当前分配块的边界。零阶引用操作按上述活块头规则处理。

热路径检查相关权威路径、块头引用与free-list双向连接，不扫描空闲块尾记录，也不
重写最终合并块的全部页。小请求的检查受树/根目录深度约束，重写受split/coalesce
层数约束；完整一致性由 `physical_page_allocator_audit()` 验证。调用者只可在未发布或
独占finalized实例上审计；该接口不分配、回收或I/O，固定深度栈遍历包括inactive节点、
活跃head、free-list成员与覆盖/计数，损坏fatal。非法参数INVALID，未finalized为STATE。

finalized元数据操作使用每allocator的raw锁，保存/恢复本CPU的IRQ并禁止抢占；这里
互斥只覆盖分配器元数据，不证明完整SMP或硬实时。耗尽后的压力回调、I/O等待不在buddy修改区内。

绑定前只允许顺序发放从未释放过的页；合法 bootstrap 释放完成即返回。越界、重复或
不属于当前 owner 的释放触发 fatal trap。
显式 order API 只对 finalized 分配器开放，bootstrap 调用返回 `STATE`。

RISC-V 启动路径先以未绑定状态顺序分配最终页表页；高半区和最终 Sv39 根激活后才
绑定 `direct_map_page_access` 并立即 finalize，后续消费者使用 buddy：单页接口为 order-0，scheduler 的 8 KiB 栈为 order-1 连续分配。独立过渡页表使用内核镜像内的静态页池，不属于正式
分配器。

当前 metadata 必须来自一个从未发放且足够大的连续 range tail；总空闲页足够但被
bootstrap 分散耗尽时 finalize 仍返回 `EMPTY`。当前 QEMU 满足该约束的证据来自
512 MiB、1 GiB 和 16 GiB 启动验证；开发板必须按
真实 DTB 保留区和启动占用重新核对。若未来早期分配规模或稀疏内存使其不成立，应
改为每 range metadata 或稀疏索引，而不是退回固定容量 heap。模块还没有清零分配、
多回收器优先级、跨核压力回收/快照协议、NUMA、热插拔、CMA 或 per-CPU page cache。

## 验证

```sh
make test-allocator-preemption-host
make test-allocator-release-host
make test-allocator-concurrency-host
make test-page-riscv
make test-riscv
```

聚焦测试保留原有区间、耗尽、绑定、映射和失败输出契约，并覆盖 bootstrap owner/
recycled/tail 导入、metadata 扣除、order 对齐、强制 split 与多级 coalesce、finalize
失败保留，以及 finalized resolve 拒绝空闲页。测试还覆盖 order-0 与高阶整块多引用、COW 来源借用、末引用归还、
合法 owner 的释放和压力回收的单次重试；非法释放由独立 fatal-path 测试验证，不再注入
人工释放失败。

host fatal 测试仍要求非法释放终止于 `SIGILL`，同时检查 wrong-order、tail、重复释放、
非对齐地址、未初始化分配器、计数损坏和 buddy 元数据损坏的 guard 原因、地址、order
及有条件的 metadata 字段；不要求整句文案或 trap PC 相同。fixture 只替换硬件 console
sink 来读取真实分配器输出，并检查合法共享单页及连续页释放保持无输出。

## 内存快照与后台压力通知

`kernel_memory_snapshot()` 输出字节单位的只读快照，由 procfs 与 sysinfo 共用。查询固定当前回收绑定的组引用，再分别取得可用的缓存保护；忙对象可保守漏计，不提供跨对象的瞬间一致快照。不分配、不回收、不发起 I/O。总量/空闲量来自 buddy，共享匿名量由 `mm/memory_object.c` 在后备页发布和最终释放时维护，fork/别名/临时引用不重复计量。文件缓存按唯一 cache entry 计量，块缓冲按 ext4 bcache 已分配的 payload 计量，不把堆开销重复归入 Buffers。

文件页回收资格在查询时检查物理引用、映射别名、装载/写回/用户固定及最后写回错误。失败页重新修改后仍不计入可回收预算，直到写回成功；共享匿名页没有 swap，不能回收。`MemAvailable=max(free-low,0)+reclaimable-min(reclaimable/2,low)`，最终夹在 `[0,total]`；low 至少一页。它是估算，不承诺任意高阶连续分配成功。`pressure_notify` 只合并事件；`pressure_wait` 检查任务/锁/backend/递归上下文，分配器不拥有 inode/mount 的 I/O 错误。

聚焦入口为 `test-page-riscv`、`test-vma-riscv`、`test-io-sleep-riscv`；包括共享后备页 fork 不重复计数、最终归零、缓存压力及 worker 启动分配失败回滚。

多个磁盘缓存的统计累加各自驻留、脏页与写回页，MemAvailable 只扣一次全局
低水位余量。通知为 O(实例数)，全组脏页数随实际脏项增减维护，完整快照为 O(总缓存项数)。
安全 OOM 路径等待共同一轮的首次实际释放进展，或所有参与 worker 完成；不逐盘
串行等待。worker 与等待者各自保持 owner 引用，注销和唤醒不依赖借用悬空指针。
统一内存后备对象的 tmpfs 页与共享匿名页同计 Shmem，不进入磁盘回收候选。

### 内核抢占与回调分发

分配器不能依赖“单 hart 等于不会交错”：内核线程保持 timer 抢占能力，异步 journal
worker 在 SIE 开启时也调用堆和物理页分配器。`kernel/irq.h` 的 scope 只负责保存和
恢复本 hart 的 SIE；嵌套进入不会提前打开中断，所有正常/失败返回均恢复调用者状态。
bootstrap 回收链也使用该边界，避免 idle 安全 IRQ 返回中执行另一 owner 后破坏链。

每次 buddy 分配尝试释放 raw 锁后才分发压力回调。回收、通知、等待与快照组成一个绑定，在 allocator raw 内完整发布或摘除；分配在原有持锁范围内取得已有缓存组引用，不为读取绑定再次加锁。回调在锁外执行并恢复调用者的 IRQ 状态；远端 CPU 的正常分配也可发布通知。注销不等待另一个调用栈返回，最后一个实例、绑定和在途回调各自归还引用后才释放组。

组的短锁保护成员和等待轮次；遍历资格保持成员存活。同步干净回收只尝试取得缓存 mutex，忙时跳过，不等锁、不写磁盘；释放数据页后由已有 worker 清理剩余节点。脏数据仍由原 worker 回写。等待前结束本次回收递归深度，等待期间由回调引用覆盖组寿命；返回后只重试 buddy 分配，不访问已经摘除的绑定。

宿主抢占回归在生产函数入口/出口逐一模拟一次 timer 切换，IRQ-off 时推迟到恢复后；
断言只检查公共分配/引用/释放结果、不同 owner 的内容互不覆盖、heap 统计和空闲页归还，
不锁定私有布局或固定调用次数。旧实现会出现合法请求失败；定点调查还证明两个成功
请求取得同页，两个 owner 各释放一次即触发 `already-free`。修复后覆盖页发放、释放合并与并发分配、引用交错、
slab 分配/释放和私有 slab 发布；压力模型验证递归抑制、等待期间另一任务进展/注销，
以及 IRQ 开/关两种入口状态恢复。这些反例确认分配器缺陷，不能单独证明历史
`Virtqueue size exceeded` 的具体来源。

固定参考 `references/linux/mm/page_alloc.c`（Linux commit
`f4cdf7ca9a1fdcca413157df19753f388a5a224e`）要求 `__rmqueue` 在 `zone->lock`
内执行，`rmqueue_bulk` 使用 `spin_lock_irqsave` 保护摘链和 split。这里沿用的是共享
元数据必须串行化的契约；BoarOS 当前实现只覆盖单 hart 的 IRQ/任务交错，并未移植 Linux
的 SMP 锁或 per-CPU 分配机制。


COST 构建维护 `allocated_peak_pages`：每次 bootstrap/buddy 成功发放及 finalize
发布 metadata 占用后，按 total-available 更新初始化以来的真实最大受管占用。
共享引用增加不算第二次分配，释放不降低高水位；连续分配计完整 order。
默认构建编译掉字段和更新；proc 诊断输出的口径见[procfs](procfs.md)。
