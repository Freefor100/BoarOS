# 内核线程与抢占调度学习总结

本文整理实现最小可抢占内核/用户任务需要掌握的执行上下文、ABI、状态转换、栈与地址空间所有权和 timer 调度知识，并记录 BoarOS 当前已经确定的选择。当前接口和限制以[内核线程调度模块](../modules/kernel-scheduler.md)为准。

## 内核线程需要保存什么

线程不只是一个入口函数。它至少包含可恢复的 CPU 执行状态、独立内核栈、调度状态和拥有的资源。切换出去时必须保留足够状态，使它以后看起来像一次普通函数调用返回；被异步中断时则必须保留被打断指令可能仍依赖的完整现场。这两种要求产生两类不同上下文。

Trap Frame 面向任意指令边界。中断发生前没有调用者按 ABI 准备现场，所以入口必须保存全部可写整数寄存器以及 `sstatus/sepc/scause/stval`。它位于被中断线程自己的内核栈上，最终由 `sret` 恢复。

Switch context 面向 `riscv_context_switch(previous, next)` 这一普通函数调用。编译器已经按 psABI 允许 caller-saved 寄存器被调用破坏；调用链只要求 callee-saved 寄存器恢复。RV64 整数 psABI 中，`s0..s11` 和 `sp` 是 callee-saved，`ra` 决定 `ret` 的恢复点。BoarOS 还保存 `tp`，因为它被确定为当前任务指针。于是基础 switch context 是 `ra/sp/tp/s0..s11`，不重复保存 `a*`、`t*`、Trap CSR 或完整 Trap Frame。

这种分工也适用于会阻塞的系统调用：线程可以在任意内核调用深度通过 switch context 暂停，恢复后继续原 C 调用链；用户或中断现场仍由该线程栈上的 Trap Frame 管理。用户 F/D 状态再由 per-task FP image 按 FS 状态单独保存，不能把它误当成普通 C callee-saved 寄存器。若只交换最外层 Trap Frame，普通内核调用链的阻塞点就无法自然保存。

## `tp` 为什么适合表示 current

RISC-V psABI 把 `tp` 作为固定用途寄存器，普通函数不能把它当临时寄存器。内核可以约定它始终指向当前可调度任务，从而无需全局查找或用 `sp & page_mask` 推导对象。

`sp` 掩码方案把线程对象布局、栈大小和对齐永久耦合起来；一旦改成多页栈、guard page 或独立控制块，所有调用点都要变化。`tp=current` 让单页布局保持 scheduler 私有。代价是内核不能同时把 `tp` 用作 C TLS 基址。用户态可以拥有自己的 `tp`；trap 入口通过 `sscratch <-> tp` 暂存它并恢复内核 current，返回用户态时再反向交换。

## 从 Timer Trap 到抢占再返回

一次已经运行过的线程 A 被 timer 抢占并切到线程 B，执行顺序是：

```text
A 普通代码
-> timer trap 在 A 栈建立完整 Trap Frame
-> dispatcher 重设 deadline、累计 elapsed tick
-> scheduler 把 A 放回 ready queue，选择 B
-> switch context 保存 A 的 scheduler 调用链，恢复 B 的调用链
-> B 从自己先前的 scheduler 调用继续
-> B 的 dispatcher 返回
-> B 栈上的 Trap Frame 恢复并 sret
-> B 回到自己被中断的位置
```

若 B 从未运行过，它的初始 `ra` 指向 trampoline，初始 `sp` 是自己的栈顶，`s0/s1` 临时携带入口和参数。trampoline 开启 SIE 后调用入口；入口返回则关闭 SIE 并进入线程退出路径。

首次运行用户任务不是调用一个 U-mode C 函数。Scheduler 预先在任务内核栈上构造 Trap Frame，令 switch context 的 `ra` 指向公共 trap return、`sp` 指向该 Frame。第一次被选中时，context switch 直接进入返回汇编，验证 U-mode 来源后由 `sret` 恢复用户 PC、SP、TP 和整数寄存器。这样首次进入和以后从 trap 恢复共用一条架构路径。

这个流程说明“timer handler 返回”不一定立刻回到触发本次中断的线程。context switch 先换了 C 调用链和栈，dispatcher 最终返回的是被恢复线程先前留下的 trap 调用。每个线程始终使用自己的 Trap Frame 和内核栈。

## FIFO、时间片与 elapsed

最小 round-robin 可以用 FIFO ready queue 表示：时间片结束时，把仍可运行的普通线程放到队尾，从队首取下一个。队首/队尾指针使入队和出队都是 O(1)。idle 是没有普通线程可运行时的特殊上下文，不应进入普通队列，否则会与真实工作竞争时间片。

BoarOS 的 timer backend 保留 deadline 相位，并可能一次报告多个迟到 tick。Scheduler 接收真实 `elapsed_ticks`，但当前一次 timer trap 最多切换一次。连续补做多次切换既不会让期间未执行的线程获得实际 CPU 时间，还会增加无意义的上下文开销。当前一个非零 elapsed 事件就耗尽一 tick 时间片；以后增加多 tick 时间片时可以在 scheduler 内部扣减，而不改变 timer 后端。

一次 context switch 只保存十几个寄存器，ready 操作是 O(1)。100 Hz 表示正常情况下每个 hart 最多每 10 ms 做一次调度检查。无 READY 竞争者时只验证少量状态并原线程返回；因此当前单 hart 内核不需要为了性能提前引入复杂策略或 tickless 状态。真正的优化应基于调度延迟、切换次数、栈高水位和空闲唤醒测量。

## 状态与所有权必须一起变化

当前任务生命周期为：

```text
allocated page -> READY -> RUNNING -> READY
                                  |
                                  +-> BLOCKED --event/deadline/signal--> READY
                                  |
                                  +-> EXITED -> idle releases page
```

队列操作不只是移动指针，还转移“谁拥有这张页、谁可能仍在使用这张栈”的事实。创建只有在页访问、元数据、canary 和初始 context 全部成功后才能提交 READY；此前失败必须回滚页。RUNNING 线程返回时，当前 SP 仍在自己的页中，因此不能边退出边释放。它先进入 EXITED 并永不恢复，等 idle 已运行在静态 boot stack 上再释放。

用户任务持有 MM 句柄，而不是直接拥有页表树。首次创建采用移动所有权：入口、用户栈权限、初始 Frame 和 TID 建立成功后，MM 才从调用者转交任务；失败时调用者仍拥有它。普通 clone 通过 `kernel_mm_fork()` 建立独立逻辑地址空间，父子物理页先以 COW 共享；`acquire` 则表达多个 owner 共享同一 MM，未来 `CLONE_VM` 可以复用这个引用边界而不伪造页表所有权。调度切换在修改队列/current 前使用任务创建时缓存的 `satp`，不会在 tick 热路径解析 MM；ASID 0 的根切换仍会全局刷新 TLB。父子/zombie/wait 的生命周期知识见[进程生命周期学习总结](process-lifecycle.md)。

退出切换把旧寄存器写入一份永不入队的 discard context。这样退出线程没有可再次选择的 switch context，idle 回收页也不会留下悬空恢复点。若退出路径发现 `tp`、状态、边界或 canary 损坏，它不能像普通函数那样返回错误；安全做法是记录错误、切到可信 idle 栈，再由仍能返回状态的 timer 调用链执行 fatal 诊断。

## Task、线程组和 Linux 身份

Linux 的 task 表示一条可独立调度的执行流。每个 task 有自己的 TID；同一线程组共享一个 TGID，组首 task 满足 `TID == TGID`。用户通常把 TGID 称为进程 PID，因此 `getpid()` 返回 TGID，`gettid()` 返回当前 task 的 TID。线程组身份与 MM 是否共享是相关但不同的选择：clone flags 可以分别控制加入线程组和共享地址空间，内核不应把“同一 MM”硬编码为“同一 PID”。

BoarOS 用不透明 `kernel_task` 保存调度状态、TID、组首关系和 MM 引用，syscall dispatcher 显式接收 caller task。当前每个用户任务都是自己的组首，所以 `getpid/gettid` 数值相等；字段和 ABI 已经按最终语义分离。内核任务与 idle 没有用户可见身份，ID 0 留作内部“无 PID/TID”，用户 ID 从 1 开始。

有界 ID 可用位图管理：一位表示一个数值是否占用，分配搜索和释放不会额外分配内存，32768 个 ID 只需 4 KiB。线性扫描最坏为 O(limit)，但循环游标使连续创建通常很快；只有进程创建/退出触碰它，不进入 timer/context switch。未来若真实并发创建使扫描或全局锁成为瓶颈，可换成分层位图或 per-CPU 缓存，而不改变 TID/TGID ABI。

回收顺序必须与可观察生命周期一致：先释放 task 的 MM 引用，再归还 TID，最后释放仍承载内核栈和元数据的任务页。任何阶段失败都保留 exited 节点并从准确阶段重试；只有全部完成后才发布 completion。以后实现 `wait` 时，退出后的 zombie 元数据和父进程观察点会延长“退出”和“最终释放 PID”之间的生命周期，不能直接沿用当前立即完成记录作为完整 Linux wait 语义。

## 内核栈大小、对齐和保护

RISC-V psABI 要求函数入口栈保持 16 字节对齐。线程初始栈顶必须满足这一点，context switch 也必须恢复原 ABI 对齐。中断会在当前栈额外压入 BoarOS 的 288 字节 Trap Frame，再调用 C dispatcher 和 scheduler，因此评估栈大小不能只看线程入口的局部变量。

当前普通线程把私有元数据、128 字节 switch context、canary 和向下增长的栈放在同一个 4 KiB 页。实际 timer-only 测试让 worker 带着保存区经历多次 Trap Frame 和调度调用链，证明当前最小路径有可用余量。Canary 只能发现越过栈底后的部分破坏，不能像未映射 guard page 那样在第一次越界访问时立即 fault；它也不是任意内存破坏的恢复机制。

未来是否扩大栈或增加 guard page，应结合真实调用深度和高水位。把页布局留在 scheduler 内部、通过 `tp` 获取 current，正是为了让这种变化不扩散成公共 ABI。

## 单 hart 为什么可以只关本地中断

ready/exited 队列会同时被普通线程创建路径、timer handler 和 idle 回收访问。单 hart 上，保存并关闭 SIE 可以阻止当前 hart 在修改一半时被 timer 抢占，因此足以形成临界区。恢复时只恢复旧 SIE 位，避免一个原本在关中断环境中的调用意外开中断。

第二个 hart 不受本 hart 的 SIE 控制，仍可同时修改共享队列；因此“关中断”不是 SMP 锁。接入 SMP 时应在当前私有队列边界增加锁，或改成 per-hart runqueue，再定义跨 hart 唤醒和内存序。当前没有第二个可验证使用点，不预建这些接口。

## BoarOS 当前选择

- RISC-V 异步现场继续使用完整 Trap Frame，普通调度使用独立的 psABI switch context。
- 内核 `tp` 固定为 current；用户 `tp` 独立保存，`sscratch` 只在 U-mode 保存 current，内核态保持为零。
- 单 hart FIFO round-robin，一个 tick 时间片，一次 trap 最多切换一次。
- 普通内核/用户任务使用私有 4 KiB 单页内核栈布局；用户任务持有可共享 MM 引用和独立 TID/线程组身份；boot context 成为永久 idle，继续使用静态 boot stack。
- 内核线程入口返回即退出；用户任务通过 syscall 或同步故障退出。idle 在另一张栈上依次释放 MM 引用、TID 和任务页。
- 队列临界区保存并关闭 SIE；interruptible wait 使用同一 blocked 链并由未阻塞 pending signal 返回 `SIGNALLED`，vfork 等资源生命周期等待保持不可中断。F/D 状态由 scheduler switch 的 FS Dirty 检查按需保存/恢复；不为尚未实现的 SMP、优先级或 V 状态建立占位层，MM/身份/信号字段则是 `clone/fork/exec/wait` 已确定路径的必要永久机制。

这些选择形成完整、可测的内核线程闭环，同时把以后可能变化的策略、栈布局和 per-hart 组织留在模块内部。RISC-V context 机制可在 QEMU `virt` 与 VisionFive 2 复用；平台 timebase 仍由 DTB 决定。LoongArch 需要自己的 switch context、CSR/中断和 16 KiB 栈页实现，不能复用 RISC-V 汇编。

## 验证和调试经验

- 初始化测试要证明失败尝试不会把 singleton 置为半初始化，输出参数在失败时保持不变。
- 页访问失败必须同时检查空闲页计数恢复，才能发现“返回了错误但泄漏页”的实现。
- 只让线程打印一次不能证明抢占；worker 必须不调用 yield 并持续忙等，由真实 timer 形成 A -> B -> A。
- 给 `s0..s11` 设置独立哨兵并跨多次抢占比较，可发现错误偏移、漏保存和 32/64 位宽度错误；同时检查最终对象反汇编，验证实际链接指令而非源码文本。
- 记录每个 worker 的 `sp/tp`、最终 idle `sp/tp` 和分配器空闲计数，能同时验证独立栈、current ABI、退出切换和页回收。
- 用户任务测试还应跨真实 timer 抢占检查用户 `gp/sp/tp/s0..s11`；让两个任务以不同栈和身份共享正常 MM，并用另一独立根隔离故障任务，可以同时验证共享末引用、`satp` 切换和故障隔离。完成后还要核对全部 MM、叶子页、页表页、TID 和任务页归还。
- 正常生产内核会永久 idle，有限关机逻辑应放在测试 ELF 的链接包装中；生产映像需用符号表确认不含测试 worker。
- 静态分析适合发现 C 状态路径中的空指针、未初始化、双重释放和释放后使用；汇编寄存器集合、Trap Frame/context 配合及真实抢占顺序仍需要反汇编和 QEMU 端到端测试。

## 阻塞与唤醒（当前结论）

- BoarOS 的阻塞机制沿用 wait4 确立的模式：关中断内检查条件、置 BLOCKED、经 `riscv_context_switch` 切走；唤醒方把任务置 READY 并入 ready 队尾，被唤醒者从原调用点返回后必须重查条件。所有 BLOCKED 任务都在全局 blocked 链上，事件通道（wait_queue 令牌）与超时（deadline 刻度）是任务的字段而不是独立节点，唤醒按 FIFO 走链。
- 单 hart 下丢失唤醒的唯一来源是"检查条件与阻塞之间开中断"；因此内核线程（trampoline 运行在 SIE=1）调用阻塞接口前必须用 `riscv_interrupt_save/restore` 收敛临界区，而 syscall/trap 上下文天然关中断。Linux 的 `schedule()` 把这一职责收进调度器本身并保存/恢复中断状态，等 BoarOS 引入线程和 SMP 时需要对齐这一语义。
- 超时唤醒与事件唤醒共用一条 blocked 链：tick 处理器在抢占检查之前先扫描到期 deadline，使刚到期的任务能在同一次切换中被选中；唤醒延迟上界是一个 tick 周期。Linux 用红黑树/timer wheel 组织到期任务，等待队列按需唤醒；BoarOS 的 O(阻塞数) 走链是有意的阶段性简化，扩展路径已在模块文档声明。

## FP 状态为什么不塞进基础 Trap Frame

RISC-V `sstatus.FS` 给出了 Initial/Clean/Dirty 状态，适合把少见的 F/D 使用从每次整数 trap 和普通内核线程切换中分离。BoarOS 的基础 Frame 保持固定 288 字节；用户 task 另有 272 字节的 32 个 D 寄存器、fcsr 和 saved 标志。context switch 只在前一个 task 为 Dirty 时保存，再在下一个 task 有 saved image 时恢复，恢复后把 FS 留在 Clean。

这要求 FP 保存代码是唯一触碰 F/D 的汇编边界，并且在 scheduler 关闭 SIE 的窗口内与 current/task owner 一起切换。exec 必须清空硬件和 image，signal frame 必须把 image 纳入 ucontext；否则旧映像或 handler 会观察到不属于自己的浮点寄存器。当前只覆盖单 hart F/D，V 扩展和 SMP owner 协议留在后续独立阶段。

## 资料依据

- [RISC-V ELF psABI](https://riscv-non-isa.github.io/riscv-elf-psabi-doc/)：RV64 调用约定、callee-saved 寄存器和栈对齐。
- `references/riscv/riscv-privileged-20260120.pdf`：SIE、trap 进入/返回和特权 CSR。
- `references/linux/arch/riscv/kernel/entry.S`：Linux RISC-V `__switch_to` 保存集合、`tp=current` 和 trap/current 配合。
- `references/linux/arch/riscv/include/asm/switch_to.h`：Linux RISC-V switch 调用边界与扩展状态组织。
