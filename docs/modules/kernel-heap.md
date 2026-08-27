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
- slab bitmap 区分有效释放、内部地址、非本堆页和 double free；底层页释放失败时恢复 slab 链、bitmap 与计数。
- 统计记录调用、失败、活对象、当前页与峰值页。它用于资源回收检查，不等同于按调用点或大小分布的性能 profiler。

当前实现针对单 hart 启动和文件系统路径，没有锁与 per-CPU cache。size-class 热路径是一次链首访问、slot 链更新和 bitmap 更新；新建/回收 slab 或大对象才进入 buddy。引入 SMP 后必须先定义锁边界，再由争用与缓存基准决定是否增加 per-CPU magazine。

## 验证

```sh
make test-heap-riscv
make test-page-riscv
```

测试覆盖对齐、全部 size class、精确页数、大对象 order、复用、zeroing、溢出、耗尽、resize 数据保留、double free、空 slab 回收和统计。生产根启动还在 PID 1 退出后要求 `live_allocations=0`、`current_pages=0` 且物理空闲页回到启动基线。

当前检查能约束分配次数和占页峰值，但 QEMU 启动时间不能替代目标开发板上的 cache miss、锁争用和周期基线。SMP 或长期文件负载出现后再在相同对象分布下比较全局锁、per-CPU cache 与不同 size class，现阶段不据功能测试宣称吞吐优势。
