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

范围失效与非叶变更不能混用。固定`references/riscv/riscv-privileged-20260120.pdf`
（Privileged Architecture 20260120，SHA-256
`d0f818af6fa519d39e68f822aa795bff9f38032a2f352afdf43e91c0d480e408`）的SFENCE.VMA规则要求：
非叶PTE变更采用`rs1=x0`；只指定叶VA不足以覆盖缓存的非叶翻译。
栈窗口发布新的level-0表或摘空表，因此同步全范围；已有父表内的叶更新可按地址同步。
真实反例检查新父项的RFENCE参数，另由CPU1缓存旧别名，撤销后换物理页，核对新内容；
调用者保持新旧物理owner直到确认，不能通过旧页恰好未被复用掩盖陈旧翻译。

私有资源准备与最终接纳要分别验证状态。一次强制交错让普通CPU0创建者在分配前park，
另一CPU0服务任务确认CPU1无执行/就绪/已发布owner并完成HSM停止，再恢复创建者。
旧路径沿用准备前的ONLINE快照，最外层raw释放后向STOPPED hart发IPI，OpenSBI返回-3。
最终rank40接纳点重查CPU资格后，失败锁外归还私有栈、表及metadata，join保持未接纳。
这不需要把私有构造加入已发布任务计数，也不需要新增worker层。

归还最后存储借用不等于当前上下文有资格收集。受控任务可以在CPU0归还借用，
但业务资源清理仍由普通CPU0服务执行；旧路径仅判断CPU编号，会撞上collector的受控上下文断言。
现在CPU1与受控CPU0都保留真实退休队列owner，普通CPU0才内联收集。
反例在put后的IRQ关闭区检查owner，再核对join后的基线，避免把后续合法idle收集当作提前释放。

这些反例、双向迁移和组合程序均有真实MTTCG双RAM结果，支持受控两核调度核心的正确性闭环；
原调度证据不覆盖共享用户MM、设备业务并发或LA多核。受控私有匿名MM的新证据及边界见[内存许可与失效完成](memory-management.md#私有匿名并发的许可与失效完成)。

生产接口和当前验证边界见[RV两核模块](../modules/riscv-smp.md)。
