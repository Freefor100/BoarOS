# 物理页分配模块

本文描述物理页分配器从分页启动期到运行期 buddy 模式的稳定接口。相关概念、算法选择理由和分页关系见 [内存管理学习总结](../learning/memory-management.md)。它消费 `boot_memory_layout.usable[]`，不重新发现 RAM，也不解释 DTB。

## 入口与契约

| 文件 | 当前职责 |
|---|---|
| `include/kernel/page.h` | 从构建目标导出页大小和掩码 |
| `include/kernel/physical_page.h`、`kernel/physical_page.c` | 管理显式分配器对象、bootstrap→buddy 状态迁移、单页共享引用、连续页所有权、压力回收入口和计数 |
| `tests/riscv/physical_page_cases.c` | 验证 bootstrap 兼容、buddy split/coalesce、所有权状态和耗尽语义 |
| `tests/riscv/physical_page_main.c`、`tests/page-riscv.sh` | 构建并运行独立的 QEMU 聚焦测试内核 |
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
Sv39 和 MM 的单页接口不需要识别分配器模式；scheduler 初始化要求 finalized allocator，为独立内核栈取得 order-1 连续页。分配耗尽仍返回 `EMPTY`，分配调用不会
修改输出或 `available_pages`。

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
`physical_page_reference_count()` 供 COW 和缓存回收判断共享状态。高阶块仍是单 owner，
不能通过单页 acquire 拆分引用，避免让连续分配的 tail 生命周期失去统一边界。引用达到
`UINT32_MAX`、对 free/internal/tail 操作或在 bootstrap 阶段 acquire 都是
不变量错误并触发 fatal trap；合法 acquire/release 只改变引用和最终可用页计数。

分配器还允许注册一个压力回收回调。一次 buddy 分配返回 `EMPTY` 时，若当前不在回调中，
分配器以所需页数调用非阻塞干净页回收器；只有可睡眠且无 worker 所需锁的普通任务路径，才可等一轮后台回收，然后只重试一次；递归抑制防止回收器内部的堆/页分配再次进入自身。
同一分配器的磁盘缓存登记到共同 owner，干净回收按实例轮转；停止一个实例不注销其他实例。最后实例及等待者都释放引用后才销毁共同 owner。

`physical_page_resolve()` 为持有分配页的调用者提供受检查的物理地址访问。bootstrap
模式只能按发放历史检查；finalized 模式要求页首位于 allocated 权威块，允许连续块内的尾页页首，明确
拒绝 free 和 internal 页。两种模式都只在访问回调返回非空指针后写输出。

`physical_page_total()` 保留所有对齐可用页的原始总数；
`physical_page_available()` 在 finalized 后排除 bootstrap owner 与 metadata；
`physical_page_metadata_pages()` 只在 finalized 后返回内部占用。metadata由每页16字节载荷、有序根目录和3-bit隐式树组成；只有活跃块头使用载荷。
令总页数P、根数R，当前字节预算为 `16*P + 32*R + ceil(3*(2*P-R)/8)`，最终按页取整。
4 KiB下主体约占RAM的0.409%，16 KiB约0.102%；根目录与页取整另计，不能继续沿用
旧表示的0.391%作为新资源基线。metadata来自未发放连续区间，全部标为internal。

## 算法与限制

初始化只遍历可用区间，不扫描 RAM。bootstrap 单页顺序分配为 O(区间数)，释放时的
重复释放检测仍会扫描临时回收链；这条路径只服务最终页表建立前的硬件迁移。
finalize 初始化逐页 metadata，成本为 O(物理页数)，但每次启动只发生一次。

finalized分配搜索order入口，根目录按PA或平坦页索引二分；相关owner沿隐式树查找。
free/split/allocated/internal为权威状态，inactive节点不拥有页，编码5–7为损坏。
分裂先准备子状态再发布split，合并先摘链/退休子节点再发布free。无跨range合并；
旧尾载荷不参与owner判断，不能用一段旧尾的状态替代树验证。

热路径检查相关权威路径、块头引用与free-list双向连接，不扫描空闲块尾记录，也不
重写最终合并块的全部页。小请求的检查受树/根目录深度约束，重写受split/coalesce
层数约束；完整一致性由 `physical_page_allocator_audit()` 验证。调用者只可在未发布或
独占finalized实例上审计；该接口不分配、回收或I/O，固定深度栈遍历包括inactive节点、
活跃head、free-list成员与覆盖/计数，损坏fatal。非法参数INVALID，未finalized为STATE。

一次元数据操作仍保存/恢复本CPU的IRQ，保证单核timer抢占不能插入半成品；这里
没有跨核互斥或硬实时承诺。耗尽后的压力回调、I/O等待不在buddy修改区内。

`test-allocator-cost-host`在4/16 KiB下用公开API强制64/512/4096/32768页split/coalesce，
并做10000次随机操作的独立owner模型、每步审计、多range/保留洞和finalize失败验证。
工作量计根目录/树/块头/order入口的逻辑检查和重写，重复验证重复计，批量初始化须
按实际记录计；finalize与独立审计不并入运行期热路径窗口。请求页数N、相关根深度H、
目录二分深度L的门禁为检查 `32*(H+1)^2+16*N+64*(L+1)`、重写 `8*(H+1)+4*N`。
分裂每层最多7次逻辑重写、合并每层最多7次，入口/最终发布余量由上述常数覆盖；
邻居权威检查按相关路径计，ROOT上限由最大19个usable range及order31约束，L不超过11。
这不是固定私有调用次数或循环行号。32768页反例新检查66/67、重写94/94；
旧尾计数与新全部逻辑计数的口径差异见[学习记录](../learning/memory-management.md#buddy-元数据成本核实2026-10-08)。

COST新增三项非直方图指标，在每次元数据区内局部累计，恢复IRQ后集中提交；默认关闭，
不持对象引用。窗口内时间排除压力I/O，完整IRQ-off分布沿用现有观测；成本计数并不
替代客户机时间。阶段A的组合回归与匹配时间结论尚在执行，不能据窄测试宣称阶段完成。

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
多回收器优先级、SMP 并发锁、NUMA、热插拔、CMA 或 per-CPU page cache。

## 验证

```sh
make test-allocator-preemption-host
make test-allocator-release-host
make test-allocator-cost-host
make test-page-riscv
make test-riscv
```

聚焦测试保留原有区间、耗尽、绑定、映射和失败输出契约，并覆盖 bootstrap owner/
recycled/tail 导入、metadata 扣除、order 对齐、强制 split 与多级 coalesce、finalize
失败保留，以及 finalized resolve 拒绝空闲页。测试还覆盖 order-0 多引用、末引用归还、
合法 owner 的释放和压力回收的单次重试；非法释放由独立 fatal-path 测试验证，不再注入
人工释放失败。完整启动测试在 512 MiB、1 GiB 和 16 GiB QEMU 配置下通过 direct map
执行分配—解析—释放—再分配，并精确要求
`total - available == Sv39 table_pages + metadata_pages`；生产 idle 测试还要求 buddy
模式在 timer 启动前已经生效。

host fatal 测试仍要求非法释放终止于 `SIGILL`，同时检查 wrong-order、tail、重复释放、
非对齐地址、未初始化分配器、计数损坏和 buddy 元数据损坏的 guard 原因、地址、order
及有条件的 metadata 字段；不要求整句文案或 trap PC 相同。fixture 只替换硬件 console
sink 来读取真实分配器输出，并检查合法共享单页及连续页释放保持无输出。

## 内存快照与后台压力通知

`kernel_memory_snapshot()` 输出字节单位的只读快照，由 procfs 与 sysinfo 共用。调用者在当前单 hart 的关中断边界内查询；不分配、不回收、不发起 I/O。总量/空闲量来自 buddy，共享匿名量由 `mm/memory_object.c` 在后备页发布和最终释放时维护，fork/别名/临时引用不重复计量。文件缓存按唯一 cache entry 计量，块缓冲按 ext4 bcache 已分配的 payload 计量，不把堆开销重复归入 Buffers。

文件页回收资格在查询时检查物理引用、映射别名、装载/写回/用户固定及最后写回错误。失败页重新修改后仍不计入可回收预算，直到写回成功；共享匿名页没有 swap，不能回收。`MemAvailable=max(free-low,0)+reclaimable-min(reclaimable/2,low)`，最终夹在 `[0,total]`；low 至少一页。它是估算，不承诺任意高阶连续分配成功。`pressure_notify` 只合并事件；`pressure_wait` 检查任务/锁/backend/递归上下文，分配器不拥有 inode/mount 的 I/O 错误。

聚焦入口为 `test-page-riscv`、`test-vma-riscv`、`test-io-sleep-riscv`；包括共享后备页 fork 不重复计数、最终归零、缓存压力及 worker 启动分配失败回滚。

多个磁盘缓存的统计累加各自驻留、脏页与写回页，MemAvailable 只扣一次全局
低水位余量。通知与 dirty 阈值判断为 O(实例数)，完整快照为 O(总缓存项数)。
安全 OOM 路径等待共同一轮的首次实际释放进展，或所有参与 worker 完成；不逐盘
串行等待。worker 与等待者各自保持 owner 引用，注销和唤醒不依赖借用悬空指针。
统一内存后备对象的 tmpfs 页与共享匿名页同计 Shmem，不进入磁盘回收候选。

### 内核抢占与回调分发

分配器不能依赖“单 hart 等于不会交错”：内核线程保持 timer 抢占能力，异步 journal
worker 在 SIE 开启时也调用堆和物理页分配器。`kernel/irq.h` 的 scope 只负责保存和
恢复本 hart 的 SIE；嵌套进入不会提前打开中断，所有正常/失败返回均恢复调用者状态。
bootstrap 回收链也使用该边界，避免 idle 安全 IRQ 返回中执行另一 owner 后破坏链。

每次 buddy 分配尝试结束后才进行压力回调分发。分发另行关闭 IRQ，使 callback 函数与
context 的读取、非阻塞干净回收、以及等待回调取得自身 owner 之间不能被卸载插入。
`pressure_wait` 在显式睡眠前增加既有缓存组引用，睡眠期间允许其他任务运行、释放页
和卸载最后缓存；此时没有未完成的 buddy/heap 元数据修改。回收递归深度在进入等待前
清零，返回后只重新尝试 buddy 分配，不再借用旧缓存 context。这不是跨 I/O 持有分配器锁。
干净回收仍在 IRQ-off 的调用契约内扫描，后续若需可抢占扫描必须另行定义缓存游标与引用。

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
默认构建编译掉字段和更新。`tests/cost/page_test.c` 验证 bootstrap、metadata、
连续页、耗尽到满池和释放后的峰值保持；proc 诊断输出的口径见[procfs](procfs.md)。
