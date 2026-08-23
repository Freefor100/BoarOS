# RISC-V 启动模块

本文描述当前代码的稳定事实。构建、地址空间、固件交接和项目选择的解释见 [RISC-V 启动学习总结](../learning/riscv-boot.md)，异常入口与返回契约见 [RISC-V Trap 模块](riscv-trap.md)，启动内存边界见 [DTB 与启动内存布局模块](dtb-memory.md)，分页切换见 [RISC-V Sv39 分页模块](riscv-sv39.md)，启动后的 timer/idle 契约见 [RISC-V Timer 与内核 Tick 模块](riscv-timer.md)。

## 范围与入口

`make all` 使用 RISC-V bare-metal GCC 生成根目录 ELF `kernel-rv`。QEMU `virt` 通过 `-kernel` 装载 ELF，默认 OpenSBI 初始化机器后以 S-mode 跳到 `_start`。

| 文件 | 当前职责 |
|---|---|
| `Makefile` | 编译、链接以及运行、测试、调试入口 |
| `arch/riscv/linker.ld` | ELF 入口、加载段、地址布局和启动栈 |
| `arch/riscv/boot.S` | 建立最小 C 运行环境、初始化 trap CSR、调用 `kernel_main` 并完成高半区执行上下文切换 |
| `kernel/dtb.c`、`kernel/boot_memory.c` | 读取 DTB 内存事实并生成可用物理区间 |
| `kernel/physical_page.c` | 对齐可用区间并建立单页物理分配器 |
| `arch/riscv/direct_map.c` | 校验 direct-map 范围并转换物理/虚拟地址 |
| `arch/riscv/sv39.c` | 建立 2 MiB/4 KiB 叶子并启用 Sv39 |
| `kernel/main.c` | 建立启动内存布局、物理页分配器和启动地址空间，启动 timer 后进入 `wfi` |
| `arch/riscv/virt_uart.c` | QEMU `virt` NS16550A 轮询输出 |
| `arch/riscv/sbi.c` | 通过 SBI Base/TIME/SRST 探测扩展、设置 timer 和请求关机 |
| `tests/boot-riscv.sh`、`tests/idle-riscv.sh`、`tests/high-half-trap-riscv.sh`、`tests/no-identity-riscv.sh` | 验证 ELF/运行时地址契约、有限测试启动、正常 idle、高半区 trap 和最终低 RAM 失效 |

## 入口契约与不变量

- OpenSBI 进入 `_start` 时，`a0` 是 boot hart ID，`a1` 是 DTB 的 guest 物理地址。入口立即保存二者，建立环境后再按 C ABI 传给 `kernel_main`。
- 链接脚本把内核 VMA 放在 `0xffffffff80000000`，同时把第一个 `PT_LOAD` 的物理地址和 ELF 入口设为 `0x80200000`。该物理地址位于 QEMU `virt` 从 `0x80000000` 开始的 RAM 中；分页关闭时，`-mcmodel=medany` 生成的 PC 相对代码在低物理装载地址执行。
- `_start` 不继承固件栈。它初始化 `gp`、关闭 S-mode 中断、将 `__bss_start..__bss_end` 清零，再使用 BSS 末尾 4 KiB、16 字节对齐的单 hart 启动栈。
- 启动栈可用后，`_start` 将 `sscratch` 清零并把 `riscv_trap_entry` 写入 Direct-mode `stvec`。入口可以在当前内核栈保存完整整数 Frame 并执行 `sret`；生产 dispatcher 正式处理 timer interrupt，其他未处理 trap 仍进入致命诊断。
- DTB 地址不是常量。`kernel_main` 读取第一段 RAM 和静态保留区，再加入 `[__kernel_start, __kernel_end)` 与 DTB 自身范围；只有形成非空可用区间后才报告启动布局。
- 启动布局成功后，`kernel_main` 以 RISC-V 构建期固定的 4 KiB 页粒度初始化物理页分配器；不足一页的区间边缘不会进入分配器。
- `kernel_main` 在 Bare 状态构建两张页表。过渡页表从内核镜像内 5 个静态 4 KiB 页取得页表页，把覆盖镜像的 2 MiB 对齐物理包络同时映射到低地址和高半区，并精确映射 QEMU UART；两组镜像叶子只在中断关闭的切换窗口内临时使用 RWX。最终页表从正式物理页分配器取得页表页，只建立严格权限的高半区内核、固定偏移且全局 NX 的 RAM direct map，以及独立 UART MMIO 映射。
- 第一次写入 `satp.MODE=8` 后，过渡页表保证低地址返回路径和当前栈仍可访问；`riscv_relocate_to_high` 再把 `ra`、`sp`、`stvec` 加上固定偏移，跳到高地址代码并重新建立高地址 `gp`。高半区 continuation 第二次切换 `satp` 到最终页表，此后低地址 RAM 不可访问；随后绑定高地址物理页访问函数并修正最终页表对象的 allocator 指针，物理页回收节点由此只通过 direct map 访问。
- 过渡页表页属于内核静态镜像，不进入正式物理页分配器；正常启动日志中的最终页表数必须等于正式物理页总数与可用页数之差。
- 最终页表和 direct-map 访问路径验证完成前保持中断关闭。随后从 DTB timebase 启动 SBI timer，先设置未来 deadline，再开启 STIE 和 SIE；成功路径永久执行 `wfi`，不再以关机表示启动完成。
- UART 基址 `0x10000000` 是 QEMU `virt` 的 guest MMIO 地址，不是 RAM 或宿主 I/O 端口。发送路径轮询 LSR bit 5，再向 THR 写一个字节。
- 关机使用 SBI System Reset 扩展：`a7=0x53525354`、`a6=0`、`a0=0`、`a1=0` 后执行 `ecall`。调用若返回则输出失败信息并停在 `wfi`，不会报告假成功。

## 当前限制

当前代码只处理单 hart、固定 QEMU `virt` UART、固定高半区内核 VMA、最终 high/direct RAM 映射、S-mode 整数 Trap Frame/返回、第一段 DTB 物理内存和静态保留区、SBI timer/100 Hz tick，以及最小单页分配。QEMU ELF 的物理装载地址仍固定为 `0x80200000`；VisionFive 2 的装载地址和固件入口必须在板级适配时单独提供，不能直接沿用该平台常量。除 timer 外的生产中断、用户态 trap、调度、运行期映射修改或 LoongArch64 尚未实现；额外挂载的 VirtIO 块设备和网卡尚未访问。

## 验证入口

```sh
make run-riscv
make test-riscv
make test-sv39-riscv
make test-sv39-fault-riscv
make test-trap-return-riscv
make test-timer-riscv
make test-idle-riscv
make test-high-half-trap-riscv
make test-no-identity-riscv
make debug-riscv
```

`make test-riscv` 依次运行 DTB/布局、物理页分配、direct-map 转换、Sv39 建表、只读权限故障、Trap Frame 返回和 timer 聚焦测试。完整启动使用只包装 tick 的有限测试 ELF，在 512 MiB、1 GiB 与 16 GiB 下各启动一次；`readelf` 要求 ELF 入口为 `0x80200000`，所有 `PT_LOAD` 都采用高 VMA、低物理装载地址且首段为 `0xffffffff80000000 -> 0x80200000`。运行时要求 hart ID 为 0、DTB 地址随 RAM 大小变化、解析出的基址恒为 `0x80000000`、大小分别为 `0x20000000`、`0x40000000` 与 `0x400000000`、timebase 为 10 MHz、`satp.MODE=8`、direct map 能写读并释放复用物理页、页表计数一致，并用 ELF 符号精确验证 PC、SP、GP 和 `stvec`。测试 ELF 在真实 timer trap 至少两次返回后关机；另一个 runner 要求未包装的 `kernel-rv` 持续 idle。高半区 trap 与 no-identity 测试在最终地址空间即将启动 timer 的边界注入原探针，不依赖生产成功路径关机。`make debug-riscv` 会在第一条 guest 指令前暂停，并在宿主 TCP 端口 1234 等待 GDB。
