# 内核线程调度模块

本文描述当前单 hart 内核/用户任务、RISC-V switch context、FIFO 调度、地址空间切换和退出回收的稳定契约。理解抢占、上下文和栈生命周期所需的背景知识见[内核线程与抢占调度学习总结](../learning/kernel-scheduling.md)。

## 范围与入口

| 文件 | 当前职责 |
|---|---|
| `include/arch/riscv/context.h`、`arch/riscv/context.c` | 定义并初始化 RISC-V switch context，提供当前 `tp` 和 SIE 临界区操作 |
| `include/arch/riscv/thread.h` | 定义位于 scheduler thread 对象首部、供 Trap 汇编访问的固定 RISC-V 线程状态前缀 |
| `arch/riscv/context_switch.S` | 保存/恢复 `ra`、`sp`、`tp`、`s0..s11`，首次进入线程 trampoline |
| `include/kernel/scheduler.h`、`kernel/scheduler.c` | 管理静态 idle、内核/用户任务、FIFO 队列、地址空间和页所有权 |
| `arch/riscv/trap.c` | 在 timer backend 和 tick 计数成功后调用 scheduler |
| `kernel/main.c` | 在 timer 启动前初始化 scheduler，并在 boot idle 栈上回收退出线程 |
| `tests/riscv/context_cases.c`、`tests/context-riscv.sh` | 验证 context 初始化、CSR 操作和最终汇编保存集合 |
| `tests/riscv/scheduler_cases.c`、`tests/scheduler-cases-riscv.sh` | 验证状态、失败原子性、FIFO、退出和页回收 |
| `tests/riscv/scheduler_boot.c`、`scheduler_workers.S`、`tests/scheduler-riscv.sh` | 用真实 timer 验证不主动让出的两个线程被抢占和恢复 |
| `tests/riscv/user_boot.c`、`user_payload.S`、`tests/user-riscv.sh` | 验证 U-mode 任务、跨 `satp` 调度、退出/故障记录和完整回收 |
| `tests/riscv/user_elf_boot.c`、`user_elf_program.S`、`tests/user-elf-riscv.sh` | 把装载器产出的地址空间交给 scheduler，运行完整静态 ELF |

架构 context 接口为：

```c
enum riscv_context_status riscv_context_init(
    struct riscv_switch_context *context,
    uintptr_t stack_top,
    void (*entry)(void *),
    void *argument,
    void *thread_pointer);

enum riscv_context_status riscv_context_init_user(
    struct riscv_switch_context *context,
    uintptr_t trap_frame_pointer,
    void *thread_pointer);

void riscv_context_switch(
    struct riscv_switch_context *previous,
    const struct riscv_switch_context *next);
```

`struct riscv_switch_context` 固定为 16 字节对齐的 128 字节结构，字段是 `ra`、`sp`、`tp` 和 `s0..s11`。内核线程首次运行 trampoline；用户任务则把 context 的 `ra/sp` 指向公共 Trap 返回入口和预构造 Frame，第一次被调度就经 `sret` 进入 U-mode。

通用 scheduler 接口为：

```c
enum kernel_scheduler_status kernel_scheduler_init(
    struct physical_page_allocator *allocator,
    uintptr_t idle_stack_low,
    uintptr_t idle_stack_high);

enum kernel_scheduler_status kernel_thread_create(
    void (*entry)(void *),
    void *argument);

enum kernel_scheduler_status kernel_user_thread_create(
    struct riscv_sv39_user_space *space,
    uintptr_t entry,
    uintptr_t stack_pointer,
    uintptr_t thread_pointer);

enum kernel_scheduler_status kernel_scheduler_on_tick(
    uint64_t elapsed_ticks);

enum kernel_scheduler_status kernel_scheduler_reap_one(
    struct kernel_thread_completion *completion);
```

`kernel_scheduler_reap_one()` 每次只回收 exited FIFO 的一个线程，并返回其完成记录。记录区分内核/用户线程以及入口返回、退出系统调用和用户态故障，`status`、`detail` 保存具体完成信息；当前内核线程入口返回产生 `{kernel, returned, 0, 0}`。队列为空返回 `KERNEL_SCHEDULER_STATUS_EMPTY`，不把空队列伪装成成功。

用户任务创建要求入口至少按压缩指令的 2 字节 IALIGN 对齐并落在 U+X 页，用户 SP 按 psABI 的 16 字节边界对齐且 `stack_pointer-1` 落在 U+R+W 页，地址空间还必须与 scheduler 使用同一分配器。成功把地址空间从调用者 move 到任务；失败不转移所有权。错误状态还区分页表/`satp` 状态，失败或队列为空时输出参数保持不变。

`riscv_user_elf_load()` 返回的 LIVE 地址空间、入口和栈指针可以直接传给 `kernel_user_thread_create()`。装载器与 scheduler 仍是两个所有权阶段：装载成功后调用者持有地址空间，线程创建成功后才由 scheduler 持有；创建失败时调用者必须销毁仍由自己持有的空间。生产启动当前没有可执行文件来源，因此只在测试 kernel 中连接这两层。

## 初始化、当前线程和临界区

稳定启动顺序为：

```text
final Sv39/direct map
-> 绑定物理页访问
-> scheduler_init(boot stack bounds)
-> timer_start
-> boot idle: reap one until EMPTY -> wfi
```

初始化只允许一次，要求当前 SP 位于传入的 boot stack、SIE 已关闭、物理页分配器有效，且当前地址空间为 Bare 或 Sv39 ASID 0。成功后静态 idle 成为 current，记录当前 `satp` 为内核地址空间并把 `tp` 设置为 idle 对象。BoarOS 内核 ABI 固定 `tp=current thread`；用户 `tp` 只保存在 Trap Frame/`sscratch`，不替代内核 current。

`kernel_thread_create()` 自己保存并关闭 SIE，完成验证、分配和入队后恢复调用者原来的 SIE 状态。`kernel_scheduler_on_tick()` 和 `kernel_scheduler_reap_one()` 只在 SIE 已关闭时调用；timer trap 天然满足这一点，boot idle 则显式保存/关闭/恢复 SIE。该排他方式只对单 hart 成立，不代表已经具备 SMP 并发安全。

## 状态、时间片与切换

当前只有特殊 `IDLE` 以及普通线程的 `READY`、`RUNNING`、`EXITED`：

```text
allocate -> READY -> RUNNING -> READY
                         |
                         +-> EXITED -> idle reap one -> physical release
IDLE <------- ready empty / last running thread exits
```

ready queue 是侵入式 FIFO。每个非零 elapsed timer 事件最多切换一次；没有 READY 竞争者时保持当前任务。切换前先把下一任务的 ASID 0 `satp` 写入硬件并全局 `SFENCE.VMA`，成功后才修改队列和 current，再切 switch context。所有用户根表借用同一内核高半区，因此切根期间当前内核代码、栈和任务对象保持可访问。

Trap Frame 已保存被中断点的全部整数现场，switch context 只保存当前 scheduler 调用链按 psABI 必须保持的寄存器。恢复某线程的 switch context 后，它先从先前的 scheduler 调用返回，再沿该线程栈上的 dispatcher 和 Trap Frame 执行 `sret`，最终回到原被中断位置。

## 栈与退出所有权

每个普通任务占用一个 4 KiB 线程页，页内保存元数据、switch context、canary 和向下增长的内核栈。用户任务另外独占一个 `riscv_sv39_user_space`，它拥有低半区叶子页和页表页；当前代码页、数据/用户栈页均由该对象统一回收。

入口返回或用户退出/故障时，线程页仍承载当前 SP，不能立即释放。退出路径先切到下一地址空间，把完成记录随任务移到 exited 队列，再通过 discard context 切离当前栈。idle 回收用户任务时先销毁其非活动地址空间，再释放线程页；若线程页释放失败，已销毁状态保留在队首，重试不会二次销毁。只有全部释放成功才移除节点并写出完成记录。

每个 scheduler thread 以固定 32 字节 `struct riscv_thread_state` 开头，依次保存 `kernel_sp`、U 入口暂存的 `user_sp`、用户任务标记和任务 `satp`。C 静态断言与测试共同约束汇编偏移；其余 scheduler 元数据不属于汇编 ABI。

调度前检查 current/`tp`、状态、栈边界、canary 和队列首尾。timer 路径发现错误时把精确 scheduler 状态交给 trap fatal 诊断。线程退出无法返回错误；若它发现不可恢复的不变量损坏，则锁存首个 fatal 状态并通过 discard context 切回已保存的 idle，由原 timer 调用链报告错误，不在可疑线程栈上继续运行或释放所有权不明的页。

## 验证与限制

```sh
make test-context-riscv
make test-scheduler-cases-riscv
make test-scheduler-riscv
make test-user-riscv
make test-user-elf-riscv
make test-riscv
```

context/state 测试覆盖内核与用户首次 context、固定线程前缀、创建失败原子性、FIFO、完成记录和内核线程页回收。用户测试覆盖入口/栈权限校验、U -> 内核 worker -> U 的真实 timer 调度、内核执行期间 `sscratch=0`、未知 syscall、正常退出、用户页故障、完成记录顺序和全部页计数复原；ELF 集成测试再验证装载器输出能直接成为真实用户任务并完整回收。生产 ELF 不含这些测试任务。

当前限制为 RISC-V64 单 hart、ASID 0 全局 TLB 刷新、一个 tick 时间片、FIFO、4 KiB 单页内核栈和 canary。一个用户任务直接拥有一个地址空间，尚无进程/PID、共享地址空间的多线程、BLOCKED/sleep/wait/join、主动 yield、优先级、SMP、guard page、多页栈、F/V 上下文或 LoongArch context。
