# 内核线程调度模块

本文描述当前单 hart 内核线程、RISC-V switch context、FIFO 调度和退出回收的稳定契约。理解抢占、上下文和栈生命周期所需的背景知识见[内核线程与抢占调度学习总结](../learning/kernel-scheduling.md)。

## 范围与入口

| 文件 | 当前职责 |
|---|---|
| `include/arch/riscv/context.h`、`arch/riscv/context.c` | 定义并初始化 RISC-V switch context，提供当前 `tp` 和 SIE 临界区操作 |
| `include/arch/riscv/thread.h` | 定义位于 scheduler thread 对象首部、供 Trap 汇编访问的固定 RISC-V 线程状态前缀 |
| `arch/riscv/context_switch.S` | 保存/恢复 `ra`、`sp`、`tp`、`s0..s11`，首次进入线程 trampoline |
| `include/kernel/scheduler.h`、`kernel/scheduler.c` | 管理静态 idle、普通内核线程、FIFO ready/exited 队列和页所有权 |
| `arch/riscv/trap.c` | 在 timer backend 和 tick 计数成功后调用 scheduler |
| `kernel/main.c` | 在 timer 启动前初始化 scheduler，并在 boot idle 栈上回收退出线程 |
| `tests/riscv/context_cases.c`、`tests/context-riscv.sh` | 验证 context 初始化、CSR 操作和最终汇编保存集合 |
| `tests/riscv/scheduler_cases.c`、`tests/scheduler-cases-riscv.sh` | 验证状态、失败原子性、FIFO、退出和页回收 |
| `tests/riscv/scheduler_boot.c`、`scheduler_workers.S`、`tests/scheduler-riscv.sh` | 用真实 timer 验证不主动让出的两个线程被抢占和恢复 |

架构 context 接口为：

```c
enum riscv_context_status riscv_context_init(
    struct riscv_switch_context *context,
    uintptr_t stack_top,
    void (*entry)(void *),
    void *argument,
    void *thread_pointer);

void riscv_context_switch(
    struct riscv_switch_context *previous,
    const struct riscv_switch_context *next);
```

`struct riscv_switch_context` 固定为 16 字节对齐的 128 字节结构，字段是 `ra`、`sp`、`tp` 和 `s0..s11`。该布局是 RISC-V 架构 ABI，不是完整 Trap Frame。首次运行从 trampoline 开启 SIE，以 `entry(argument)` 调用入口；入口返回后关闭 SIE 并进入不返回的 `kernel_thread_exit()`。

通用 scheduler 接口为：

```c
enum kernel_scheduler_status kernel_scheduler_init(
    struct physical_page_allocator *allocator,
    uintptr_t idle_stack_low,
    uintptr_t idle_stack_high);

enum kernel_scheduler_status kernel_thread_create(
    void (*entry)(void *),
    void *argument);

enum kernel_scheduler_status kernel_scheduler_on_tick(
    uint64_t elapsed_ticks);

enum kernel_scheduler_status kernel_scheduler_reap_one(
    struct kernel_thread_completion *completion);
```

`kernel_scheduler_reap_one()` 每次只回收 exited FIFO 的一个线程，并返回其完成记录。记录区分内核/用户线程以及入口返回、退出系统调用和用户态故障，`status`、`detail` 保存具体完成信息；当前内核线程入口返回产生 `{kernel, returned, 0, 0}`。队列为空返回 `KERNEL_SCHEDULER_STATUS_EMPTY`，不把空队列伪装成成功。

错误状态还区分非法参数、未初始化、重复初始化、内存耗尽、页访问失败、非法状态、队列损坏、栈损坏和页释放失败。失败或队列为空时输出参数保持不变；创建在提交 ready queue 前失败时释放已经取得的页，回滚释放本身失败则保留精确的页释放错误。

## 初始化、当前线程和临界区

稳定启动顺序为：

```text
final Sv39/direct map
-> 绑定物理页访问
-> scheduler_init(boot stack bounds)
-> timer_start
-> boot idle: reap one until EMPTY -> wfi
```

初始化只允许一次，要求当前 SP 位于传入的 boot stack、SIE 已关闭且物理页分配器有效。成功后静态 idle 成为 current，并把 `tp` 设置为 idle 对象。BoarOS 内核 ABI 固定 `tp=current kernel thread`；当前不使用 C TLS，也不允许外部通过 `sp` 页对齐推导线程对象。

`kernel_thread_create()` 自己保存并关闭 SIE，完成验证、分配和入队后恢复调用者原来的 SIE 状态。`kernel_scheduler_on_tick()` 和 `kernel_scheduler_reap_one()` 只在 SIE 已关闭时调用；timer trap 天然满足这一点，boot idle 则显式保存/关闭/恢复 SIE。该排他方式只对单 hart 成立，不代表已经具备 SMP 并发安全。

## 状态、时间片与切换

当前只有特殊 `IDLE` 以及普通线程的 `READY`、`RUNNING`、`EXITED`：

```text
allocate -> READY -> RUNNING -> READY
                         |
                         +-> EXITED -> idle reap one -> physical release
IDLE <------- ready empty / last running thread exits
```

ready queue 是侵入式 FIFO。每个非零 elapsed timer 事件消耗当前的一 tick 时间片；即使 backend 一次补记多个迟到 tick，一次 trap 也最多切换一次。没有 READY 竞争者时保持当前线程，不执行 context switch。普通线程被抢占时进入队尾，队首成为 RUNNING；idle 永不进入 ready queue。

Trap Frame 已保存被中断点的全部整数现场，switch context 只保存当前 scheduler 调用链按 psABI 必须保持的寄存器。恢复某线程的 switch context 后，它先从先前的 scheduler 调用返回，再沿该线程栈上的 dispatcher 和 Trap Frame 执行 `sret`，最终回到原被中断位置。

## 栈与退出所有权

每个普通线程当前占用一个由物理页分配器提供的 4 KiB 页。页内私有保存线程元数据、switch context、栈 canary 和向下增长的内核栈；栈顶保持 16 字节对齐。该布局不公开，不是通用 scheduler ABI，因此以后可以在模块内部换成独立控制块、多页栈或 guard page。

入口返回时页仍承载当前 SP，不能立即释放。退出路径关闭 SIE，把完成记录随线程移到 exited 队列，再把无需恢复的现场写入 scheduler 私有 discard context，切到下一个 READY 或此前保存过的 boot idle。idle 在自己的静态栈上按 FIFO 每次复制一条完成记录并释放一页；只有释放成功才移除队首，因而释放失败不会丢失线程所有权，连续退出也不会合并或覆盖完成原因。

每个 scheduler thread 以 `struct riscv_thread_state` 开头，当前固定字段只有 Trap 入口需要的 `kernel_sp`。该前缀的偏移和大小同时由 C 静态断言与测试约束；内核线程的 `kernel_sp` 等于其栈顶。其余 scheduler 私有元数据不属于汇编 ABI。

调度前检查 current/`tp`、状态、栈边界、canary 和队列首尾。timer 路径发现错误时把精确 scheduler 状态交给 trap fatal 诊断。线程退出无法返回错误；若它发现不可恢复的不变量损坏，则锁存首个 fatal 状态并通过 discard context 切回已保存的 idle，由原 timer 调用链报告错误，不在可疑线程栈上继续运行或释放所有权不明的页。

## 验证与限制

```sh
make test-context-riscv
make test-scheduler-cases-riscv
make test-scheduler-riscv
make test-riscv
```

context runner 检查最终对象的保存/恢复偏移，拒绝把 caller-saved、`gp` 或其他共享状态塞入 switch context。状态测试覆盖初始化前调用、失败初始化、SIE 前置条件、空队列输出不变、创建失败回滚、耗尽、FIFO 入口返回、两条独立完成记录和逐页回收。真实抢占测试创建两个从不 yield 的汇编线程，验证 timer-only 的 A -> B -> A、独立 `sp/tp`、`s0..s11` 哨兵、返回退出、boot idle 恢复和空闲页计数复原；生产 ELF 不含这些 worker。

当前限制为 RISC-V64 单 hart、一个 tick 时间片、FIFO、4 KiB 单页栈和 canary 检测。尚无用户任务/地址空间切换、BLOCKED/sleep/wait/join、主动 yield、优先级、SMP 锁与 per-hart runqueue、guard page、多页栈、F/V 上下文或 LoongArch switch context。
