# RISC-V 用户态与系统调用学习总结

本文整理从内核线程走到可执行 U-mode 任务所需的特权级、地址空间、现场切换、系统调用和资源所有权知识，并记录 BoarOS 当前已经验证的选择。稳定接口和限制以 [RISC-V Trap 模块](../modules/riscv-trap.md)、[RISC-V Sv39 分页模块](../modules/riscv-sv39.md)、[内核 MM 模块](../modules/kernel-mm.md)、[用户内存访问模块](../modules/kernel-uaccess.md)、[进程文件资源模块](../modules/kernel-files.md)、[内核任务调度模块](../modules/kernel-scheduler.md)和 [系统调用解码模块](../modules/kernel-syscall.md)为准。

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

## MM、任务与线程组为什么要分开

任务表示一条可调度执行流，拥有内核栈、Trap Frame、switch context 和 TID；线程组用 TGID 表示 Linux 用户看到的进程身份。MM 只表示虚拟地址空间，文件表、当前目录、凭据和信号处理方式还有各自的共享规则。把这些都塞进一个“最小进程”在一进程一线程时可以运行，却会把 `clone/fork/exec/wait` 需要独立变化的生命周期锁死。

BoarOS 的通用 `kernel_mm` 是可 acquire/move/release 的引用，RISC-V 用一个 4 KiB 记录页保存引用计数和唯一 Sv39 owner。生产 scheduler task 还持有一张 fd 表和一个 fs context，以及自己的 TID 和组首关系，并缓存切换所需 `satp`。这些资源有独立对象和借用接口；切换本身直接使用缓存，不在 tick 路径解析记录页或执行 files/fs 生命周期。当前测试中的两个独立线程组已共享一个 MM，未来更换 LoongArch 后端也不改变 RISC-V switch context 汇编前缀。

所有权转移必须是成功才发生：ELF 装载器先产出 LIVE Sv39 地址空间，MM 创建成功后把它变成 MOVED，用户任务创建成功后再把 MM 句柄变成 MOVED。共享时 `acquire` 只增加引用，最后一份 `release` 才销毁页表树。物理页和堆释放遵循 fail-stop 契约，非法 owner、引用或 allocator metadata 直接 fatal，不把释放失败包装成跨层 CLEANUP；真实 VFS/block I/O 错误才由所属 owner 延后处理。状态化 owner 仍用于表达确有外部资源未完成的生命周期。

## Linux 风格 RISC-V 系统调用 ABI

RISC-V Linux 用户 ABI 用 `a7` 传系统调用号，`a0..a5` 传最多六个参数，返回值放在 `a0`。用户执行 `ECALL` 后，`sepc` 指向 `ECALL` 本身；若系统调用要返回用户代码，内核必须把 `sepc` 前移 4 字节，否则会再次执行同一条指令。

BoarOS 当前实现 `openat(56)`、`close(57)`、`pipe2(59)`、`dup(23)/dup3(24)/fcntl(25)`、`lseek(62)`、`read(63)/readv(65)`、`write(64)/writev(66)`、`getdents64(61)`、`fstat(80)`、`newfstatat(79)`、`exit(93)`、`exit_group(94)`、`set_tid_address(96)`、`restart_syscall(128)`、`kill(129)/tkill(130)/tgkill(131)`、`rt_sigsuspend(133)`、`rt_sigaction(134)`、`rt_sigprocmask(135)`、`rt_sigpending(136)`、`rt_sigreturn(139)`、`uname(160)`、`getpid(172)`、`getppid(173)`、`gettid(178)`、`brk(214)`、普通进程与线程子集 `clone(220)`、`execve(221)`、`mmap(222)`、`munmap(215)`、`mprotect(226)` 和 `wait4(260)`。文件调用从 current task 借用 files/fs/MM，并返回 fd、字节数或负 Linux errno；`uname` 使用 Linux 六个 65 字节字段、总计 390 字节的 `new_utsname`，用户目标无效时返回 `-EFAULT`；getpid/gettid/getppid 分别观察线程组、任务和父进程身份。raw `brk` 返回调整后的精确地址或拒绝时的原地址，不能套用 libc 的 0/-1 包装语义。clone 是特殊的双返回系统调用，架构层必须复制 syscall 入口 Trap Frame，并分别设置父子 `a0`；wait 可能在内核中阻塞并在唤醒后才完成同一次 ecall。未知调用返回 `-ENOSYS`（错误号 38），普通返回路径前移 `sepc` 后继续执行；信号 handler 返回通过固定 VDSO ecall stub 恢复。

系统调用解码与架构 Trap 分开：Trap 层负责寄存器、`sepc`、显式 current task、signal return 和 restart；通用解码层负责编号、参数和结果语义。普通调用返回时推进旧 `sepc` 并写 `a0`；被 signal 打断的 interruptible syscall 暂存 `ERESTARTSYS`，公共返回尾按 handler 的 `SA_RESTART` 决定跳过 ecall 或重新执行；exec 成功则不能返回旧指令流，而要由 scheduler 安装新地址空间和全新 Trap Frame。普通 clone 已建立父子、zombie/wait、stop/continue 和 reparent 生命周期；线程子集已验证共享资源、pthread create/join、组长先退、非组长 exec 与 exit_group，完整线程组矩阵仍有限。

## Exec 为什么需要两阶段提交

`execve` 替换的是当前任务的程序映像，不是创建一个新 PID。路径查找、用户字符串复制、ELF 校验和新页表分配都可能失败，所以它们必须在旧 MM 仍活动时完成；失败可以释放临时 owner，并让旧程序从 `ECALL` 后继续运行。新入口、栈和 `satp` 全部验证后，scheduler 才切换地址空间。切换完成后旧用户指针和旧 PC 已经失效，这就是 point of no return；若真实 VFS/block I/O 清理失败，只能保留对应 owner 继续处理，不能再返回旧程序。

成功 exec 保留 task、TID/TGID、cwd、文件表以及没有 `FD_CLOEXEC` 的打开文件描述；被标记的 fd 在提交后关闭。Trap Frame 必须整体重建，而不是只改 `sepc/sp`：否则旧参数寄存器、callee-saved 寄存器或用户 `tp` 会泄漏到新程序。BoarOS 当前把除新 PC、SP、架构指定 `tp` 和 Trap 返回元数据外的整数现场清零。线程组 exec 的成员收拢和共享 MM 协调已有生产入口验证，完整 Linux 生命周期矩阵仍有限。

## 内核为什么不能直接解引用用户指针

系统调用参数只是用户虚拟地址。即使当前用户页表正在 `satp` 中，S-mode 对带 `PTE_U` 页的访问仍受架构特权控制；RISC-V 通常需要临时设置 `sstatus.SUM`。地址还可能未映射、权限错误或在复制中途跨页，若内核普通 load/store 直接触发 page fault，而 Trap 只会把 S-mode fault 当作 fatal，就会把一个应返回 `-EFAULT` 的用户错误升级成内核崩溃。

Linux RISC-V 的高性能 usercopy 会先验证用户范围，设置 SUM 后直接访问用户虚拟地址，并给可能故障的汇编指令登记 exception table。S-mode page fault 到来时，Trap 根据故障 PC 查表，把 `sepc` 修正到 fixup 路径，最终返回尚未复制的字节数。硬件 TLB 负责地址翻译，适合大缓冲区；代价是 SUM 开关纪律、异常表链接布局、汇编复制循环和 fault fixup 都必须完整正确。

BoarOS 当前逐基页调用 `kernel_mm_lookup()`，按复制方向检查 `USER|READ` 或 `USER|WRITE` 后通过高半区 direct map 访问物理页。每个片段不跨页，固定范围越界在复制前失败，后续页故障则保留已经复制的连续前缀；有界字符串读取还会区分 NUL、容量耗尽和用户 fault。该路径不会产生可恢复的 S-mode fault，也不需要修改 SUM。`uname` 是固定 390 字节，文件 `read` 已用 4 KiB staging chunk 处理更大缓冲区；每个涉及页仍执行一次三级软件遍历，目前没有开发板吞吐数据，不能把 QEMU 正确性测试当作性能结论。

软件遍历是当前正确性路径，不是 syscall ABI 的组成部分。公共 uaccess 接口只表达 MM、用户地址、内核缓冲区、长度和已复制前缀；将来可以在 RISC-V 内部为当前活动 MM 增加 SUM+异常表快路径，在 LoongArch 使用其架构机制，而 `uname`、VFS 和错误码语义保持不变。SMP、COW 或运行期 unmap 出现后，还必须用 MM 读锁或页固定保证“查到映射”和“完成复制”之间的物理页生命周期。

`uname.sysname` 报告 `Linux`，表示内核选择 Linux 用户 ABI personality，不表示内部实现来自 Linux 源码。`release` 是操作系统自身的发行标识，不是 syscall、文件系统或并发能力的位图；但现实程序可能把它当成内核能力的近似信号，所以借用一个尚未达到的 Linux 版本会误导用户态兼容路径。BoarOS 当前报告自身开发版本 `0.1.0-boaros-dev`，以后随项目发布状态升级；Linux ABI 兼容程度由明确的行为测试和兼容性矩阵表达，不由 release 字符串代替。机器字段由架构构建选择为 `riscv64`，以后 LoongArch 使用 `loongarch64`。这些字符串与 390 字节结构布局都属于用户可观察 ABI；主机名和 UTS namespace 可在以后改成受锁保护的动态快照。

## 用户故障与内核故障必须分开

`sstatus.SPP` 能区分 trap 来源。U-mode page fault 先按当前 MM 的 VMA 权限和 fault policy 分类：合法匿名栈/`brk` heap 空洞补零页并重试原指令，VMA/权限不允许才记录包含 `scause/stval` 的用户故障；补页 OOM 以资源原因终止，父进程看到 wait status 9。PTE 已存在却仍发生允许访问的 page fault、页表状态错误和未完成清理说明内核不变量可能破坏，不能伪装成普通用户错误。S-mode 未处理故障同样走 fatal 诊断和关机，不能套用“杀掉当前用户任务”继续运行。

这一策略要求故障任务拥有独立内核栈和有效资源 owner。调度器必须先切到其他可信栈，才能释放故障任务的任务页；用户页表也不能在 `satp` 仍指向它时销毁。BoarOS 的 idle reaper 在内核根页表和 boot stack 上处理完成队列，依次关闭 fd/open file description，释放 fs context、MM 引用、TID 和任务页。

## 当前项目选择与平台边界

BoarOS 先用手工映射探针验证首次 `SRET`、真实 timer 抢占、U-mode syscall、同步页故障、调度恢复和完整资源回收，再用独立链接的静态 ELF 验证装载器产出的代码、数据、BSS 和 Linux 形态的 `argc/argv/envp/auxv` 初始栈。生产路径进一步从 DTB 发现的 VirtIO 块设备按能力读写或只读挂载 ext4，以精确随机读构造 source-backed `ET_EXEC`/`ET_DYN`/`PT_INTERP` 映像；静态 `/init` 与动态 musl PIE、解释器、额外 DSO、TLS 已实际进入 U-mode，动态重定位由用户态链接器完成。PID 1 通过文件描述符读取同一根上的普通文件，再连续 exec 两个独立静态 ELF，并验证 `brk` heap、普通 clone/wait/reparent，最后沿 exec/files/fs/MM/TID/task 顺序释放全部资源。`make test-userland-riscv` 进一步以静态和动态 musl 程序作为 PID 1 运行 stdio、readdir、read/lseek/fstat、dup、F/D 浮点抢占、signal handler/sigreturn、可中断 nanosleep、pipe、pthread、TLS 和 dlopen，并覆盖组长先退、非组长 exec 与 exit_group；这是编译器生成用户程序入口。用户产物由 `musl-gcc` specs 提供 musl 头文件、启动对象和库搜索路径；链接命令不能再把交叉工具链的 glibc sysroot 通过 `-L` 放到它之前，否则会生成启动对象和 libc 来源混杂、但仍可能成功链接的错误 ELF。

静态 musl 启动阶段对内核的 ABI 依赖很小：`set_tid_address`（`__init_libc` 记录 clear_tid）、`brk`、`mmap`/`mprotect`、stdio 的 `write/writev`、退出时的 `exit_group`；运行 signal/pipe 测试时还会调用 `rt_sigaction/rt_sigprocmask/rt_sigreturn`、`nanosleep`/`restart_syscall` 和 `pipe2`。`AT_RANDOM` 缺失时 musl 使用固定栈保护常量，不要求熵源；`isatty` 经 `ioctl` 返回 `ENOSYS` 后按非 tty 处理（stdout 全缓冲）。这些是编译器生成用户程序与手写汇编探针的本质差异：程序依赖 libc 启动序列，而 libc 依赖一小组必须真实可用的 syscall。

Sv39、`satp`、`sscratch`、Trap Frame 和 RISC-V syscall 寄存器约定属于架构层，可在 QEMU `virt` 与 VisionFive 2 复用。SBI/固件交接、RAM 与 MMIO 布局、timebase、UART 和中断控制器仍属于平台层；QEMU 上通过 U-mode 测试不等于开发板适配已经完成。

## 验证和调试经验

- 必须让测试真实执行 `SRET` 进入 U-mode；只检查构造出的 PTE 或 Frame 不能验证硬件权限和返回状态。
- 用户程序应主动改变 `gp/sp/tp/s0..s11`，跨真实 timer 抢占后逐项核对，才能同时覆盖可信内核 `gp` 重载、换栈、Trap Frame、switch context 和恢复顺序。
- 用两个独立用户根页表分别运行正常任务与故障任务，可以验证 `satp` 切换、低半区隔离和“用户故障不杀内核”。
- ELF 集成测试应嵌入完整链接产物并经生产解析器装载；直接复制测试汇编字节只能验证 U-mode 路径，不能验证 ELF program header、BSS 或页权限物化。
- Exec 集成测试应让旧程序先填充所有 callee-saved 寄存器和 `tp`，再由新程序检查它们没有泄漏；同时用跨映像 fd offset、CLOEXEC 和 PID/TID 检查“替换映像但保留进程身份”的边界。
- 内核 worker 在用户任务被抢占后检查 `sscratch=0`，能发现入口忘记清 scratch 导致后续 S-mode trap 误判来源。
- 创建失败测试既要检查返回错误，也要检查地址空间或 MM owner 仍在调用者手中；回收测试比较开始和结束的物理空闲页数，覆盖叶子页、各级页表、MM 记录页和任务页，并验证 TID 可重新分配。非法 allocator 释放由 fatal-path 测试覆盖，真实 ext4/block I/O 失败则检查所属 owner 在后续清理机会继续处理。
- 汇编返回路径应检查最终 ELF 反汇编。符号可能被链接器拆成多个范围，只截取入口符号容易漏掉公共 return 路径。
- fatal 测试要在用户根仍为当前 `satp` 时破坏返回凭据，确认 supervisor-only 高半区 UART 能打印诊断并关机；等任务回到内核根后再测试无法发现低地址设备映射缺失。

## 资料依据

- `references/riscv/riscv-privileged-20260120.pdf`：U/S 特权转换、`sstatus`、`sscratch`、`SRET` 和 Trap CSR。
- `references/linux/arch/riscv/kernel/entry.S`：Linux RISC-V 的 `sscratch`/`tp` 换栈、用户/内核来源判断和返回路径。
- `references/linux/arch/riscv/include/asm/syscall.h`、`references/linux/arch/riscv/include/uapi/asm/unistd.h` 与通用 UAPI syscall 定义：RISC-V Linux 的参数寄存器、返回值和系统调用编号来源。
- `references/linux/arch/riscv/include/asm/uaccess.h`、`references/linux/arch/riscv/lib/uaccess.S` 和 `references/linux/arch/riscv/mm/extable.c`：SUM、复制循环、异常表与 fault fixup 的 Linux 实现依据。
- `references/linux/include/uapi/linux/utsname.h` 与 `references/linux/kernel/sys.c`：`new_utsname` 的六字段布局、`uname(160)` 复制和 `-EFAULT` 语义。
