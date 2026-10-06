# LoongArch QEMU 首阶段

当前验收范围为 QEMU virt、LA464、单核、LA64、16 KiB/三级页表及整数用户态。
入口为 `arch/loongarch/boot.S` 与 `main.c`，平台事实独立放在
`platform/loongarch_virt.c`。`kernel-la` 是内嵌独立 ELF 的首阶段验收内核，
尚无 PCI 根盘或静态 musl 用户环境；默认 `all` 和 RV 测试入口保持原路径。

```sh
make kernel-la                 # LP64S 整数内核和独立用户 ELF
make run-loongarch             # QEMU 无 BIOS 直接 ELF 启动，串口可交互
make test-loongarch-boot        # 512 MiB/1 GiB 启动与后续契约
make test-loongarch             # 同时准备固定 Linux，对照同一个用户 ELF
make test-stack-usage-la
```

`run-loongarch` 进入 console 探针后，依次在 poll/read ready 标记出现时输入 `g` 和 `r`（各回车），完成后关机。

`prepare-la-tools` 从固定 QEMU 源码构建模拟器；`prepare-la-linux` 从固定 Linux
defconfig 建立 16 KiB/三级页表、initramfs 对照。缓存分别位于 `build/qemu-la`、
`build/linux-la`、`build/loongarch`；身份变化时拒绝混用。`make prune-build`
保留工具、对象、ELF 和身份，删除运行日志及 initramfs。`QEMU_LOONGARCH64` 和
`LA_CROSS_COMPILE` 可覆盖工具位置，版本变化需要重新验收。

内核物理装载地址为2MiB，高地址为缓存DMW的`0x9000000000200000`。
开启分页前临时建立低地址DMW，跳到高地址后立即撤销；用户模式不可使用
内核PLV0的DMW。平台从直接启动传入的EFI system table定位DTB，检查签名、
表数与范围；按DTB中所有RAM bank排序，扣除启动信息、DTB、内核和保留区。
物理分配器复用通用buddy实现，页大小在构建期固定为16KiB。

架构 MMU 设置 PWCL/PWCH、STLBPS 和独立 TLB refill 入口；根目录/中间目录缺失时
refill 写入无效 paired entry，交由普通用户缺页路径处理，不能读取物理地址零。
用户 PTE 的 PLV3、NR/NX、dirty 与软件 COW/PROT_NONE 状态由 LA 后端拥有。
撤映射先失效转换再归还页；地址空间切换使用 PGDL、ASID0 和全量失效。
用户页、目录页及 COW 引用均按通用 MM owner 协议转移或回滚，所有权损坏 fatal。
内核当前使用 PLV0 DMW，LA 任务栈有 canary 和回收高水位统计，尚无虚拟 guard page。

调度器通过 `arch/task.h`、`context.h`、`timer.h` 和 `mmu.h` 构建期选择实现。
LA 保存完整整数 trap frame（304 字节）、内核 ABI context、用户 SP/TP/ERA/PRMD；
KS0 用于区分并交换用户/内核 TP。timer 使用 CPUCFG 频率、RDTIME.D 和 one-shot
TCFG，接入共用 tick、deadline、预算与调度；未启用 UART 外部 IRQ，timer 轮询
就绪输入并唤醒阻塞 read/ppoll owner。EUEN 保持关闭，FP/LSX/LASX 状态不宣称支持。

内存 reader 经共用 source、映像、MM、任务创建与 syscall dispatch 路径进入 PLV3。
LA syscall 从 a7/a0–a5 解码，返回 a0 并恢复 ERA+4。未知 syscall 返回 ENOSYS；
坏复制指针返回 EFAULT；权限/未映射故障和访问内核地址按实际用户故障处理。
合法缺页 OOM 使用 RESOURCE/NO_MEMORY 退出，不能误报为用户地址非法。
rt_sigaction/rt_sigreturn 尚无 LA handler frame，明确返回 ENOSYS。

2026-10-06 在 512 MiB 和 1 GiB 下完成：

- 内核保留区/RAM 布局、真实页写读、16 KiB slab 位图及 MMU/COW/PROT_NONE 回收；非法/重复页释放与损坏页表 owner 在两种 RAM 各自独立启动中均 fatal。
- 两个不主动 yield 的内核任务、fork 出的两个用户计算任务均经 timer 前进；整数寄存器、SP、TP 和恢复 PC 保持。
- 正常段、整页 FILE、BSS 尾页、初始栈/AT_PAGESZ、跨页复制、PID/TID、yield、时钟/睡眠、匿名 mmap/mprotect/munmap、exit/exit_group。
- 只读、PROT_NONE、NX、撤映射后访问及内核 DMW 访问；9 组同 ELF 的 Linux/BoarOS 退出契约一致。
- 损坏 header、错误 machine、重叠布局、16 个构造分配失败边界、两处任务/栈构造 OOM 和缺页 OOM；失败不发布任务，页数回到预热基线，最后 heap live 为零。
- 可信 idle 栈回收 13 个任务栈，实测最小余量 31,016 字节、最大使用 1,736 字节；编译器 2,066 条函数记录（含 fatal fixture）最大单帧 3,392 字节。单帧检查不证明完整调用链上界。

本轮 RV allocator、MM/VMA、uaccess、context、scheduler、exec 及 `test-riscv`、
真实静态/动态用户程序、五种固定 glibc 形态、1,344 条 ABI 差分、栈检查与 SQLite
DELETE/WAL 恢复回归通过。上述结果不代表 LA 完整线程、信号、libc 或比赛 Harness。
下一阶段 PCI→VirtIO 块→ext4→静态 musl 仍待人决定，SMP、实板与动态加载不在本阶段。

固定依据为`references/qemu` v11.1.0，commit
`84f07211cc5b4fc6a371559bf8a5de4fb068e648`的`hw/loongarch/boot.c`、
`include/hw/loongarch/virt.h`和FDT生成代码。工具为本机
`loongarch64-unknown-linux-gnu-gcc`15.1.0；内核使用LP64S、禁用浮点和LSX/LASX。
QEMU从该固定源码在`build/qemu-la`构建，未引入新的产品依赖。
Linux 对照 commit 为 `f4cdf7ca9a1fdcca413157df19753f388a5a224e`；CSR、PTE、
refill 和 syscall 核对路径及调试经验见[LA 学习记录](../learning/loongarch-bringup.md)。
