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
`kernel_thread_set_cpu_mask()`要求受控任务已BLOCKED、off_cpu且没有runnable/pending成员，否则BUSY。
迁移资格在新栈`switch_finish`释放旧执行owner以后取得，不读取远端live trap frame或运行中栈。

IPI软件事件使用release发布、exchange消费；接收先清SSIP。通知在最外层raw释放后进入固件，
不在对象或调度raw锁内调用SBI。idle关闭IRQ复查资格和事件后WFI，保持SSIE有效。

栈窗口仅CPU0修改。两页map/unmap为一次操作，缺表先私有分配，可能睡眠后重新查表；已有表不申请候选。
全部预检后发布叶子，撤销后先摘空表，完成本地SFENCE与在线远端RFENCE，才释放表、栈及槽位。
RFENCE错误走fatal并保留owner。单页接口仍是批量接口的包装。
受控任务不得进入MM、文件、设备、协议或系统压力回调；并行allocator使用CPU0预留、无压力hook的私有arena。

## 验证入口与证据边界

```sh
make test-smp-host
make kernel-rv-smp
make test-smp-riscv QEMU_RISCV64=build/data-path-baselines/sync-b3/qemu-system-riscv64 \
  QEMU_RISCV_FIRMWARE=build/data-path-baselines/sync-b3/firmware.bin
make run-smp-riscv
```

host直接链接生产DTB、调度/策略/runqueue/raw实现，验证稀疏身份、失败原子性、资格FIFO及同时claim的唯一owner。
`tests/smp/riscv.py`使用MTTCG、2/1 CPU及512MiB/1GiB，记录vCPU线程、工具和输入SHA、完整串口及native退出状态。
当前窄测试检查无yield的同CPU计算任务抢占、跨CPU等待迁移、SP和全32个F/D寄存器/FCSR、私有arena内容与owner、
空页表和栈回收。最终页基线在可信idle栈上核对，完整审计在CPU1 STOPPED后执行；超时不计为回收通过。

本地依据：`references/opensbi` v1.8.1 `74434f255873d74e56cc50aa762d1caf24c099f8`，
`references/linux` `f4cdf7ca9a1fdcca413157df19753f388a5a224e`，
`references/qemu` `84f07211cc5b4fc6a371559bf8a5de4fb068e648`。
本地执行器为封存QEMU11.1.2/OpenSBI1.8.1；CI身份另记。启动交接依据见[学习说明](../learning/smp-handoff.md)。
