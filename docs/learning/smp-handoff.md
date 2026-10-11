# 两核启动与执行交接

HSM start返回成功表示固件接纳请求，不保证目标已经进入内核。固定OpenSBI v1.8.1的
`references/opensbi/lib/sbi/sbi_ipi.c:sbi_ipi_send_many()`只接受当前可中断的目标集合；
`sbi_ecall_hsm.c`与`sbi_hsm.c`分别规定请求入口和状态转换。
基于commit `74434f255873d74e56cc50aa762d1caf24c099f8`，首次真实启动复现：
紧接start发IPI返回`SBI_ERR_INVALID_PARAM`(-3)，目标尚处START_PENDING。
内核先等待CPU1发布本地初始化完成，再发IPI，最后由timer/IPI双握手发布ONLINE。
原失败和修复验证限定于启动时序，没有借此改变IPI失败的fatal契约。

跨核调度需要区分候选、执行owner和旧栈存活。统一锁内的候选摘除与on_cpu获取是一个发布点；
context/FPU尚未保存时，旧任务即使已被wake，也不能重新发布为可执行。
旧任务的on_cpu由新栈完成点释放，然后才发布ready_pending或允许可信栈回收。
全局runnable只持一份成员owner，每CPU资格节点借用这份成员，并不另加引用。
这一结构提供有限CPU资格选择，不意味着共享MM、文件或设备业务数据已有同步。

内核任务没有用户PID，deadline索引不能只用(deadline, tid)作为唯一键。
四个tid=0任务登记相同期限，经AVL旋转后，按原比较器删除可走错分支并触发fatal；
生产wait.c链接的host反例已复现。加入任务存储地址作为最后tie-break后，任意同期限节点仍可精确摘除，
不改变具有唯一TID的用户任务排序；任务owner覆盖节点在树中的寿命。

本地SFENCE只影响本hart。内核栈及空页表的归还顺序是清PTE/摘父项、失效、确认、释放；
固定OpenSBI的`lib/sbi/sbi_ecall_rfence.c`和`sbi_tlb.c`实现同步远端确认。
任务页与槽位不能在确认前复用，失败也不能按普通资源耗尽返回后继续释放。
当前映射软件writer仍只有CPU0，缺表分配可能睡眠，所以候选保持私有并在恢复后重新解析父项。

生产接口和当前验证边界见[RV两核模块](../modules/riscv-smp.md)。
