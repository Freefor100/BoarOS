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

## 映射与容量

匿名映射通过 `MAP_HUGETLB` 使用本架构的大页尺寸。SysV `SHM_HUGETLB`
共用高阶物理页池，段、附加和映射各自持有后备对象。[SysV 共享内存](sysv-shm.md)。

`/proc/sys/vm/nr_hugepages` 设置持续大页容量，初始为零。`/proc/meminfo` 报告
总量、空闲、预留、临时超额和页大小。池扩张受可分配连续物理页限制，缩减时仍在
使用的页归还后释放。预留对应实际可用于缺页的大页。

共享映射复用后备槽，fork 的共享子映射使用同一个对象。私有映射的初始预留由
创建者持有。无法取得私有写时复制所需的额外页时，预留持有者可以撤销未持有
预留的子映射，在用户 pin 和 TLB 同步结束后原地升级。没有预留的映射缺页时若没有
可用物理页，收到 SIGBUS。共享 futex 等待持有映射对象，直到等待退出。

大页的地址和区间修改以原生页大小为边界。非固定地址选择避开向下增长区域的保护
间隔。私有 `MADV_DONTNEED` 先取得恢复预留所需的容量，再让 PTE 退出。
共享 `MADV_DONTNEED` 撤销当前地址空间内的叶，其他映射仍可访问后备，`mincore`
可以观察共享槽的驻留。暂未提供 hugetlbfs、多种 hstate、透明大页或大页 mremap。

`make test-hugetlb-host` 检查物理池、预留、并发挂接、futex、proc 接口及 SysV
段生命周期。`tests/userland/hugetlb.py` 在 RV64 或 LA64 内核上检查匿名私有和
共享映射、实际缺页、写时复制、映射边界、pipe 页引用、SysV 附加和 SIGBUS。

[Linux 大页预留语义](https://docs.kernel.org/mm/hugetlbfs_reserv.html)。
