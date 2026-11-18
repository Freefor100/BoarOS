# 原生大页与复合页

原生大页由架构页表的大叶表示，物理分配和生命周期以整块为单位。

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

## 验证

- `make test-mm-huge-host` 检查 4 KiB 和 16 KiB 模型中的基页索引，以及复合页的
  头页引用、页内偏移、访问权限、写时复制和退役后 pin 寿命。
- `make test-huge-riscv` 使用实际 Sv39 页表访问 2 MiB 叶，检查 fork、写时复制、
  保护权限以及整块退役。
- LA 的 `tests/loongarch/mmu.c` 使用实际的 32 MiB PMD，检查两半、末尾地址、
  fork、写时复制、pin 和退役。

这一层提供物理页分配、页表和用户 pin 的所有权。通过内核接口发布大叶时，
pipe 对 vmsplice 窗口持有整块复合页引用，tee 和部分 splice 继续共享该所有权，
完整 splice 移交引用。显式大页 mmap 的容量管理和 ABI 由大页池与 VMA 层提供。
