# 显式大页：后端阶段

本分支尚未开放 `MAP_HUGETLB`。以下是供后续 hstate/VMA/fault 层接入的真实大叶支持，
不是把连续基本页作为大页返回给用户。

## 已实现的层

- RV64 Sv39：2 MiB 中间层叶，order 9；LA64：32 MiB PMD，order 11。
- `kernel_mm_leaf` 快照记录叶起点、整块物理 head、order、PTE cookie、有效权限与 COW。
  快照不是独立引用；调用者必须在 MM 保护内取得实际 owner，不能凭过期 head 防止 ABA。
- prepared-order 发布只消费已经准备好的基本页表 owner，不在 MM raw 资格内分配；
  大叶只需要中间表，不创建基本页叶表。数据 owner 的实际 order 必须匹配。
- fork、COW 替换/唯一 owner 原地升级、PROT_NONE 往返、稀疏枚举、范围摘除及销毁。
  驻留/COW 计数按基本页，物理引用按整块。部分大叶修改先失败，不默默拆分。
- TLB 退休批次携带 head/order，确认本地/远端翻译同步后再归还引用。
- 用户 pin 保留 head/order，同时暴露对应基本页窗口和原物理字节身份；两个 pin 可引用
  同一个大叶。futex 只读和 LA 指令读取也不再把 tail 当作独立 owner。

LA 的 `LDDIR`/`LDPTE` 路径直接支持大叶；一个 32 MiB PMD 覆盖两个 16 MiB EntryLo，
无需另写软件 refill 分支。实现参考 Linux 的
[页表位定义](https://raw.githubusercontent.com/torvalds/linux/master/arch/loongarch/include/asm/pgtable-bits.h)
与 [TLB 处理](https://raw.githubusercontent.com/torvalds/linux/master/arch/loongarch/mm/tlbex.S)，
并通过两半、末字节及切回基本页的实际 QEMU 访问核验。

## 验证与边界

- `make test-mm-huge-host`：真实 RV 页表代码及通用 MM pin；完整 head/order 生命周期、
  PROT_NONE、COW、部分修改拒绝、退休后 pin 保活和最终资源恢复。
- `make test-huge-riscv`：独立 QEMU 内核，实际 SATP/SUM 访问 2 MiB 叶，覆盖 fork、
  COW 替换后的活动翻译、原地升级、PROT_NONE、部分范围拒绝与整块退休。
- LA `tests/loongarch/mmu.c`：512 MiB 和 1 GiB RAM 布局下的大叶/旧 MMU 契约通过；
  UP/SMP2 构建通过。尚未验证 SMP 大叶 guest。
- 原 `test-sv39-riscv` 后续 bootstrap fixture 在根页引用查询处 fatal；定向替换回原始
  MMU、MM 和测试三个对象也复现。独立大页测试不依赖该 fixture。
- LA 完整 user contract 后续存在一页资源差异；原始 MMU/test 对象对照也复现。
  不把上述两个旧完整套件标为通过，不宣称性能收益。

## 尚未开放的层

需要完成 native hstate 的真实物理池与 reservation 所有权，随后接入 VMA、缺页、
mmap/munmap/mprotect/madvise/mremap 边界及 Linux ABI。特别是私有 fork 的 reservation
不能简单复制共享计数，NORESERVE、COW 和回滚也不能消耗其他映射的预留。
[Linux reservation 说明](https://docs.kernel.org/mm/hugetlbfs_reserv.html)区分了共享/私有
预留以及 COW 时的容量归属，不能用只增加计数但未保留物理容量的实现替代。

旧基本页发布/COW/移动入口拒绝大叶；syscall 未开放前不能从用户调用获得半接入的大叶。
