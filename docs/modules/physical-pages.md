# 物理页分配模块

本文描述启动期物理页分配器的稳定接口。它消费
`boot_memory_layout.usable[]`，不重新发现 RAM，也不解释 DTB。

## 入口与契约

| 文件 | 当前职责 |
|---|---|
| `include/kernel/page.h` | 从构建目标导出页大小和掩码 |
| `include/kernel/physical_page.h`、`kernel/physical_page.c` | 初始化显式分配器对象并提供单页分配、释放和计数查询 |
| `tests/riscv/physical_page_cases.c` | 验证区间对齐、状态变化和错误契约 |
| `tests/riscv/physical_page_main.c`、`tests/page-riscv.sh` | 构建并运行独立的 QEMU 聚焦测试内核 |

RISC-V64 构建固定 `BOAROS_PAGE_SHIFT=12`。初始化把每个字节粒度可用区间
向内收缩到完整页，拒绝溢出、乱序或重叠输入；不足一页的碎片被忽略。失败不会
修改调用者已有的分配器状态。

`physical_page_allocate` 返回页对齐物理地址，优先复用已释放页，再从地址最低的
未耗尽区间顺序分配。耗尽或非法调用不会修改输出地址或分配器状态。
`physical_page_release` 只接受同一分配器已经发放且尚未释放的页，明确区分非法
地址与重复释放。释放后的页首 64 位属于分配器内部链表节点，重新分配后页内容
不作保证。

## 算法与限制

分配器保存每个对齐区间的当前游标，因此初始化只遍历区间，不扫描或写入全部
RAM。释放页组成侵入式回收链表；重复释放检测扫描该链表，复杂度为 O(已回收
页数)。当前单 hart 启动不需要锁。

当前分页关闭，RISC-V 以物理地址直接访问回收节点。首次 Sv39 内核映射必须继续
覆盖托管 RAM；LoongArch 接入时再根据其直接映射窗口增加真实的架构地址转换。
模块尚不支持连续多页、清零分配、伙伴系统、并发、NUMA 或热插拔。

## 验证

```sh
make test-page-riscv
make test-riscv
```

聚焦测试覆盖多区间和非对齐边缘、耗尽时输出不变、释放后复用、计数恢复、重复
释放、未发放或非对齐地址，以及非法布局。启动测试在 512 MiB 和 1 GiB QEMU
配置下要求初始化后的总页数非零且等于空闲页数。
