# RISC-V Trap 模块

本文描述当前 RISC-V S-mode trap 入口、整数上下文 ABI、返回与致命失败契约。架构机制和设计理由见 [RISC-V Trap 学习总结](../learning/riscv-traps.md)。

## 范围与入口

| 文件 | 当前职责 |
|---|---|
| `include/arch/riscv/trap.h` | 定义汇编/C 共享的 Trap Frame 布局、CSR 位和 cause 编码 |
| `arch/riscv/boot.S` | 在启动栈可用后清零 `sscratch` 并安装 Direct-mode `stvec` |
| `arch/riscv/trap_entry.S` | 保存完整整数现场、调用 C dispatcher、验证并恢复或拒绝返回 |
| `arch/riscv/trap.c` | 提供生产 dispatcher、fatal 和非法返回诊断 |
| `tests/riscv/trap_return_main.c`、`trap_return_sie_main.c`、`trap_return_trigger.S` | 提供只在测试 ELF 中生效的同步与异步返回 handler、独立非法状态探针和寄存器探针 |
| `tests/trap-return-riscv.sh` | 在 QEMU 中验证低地址恢复、返回状态和拒绝路径 |
| `tests/riscv/high_half_trap.c`、`tests/high-half-trap-riscv.sh` | 在最终 Sv39 地址空间验证高半区恢复及 fatal 回归 |

## Trap Frame ABI

`struct riscv_trap_frame` 位于发生 trap 时的当前内核栈上。入口将栈向低地址扩展 288 字节，因此 C dispatcher 获得的 Frame 地址保持 16 字节对齐。

- 偏移 `0..240` 按 x1 到 x31 的架构编号保存 `ra`、原始 `sp`、`gp`、`tp` 和其余整数寄存器；x0 恒为零，不保存。
- 偏移 `248`、`256`、`264`、`272` 依次保存 `sstatus`、`sepc`、`scause`、`stval`，结构尾部补齐到 288 字节。
- 汇编常量和 C 结构定义共享同一头文件；所有字段偏移和结构总大小均由编译期断言核对。
- 当前 RV64IMAC 构建不包含浮点和向量指令，Trap Frame 不保存 F/V 状态；这些状态也不属于当前整数入口 ABI。

入口在使用临时寄存器前先保存全部可写整数寄存器，再计算原始 `sp` 并读取 CSR。完成保存后以 `a0=frame` 调用：

```c
void riscv_trap_dispatch(struct riscv_trap_frame *frame);
```

dispatcher 返回表示 Frame 已经可以恢复；不认识或不能处理的事件必须进入不返回的 fatal 路径。当前生产 dispatcher 尚未安装正式异常或中断 handler，因此所有生产 trap 仍准确输出原始四个 CSR 并通过 SBI 关机。可恢复 handler 只存在于专用测试 ELF，通过链接器包装 dispatcher；其他 cause 仍转交真实生产 dispatcher，不会改变正常内核策略。

## 返回契约

当前入口只允许返回 S-mode。dispatcher 返回后，汇编要求保存的 `sstatus.SPP=1` 且 `sstatus.SIE=0`；否则输出 `invalid trap return` 诊断并关机，不执行不安全的 `sret`。

合法返回按以下顺序完成：

1. 对 Frame 内可写且自然对齐的 `sepc` 槽执行一次 `sc.d`，显式结束可能遗留的 LR/SC reservation；若条件存储成功，写回值仍是原 `sepc`。
2. 将 Frame 中的 `sepc` 和 `sstatus` 写回 CSR。保存的 SIE 必须为零，因此寄存器恢复的关键窗口不会提前接受异步中断。
3. 恢复 x1 到 x31，最后恢复原始 `sp` 并执行 `sret`。

`sret` 根据 SPP 返回 S-mode，根据 SPIE 恢复 SIE。同步异常 handler 只有在理解故障指令长度和重试语义时才能修改 `sepc`；异步中断通常保持 `sepc` 不变，并必须在返回前解除或屏蔽中断源，否则会立即再次进入 trap。

## 不变量与限制

- `stvec` 使用 Direct 模式，所有 S-mode trap 进入同一入口；正常启动保持 `sie=0` 和全局 SIE 关闭。
- `sscratch` 在内核执行期间固定为零。当前入口假定 trap 来自具有有效、16 字节对齐内核栈的 S-mode，不执行 U-mode 换栈。
- 保存现场和 C dispatcher 期间不重新打开 SIE，不支持嵌套异步中断。同步异常可以再次进入当前栈，但 handler、栈或诊断路径自身故障后的递归失败仍没有独立恢复保证。
- 当前使用 4 KiB 单 hart 启动栈，没有独立 Trap 栈、guard page、per-hart 栈或溢出恢复。
- 当前没有运行期 handler 注册、定时器、IPI、外部中断控制器、用户态 trap、调度切换或 F/V 上下文管理。未来 U-mode 入口需要先通过 `sscratch` 找到任务内核栈，再复用相同 Frame 和 dispatcher ABI。

## 验证入口

```sh
make test-trap-return-riscv
make test-trap-riscv
make test-high-half-trap-riscv
make test-riscv
```

`test-trap-return-riscv` 使用显式 32 位 `EBREAK` 验证同步 handler 将 `sepc` 前移 4 字节后返回，再设置 `sip.SSIP`、`sie.SSIE` 和全局 SIE，验证真实 supervisor software interrupt 的保存、清 pending 与返回。汇编探针为除 `sp`、`gp` 外的寄存器设置独立 64 位哨兵，并对全部 x1..x31 的返回值、原始 SP/GP、Frame 对齐、cause 以及 SPP/SPIE/SIE 进行检查；两个测试 ELF 分别构造 SPP 无效和保存 SIE 开启的返回状态，要求各自进入 bad-return 路径。脚本还检查最终 ELF 的入口反汇编，要求 dummy `sc.d` 位于 Frame 槽地址准备之后、`sepc` 写回和 `sret` 之前。

`test-trap-riscv` 保留低地址未处理 breakpoint 的 fatal 契约。`test-high-half-trap-riscv` 在最终 Sv39 页表启用后先完成一次 SSIP 恢复，核对 `sepc` 和 Frame 都位于高半区，再触发原有 breakpoint 并要求生产 fatal 路径不返回。Sv39 权限故障和 no-identity 测试继续验证页故障诊断格式与行为。
