# RISC-V 启动模块

本文描述当前代码的稳定事实。构建、地址与固件交接的个人理解见 [RISC-V 启动学习记录](../learning/riscv-boot.md)，异常诊断契约见 [RISC-V 致命 Trap 模块](riscv-trap.md)，启动内存边界见 [DTB 与启动内存布局模块](dtb-memory.md)。

## 范围与入口

`make all` 使用 RISC-V bare-metal GCC 生成根目录 ELF `kernel-rv`。QEMU `virt` 通过 `-kernel` 装载 ELF，默认 OpenSBI 初始化机器后以 S-mode 跳到 `_start`。

| 文件 | 当前职责 |
|---|---|
| `Makefile` | 编译、链接以及运行、测试、调试入口 |
| `arch/riscv/linker.ld` | ELF 入口、加载段、地址布局和启动栈 |
| `arch/riscv/boot.S` | 建立最小 C 运行环境、安装 trap 入口并调用 `kernel_main` |
| `kernel/dtb.c`、`kernel/boot_memory.c` | 读取 DTB 内存事实并生成可用物理区间 |
| `kernel/physical_page.c` | 对齐可用区间并建立单页物理分配器 |
| `kernel/main.c` | 建立启动内存布局和物理页分配器，输出诊断后关机 |
| `arch/riscv/virt_uart.c` | QEMU `virt` NS16550A 轮询输出 |
| `arch/riscv/sbi.c` | 通过 SBI SRST 请求关机 |
| `tests/boot-riscv.sh` | 在两种内存配置下验证启动链 |

## 入口契约与不变量

- OpenSBI 进入 `_start` 时，`a0` 是 boot hart ID，`a1` 是 DTB 的 guest 物理地址。入口立即保存二者，建立环境后再按 C ABI 传给 `kernel_main`。
- 链接脚本把 `_start` 和第一个 `PT_LOAD` 放在 guest 物理地址 `0x80200000`。该地址位于 QEMU `virt` 从 `0x80000000` 开始的 RAM 中；当前分页关闭，代码使用恒等地址。
- `_start` 不继承固件栈。它初始化 `gp`、关闭 S-mode 中断、将 `__bss_start..__bss_end` 清零，再使用 BSS 末尾 4 KiB、16 字节对齐的单 hart 启动栈。
- 启动栈可用后，`_start` 把 `riscv_trap_entry` 写入 Direct-mode `stvec`，因此第一个 C 调用开始受致命 trap 诊断路径保护。
- DTB 地址不是常量。`kernel_main` 读取第一段 RAM 和静态保留区，再加入 `[__kernel_start, __kernel_end)` 与 DTB 自身范围；只有形成非空可用区间后才报告启动布局。
- 启动布局成功后，`kernel_main` 以 RISC-V 构建期固定的 4 KiB 页粒度初始化物理页分配器；不足一页的区间边缘不会进入分配器。
- UART 基址 `0x10000000` 是 QEMU `virt` 的 guest MMIO 地址，不是 RAM 或宿主 I/O 端口。发送路径轮询 LSR bit 5，再向 THR 写一个字节。
- 关机使用 SBI System Reset 扩展：`a7=0x53525354`、`a6=0`、`a0=0`、`a1=0` 后执行 `ecall`。调用若返回则输出失败信息并停在 `wfi`，不会报告假成功。

## 当前限制

当前代码只处理单 hart、固定 QEMU `virt` UART、直接物理地址、不可恢复的致命 trap、第一段 DTB 物理内存和静态保留区，以及最小单页分配。它尚未处理中断、分页、用户态 trap 或 LoongArch64；额外挂载的 VirtIO 块设备和网卡尚未访问。

## 验证入口

```sh
make run-riscv
make test-riscv
make debug-riscv
```

`make test-riscv` 先分别运行 DTB/布局与物理页分配聚焦测试，再在 512 MiB 与 1 GiB 下各启动一次正常内核，要求 hart ID 为 0、DTB 地址随 RAM 大小变化、解析出的基址恒为 `0x80000000`、大小分别为 `0x20000000` 与 `0x40000000`、启动布局非空，且 QEMU 经 SBI 正常退出。`make debug-riscv` 会在第一条 guest 指令前暂停，并在宿主 TCP 端口 1234 等待 GDB。
