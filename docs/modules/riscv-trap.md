# RISC-V Trap 模块

本文描述当前 RISC-V S/U-mode trap 入口、整数上下文 ABI、返回与失败契约。架构机制和设计理由见 [RISC-V Trap 学习总结](../learning/riscv-traps.md)，正式 timer handler 见 [RISC-V Timer 与内核 Tick 模块](riscv-timer.md)，trap 内的线程切换见[内核线程调度模块](kernel-scheduler.md)。

## 范围与入口

| 文件 | 当前职责 |
|---|---|
| `include/arch/riscv/trap.h` | 定义汇编/C 共享的 Trap Frame 布局、CSR 位和 cause 编码 |
| `arch/riscv/boot.S` | 在启动栈可用后清零 `sscratch` 并安装 Direct-mode `stvec` |
| `arch/riscv/trap_entry.S` | 保存完整整数现场、调用 C dispatcher、验证并恢复或拒绝返回 |
| `arch/riscv/trap.c` | 提供生产 dispatcher、timer/scheduler fatal 和非法返回诊断 |
| `tests/riscv/trap_return_main.c`、`trap_return_sie_main.c`、`trap_return_trigger.S` | 提供只在测试 ELF 中生效的同步与异步返回 handler、独立非法状态探针和寄存器探针 |
| `tests/trap-return-riscv.sh` | 在 QEMU 中验证低地址恢复、返回状态和拒绝路径 |
| `tests/riscv/high_half_trap.c`、`tests/high-half-trap-riscv.sh` | 在最终 Sv39 地址空间验证高半区恢复及 fatal 回归 |
| `tests/riscv/timer_boot.c`、`tests/timer-riscv.sh` | 通过真实 supervisor timer interrupt 验证生产 dispatcher 返回 |
| `tests/riscv/user_boot.c`、`user_payload.S`、`tests/user-riscv.sh` | 验证真实 U-mode 换栈、抢占恢复、ecall、用户故障和资源回收 |

## Trap Frame ABI

`struct riscv_trap_frame` 位于发生 trap 时的当前内核栈上。入口将栈向低地址扩展 288 字节，因此 C dispatcher 获得的 Frame 地址保持 16 字节对齐。

- 偏移 `0..240` 按 x1 到 x31 的架构编号保存 `ra`、原始 `sp`、`gp`、`tp` 和其余整数寄存器；x0 恒为零，不保存。用户 `gp` 保存后，入口以禁止 relaxation 的 PC 相对序列重载内核 `__global_pointer$`，C dispatcher 不依赖用户提供的全局数据基址。
- 偏移 `248`、`256`、`264`、`272` 依次保存 `sstatus`、`sepc`、`scause`、`stval`；偏移 `280` 保存可信的内核 `tp`，结构总长 288 字节。
- 汇编常量和 C 结构定义共享同一头文件；所有字段偏移和结构总大小均由编译期断言核对。
- 当前 RV64IMAC 构建不包含浮点和向量指令，Trap Frame 不保存 F/V 状态；这些状态也不属于当前整数入口 ABI。

入口在破坏用户现场前先通过 `sscratch` 暂存用户 `tp`，并在切换可信栈后保存全部可写整数寄存器、原始 `sp` 和 CSR。完成保存后以 `a0=frame` 调用：

```c
void riscv_trap_dispatch(struct riscv_trap_frame *frame);
```

dispatcher 返回表示当前恢复到的线程 Frame 已经可以恢复。生产路径处理三类事件：supervisor timer 先重设 deadline、累计 tick 并调用 scheduler；U-mode ecall 把 `a7` 和 `a0..a5` 复制到通用 syscall 解码器；U-mode 同步故障以 `{user-fault, scause, stval}` 终止当前任务。未知 syscall 把 `-ENOSYS` 写回 `a0` 并把 `sepc` 前移 4 字节；`exit(93)` 不返回。S-mode 未处理事件和不支持的中断仍输出原始 CSR 并通过 SBI 关机。

## 返回契约

dispatcher 返回后，汇编总是要求 Frame 中 `sstatus.SIE=0`，避免在寄存器恢复窗口提前接受中断。SPP=1 直接返回 S-mode；SPP=0 还要求 Frame 的 `kernel_tp` 非零、等于当前内核 `tp`，且线程前缀标记为用户任务，否则进入 `invalid trap return`，不会把任意 S-mode `tp` 当指针解引用。

合法返回按以下顺序完成：

1. 对 Frame 内可写且自然对齐的 `sepc` 槽执行一次 `sc.d`，显式结束可能遗留的 LR/SC reservation；若条件存储成功，写回值仍是原 `sepc`。
2. 将 Frame 中的 `sepc` 和 `sstatus` 写回 CSR。保存的 SIE 必须为零，因此寄存器恢复的关键窗口不会提前接受异步中断。
3. S-mode 返回前保持 `sscratch=0`；U-mode 返回前把 current thread 写入 `sscratch`，随后恢复包括用户 `tp/sp` 在内的 x1..x31 并执行 `sret`。

`sret` 根据 SPP 返回 S-mode，根据 SPIE 恢复 SIE。同步异常 handler 只有在理解故障指令长度和重试语义时才能修改 `sepc`；异步中断通常保持 `sepc` 不变，并必须在返回前解除或屏蔽中断源，否则会立即再次进入 trap。

## 不变量与限制

- `stvec` 使用 Direct 模式，所有 S-mode trap 进入同一入口；启动和页表切换阶段保持 `sie=0` 与全局 SIE 关闭，最终地址空间稳定后只开启 STIE 和全局 SIE。
- `sscratch` 在内核执行期间固定为零，用户执行期间保存 current thread。入口先用 `csrrw` 与用户 `tp` 交换，从可信线程前缀取得 `kernel_sp`；保存用户 `tp/sp` 后立即把 `sscratch` 清零，再进入 C。
- 保存现场和 C dispatcher 期间不重新打开 SIE，不支持嵌套异步中断。同步异常可以再次进入当前栈，但 handler、栈或诊断路径自身故障后的递归失败仍没有独立恢复保证。
- boot idle 使用 4 KiB 静态启动栈，普通内核线程各使用私有 4 KiB 单页栈；没有独立 Trap 栈、guard page、per-hart IRQ 栈或溢出恢复。
- 当前只有静态 timer、U ecall 和用户同步故障处理，没有运行期 handler 注册、IPI、外部中断控制器、故障信号/上送或 F/V 上下文管理。用户故障直接终止任务，不支持修页、按需分页或用户级异常处理。

## 验证入口

```sh
make test-trap-return-riscv
make test-trap-riscv
make test-high-half-trap-riscv
make test-timer-riscv
make test-scheduler-riscv
make test-user-riscv
make test-user-fatal-riscv
make test-riscv
```

`test-trap-return-riscv` 使用显式 32 位 `EBREAK` 验证同步 handler 将 `sepc` 前移 4 字节后返回，再设置 `sip.SSIP`、`sie.SSIE` 和全局 SIE，验证真实 supervisor software interrupt 的保存、清 pending 与返回。汇编探针为除 `sp`、`gp` 外的寄存器设置独立 64 位哨兵，并对全部 x1..x31 的返回值、原始 SP/GP、Frame 对齐、cause 以及 SPP/SPIE/SIE 进行检查；两个测试 ELF 分别构造 SPP 无效和保存 SIE 开启的返回状态，要求各自进入 bad-return 路径。脚本还检查最终 ELF 的入口反汇编，要求 dummy `sc.d` 位于 Frame 槽地址准备之后、`sepc` 写回和 `sret` 之前。

`test-user-riscv` 用两个独立用户地址空间验证 U-mode：主任务在用户循环中被真实 timer 抢占，内核 worker 运行时观察 `sscratch=0` 并设置共享页标志，主任务恢复后核对用户 `gp/sp/tp/s0..s11`、未知 syscall 返回和 `exit(93)`；另一任务访问未映射地址，要求生成用户故障完成记录且不影响主任务。runner 最终要求内核线程、两个用户任务及其地址空间全部回收。

`test-user-fatal-riscv` 在同一真实用户路径中让测试 wrapper 破坏已返回 ecall Frame 的可信 `kernel_tp`，要求汇编拒绝返回，并在用户根仍活动时经 supervisor-only 高半区 UART 输出唯一 fatal 行后关机。它防止诊断路径暗中依赖只存在于内核根的低地址设备映射。

`test-timer-riscv` 使用实际 QEMU DTB、OpenSBI TIME 和 cause 5，让生产 dispatcher 至少两次经完整 Trap Frame 路径返回 `wfi`。`test-scheduler-riscv` 让两个不主动让出的内核线程只靠 timer 在各自 Frame/栈之间切换。`test-trap-riscv` 保留低地址未处理 breakpoint 的 fatal 契约；`test-high-half-trap-riscv` 验证最终 Sv39 高半区的 SSIP 恢复和 fatal 路径。
