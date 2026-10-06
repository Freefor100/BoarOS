# LA64 首阶段：平台、页表与真实用户态

2026-10-06 的交付范围是 QEMU virt/LA464 单核、16 KiB/三级页表与整数 ELF。
它用第二架构验证共用 MM、ELF、调度和 syscall 的边界；PCI 根盘、musl、动态
加载、用户信号 handler、浮点/SIMD、实板和 SMP 都没有由本轮结果证明。
模块入口和完整重建命令见[LA 首阶段](../modules/loongarch-boot.md)。

## 固定输入

| 输入 | 身份及核对路径 |
|---|---|
| `references/qemu` | v11.1.0，`84f07211cc5b4fc6a371559bf8a5de4fb068e648`；`hw/loongarch/boot.c`、`virt.c`、`include/hw/loongarch/virt.h`、`target/loongarch/tcg/tlb_helper.c` |
| `references/linux` | Linux 7.2，`f4cdf7ca9a1fdcca413157df19753f388a5a224e`；`arch/loongarch/include/asm/{pgtable,pgtable-bits,loongarch}.h`、`mm/tlb.c`、`mm/tlbex.S`、`kernel/entry.S`、`kernel/traps.c`、`kernel/cpu-probe.c` 与通用 syscall 号码 |
| `references/loongarch-documentation` | `e0d6592229d9e00e512bd28b688a7f20b171714f`；卷一 CSR、异常返回、DMW、页表遍历与 INVTLB |
| `references/loongarch/` 卷一 PDF | v1.11，文件名和 SHA-256 归 `references/sources.tsv`；不把 QEMU 具体平台参数归为 ISA 唯一要求 |
| `references/musl/musl-1.2.5.tar.gz` | 固定 SHA-256 归 `sources.tsv`；`arch/loongarch64/syscall_arch.h` 核对 syscall inline-asm clobbers |
| LA GCC | `/opt/loongarch64-tools/bin/loongarch64-unknown-linux-gnu-gcc` 15.1.0，二进制 SHA-256 `93ffc1acbb540affea06c1c986a1f3869a48383a7fba849710cfe20e8fdd4cd0` |

QEMU 用宿主 GCC 16.2.1 构建，仅启用 loongarch64-softmmu；Linux 使用固定
源码、LA defconfig、`CONFIG_16KB_3LEVEL=y` 和 initramfs。准备脚本检查 reference
HEAD 与洁净状态，缓存记录 compiler 路径/版本/哈希和配置 profile；版本变化不复用旧身份。
构建选项是 `-march=loongarch64 -mabi=lp64s -msoft-float -mno-lsx -mno-lasx`。

## 为什么这些边界必须拆开

RV 的 `satp`、trap frame 与取指同步不能成为通用层字段。共用策略现在使用
构建期选择的地址空间 context、MMU、IRQ、timer 和用户寄存器操作。VMA、缺页
来源、驻留记录、COW 与清理引用留在共用 MM；LA 只实现 PTE、TLB、CSR 和异常。
ELF 布局、栈和 auxv 也只有一份策略，machine/HWCAP/trampoline 由架构选定。

内存 ELF 不是绕过文件解析的另一路装载器。source 复制 reader 描述符，backing
由调用者保证不可变且存活至最后引用。FILE run 在没有 OFD 时仍需分配并精确复制
内容；只有 ZERO run 才能作为零页。fixture 的整页常量和跨页 BSS 检查覆盖此区别。
文件 source 继续借用 page cache，再按 COW 取得独立 MM owner。

启动的 EFI/FDT、RAM bank、UART 和关机设备归 QEMU 平台，不复用 RV SBI/PLIC
常量。QEMU RAM 可能不连续，必须先记录全部 bank 并扣掉保留区；物理分配器才能
接管。临时低地址 DMW 在跳到高地址后撤销，保留的 cached/uncached DMW 仅 PLV0。

16 KiB 还暴露了 slab 的隐含 4 KiB 假设：固定四个 bitmap word 无法覆盖所有
16 字节 slot。新测试先在第 256 个附近出现 owner 问题，再将 bitmap word 数
从页大小推导；300 个独立小对象及完整回收通过，RV 的计算结果仍为四个 word。

## TLB、异常与 owner

16 KiB 基页、每级 11 位索引的位移为 14/25/36，用户 VA 上限按固定 Linux 为
2^47。PWCL/PWCH 与 STLBPS 对应此布局，PGDL 保存用户根。首阶段 ASID 固定为零，
地址空间切换清全部 TLB；撤页逐地址失效后释放 owner。refill 在目录缺失时发布
无效 paired entry，由普通异常解析 VMA，不能把物理零当成下一层页表。

软件 COW 位必须独立于硬件可访问状态。`mprotect(PROT_NONE)` 后再恢复 RW 仍应
保留 COW，不能提前给共享私有页 writable PTE。该恢复测试先失败再修复，之后
父子内容隔离和页/目录引用回收通过。QEMU 单核结果不证明实板弱序、DMA 或 SMP。

正常 syscall 的 a0 返回值和 ERA+4、timer 的原 PC、用户 SP/TP 和保存寄存器
分别测试。KS0 交换 TP，用户 trap 切到任务内核栈；任务结束先切离，再由可信
idle 栈回收。用户非法地址/权限产生用户信号，合法缺页 OOM 是资源退出，内部
页表 owner 损坏 fatal。审查新增的缺页 OOM 测试先得到 SIGSEGV，再修正为
RESOURCE/NO_MEMORY；对应页基线与 heap live 验证保留。

首阶段 UART 没有外部 IRQ；已有阻塞 console read/ppoll 需要 timer 轮询唤醒。
测试等待真实 ready marker，再延迟输入：缺少轮询时可重复超时，加上唤醒后
两种 RAM 和固定 Linux 均完成。此处没有启用假设备 IRQ 或用主动 yield 掩盖等待。

## 对照与证据边界

同一个独立 LA ELF SHA-256 为
`b5037da9de36dfbd685523784866132e26d5b5daa3f24ccf084ae631f92ef827`。
9 组包含 syscall、timer 双用户计算、exit_group、RO/PROT_NONE/NX/unmapped/
kernel 访问及 console。返回值、进程关系、时间下界、跨页内容由 ELF 内部判定；
host 比较正常退出/信号结果，不比较偶然的 PID、地址和时刻。

首次 Linux 对照失败暴露的是测试 syscall 汇编缺少 t0–t8 clobber，编译器错误
地把跨 syscall 的临时值留在这些寄存器中。按固定 musl 修正后，同一重编译 ELF
在双方通过；不能把这次失败记为内核 ABI 修复。寄存器抢占探针只调用 RDTIME，
独立验证 timer 保存，避免把 syscall 合法 clobber 误当成上下文损坏。

额外的 16 个 ELF 构造 OOM 边界、任务/栈创建 OOM 和缺页 OOM 是 BoarOS 机制测试，
不伪装为 Linux 差分。所有基本 case 回到预热页数，最终 fixture heap live 为零；
13 个任务栈只在不运行后扫描并释放，最小余量 31,016 字节。非法/重复页释放和页表 owner 损坏另在两种 RAM 各自独立启动中进入 fatal。
canary/高水位和编译器单帧门禁互补，但 LA 尚无虚拟 guard page。

本轮 RV 完整架构测试、真实 musl、固定 glibc 五种形态、1,344 条 ABI 差分及
SQLite DELETE/WAL/重启恢复回归通过。它们保护共用层的既有行为，不构成 LA
根盘/libc 的验收。LA 用户 handler、FP/SIMD、实板、SMP 和完整比赛仍待后续证据。

## 第二阶段的 VirtIO 边界

远程网络预算修复 ae2a525 已合入本地主线，当前无覆盖宏的8/4/2及27候选门禁
分别验证；12个selftest在本机原环境通过。本阶段沿已选路线提取共用VirtIO块
核心，保留RV transport与旧入口，PCI布局和IRQ事实单独实现。

真实RV块fixture复现了新的回调表陷阱：静态const表保存链接高VA，在尚无高地址
映射的物理入口调用会产生instruction-access fault。改为从当前PC初始化MMIO
回调，再在合法映射阶段发布设备；同一真实fixture恢复，生产根与四组合睡眠I/O
均通过。宿主wire模型无法检出物理别名与链接VA的区别，必须保留QEMU启动证据。
