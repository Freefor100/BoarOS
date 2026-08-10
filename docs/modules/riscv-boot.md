# RISC-V 启动模块

本文描述当前代码的稳定事实。构建、地址与固件交接的个人理解见 [RISC-V 启动学习记录](../learning/riscv-boot.md)。

## 范围与入口

`make all` 使用 RISC-V bare-metal GCC 生成根目录 ELF `kernel-rv`。QEMU `virt` 通过 `-kernel` 装载 ELF，默认 OpenSBI 初始化机器后以 S-mode 跳到 `_start`。

| 文件 | 当前职责 |
|---|---|
| `Makefile` | 编译、链接以及运行、测试、调试入口 |
| `arch/riscv/linker.ld` | ELF 入口、加载段、地址布局和启动栈 |
| `arch/riscv/boot.S` | 建立最小 C 运行环境并调用 `kernel_main` |
| `kernel/main.c` | 验证 DTB 交接、输出启动信息并关机 |
| `arch/riscv/virt_uart.c` | QEMU `virt` NS16550A 轮询输出 |
| `arch/riscv/sbi.c` | 通过 SBI SRST 请求关机 |
| `tests/boot-riscv.sh` | 在两种内存配置下验证启动链 |

## 入口契约与不变量

- OpenSBI 进入 `_start` 时，`a0` 是 boot hart ID，`a1` 是 DTB 的 guest 物理地址。入口立即保存二者，建立环境后再按 C ABI 传给 `kernel_main`。
- 链接脚本把 `_start` 和第一个 `PT_LOAD` 放在 guest 物理地址 `0x80200000`。该地址位于 QEMU `virt` 从 `0x80000000` 开始的 RAM 中；当前分页关闭，代码使用恒等地址。
- `_start` 不继承固件栈。它初始化 `gp`、关闭 S-mode 中断、将 `__bss_start..__bss_end` 清零，再使用 BSS 末尾 4 KiB、16 字节对齐的单 hart 启动栈。
- DTB 地址不是常量。`kernel_main` 只检查大端魔数 `d0 0d fe ed`，尚未解析设备树。
- UART 基址 `0x10000000` 是 QEMU `virt` 的 guest MMIO 地址，不是 RAM 或宿主 I/O 端口。发送路径轮询 LSR bit 5，再向 THR 写一个字节。
- 关机使用 SBI System Reset 扩展：`a7=0x53525354`、`a6=0`、`a0=0`、`a1=0` 后执行 `ecall`。调用若返回则输出失败信息并停在 `wfi`，不会报告假成功。

## 当前限制

当前代码只处理单 hart、固定 QEMU `virt` UART 和直接物理地址。它没有 trap 入口、中断、分页、内存分配、设备树解析或 LoongArch64 支持；额外挂载的 VirtIO 块设备和网卡尚未访问。

## 验证入口

```sh
make run-riscv
make test-riscv
make debug-riscv
```

`make test-riscv` 在 512 MiB 与 1 GiB 下各启动一次，要求启动行仅出现一次、hart ID 为 0、DTB 地址随 RAM 大小变化且 QEMU 经 SBI 正常退出。`make debug-riscv` 会在第一条 guest 指令前暂停，并在宿主 TCP 端口 1234 等待 GDB。
