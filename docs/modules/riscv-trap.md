# RISC-V 致命 Trap 模块

本文描述当前 S-mode trap 诊断路径的稳定事实。该路径用于让早期内核准确报告失败，不承担异常恢复、中断分发或用户态切换。

## 范围与入口

| 文件 | 当前职责 |
|---|---|
| `arch/riscv/boot.S` | 在启动栈可用后安装 Direct-mode `stvec` |
| `arch/riscv/trap_entry.S` | 读取 trap CSR 并尾调用 C fatal handler |
| `arch/riscv/trap.c` | 输出诊断并通过 SBI 关机 |
| `tests/riscv/trap_main.c` | 提供独立测试内核入口并公布预期 breakpoint 地址 |
| `tests/riscv/trap_trigger.S` | 在公开符号处执行 `ebreak` |
| `tests/trap-riscv.sh` | 在 QEMU 中验证完整 trap 数据流 |

## 入口契约与不变量

- `stvec` 使用 Direct 模式，在 BSS、`gp` 和 16 字节对齐的启动栈就绪后、首次调用 C 前安装；全局 S-mode 中断仍保持关闭。
- `riscv_trap_entry` 假定 trap 来自使用有效内核栈的 S-mode 代码。它不切换栈、不使用 `sscratch`，也不保存完整通用寄存器。
- 汇编入口把原始 `scause`、`sepc`、`stval`、`sstatus` 依次放入 `a0` 到 `a3`，随后尾调用不返回的 `riscv_trap_fatal`。
- fatal handler 不解码、不修改 CSR；它按固定字段顺序输出四个值，然后调用 SBI System Reset 关机。`stval` 的具体内容由异常类型和实现决定。
- 当前路径不修改 `sepc`、不执行 `sret`，因此所有到达该入口的 trap 都不可恢复。handler 自身再次发生 trap 的行为未定义。

## 验证入口

```sh
make test-trap-riscv
```

该目标构建独立 ELF `build/riscv/tests/kernel-trap-rv`，不替换正常 `kernel-rv` 的入口。测试内核先打印 `trap_test_breakpoint` 地址，再在该符号处执行 `ebreak`；脚本要求 `scause=0x3`、`sepc` 与预告地址一致、其余 CSR 使用十六进制输出、故障指令之后的路径没有执行，并确认 QEMU 经 SBI 正常退出。

正常启动回归仍由 `make test-riscv` 覆盖。当前尚未实现 trap frame、异常恢复、异步中断、用户态 trap、独立 trap 栈或嵌套 trap 防护。
