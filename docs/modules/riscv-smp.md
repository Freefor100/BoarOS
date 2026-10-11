# RV 受控两核调度

默认构建和LA仍为单CPU。`kernel-rv-smp`独立配置最多两个逻辑CPU，CPU0是固件选择的boot hart，
CPU1运行显式创建的受控内核任务。用户任务、现有worker、共享MM和系统压力回收只在CPU0执行。
这项范围不提供用户线程跨核或共享用户MM的并发语义。

## 启动与本地状态

`dtb_read_cpu_info()`验证固定容量的CPU快照，保留真实hart ID，忽略disabled CPU和cpu-map拓扑节点；
boot hart固定在逻辑0，其余按ID排序。重复、损坏、缺boot hart或超容量失败时不改输出。
`secondary.S`使用独立16KiB静态栈与镜像mailbox，不清BSS；过渡映射后的高地址入口重绑gp/tp、
stvec/sscratch及最终satp。内核仍通过`tp→任务→CPU`定位，用户TLS契约保持。

`smp.c`探测HSM/IPI/RFENCE，启动与停止预算各5秒。HSM STARTED不作为内核ONLINE证明：
CPU1完成本地timer和IPI握手才发布ONLINE。停止先收完受控任务的存储owner，确认静态idle、
无切换前驱及就绪资格，再请求HSM stop；CPU0禁止抢占直到STOPPED确认。失败保留可能仍被使用的owner。

timer的周期、下一tick与armed状态属于CPU；本地RR和RT记账保持独立。CPU0推进全局tick、
coarse time、load、等待期限和服务轮询。CPU1新增期限通知CPU0重装事件。

## 调度与资源

统一rank40短锁保护全局runnable成员、每CPU资格索引和执行owner。资格节点内嵌任务并借用全局成员；
摘取任务同时摘除所有CPU索引。选择不扫描另一CPU的任务，索引维护受两CPU容量约束。
普通创建接口默认CPU0；`kernel_thread_create_joinable_mask()`只供CPU0创建带join owner的受控任务。
可睡眠的私有准备不取得已发布owner；最终rank40接纳点重查CPU在线状态，失效时锁外回滚且不发布join。
`kernel_thread_set_cpu_mask()`要求受控任务已BLOCKED、off_cpu且没有runnable/pending成员，否则BUSY。
迁移资格在新栈`switch_finish`释放旧执行owner以后取得，不读取远端live trap frame或运行中栈。
最后存储借用可以在任一受控CPU归还；实际collector只在普通CPU0服务上下文执行，
受控CPU0和CPU1均把owner留给退休队列并通知，不通过放宽服务断言执行清理。

IPI软件事件使用release发布、exchange消费；接收先清SSIP。通知在最外层raw释放后进入固件，
不在对象或调度raw锁内调用SBI。idle关闭IRQ复查资格和事件后WFI，保持SSIE有效。

栈窗口仅CPU0修改。两页map/unmap为一次操作，缺表先私有分配，可能睡眠后重新查表；已有表不申请候选。
全部预检后发布叶子，撤销后先摘空表，完成本地SFENCE与在线远端RFENCE，才释放表、栈及槽位。
发布或摘除非叶父项使用全范围失效，不能只指定某个叶VA；已有父表中的叶修改可使用范围RFENCE。
RFENCE错误走fatal并保留owner。单页接口仍是批量接口的包装。
受控任务不得进入MM、文件、设备、协议或系统压力回调；并行allocator使用CPU0预留、无压力hook的私有arena。

## 验证入口与证据边界

```sh
make test-smp-host
make kernel-rv-smp
make test-smp-riscv QEMU_RISCV64=build/data-path-baselines/sync-b3/qemu-system-riscv64 \
  QEMU_RISCV_FIRMWARE=build/data-path-baselines/sync-b3/firmware.bin
make test-smp-userland-riscv QEMU_RISCV64=build/data-path-baselines/sync-b3/qemu-system-riscv64 \
  QEMU_RISCV_FIRMWARE=build/data-path-baselines/sync-b3/firmware.bin INIT_CONFIG=config/init.json
make run-smp-riscv
```

host直接链接生产DTB、调度/策略/runqueue/raw实现，验证稀疏身份、失败原子性、资格FIFO及同时claim的唯一owner。
`tests/smp/riscv.py`使用MTTCG、2/1 CPU及512MiB/1GiB，记录vCPU线程、工具和输入SHA、完整串口及native退出状态。
2026-10-11本地验收覆盖：

| 契约 | 真实客户机证据 |
|---|---|
| 执行与状态 | 无主动yield的两核计算、mask=3双资格与唯一owner、双向阻塞迁移；SP、返回PC、s0–s11、32个F/D寄存器及FCSR |
| 通知与等待 | 全局tick不翻倍、提交后尚未切走的提前wake、CPU1 timer关闭后的WFI边界、远端新增deadline；RW读者批次/writer FIFO、CPU1 FIFO yield与RR timer |
| 退休与栈 | CPU1/受控CPU0/迁移后归还最后借用、私有创建睡眠后遇STOPPED的回滚；延迟RFENCE确认、第二张缺表OOM、旧翻译替换及槽位复用 |
| 私有allocator | 两核4MiB预留arena，buddy/共享引用/所有slab class/大对象/resize、耗尽、内容与独立owner账本；结束后独占audit并归还arena |
| 组合 | CPU0原静态与pthread ELF、CPU1受控计算及私有arena；PID1、页/堆/任务栈、等待与根盘/设备收口 |

同一SMP测试核完成2/1CPU各512MiB/1GiB的自然退出。最终页基线在可信idle栈上核对，
完整审计在CPU1 STOPPED后执行；不是用串口marker替代内部owner断言。
缺RFENCE扩展、start拒绝、实际guard fault、STOPPED确认失败与RFENCE返回错误在独立双RAM启动验证；
这些启动不计自然退出或资源回收成功。默认RV完整回归及512MiB/1GiB/16GiB启动、双侧glibc五形态、
1366ABI、userland/guard/栈和相关SQLite/NBD、设备、网络及终端门禁通过；未重跑完整比赛Harness或全断电矩阵。

默认关闭COST。当前固定工具产物中，SMP任务为2752字节（单CPU2688），CPU记录1952字节（单CPU272）；
每个任务仍持一个metadata页，没有为资格节点增加引用或页。secondary常驻16KiB栈及独立idle/CPU记录；
同配置生产ELF的BSS比单CPU多25056字节，包含新增记录和链接对齐。
资格挑选O(1)，入队/摘除最多维护两份资格索引。每次启动报告IPI发送/接收、RFENCE请求与整启动耗时，
次数随交错变化，不作为固定协议常数；未以这些正确性运行宣称并行吞吐收益。
`SMP=1 COST_DIAGNOSTICS=1`在构建前拒绝，单CPU成本路径保持。

实际RFENCE执行器及输入SHA由runner记录；使用`-bios default`时只记录默认选择，
精确固件身份需显式传入`QEMU_RISCV_FIRMWARE`。本地封存执行器和CI执行器各自记录，
本地通过不能代替托管CI结果。

本地依据：`references/opensbi` v1.8.1 `74434f255873d74e56cc50aa762d1caf24c099f8`，
`references/linux` `f4cdf7ca9a1fdcca413157df19753f388a5a224e`，
`references/qemu` `84f07211cc5b4fc6a371559bf8a5de4fb068e648`。
本地执行器为封存QEMU11.1.2/OpenSBI1.8.1；QEMU SHA-256
`5713c546693820d225e87795a14e8177343cb7ac4d9b0610a9e41c7203eb023d`，固件SHA-256
`894e2aef99590fc07ec6c60ab00282b8bc5d5d5bb2a1d0c6ada0c52df24274c0`。
CI身份另记。启动交接及非叶失效依据见[学习说明](../learning/smp-handoff.md)。
