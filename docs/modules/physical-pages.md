# 物理页分配模块

本文描述物理页分配器从分页启动期到运行期 buddy 模式的稳定接口。相关概念、算法选择理由和分页关系见 [内存管理学习总结](../learning/memory-management.md)。它消费 `boot_memory_layout.usable[]`，不重新发现 RAM，也不解释 DTB。

## 入口与契约

| 文件 | 当前职责 |
|---|---|
| `include/kernel/page.h` | 从构建目标导出页大小和掩码 |
| `include/kernel/physical_page.h`、`kernel/physical_page.c` | 管理显式分配器对象、bootstrap→buddy 状态迁移、单页/连续页所有权和计数 |
| `tests/riscv/physical_page_cases.c` | 验证 bootstrap 兼容、buddy split/coalesce、状态分区和失败不变量 |
| `tests/riscv/physical_page_main.c`、`tests/page-riscv.sh` | 构建并运行独立的 QEMU 聚焦测试内核 |

RISC-V64 构建固定 `BOAROS_PAGE_SHIFT=12`。初始化把每个字节粒度可用区间
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
匹配。wrong-order、allocated tail、internal、越界和非对齐地址返回 `INVALID`；任何
已空闲 head/tail 再释放返回 `DOUBLE_FREE`。现有 `physical_page_allocate/release`
签名保持不变，在 finalized 模式委托给 order 0，因此 Sv39、MM 和 scheduler 不需要
识别分配器模式。失败不改写分配输出或 `available_pages`。

`physical_page_resolve()` 为持有分配页的调用者提供受检查的物理地址访问。bootstrap
模式只能按发放历史检查；finalized 模式还要求 metadata 为 allocated head/tail，明确
拒绝 free 和 internal 页。两种模式都只在访问回调返回非空指针后写输出。

`physical_page_total()` 保留所有对齐可用页的原始总数；
`physical_page_available()` 在 finalized 后排除 bootstrap owner 与 metadata；
`physical_page_metadata_pages()` 只在 finalized 后返回内部占用。metadata 每个物理页
使用 12 字节，包含双向链索引、order 与所有权状态；16 GiB/4 KiB 的理论完整 RAM
需要 48 MiB，即约 0.293% RAM，实际值按排除固件和内核后的页数向上取整。

## 算法与限制

初始化只遍历可用区间，不扫描 RAM。bootstrap 单页顺序分配为 O(区间数)，释放时的
重复释放检测仍会扫描临时回收链；这条路径只服务最终页表建立前的硬件迁移。
finalize 初始化逐页 metadata，成本为 O(物理页数)，但每次启动只发生一次。

finalized 分配最多检查 32 个 order；free-list head 的插入和双向摘链为 O(1)，split
与 coalesce 为 O(order)，不会扫描同 order 的其他空闲块。为了让 interior release 和
resolve 能精确判定所有权，分配或释放 order N 块还会更新本次块内 `2^N` 条状态；
常用 order-0 热路径只更新一个页记录。当前单 hart 不需要锁，SMP 接入前必须把
free-list 与计数纳入同一同步边界。

绑定前只允许顺序发放从未释放过的页；释放返回 `PHYSICAL_PAGE_STATUS_STATE`。
显式 order API 只对 finalized 分配器开放，bootstrap 调用返回 `STATE`。

RISC-V 启动路径先以未绑定状态顺序分配最终页表页；高半区和最终 Sv39 根激活后才
绑定 `direct_map_page_access` 并立即 finalize，scheduler 及其后的所有消费者因此只
看到 buddy order-0 行为。独立过渡页表使用内核镜像内的静态页池，不属于正式
分配器。

当前 metadata 必须来自一个从未发放且足够大的连续 range tail；总空闲页足够但被
bootstrap 分散耗尽时 finalize 仍返回 `EMPTY`。当前 QEMU 满足该约束的证据来自
512 MiB、1 GiB 和 16 GiB 启动验证；开发板必须按
真实 DTB 保留区和启动占用重新核对。若未来早期分配规模或稀疏内存使其不成立，应
改为每 range metadata 或稀疏索引，而不是退回固定容量 heap。模块还没有清零分配、
并发锁、NUMA、热插拔、CMA 或 per-CPU page cache。

## 验证

```sh
make test-page-riscv
make test-riscv
```

聚焦测试保留原有区间、耗尽、绑定、映射和失败输出契约，并新增 bootstrap owner/
recycled/tail 导入、metadata 扣除、order 对齐、强制 split 与多级 coalesce、wrong
order、interior/internal/outside/double-free 状态树、finalize 失败保留，以及 finalized
resolve 拒绝空闲页。完整启动测试在 512 MiB、1 GiB 和 16 GiB QEMU 配置下通过
direct map 执行分配—解析—释放—再分配，并精确要求
`total - available == Sv39 table_pages + metadata_pages`；生产 idle 测试还要求 buddy
模式在 timer 启动前已经生效。
