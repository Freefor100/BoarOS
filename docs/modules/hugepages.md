# 显式大页：原生匿名映射

支持匿名 `MAP_HUGETLB` 的 PRIVATE/SHARED、原生粒度、NORESERVE、真实池预留及
fork/COW 生命周期。不是把连续基本页作为大页返回给用户，也不是透明大页。

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

## 池与用户语义

`/proc/sys/vm/nr_hugepages` 控制实际原生连续页池；默认零，不能凭计数承诺不存在的容量。
`/proc/meminfo` 报告 Total/Free/Rsvd/Surp 和原生 Hugepagesize。扩容可部分成功，读取值为
实际 persistent 容量。缩容保留仍被 PTE/pin 持有的 surplus，最后引用退还时再释放。

PRIVATE 初始映射预留不传给 fork 子映射；COW 候选不能消费其它槽位预留。无可用候选时，
预留拥有者按 lineage 撤销非拥有子映射，完成 pin/TLB 排空并重验 cookie 后尝试原地写。
被撤销子映射以后 fault 为 SIGBUS。共享映射持一个真实 backing cache owner，futex key
另持 map 生命周期引用。NORESERVE 在 fault 时无容量产生 SIGBUS，而不是伪造驻留。

DONTNEED 的私有预留恢复在撤 PTE 前取得令牌：容量不足时保留原 PTE/内容；成功后仅在
TLB 同步完成时转交 detached owner。起点要求原生对齐，尾部依照
[Linux madvise 实现](https://kernel.googlesource.com/pub/scm/linux/kernel/git/torvalds/linux/+/aa5e9884a2d63aa20fc3396d369382c1ecd16109/mm/madvise.c)
向下对齐，未覆盖完整大页不丢弃。共享 DONTNEED 只撤本 MM PTE，backing 仍可由 mincore
观察。mprotect/munmap 的部分大页边界在修改前拒绝；不会隐式拆成基本页。

`make test-hugetlb-host` 覆盖池双粒度、并发/abort/pin/surplus、futex owner、proc 控制，
以及真实 MM 私有/共享 fault、fork/COW、父缺容量撤子、PROT_NONE、mincore 全基本页位、
DONTNEED、NORESERVE 与最终回收。专项 guest runner 为 `tests/userland/hugetlb.py`，
使用已有架构工具链、单独 /init 和 ext4 fixture；保存内核/ELF/source 哈希与 guest 日志。
不依赖旧完整 user suite，不以 host 测试代替实际大叶访问。

## 明确未支持

- hugetlbfs 文件映射、SHM_HUGETLB、多种 hstate 大小、THP、NUMA 分配策略。
- 大页 mremap 当前返回 EOPNOTSUPP，旧基本页移动入口拒绝大叶。
- 不支持大页拆分；部分大页 VMA flag 编辑返回错误。
- 尚未声称 SMP guest 大页专项通过，也没有性能测量收益声明。

[Linux reservation 说明](https://docs.kernel.org/mm/hugetlbfs_reserv.html)是私有/共享预留
及 COW 归属的依据。最终 root shutdown 在用户、回调和 proc 访问静止后释放 registry
持久引用；活 map/pin 存在时失败，不能用跳过资源基线来隐藏泄漏。

## 此阶段验收记录

- 双架构专用 guest 完整通过：RV 2 MiB / LA 32 MiB，6 组功能 marker，PID 1
  正常退出，heap-live=0 且物理页恢复 root baseline。第一轮曾发现 registry 单对象/单页
  残留；接入最终 shutdown 后以相同程序复验通过，未放宽 runner 的 cleanup 条件。
- host 对第二个 fork registration 注入 stale epoch，覆盖前一个已注册 child map 的清理；
  hstate 层另覆盖实际 revoke epoch 改变后拒绝旧 registration。跨 MM 自持 pin 引发
  parent COW 重试时不死锁，解除 pin 后成功；3 次 fork/子 COW 与整大页区间部分 unmap
  检查实际池守恒。超容量映射失败不消耗预留，shutdown 空池重复调用不泄漏。
- 这些注入不是完整的多 CPU guest 时序证明；fork 与 revoke 的强制并发 guest 尚未覆盖。

### 与 GROWSDOWN 的组合

整合基线 a79797f0 后，匿名基本页的 GROWSDOWN 继续由原路径处理；HUGETLB 与
GROWSDOWN 同时请求仍为 EINVAL。huge mmap 的资源核算明确不是 growdown 数据类，
非固定提示地址和 top-down 搜索均尊重已有 growdown/stack guard 区。
`tests/host/mm_hugetlb.c` 覆盖提示落入 guard 和位于 mmap ceiling 之上的 stack guard
向下覆盖候选地址的两种组合，防止只修 syscall 标志而破坏隔离间隔。

### Pipe 消费者与整合回归

pipe buffer 的基本页窗口另带 compound order；vmsplice 独立取得整块 head 引用，tee、
部分 splice 克隆同一 owner，全 splice 转移 owner，读取、关闭和失败回滚均按 order 归还。
基本页快路径仍为 order 0，buffer 利用现有 padding，不增加描述符大小，不用复制替代借页。
最终专项扩为 8 组，新增内部窗口 vmsplice、fork/COW、tee、部分/完整 splice、原映射
unmap 后读取，以及未读取就 close 的池容量恢复。

独立审查发现 16 KiB host pipe 套件超时，f838aca8 基线同环境通过。最小对照确认新
Sv39 leaf resolver 的上级索引误随 allocator granule 偏移，与原发布路径不一致，已改回
一致的 30/21/BOAROS_PAGE_SHIFT。旧对象在 16 KiB snapshot 断言失败，修复后 4/16 KiB
最小测试及原 pipe 16 KiB 全套退出 0。`mm-base-leaf.sh` 已纳入 `test-hugetlb-host`。
此 host 模型不是 LA 硬件页表验证；LA 真实 32 MiB 页由专用 guest 另行验证。
