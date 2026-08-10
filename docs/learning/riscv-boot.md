# RISC-V 启动学习记录

这份记录来自启动阶段的实际提问和调试。当前代码契约以 [RISC-V 启动模块](../modules/riscv-boot.md) 为准。

## 源码怎样变成 QEMU 中运行的内核？

**理解**

CPU 不执行 C、汇编文本或 Makefile。Make 根据依赖调用 `riscv64-unknown-elf-gcc`：C 和 `.S` 先变成 RISC-V 对象文件，链接器再按 `arch/riscv/linker.ld` 合成 `kernel-rv`。这个 ELF 同时保存机器码、`PT_LOAD` 段的 guest 装载地址、入口 `0x80200000` 和调试符号。

`qemu-system-riscv64` 模拟整台 `virt` 机器，而不是在宿主 Linux 中运行普通进程。`-kernel kernel-rv` 让 QEMU 解析 ELF 并把加载段放进 guest RAM；虚拟 CPU 仍从 reset ROM 开始，经过默认 OpenSBI 后才以 S-mode 进入 ELF 的 `_start`。

```text
C / .S -> RISC-V 对象 -> linker script -> kernel-rv ELF
                                                |
QEMU reset ROM -> OpenSBI --------------------> _start -> kernel_main
```

**证据**

```sh
make all
file kernel-rv
riscv64-unknown-elf-readelf -h -l -s kernel-rv
```

`file` 应识别 ELF64 RISC-V；`readelf` 应显示入口 `0x80200000` 和对应的加载段。`riscv64-unknown-elf-nm -u kernel-rv` 不应列出未解析符号。

## 固定地址、内存、端口分别是谁的？

**理解**

QEMU `virt` 为虚拟 CPU 建立 guest 物理地址空间。分页尚未开启，因此当前内核直接使用 guest 物理地址，但同一地址空间既包含 RAM，也包含 ROM 和 MMIO：

| 地址或位置 | 含义 | 决定者 |
|---|---|---|
| `0x1000` | reset ROM 中的复位入口 | QEMU `virt` |
| `0x80000000` 起 | guest RAM；当前 OpenSBI 位于开头 | QEMU 机型、`-m` 与固件布局 |
| `0x80200000` 起 | BoarOS ELF 加载段和 `_start` | 链接脚本选择，QEMU 按 ELF 装载，OpenSBI 按交接信息跳转 |
| `0x10000000` | NS16550A UART 的 MMIO 寄存器 | QEMU `virt` 地址图 |
| `a1` 指向的位置 | QEMU 生成的 DTB，位于 guest RAM 且会随内存布局改变 | QEMU 生成，OpenSBI 传递 |
| TCP `1234` | `-s` 创建的宿主 GDB 监听端口 | QEMU 宿主进程 |

RAM 地址指向可存放字节的虚拟内存条；ROM 是 guest 只读启动代码；MMIO 地址由 QEMU 译码后送到设备模型。宿主 TCP 端口不在 guest 地址空间，也不是 UART 的“端口”。真机上相同角色由 SoC 地址图、固件和设备树承担。

**证据**

`make debug-riscv` 后连接 GDB，可以依次观察 reset `0x1000`、OpenSBI `0x80000000` 和 `_start 0x80200000`。分别使用 512 MiB 与 1 GiB 运行时，入口保持不变，启动行中的 DTB 地址改变。

## 为什么必须先执行汇编入口？

**理解**

C 调用约定已经假设栈、`gp` 和零初始化静态数据可用，固件却只承诺 `a0/a1` 等入口信息，留下的 `sp` 不属于内核。`boot.S` 因而在不能依赖 C 环境时保存参数、初始化 `gp`、关闭尚无处理函数的中断、清零 BSS、建立 16 字节对齐的 4 KiB 栈，再调用 `kernel_main`。如果 C 入口意外返回，汇编停在 `wfi` 循环，避免执行未知地址。

**证据**

在 `_start` 与 `kernel_main` 设置断点，前者可看到 OpenSBI 传来的 `a0/a1` 和尚未建立的内核栈，后者可看到 `sp` 已落入链接脚本预留的启动栈。

## UART 字符怎样到终端，SBI 怎样关闭 QEMU？

**理解**

内核向 guest 地址 `0x10000000` 写字节时，QEMU 把 store 交给 NS16550A 模型；`-nographic` 再把该设备的字符后端连接到宿主终端或重定向日志。这条路径没有调用宿主 `printf`。

S-mode 无权直接完成所有机器级操作。内核按 SBI ABI 把扩展、函数和参数放入 `a7/a6/a0/a1` 后执行 `ecall`，CPU 陷入 M-mode 的 OpenSBI；OpenSBI 处理 SRST shutdown 请求，最终让 QEMU 进程退出。

**证据**

`make run-riscv` 会显示 OpenSBI 信息和一行 `BoarOS: booted ...`，随后返回 shell。若 SBI 调用返回，代码会打印 `BoarOS: SBI shutdown failed` 并停住，测试以超时报错。

## 为什么 `make test-riscv` 曾只打印 512M 后停住？

**现象**

在真实 Linux 终端中执行测试，只出现 `RISC-V boot test: memory=512M`，进程没有按 10 秒超时返回。

**根因**

GNU `timeout` 默认把受控命令放进独立进程组。QEMU 的 `-nographic` 使用 stdio；当这个进程组相对终端处于后台并尝试读取或调整终端时，Linux job control 会发送 `SIGTTIN` 或 `SIGTTOU`。`timeout` 与 QEMU 同组并一起停止，计时器也无法继续，所以表面上像超时失效。

**修复与证据**

自动测试不需要键盘输入，因此 QEMU 的 stdin 改接 `/dev/null`，stdout/stderr 写入单次运行日志：

```sh
... qemu-system-riscv64 ... </dev/null >"$output" 2>&1
```

修复后真实 PTY 中的 512 MiB 和 1 GiB 两个实例都会结束。`run-riscv` 与 `debug-riscv` 仍保留交互终端；前者可用 `Ctrl-a x` 手工退出，后者因 `-S -s` 等待 GDB 而保持运行。

这次调试留下的通用经验是：终端中的“卡住”不一定是 guest 死循环；先检查宿主进程状态、进程组和信号，再判断 QEMU 或内核是否仍在执行。
