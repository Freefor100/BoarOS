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

真实PCI阶段在512MiB/1GiB通过两盘共享INTx、8槽DMA、读写/flush和资源回收。
两处新中断时序分别有RED→GREEN：共享level源在设备ISR快照间保持高电平时，
仅清EXTIOI快照会漏掉迟到完成，必须mask/unmask PCH-PIC以重新采样intirr；
ESTAT保留被mask的pending位，timer入口要与ECFG相交，不能分发未启用来源。
宿主PIC/OR模型强制前一时序，QEMU两个共享pin的实际盘及暂时mask场景验证组合。
PCI capability范围与BAR资源不足测试只证明解析和回滚，不替代真实DMA/IRQ证据。

### libc 暴露的 ABI 边界

固定 musl1.2.5 的 `src/thread/loongarch64/clone.s` 将 child_tid 放 a3、TLS 放 a4。
原 LA trap 沿用 RV 顺序，真实 fork 子任务没有收到 `CLONE_CHILD_SETTID` 写入；
父任务等待后观察非零子退出状态。先加入同 ELF 的子 TID 检查，再重排 LA 参数，
512MiB/1GiB 的 Linux/BoarOS均通过。PARENT_SETTID 的普通 fork 组合在当前内核
尚未接入；验证不拿该未支持组合代替 child-TID 的复现。

LA musl 的 `fstat` 首次真实根盘运行返回 ENOSYS，因为该架构通过 `statx(291)`
查询。新增接口复用已有 inode/path/fd owner，只序列化 Linux 256 字节 BASIC_STATS。
22条 RV/Linux差分同时保护组合错误优先级与跨页 EFAULT；猜测的“坏指针应先于
非法 mask”被固定 Linux 的实际结果否定，保留验证所得行为，不凭源码印象改错序。

### LP64S libc 工具与根盘 owner

系统 GCC15.1.0 的 multilib 只有 LP64D。`-mabi=lp64s` 只改变当前编译对象，静态
musl 的 crtbegin/libgcc TF helpers 仍报 ABI mismatch。采用同版本官方 archive
（来源/哈希在清单）构建 gnusf 目标、`--with-fpu=none --with-simd=none` 与匹配
runtime，不忽略 linker 错误或扩大 FPU 范围。GCC15 的宿主 C++ 选择需要固定
`-std=gnu++14 -fno-char8_t`；libgcc pthread 声明使用固定 musl headers bootstrap。
musl wrapper 必须通过 REALGCC 选中缓存 gnusf driver，不能相信其默认系统 GCC。

原 BusyBox 使用固定比赛 commit 的完整配置；较新 Linux 移除 CBQ，沿用固定
v6.6 UAPI 的 LA 导出。`headers` 先生成并清洗完整 UAPI，原样复制到安装目录，
不依赖宿主 rsync、不拼接 glibc 头或修改 BusyBox。工具输入、配置与输出身份由
`tests/loongarch/prepare_userland.py` 记录；相同静态 ELF 在两种内核 PCI ext4 上实际执行。

根启动构造先在可信永久 cleanup 任务中准备 image/files/fs，再创建cache/journal
worker，最后发布PID1。worker OOM先在真实QEMU复现fatal，再改成真实errno和完整
回滚；这样worker的部分构造可stop/join，不从idle尝试睡眠清理。永久cleanup栈在
baseline之前分配，可停止的任务/栈、用户页、cache和设备队列都必须回到该基线。
真实PCI/NBD write EIO复现了无界卸载重试；现在保留失败mount/cache/device owner，
清理三次后明确报告停止。故障路径不打印“owners released”，正常/OOM/userfault
路径则要求页、堆和BAR claim完整回收，不能把两类证据混为一次成功。

### 冷构建与缓存反例

独立审查在空配置目录复现：GCC顶层configure只生成顶层Makefile，提前读取
`gcc/Makefile`会失败。工具准备改成先执行`configure-gcc`再检查子配置，验证从
无旧对象/安装产物的缓存完成GCC、runtime、musl、完整BusyBox构建。

原缓存快速路径漏读BusyBox/UAPI选择，且只记录8个产物。先用真实main入口的
9个独立反例复现“不应verified却返回成功”，再将输入集合前移并覆盖安装树。
清单同时记录 helper/specs/headers/CRT/库、文件权限和符号链接目标，输入或产物
变更均拒绝复用。测试中的合法缓存先命中，防止所有输入都失败的假保护。
旧格式不会自动承认为新身份；本轮旧缓存先核对原产物再保存旧stamp，重新执行
构建后生成新清单。此修复只涉及构建与身份契约，不增加LA运行期能力。

冷链还复现了既有 wrapper 文件掩盖的问题：headers bootstrap 时没有 runtime/
stdio，musl auto 检测会禁用 GNU wrapper，完整 BusyBox 随后找不到 musl-gcc。
配置显式选择 `--enable-gcc-wrapper`，libgcc 安装后重新 configure，再编译 libc；
该选项也纳入缓存输入。冷目录从原始归档/空产物开始，不复用旧 wrapper 通过验收。

### LA 整数信号 ABI（2026-10-06）

固定 `references/linux` commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e` 的
`arch/loongarch/kernel/signal.c`、UAPI `sigcontext.h/ucontext.h` 和 `traps.c`
给出592字节整数帧：128字节siginfo、448字节ucontext、16字节END。musl1.2.5
（清单SHA-256 `a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4`）
的真实ucontext静态断言及同ELF双侧运行独立保护布局。LA非法指令的Linux来源为
SI_KERNEL，而RV为ILL_ILLOPC；最初探针在Linux失败后按实际架构事实修正。

Linux两种RAM先通过，BoarOS在rt_sigaction返回ENOSYS复现缺口。接入LA后端后，
相同LP64S程序完成handler、嵌套、fault修复和sigreturn；通用pending/mask/默认
动作/重启保持唯一实现。先决定EINTR或重试再构帧，sigreturn恢复PC而不推进4字节；
用户上下文不能输入PRMD或内核TP。所有恢复输入先快照，未知扩展作为用户坏帧
终止全组。FP/SIMD扩展仍缺实际owner，不能据整数帧通过宣称完整LA信号ABI。

### 真实静态 pthread 与跨内核测试边界

LP64S musl1.2.5 使用真实clone、TP、TLS、futex与SIGCANCEL；LA后端接入整数
信号后，原同步/取消/robust/生命周期函数即可作为第二架构消费者。动态DSO和
网络仍缺平台路径，选择集公开列出范围，不删改原RV入口来宣称全量通过。
固定Linux的 `kernel/exit.c` 会先futex唤醒clear-child-tid，再完成PID摘除；
musl `src/thread/pthread_create.c`、`pthread_join.c` 的线程列表同步也不保证调用者
下一次查询立即ESRCH。原测试的失败先在Linux复现，改为有界观察PID实际消失。
WAKE之后另一个任务可以在tgkill前结束，因此信号边界测试保持目标存活到handler
完成，再join；完成的WAIT仍必须返回成功，不能以新等待覆盖已完成结果。

Linux支持PI futex，BoarOS仍未支持；已有PI marker的“不修改普通word”检查是
BoarOS未支持能力边界，不能与Linux作为同一已支持语义比较。本LA静态对照只
选择非PIrobust案例，原RV负向门禁继续保留。任务/栈constructor OOM另用真实
物理页失败注入验证，精确页回滚和EAGAIN/重试不依赖Linux内存大小或偶然PID。
