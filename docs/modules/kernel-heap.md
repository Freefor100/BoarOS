# 内核堆模块

本文描述运行期页支持内核堆的接口、所有权和当前性能边界。物理页来源见[物理页分配模块](physical-pages.md)，算法背景见[内存管理学习总结](../learning/memory-management.md)。

## 接口与布局

`include/kernel/heap.h` 和 `mm/heap.c` 提供 16 字节对齐的 allocate、calloc、resize、release 与统计接口。初始化只接受已经切换到 buddy 模式的物理页分配器，并要求平台提供“堆虚拟地址到物理地址”的转换函数；堆不假定恒等映射，也不拥有独立固定 arena。

不超过 2048 字节的请求进入 16、32、64、128、256、512、1024、2048 八个 size class。每张 4 KiB slab 页同时保存 header、分配 bitmap、空闲 slot 链和对象；每个 class 维护非满 slab 双向链。slab 最后一个对象释放时立即归还整页，因此空闲 slab 不长期占用物理内存。

更大的请求按覆盖长度所需的最小 buddy order 直接分配连续页，返回页首地址。释放时通过物理页分配器保存的 allocated-head order 找回大小，不在对象前放隐藏 header，也不维护额外大对象表。这个布局让 4 KiB 请求恰好只占一页，并保持 DMA/页表消费者需要的页对齐；代价是非二次幂大对象存在 buddy 内部碎片。

## 失败和统计

- 零长度分配成功并返回空指针；释放空指针成功。
- calloc 在乘法溢出时返回 `OVERFLOW`，不修改输出。
- resize 在原容量足够时原地成功；扩容先取得新对象并复制，失败保留旧对象和输出。
- slab bitmap 区分有效释放、内部地址、非本堆页和 double free；底层物理页释放属于
  已建立的 owner 协议，释放完成后同步更新 slab 链、bitmap 与计数。检测到堆或页
  分配器不变量损坏时直接 fatal trap，不把释放失败转成重试状态。
- 统计记录调用、失败、活对象、当前页与峰值页。它用于资源回收检查，不等同于按调用点或大小分布的性能 profiler。

当前实现针对单 hart 启动和文件系统路径。共享 slab 查找/取槽、bitmap、链表、
释放与统计使用保存/恢复 SIE 的短临界区，防止开中断内核线程被 timer 抢占后产生
双 owner 或借用已回收 slab。缺 slab 时在区外分配并初始化私有页，再在区内发布和
取槽；期间其他任务也创建同类 slab 仍保持独立 owner。大对象页申请、calloc 清零和
resize 复制不放进 heap 元数据临界区，回收可重入堆。
size-class 热路径仍是一次链首访问、slot 链更新和 bitmap 更新；新建/回收 slab 或大
对象才进入 buddy。没有 per-CPU cache；SMP 接入前须将这些边界升级为跨核锁。

## 验证

```sh
make test-allocator-preemption-host
make test-allocator-release-host
make test-heap-riscv
make test-page-riscv
```

测试覆盖对齐、全部 size class、精确页数、大对象 order、复用、zeroing、溢出、耗尽、resize 数据保留、double free、空 slab 回收和统计。生产根启动还在 PID 1 退出后要求 `live_allocations=0`、`current_pages=0` 且物理空闲页回到启动基线。

当前检查能约束分配次数和占页峰值，但 QEMU 启动时间不能替代目标开发板上的 cache miss、锁争用和周期基线。SMP 或长期文件负载出现后再在相同对象分布下比较全局锁、per-CPU cache 与不同 size class，现阶段不据功能测试宣称吞吐优势。

默认关闭的 COST 构建另在实际 calloc 清零与 resize 搬迁处记录字节、次数、单次最大值
和经过 ticks，不在公共内存函数采样。字节是原实际范围，失败准备不制造成功样本；
ticks 可以包含中断与切换，不能当作独占 CPU 时间。公共字节实现仍是生产路径；
宽字候选的微实验改善没有带来原 Lua 工程收益，见[有限归因](../learning/offline-toolchain-probe.md#内存操作的有限归因与未上线候选2026-10-03)。

堆 allocate/resize 与 MM 元数据变更使用任务 `allocation_depth` 限制回收：可回收干净且无外部引用的页，不允许在分配器内部发起脏页 I/O。物理分配器递归检测使用任务 `reclaim_depth`，一个任务睡眠不把其他任务误判为自身递归。脏页回收只在外层无锁处执行；不足返回真实 EMPTY/ENOMEM，不构造重试锁链。

宿主抢占测试使用真实 heap/buddy 实现和有状态 IRQ 模型，在生产函数边界逐一切入第二个
合法 owner；验证已有 slab 的分配/释放交错、私有新 slab 的发布、对象内容独立、最终
活对象/当前页归零和空闲页回到基线。该模型补足单任务功能测试，实际 CSR 与现有内核
线程 timer 抢占仍由 RV64 物理页、堆与调度聚焦测试验证。
