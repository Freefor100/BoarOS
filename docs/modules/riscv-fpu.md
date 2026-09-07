# RISC-V 浮点状态模块

本文记录用户态 D 扩展状态在 BoarOS 中的所有权、调度切换和信号/exec 边界。整数 Trap Frame 的入口与返回契约见[RISC-V Trap 模块](riscv-trap.md)。

## 范围与状态布局

| 文件 | 当前职责 |
|---|---|
| `include/arch/riscv/fpu.h` | per-task FPU state 的布局、偏移和汇编接口 |
| `arch/riscv/fpu.S` | 唯一允许触碰 F/D 寄存器的保存、恢复、切换和 reset 代码 |
| `kernel/sched/core.c`、`kernel/sched/process.c` | 创建、调度、clone 和退出时的 FP owner 生命周期 |
| `kernel/sched/exec.c`、`kernel/sched/signal.c` | exec 清空状态、signal frame 保存/恢复 |
| `tests/userland/real.c` | 真实 U-mode FP 抢占和寄存器保持测试 |

每个用户 task 的 `struct riscv_fpu_state` 包含 32 个 64 位寄存器、`fcsr` 和一个 `saved` 标志，共 272 字节、16 字节对齐。基础整数 Trap Frame 仍为 288 字节，不把 FP 状态混入每次 trap 保存；FP 状态由当前 task 单独拥有。

## FS 状态机与调度

用户入口和新 clone 的 Frame 将 `sstatus.FS` 置为 Initial，并把 task 的内存镜像标记为 saved。只有汇编封装 `riscv_fpu_state_save/restore` 和 `riscv_fpu_switch` 使用 F/D 指令；内核 C 和普通汇编按 RV64IMAC 编译，不在 FP owner 之外触碰浮点单元。

context switch 在关闭 SIE 的临界区先观察 FS：前一个用户 task 为 Dirty 时保存 32 个寄存器和 fcsr、将 FS 收敛到 Clean，再按需加载下一个 task 的 saved image；恢复完成后保持 Clean，使下一次用户 FP 指令重新产生 Dirty。内核线程没有用户 FP owner，不保存也不恢复。task 退出时丢弃其 FP image，只加载下一个用户 task 的状态。

这条 lazy 路径把 272 字节保存和 32 次 store/load 放在真正使用 FP 的 task 切换上，而不是每次整数 trap 都支付；当前单 hart 关闭 SIE 已足以串行化 owner。引入 SMP 时必须同时保护 task 状态、远端切换和调度迁移，不能仅把字段改成原子类型。

## exec、clone 与 signal 边界

fork/clone 为 child 从清零 task 页开始新的 FP image，不复制父 task 当前硬件寄存器；child 从自己的初始用户 Frame 开始。exec 在提交新 MM 和 Trap Frame 前调用 reset：硬件 32 个寄存器、fcsr 和内存 image 都清零，并让新程序从 FS Initial 开始，避免旧映像的 callee-saved FP 值泄漏。

信号 frame 的 ucontext 保存当前 task 的 32 个 D 寄存器和 fcsr；`rt_sigreturn` 先通过用户 frame 校验，再把它恢复为下一次调度可加载的 per-task image。FP 仍不改变整数寄存器的 ptrace 顺序和 Trap Frame ABI。

## 当前边界与验证

当前只实现 RISC-V F/D 64 位寄存器视图；未实现 V 扩展、向量上下文、用户态浮点异常信号或跨 hart lazy ownership。每个 task 固定承担 272 字节状态空间，板级性能和 FPU trap 计数尚未测量，不能把 QEMU 的正确性通过写成性能结论。

```sh
make test-signal-riscv
make test-userland-riscv
make test-riscv
```

真实用户程序让浮点寄存器装入不同模式，在 timer 抢占、handler/sigreturn、clone/exec 和退出回收后检查值；signal 测试覆盖 ucontext FP 区域与恢复边界。
