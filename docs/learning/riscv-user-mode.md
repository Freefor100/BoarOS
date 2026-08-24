# RISC-V 用户态与系统调用学习总结

本文整理从内核线程走到可执行 U-mode 任务所需的特权级、地址空间、现场切换、系统调用和资源所有权知识，并记录 BoarOS 当前已经验证的选择。稳定接口和限制以 [RISC-V Trap 模块](../modules/riscv-trap.md)、[RISC-V Sv39 分页模块](../modules/riscv-sv39.md)、[内核线程调度模块](../modules/kernel-scheduler.md)和 [系统调用解码模块](../modules/kernel-syscall.md)为准。

## U-mode 解决的边界

RISC-V 的 U-mode 不能直接访问 S-mode CSR，也不能访问缺少 `U` 权限或被页表权限禁止的地址。用户代码通过 `ECALL` 主动请求内核服务；非法指令、页故障等同步异常则由硬件强制转入 S-mode。`stvec`、`sepc`、`scause`、`stval` 和 `sstatus` 描述这次边界转换，`SRET` 根据 `sstatus.SPP` 和 `sepc` 返回原执行流。

分页权限和特权级必须同时成立。把 PC 改成低地址并不等于进入用户态；只有 `SPP=0` 的 `SRET` 才进入 U-mode。反过来，处于 U-mode 也不能执行没有 `PTE_U` 的页面。代码页通常为用户可读、可执行而不可写，栈和数据页为用户可读写、不可执行。

BoarOS 的首次用户 Frame 设置 RV64 `UXL=64` 和 `SPIE=1`，保持 `SPP=0` 与 `SIE=0`。因此 `SRET` 原子进入 64 位 U-mode，并在返回完成后允许后续 supervisor 中断；中断不会在通用寄存器尚未恢复时提前进入。

## 三种执行现场为什么不能混为一体

用户任务跨越三种不同状态：

- 用户寄存器是 U-mode 程序看到的 x1..x31，其中 `sp` 指向用户栈，`tp` 可用于用户 TLS。
- Trap Frame 保存任意指令边界的完整整数现场和 trap CSR，位于该任务可信的内核栈。
- Switch context 只保存内核调用 ABI 要求的 `ra/sp/tp/s0..s11`，用于调度器暂停和恢复内核调用链。

首次进入用户态时还没有“以前留下的 trap”。Scheduler 会预构造一个 U-mode Trap Frame，再让首个 switch context 从公共 trap-return 汇编开始。以后 timer、syscall 或异常返回也经过同一恢复路径，避免维护一套只用于首次进入的长期汇编接口。

当用户态发生 trap 时，用户 `sp` 不能作为内核保存现场的地址。BoarOS 在用户执行期间令 `sscratch=current`，入口以 `sscratch <-> tp` 得到可信任务指针，保存用户 `sp` 后切到任务内核栈；内核执行期间令 `tp=current`、`sscratch=0`。用户 `gp` 也不可信：入口先保存它，再用禁止 linker relaxation 的 PC 相对序列恢复内核 `__global_pointer$`，然后才调用 C。返回用户前验证 Frame 中由入口保存的内核 `tp`，再恢复用户 `gp/sp/tp`。这套约定允许用户自由修改这些寄存器，而不会让它们成为内核指针或全局数据基址。

## 用户地址空间与内核映射

BoarOS 为每个用户任务建立独立的 Sv39 根页表。低半区包含该任务拥有的 4 KiB 用户叶子页；根表高半区借用稳定的内核根项，使 trap 后的内核代码、内核栈、direct map 和必要 MMIO 在切换地址空间后仍可访问。

“借用根项”不等于共享用户内存。每个用户根表的低半区独立，销毁时只递归释放它拥有的叶子页和页表页，不释放内核高半区。内核映射没有 `PTE_U`，U-mode 不能因为它们出现在同一根表中而访问内核。

内核在用户根活动期间仍可能需要设备，例如输出 fatal 诊断。BoarOS 因此把 QEMU UART 的最终映射放在 supervisor-only 高半区，而不是把平台 MMIO 塞进每棵私有低半区树，也不在错误路径临时切换根页表。过渡启动阶段仍使用设备物理低地址，最终根生效后一次性切到高半区别名。

当前 `satp` 使用 ASID 0，调度切换前后全局刷新 TLB。这个选择让正确性状态最少，但地址空间切换会丢弃其他翻译。以后只有在测量表明 TLB 刷新是瓶颈时，才值得加入 ASID 分配、复用代际和多 hart shootdown；这些机制不会改变用户根页表的基本所有权。

## Linux 风格 RISC-V 系统调用 ABI

RISC-V Linux 用户 ABI 用 `a7` 传系统调用号，`a0..a5` 传最多六个参数，返回值放在 `a0`。用户执行 `ECALL` 后，`sepc` 指向 `ECALL` 本身；若系统调用要返回用户代码，内核必须把 `sepc` 前移 4 字节，否则会再次执行同一条指令。

BoarOS 当前实现 `exit`，编号为 93。退出状态取参数的低 8 位并形成完成记录，任务不再恢复。未知调用返回 `-ENOSYS`（错误号 38），同时前移 `sepc` 后继续执行。系统调用解码与架构 Trap 分开：Trap 层负责寄存器和 `sepc`，通用解码层负责编号、参数和结果语义。

`exit` 只是结束当前可调度任务，不代表 BoarOS 已有完整进程。进程还需要 PID、父子关系、文件表、信号、用户内存复制、可执行文件来源与 `exec` 生命周期以及 `fork/wait` 等资源和语义；当前不以空结构伪装这些能力。

## 用户故障与内核故障必须分开

`sstatus.SPP` 能区分 trap 来源。U-mode 的非法访问是该任务的失败，BoarOS 将同步故障记录为包含 `scause/stval` 的任务完成原因，然后切走并回收它拥有的资源。S-mode 未处理故障表示内核自身不变量可能已经破坏，仍走 fatal 诊断和关机，不能套用“杀掉当前用户任务”继续运行。

这一策略要求故障任务拥有独立内核栈和用户地址空间。调度器必须先切到其他可信栈，才能释放故障任务的任务页；用户页表也不能在 `satp` 仍指向它时销毁。BoarOS 的 idle reaper 在内核根页表和 boot stack 上处理完成队列，先销毁用户地址空间，再释放任务页。

## 当前项目选择与平台边界

BoarOS 当前先用手工映射探针验证首次 `SRET`、真实 timer 抢占、U-mode syscall、同步页故障、调度恢复和完整资源回收，再用独立链接的静态 ELF 验证装载器产出的代码、数据、BSS 和 Linux 形态的 `argc/argv/envp/auxv` 初始栈能沿同一架构路径运行。已有 ELF 和参数仍来自完整的只读内核缓冲区，不等于已经具备进程/PID、文件系统 `exec` 或通用 `copy_from_user`。

Sv39、`satp`、`sscratch`、Trap Frame 和 RISC-V syscall 寄存器约定属于架构层，可在 QEMU `virt` 与 VisionFive 2 复用。SBI/固件交接、RAM 与 MMIO 布局、timebase、UART 和中断控制器仍属于平台层；QEMU 上通过 U-mode 测试不等于开发板适配已经完成。

## 验证和调试经验

- 必须让测试真实执行 `SRET` 进入 U-mode；只检查构造出的 PTE 或 Frame 不能验证硬件权限和返回状态。
- 用户程序应主动改变 `gp/sp/tp/s0..s11`，跨真实 timer 抢占后逐项核对，才能同时覆盖可信内核 `gp` 重载、换栈、Trap Frame、switch context 和恢复顺序。
- 用两个独立用户根页表分别运行正常任务与故障任务，可以验证 `satp` 切换、低半区隔离和“用户故障不杀内核”。
- ELF 集成测试应嵌入完整链接产物并经生产解析器装载；直接复制测试汇编字节只能验证 U-mode 路径，不能验证 ELF program header、BSS 或页权限物化。
- 内核 worker 在用户任务被抢占后检查 `sscratch=0`，能发现入口忘记清 scratch 导致后续 S-mode trap 误判来源。
- 创建失败测试既要检查返回错误，也要检查用户地址空间仍为调用者所有；回收测试要比较开始和结束的物理空闲页数，覆盖叶子页、各级页表和任务页。
- 汇编返回路径应检查最终 ELF 反汇编。符号可能被链接器拆成多个范围，只截取入口符号容易漏掉公共 return 路径。
- fatal 测试要在用户根仍为当前 `satp` 时破坏返回凭据，确认 supervisor-only 高半区 UART 能打印诊断并关机；等任务回到内核根后再测试无法发现低地址设备映射缺失。

## 资料依据

- `references/riscv/riscv-privileged-20260120.pdf`：U/S 特权转换、`sstatus`、`sscratch`、`SRET` 和 Trap CSR。
- `references/linux/arch/riscv/kernel/entry.S`：Linux RISC-V 的 `sscratch`/`tp` 换栈、用户/内核来源判断和返回路径。
- `references/linux/arch/riscv/include/asm/syscall.h`、`references/linux/arch/riscv/include/uapi/asm/unistd.h` 与通用 UAPI syscall 定义：RISC-V Linux 的参数寄存器、返回值和系统调用编号来源。
