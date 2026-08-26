# 内核任务调度模块

本文描述当前单 hart 内核/用户任务、RISC-V switch context、FIFO 调度、任务身份和退出回收契约。执行现场与抢占原理见[内核线程与抢占调度学习总结](../learning/kernel-scheduling.md)，地址空间所有权见[内核 MM 模块](kernel-mm.md)。

## 范围与入口

| 文件 | 当前职责 |
|---|---|
| `include/arch/riscv/context.h`、`arch/riscv/context.c` | 初始化 RISC-V switch context，提供 current `tp` 和 SIE 临界区操作 |
| `include/arch/riscv/thread.h`、`arch/riscv/context_switch.S` | 定义 Trap 汇编可见的任务前缀，保存/恢复 `ra/sp/tp/s0..s11` |
| `include/kernel/pid.h`、`kernel/pid.c` | 提供调用者给定位图的有界整数 ID 分配器 |
| `include/kernel/task.h` | 暴露不透明 current task、TID/TGID 查询和当前用户任务的只读 MM 借用 |
| `include/kernel/scheduler.h`、`kernel/scheduler.c` | 管理 idle、任务页、FIFO 队列、MM/身份所有权和完成回收 |
| `arch/riscv/trap.c` | 在 timer tick 后调用 scheduler，并把 current task 传给 syscall 层 |
| `kernel/main.c` | 在 timer 启动前初始化 scheduler，在 boot idle 栈上回收退出任务 |

主要接口为：

```c
enum kernel_scheduler_status kernel_thread_create(
    void (*entry)(void *), void *argument);

enum kernel_scheduler_status kernel_user_thread_create(
    struct kernel_mm *mm,
    uintptr_t entry,
    uintptr_t stack_pointer,
    uintptr_t thread_pointer);

enum kernel_scheduler_status kernel_scheduler_on_tick(uint64_t elapsed_ticks);

enum kernel_scheduler_status kernel_scheduler_reap_one(
    struct kernel_thread_completion *completion);

struct kernel_task *kernel_task_current(void);
enum kernel_task_status kernel_task_tid(
    const struct kernel_task *task, kernel_pid_t *tid);
enum kernel_task_status kernel_task_tgid(
    const struct kernel_task *task, kernel_pid_t *tgid);
enum kernel_task_status kernel_task_mm_borrow(
    const struct kernel_task *task, const struct kernel_mm **mm);
```

`struct kernel_task` 对公共调用者不透明。历史接口名中的 `thread` 仍表示创建或结束一条执行流；内部对象使用 task，因为它同时承载调度状态、Linux 身份关系和资源引用。

`kernel_task_mm_borrow()` 只接受 scheduler 当前正在运行的用户任务及其 LIVE MM，成功返回不增加引用计数的只读借用。它用于一次同步 syscall 操作，调用者不得保存或释放该指针；任务在内核调用链和 timer 抢占期间仍拥有 MM，退出回收则要等任务离开该调用链。这样 `uname` 不在每次用户复制时修改 MM 引用计数，未来 SMP 必须把这项借用纳入 task/MM 的读侧生命周期保护。

## 创建和所有权

每个普通任务占用一张 4 KiB 物理页，页内依次放置任务元数据、128 字节 switch context、canary 和向下增长的内核栈。用户首次 context 指向预构造 Trap Frame，第一次被调度便经公共 Trap 返回路径 `sret` 进入 U-mode。

用户任务创建要求：

- MM 使用 scheduler 的物理页分配器，并能生成有效 RISC-V `satp`；
- 入口按 RISC-V IALIGN 的 2 字节边界对齐，映射具有 U+X；
- SP 按 psABI 的 16 字节边界对齐，`SP-1` 映射具有 U+R+W。

创建先验证 MM，再分配并初始化任务页，随后分配 TID，最后才把 MM owner move 进任务并入 ready FIFO。成功消耗调用者的 LIVE MM；任何失败都保留调用者的 MM，并由 scheduler 回滚已经取得的任务页和 TID。若新任务页的立即释放也失败，scheduler 保存唯一待清理物理地址，并在清理完成前拒绝覆盖它。

内核任务和永久 idle 不分配 Linux PID/TID，也不持有用户 MM。

## TID、TGID 与 PID 分配

位图分配器管理闭区间 `1..limit`，0 保留给内核内部无身份任务。Scheduler 当前用静态 4096 字节位图，limit 为 32768；循环游标优先从最近位置继续搜索，释放后的较小空位可以重新利用。耗尽、非法释放和重复释放都有独立状态，不伪造成功。

每个现有用户任务创建为单成员线程组：`tid` 是任务 ID，`group_leader` 指向自身，TGID 等于组首 TID，`group_members=1`。这些字段和 MM 分开，因而以后 `clone` 可以独立选择共享地址空间和加入线程组；当前创建接口尚不接受 clone flags，也不会构造第二个组成员。

## 初始化、临界区与热路径

稳定启动顺序为：

```text
final Sv39/direct map
-> bind physical-page access
-> scheduler_init(boot stack bounds)
-> timer_start
-> boot idle: reap until EMPTY -> wfi
```

初始化要求当前 SP 位于传入 boot stack、SIE 已关闭、物理页分配器有效，且当前地址空间为 Bare 或 Sv39 ASID 0。成功后静态 idle 成为 current，内核 `tp` 固定指向 current task；用户 `tp` 只存在于 Trap Frame/`sscratch`。

创建接口保存并关闭 SIE，timer 和 reaper 则要求调用者已经关闭 SIE。它们串行化当前单 hart 的队列、ID 和 MM 生命周期；这不是 SMP 锁。接入多 hart 时必须在 runqueue、PID 位图、线程组和 MM 引用边界增加锁或采用 per-hart 结构。

任务在创建时缓存 `satp`。tick 和 context switch 不查询 MM、不增减引用、不遍历页表；目标 `satp` 与当前不同才调用切换函数。当前 ASID 0 会为地址空间切换执行全局 `SFENCE.VMA`，这是现阶段主要的切换成本。

## 状态、切换与退出

```text
allocate -> READY -> RUNNING -> READY
                         |
                         +-> EXITED -> idle reaper -> release
IDLE <------- ready empty / running task exits
```

Ready 和 exited 都是侵入式 FIFO。每个非零 elapsed timer 事件最多切换一次；没有 READY 竞争者时保持 current。切换前先激活目标地址空间，成功后才更新队列和 current。Trap Frame 保存任意中断点的完整整数现场，switch context 只保存 scheduler C 调用链依 psABI 必须保留的寄存器。

入口返回或用户 syscall/故障退出时，当前内核栈仍在任务页中，不能就地释放。退出路径先切到下一地址空间，把完成记录与任务移入 exited FIFO，再通过永不入队的 discard context 离开旧栈。若发现 current、边界或 canary 损坏，则锁存首个 fatal 状态并切回可信 idle，由可返回的 timer 路径报告。

Reaper 只在 idle 和内核地址空间中运行，按以下顺序收口用户任务：

```text
release one MM reference -> release TID -> release task page -> publish completion
```

MM 或页释放失败时，节点保持在 exited 队首，完成输出不变；重试从对象中记录的准确阶段继续。释放任务页前先清除 TID owner，因此若最后一步失败，重试不会重复释放 MM 或 TID。只有全部完成才出队并发布记录。内核任务没有 MM/TID，只释放任务页。

## 验证与限制

```sh
make test-context-riscv
make test-scheduler-cases-riscv
make test-scheduler-riscv
make test-mm-riscv
make test-user-riscv
make test-user-elf-riscv
make test-riscv
```

聚焦测试覆盖 context 布局、位图分配/耗尽/回收、创建失败原子性、FIFO、退出和复合释放失败。QEMU 集成测试使用真实 timer 验证没有主动 yield 的内核/用户任务被抢占并恢复；两个使用不同用户栈的单成员线程组共享同一 MM，测试要求它们的 TID/TGID 分别相等、组间 ID 不同，且首个任务回收不销毁 MM、末引用才归还全部页。反汇编检查约束实际 context switch 保存集合，并要求 tick 及其状态校验调用图不出现 MM/PID/物理页生命周期操作；这是热路径结构成本检查，不替代开发板上的周期、TLB miss 和缓存基准。

当前限制为 RISC-V64 单 hart、ASID 0、一个 tick 时间片、FIFO、4 KiB 单页内核栈和 canary。尚无第二个线程组成员、`clone/fork/wait`、BLOCKED/sleep/wakeup、主动 yield、优先级、SMP、内核栈 guard、F/V 上下文或 LoongArch context。
