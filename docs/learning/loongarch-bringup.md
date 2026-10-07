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

### 信号边界的独立反例

最终只读审查用同一LP64S ELF在固定Linux与BoarOS复现两条ABI差异：RW写入
break6后mprotect(PROT_EXEC)，Linux交付SIGFPE而Boar按普通READ复制失败误报SEGV；
另将有效592字节帧移动-8字节后sigreturn，Linux接受而Boar因额外对齐检查终止。
两条已纳入 `test-signal-loongarch`：先确认RED，再分别恢复执行资格的指令解码
和Linux允许的可读帧。解码只借用当前MM的驻留USER/EXEC页，已取指指令4字节
对齐、不跨16KiB页，trap中不切换；页/映射owner损坏仍fatal，不扩大普通READ复制。

固定Linux `arch/loongarch/mm/cache.c::protection_map` 的PROT_EXEC使用可读PTE；
当时BoarOS仍提供独立NR权限，那次没有改通用VMA与数据访问策略，不据这条
break对照宣称两者所有PROT_EXEC数据访问行为等价。更广权限策略差分需另按真实
程序与既有MM契约核对。内核交付SP对齐是构帧保证，不是任意用户恢复帧的额外
拒绝条件；恢复仍有范围、读取、扩展终止和特权隔离检查。

### EXEC 页的冷/驻留差异（2026-10-06）

固定Linux commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e` 的
`arch/loongarch/mm/cache.c::protection_map` 对非NONE用户映射发布可读PTE；
`mm/fault.c` 对未驻留页的数据读取仍要求VM_READ/VM_WRITE。由此纯EXEC冷页
读取fault，但RW物化再改EXEC，或文件页经首次取指物化后，数据读取合法。
不能把这个事实简化成VMA的EXEC总是隐含READ，否则会放宽Linux拒绝的冷页。

同一LP64S ELF先在Linux512MiB/1GiB通过，BoarOS驻留EXEC读取SEGV复现差异。
LA encode按Linux表发布有效权限，VMA、fault和uaccess策略保持共用原路径；
修复后8组冷页/驻留/取指/fork/PROT_NONE/撤执行/uaccess在两侧两种RAM通过，
根页/堆owner回到基线。原LA信号、线程、MM/COW与启动回归，以及RV MM/VMA/
uaccess/files/exec和SQLite DELETE/WAL多进程与重启门禁通过。

### 从整数 ABI 到原版动态 musl

固定musl1.2.5在LP64S下的fenv.S不生成符号，但Makefile依然用架构对象替换
通用fenv，故共享libc链接缺fetestexcept/fegetenv/__fesetround；静态整数程序
没有拉入这些对象，所以之前通过不能证明共享libc可构建。通用fallback包含
成功dummy，不用于掩盖缺口。人选择A：保留整数内核，补LA标量FPU，原版
LP64D musl在固定GCC15.1.0的匹配CRT/libgcc上完整构建，无上游代码补丁。

LA没有RV的Dirty跟踪位；首用异常15决定是否曾用FP，既保留整数END帧，也
避免让所有整数任务每次保存FP。用过的task沿已有切换接口保存和恢复32FR、
8FCC和FCSR。FCC不是八个相邻bit，而是Linux每个一字节的64位编码。信号记录
FPU magic0x46505501、size288，完整帧880字节；END只需要前8字节可读。
FPU记录最小尺寸包含4字节尾部padding，但Linux恢复只读268字节有效字段。
有效字段恰好止于页尾、padding在PROT_NONE页且大size跳到后续可读END的同ELF
反例在Linux通过，原整结构复制却误触发SIGSEGV；按有效字段边界复制后通过。
Linux FP初始化FR为全1NaN，exec首次使用不能继承旧值。FPE清除Cause与Enable
相交的位，si_code却按原始Cause优先级选择。固定Linux的`force_fcsr_sig`旁旧
注释不能代替`do_fpe`完整调用链：仅启用inexact的实际overflow/underflow运算
分别报告FPE_FLTOVF/FPE_FLTUND，双侧真实指令已核对；pending sigreturn按Linux
写回并产生SI_KERNEL。

相同静态LP64D程序先在Linux通过、BoarSIGILL复现，再经过真实FR/FCC/CSR
timer、fork、signal/修改恢复、exec、FPE和扩展坏帧矩阵。原版动态PIE和非PIE
同ELF完成late-dlopen TLS与已有pthread子集；DT_NEEDED/ORIGIN RPATH的初始
DSO TLS另测，并实际写RELRO指针验证保护。库与程序在每次运行前后核对SHA。

解释器短header原先误归ELIBBAD；固定Linux `fs/binfmt_elf.c::elf_read` 在完整
elf header读取前返回EIO，同ELF先Linux通过/Boar失败。新create_interpreter
入口由共用exec及RV/LA根启动使用，不另建错误策略。完整但损坏/错误架构仍
ELIBBAD。失败保持旧映像、TLS、线程、fd与handler，根构造失败/OOM无半成品
PID1，主/解释器OFD/source与MM均遵循原owner清理。

LP64D动态缓存v2单独保留compiler、cc1、内建headers、六个CRT/runtime、原
archive/ABI/options与完整安装树身份；syslibdir设在缓存内，guest interpreter
由显式链接参数固定/lib路径，安装不会写宿主/lib。旧SF缓存独立。两类各9个
合法先命中/单项拒绝反例通过，冷构建不复用旧source。缓存过期只拒绝，不
修改已固定输入或静默承认旧产物。

## 双架构验收 profile

原 RV GNU probe 固定2.44和退出42；LA已装的原版GNU runtime是2.42。用户选择
先固定现有工具与runtime，profile提供版本断言，不修改上游libc；安装树身份
同时覆盖目录/文件权限和链接目标，不能只比较loader内容。LA loader实际内建
`/usr/lib64`搜索路径，由本地已固定loader字符串和Linux动态启动核对，RV的
`/lib` fixture不能直接搬用。五形态同ELF在固定Linux两种RAM通过，初始Boar
静态程序在main前SIGILL；这只建立对照入口，尚不是glibc组合能力验收。

## PGDH内核栈窗口

用户选择保留DMW并只为任务栈增加分页窗口。固定QEMU v11.1.0
`target/loongarch/tcg/csr_helper.c::helper_csrrd_pgd`依BADV最高位选择PGDL/PGDH，
现有TLB refill可复用；原占位窗口base改为48位canonical高地址
0xffff800000000000。页表骨架在boot预分配且每个分配失败都回滚；叶仅借用
调度器持有的栈物理页，不能沿用户页表销毁逻辑释放它们。

guard fault时原SP可能也无法承载trap frame，入口先用scratch CSR保存临时GR
并检查，再选择保留的可信异常栈。初轮检查把合法的栈顶SP误认为下一槽guard，
真实用户启动反例复现；改用SP下方字节所属槽位后原信号/FPU/创建OOM/根盘
矩阵通过。实际guard写、SP不足、NX、撤映射访问及slot改映射分别有硬件证据，
不能把canary或正常串口启动当guard验收。

## SIMD live、used_math与固定Linux clone

1056字节内嵌image同时记录当前启用宽度、已初始化宽度和used_math。用户帧
撤销SC_USED_FP后再执行向量的同ELF反例：Linux保持原image，初版Boar把低FR
重置NaN而失败。切换以width判断owner，首用以used_math与live共同决定初始化；
恢复清除used时不能先快照handler硬件覆盖原内存image。

固定Linux`arch/loongarch/kernel/process.c::copy_thread`清除LSX/LASX_CTX_LIVE。
实际raw clone中的子任务低64位继承，向量上半部首用全1；父任务完整保持。该
固定版本的真实结果与源代码一致，不能把常识性的“fork继承全部向量位”写作
预期，再把Linux失败当fixture问题略过。Boar同反例RED后按已选Linux ABI修正。

FPU/LSX/LASX记录优先级按类别，不是列表顺序；同类重复取最后一条。合法有效
字段止于页尾、padding在PROT_NONE页且较大size跳到可读END时允许恢复。
三CPU profile的原程序、pending/只读写回失败、嵌套及clone/exec均保留可重建入口。
GNU2.42静态启动从SIGILL推进到空AT_RANDOM的SIGSEGV，GDB确认ERA=0x120000bbc
位于原`__libc_start_main_impl`。随后核对固定QEMU的FDT生成器，发现其已提供
32字节rng-seed，LA只读了内存布局而漏接材料。按RV已有不计熵策略接入后，五种
GNU形态在双侧两种RAM均通过；可信熵仍为零，random ready未置位。

## 共用VirtIO拆分与缓存反例

block迁入共用transport/split queue后，保留业务请求与buffer owner：descriptor
完成仅归还队列资格，不能释放仍由调用方使用的内存。reset确认同时绑定设备
身份，不能用另一个quiescent transport撤销本队列token。重复项在本次合法
完成之后出现时，设备失败仍保留该合法完成结果；先按inflight数整体拒绝snapshot
会改变这个既有契约，故共享层按ring容量检查并逐项验证head，block另保留八槽界限。

完成长度校验暴露旧host模型给写请求回报了513字节。模型现按实际WRITE
descriptor累计设备可写字节；reset后模型停止消费ring，不能根据已清零的idx
虚构65528个新请求。这些模型修正都基于总线行为，未放宽生产长度校验。

扩展共享transport结构后，LA独立block fixture仍使用旧对象，观察到IRQ零值
和readonly错误；生产对象已重编译，fixture的`.d`却未被Make包含。现将独立
fixture依赖一并纳入，并为定制C/汇编规则生成依赖、跟踪LA构建规则变化。
不清缓存即可重跑恢复正确布局，实际三传输块门禁及LA根owner/OOM再次通过。

PCI初始化reset失败时，只有平台BAR owner而没有块core；core销毁成功后的第二次
PCI reset失败则只剩BAR owner。若包装层仅凭core状态判断，会拒绝两者的合法清理
重试；若根数组只统计初始化成功设备，前者会漏清理。现单独记录core消费阶段，
根保留失败candidate，已释放的DMA物理地址/队列指针清零。两端真实PCI确认失败
注入及根EIO/未发布PID1/两种RAM基线已通过；不把软件注入当成QEMU真的拒绝reset。

布局计算还需先做有界减法再构造指针：UINT32_MAX附近的available偏移曾绕回
大小检查，host反例实际崩溃。共享层现验证两个ring全范围及DMA物理末端溢出，
失败不写ring；同反例返回INVALID，三传输实际块门禁继续通过。

RNG迁移保持短响应累计、5秒期限、60秒退避和ready单向语义，MMIO/PCI只提供
transport。LA缺设备/延迟/在途退出不能拿默认Linux启动种子作对照；固定Linux
先扫描FDT再解析early params。独立platform缓存开启内建RNG/net，不动已验证
core缓存。vCPU未执行时NOP掉固件seed并核对内存写回，两侧相同输入的原RNG ELF
16例通过，生产种子策略不变，详见[随机资料](random-source.md)。
新增standalone fixture依赖文件还暴露GNU make的隐式`.d.o`链接尝试，编号stem
会变成`0.d`并产生错误日志；依赖文件现显式声明由编译器生成，禁用该隐式重造。

## 共用net与LA真实PCI（2026-10-07）

net迁入共用transport/split queue后，descriptor资格与业务缓冲仍分开。平台
持有PCI/BAR/device，Ethernet只借用：worker join不代表DMA停止，必须确认reset
后才能归还在途SG owner。core stop也检查TX owner、RX loan与CPU lease；reset
拒绝时原字段保持，不能由上层释放BAR。真实九类失败×两种RAM回到基线，
宿主ASan模型另验证在途SG反例与确认后只释放一次。

原版固定BusyBox输入来自`references/oscomp-autotest` commit
`b5ec6ef8497e1818cbdec3b54bb722f036e57972`；LA使用既有完整配置静态LP64S缓存。
TAP程序ELF SHA-256 `91d9980f16276ae1babe0683e1de7947f8eced3e14180cfc4e9291496952183d`
在Linux/BoarOS两种RAM运行，原HTTP/CGI和五连接内容、UDP分片/压力、loan上限、
SG和共享IRQ及真实根owner均通过。执行时间只记录为功能运行资料，未做性能匹配。

16KiB另外暴露了测试与ABI两种问题。固定`references/linux` commit
`f4cdf7ca9a1fdcca413157df19753f388a5a224e`的`fs/pipe.c`按PAGE_SIZE分配pipe槽；
sendfile测试读走4KiB不能保证释放LA的槽，故仅该页边界改用sysconf页大小。
`fs/splice.c::splice_to_socket`每批16个bvec，RV最多64KiB而LA最多256KiB；
现有BoarOS AF_UNIX固定64KiB成本边界因而与LA的65537字节单datagram不同。
这需要发送者缓存计费与消息上限的真实修复，用户已选择Linux模型；既有RV接收
配额成本用例也要重验。不能允许两种消息边界都成功来遮住可观察差异。

## 空目录EUCLEAN是输入特性差异（2026-10-07）

扩大sendfile先暴露tmpfs卸载后rmdir失败；独立普通mkdir/rmdir也复现，排除了
必须由tmpfs、网络或sendfile触发的解释。宿主同源lwext4的1KiB索引目录：
metadata_csum开启通过，关闭立即在空目录覆盖返回EUCLEAN。LA根fixture关闭
该特性，既有RV矩阵多用开启输入；这不是LoongArch指令或页大小特例。

持久目录的inode大小为两块，INDEX+EXTENTS有效。root指向一个空leaf，leaf
首项inode=0/name_len=0/rec_len=4096；这也像inner node的包围记录，原检查
误按索引count/limit解析空leaf。修复按已验证root的indirect_levels及entry块号
判定角色，保留leaf记录和root/node结构校验。源码依据为固定
`references/linux` commit`f4cdf7ca9a1fdcca413157df19753f388a5a224e`的
`fs/ext4/namei.c::{ext4_empty_dir,is_dx_internal_node}`，实际修改为固定lwext4
`58bcf89a121b72d4fb66334f1693d3b30e4cb9c5`之上的本地空目录检查。

新的宿主八种布局在ASan/UBSan下完成空目录覆盖、长名称增长/rename和e2fsck；
独立ELF`d4a74d93e3ade80612549f23d32c9a777cbdcc08e4519470f543bac44e8603f0`
在Linux/BoarOS两种RAM完成普通及挂载/卸载后rmdir和根回收。不会用干净重跑
替代根因；checksum关闭的反例与重建入口保留在Git测试。

AF_UNIX发送者模型后续已落地：budget/sendfile/unix_sender同ELF在Linux/BoarOS
两种RAM通过，首次大消息、关闭/取消、SIGPIPE区别及真实owner另见
[单核记录](single-hart-scale.md#af_unix发送者计费与la批次2026-10-07)。内部按
本地实际packet/payload请求字节计费；发送成功次数不作为与Linux私有skb布局
一致的证明。根Linux参考先检查退出，再明确清理其proc挂载层并卸载ext4，
BoarOS仍要求完整页/堆/任务栈/设备基线。

## 共用串口、原交互与 PTY（2026-10-07）

UART硬件寄存器是平台事实；RX/TX预算、TTY发布、worker/drain和owner清理从RV实现移入
`drivers/serial/ns16550.c`。LA第一UART pin2经既有PCH/EIOINTC接入，正常输出进TTY，
fatal保持关闭中断后的轮询。固定依据与命令见[模块](../modules/riscv-uart-tty.md#la-平台与对照验收)。

首次native runner暴露QEMU默认NIC占用测试块设备addr1，修复的是fixture显式关闭默认NIC，
生产枚举仍不假定槽位。PTY原script需要PID1收养其真实command，因此Linux使用直接根盘PID1；
不能用普通supervisor子进程再把漏reap当内核差异。退出42由Linux真实panic状态解析，
BoarOS另检查实际根页/堆/任务栈/BAR基线。原输入、录制文件、尾部和独立重启内容全部验证，
不以串口PASS替代进程状态或磁盘内容。Linux/BoarOS的启动默认termios不同，
探针先设置共同基线再恢复各自初值；不归一化实际串口字节。

## pipe 的已分配页与可用容量（2026-10-07）

16KiB迁移应分别核对页片段、字节长度和owner。pipe的真实order4分配已经是16个
目标页，capacity却残留16×4096；因此LA只有四个可用槽，既有16槽wrap等待不可能完成。
独立nonblocking探针先在固定Linux两种RAM通过，旧BoarOS在第五页EAGAIN且正常回收，
修复后同ELF四侧通过。修复复用已持有的数据区；4096字节PIPE_BUF仍是用户ABI字节值。
这是实际生产容量缺陷，与探针mprotect采用错误4KiB边界分别修复，不能只放宽测试。

## LS7A RTC 的 Linux 组合阻塞与派生修复（2026-10-07）

依据为本地 `references/loongarch-documentation` commit
`e0d6592229d9e00e512bd28b688a7f20b171714f` 的
`docs/Loongson-7A1000-usermanual-EN/rtc/`、`power-management-module/` 与
`interrupt-controller/interrupt-source-assignment.adoc`，以及固定 Linux
`f4cdf7ca9a1fdcca413157df19753f388a5a224e` 的 `drivers/rtc/rtc-loongson.c`。
TOY 直接计时中断与 PM RTC_STS/W1C/RTC_EN 是不同的硬件状态，不能用一个 sticky IRQ
同时代替；固定 virt 的聚合路由也不能等同于 LS7A 实板各 pin 的分配。

原 QEMU v11.1.0 `84f07211cc5b4fc6a371559bf8a5de4fb068e648` 只映射
`0x100d0100..0x100d01ff`。Linux 启用 RTC 后，类设备初始化读 alarm 会访问
`PM1_EN=0x100d0010`，实际 native ADEM 卡在 read_alarm+0x84；只查看成功读取
TOY 或启动串口不能发现这个组合故障。旧 timer callback 只有 raise，Linux ISR
写 match0=0 后也不能撤销 IRQ。实际 qtest 先复现 PM_EN 写后读零，独立定时器测试
又复现 clock=vm 下 qemu_timedate_diff 错用 HOST，首次告警之后重新设定会延迟。

人已选择补齐派生模拟器路线，未采用 DTB compatible 降级。固定参考源码保留原样，
补丁、重建命令及身份检查入仓库；输入控制用 toy-enabled property 在 reset 时生效。
之前 generic loader 的写入会被 reset 覆盖，GDB post-reset 原型不再是最终 runner。
模型的 PM/直接 IRQ、比较器交错、回绕、复位和 pending 迁移经实际 QEMU 验证；
qtest 合成 clock 不在 TCG 迁移时钟内，目的端在 restore 前对齐该测试 clock。
原版 Linux 驱动在两种 RAM 完成两次 read/poll 告警、UTC 跨年、重新设定、坏参数/
指针、block/RNG/UART 进展、实际退出42及卸载。

BoarOS 同一环境 ELF 的真实 UTC、日志内容/OFD、别名与 fork/exec 回收及原 BusyBox
hwclock/dmesg/df 在两种 RAM 均通过。两项测试都 clear 日志，组合到同一启动会
破坏下一项的启动日志前提，必须用独立启动，而非伪造日志或改动原程序。
BoarOS RTC 仍按共用只读设备口径验收；Linux 告警成功不能记作 BoarOS 告警支持。

## 完整 ABI 的页几何与退出发布（2026-10-07）

共用完整 raw ELF 在 Linux/BoarOS 的 LA 512 MiB/1 GiB 各产生1366条完整记录，
退出42与根资源门禁通过；同一改动的 RV 1366 条也匹配。LA 16 KiB 页改变映射/
保护与跨页前缀，但不是把所有4096替换成16384：UDP仍发送8192字节、fault前缀
仍为4096字节，TCP前缀仍3072，文件块offset4094、truncate4096、解析用的4097
等保留业务含义；只有用户地址与保护页位置调整。匿名管道容量修复另有独立反例。

固定 Linux `kernel/fork.c`、`kernel/exit.c` 在 clear_child_tid 之后才发布 zombie
和递减 nr_threads。实际完整运行发现两种RAM可以读到不同的瞬时R/Z，并非结构布局
不同。探针用 bounded yield、真实 lseek/re-read 等待约定的退出阶段，保护 held OFD
与新打开路径均正确观察同一已退出组长；未修改内核来迎合调度时机，也未放宽记录
解析或删除案例。host 协议测试另外验证第四个 RAM 结果的差异会失败。

最终回归又检出 RNG runner 在 QEMU poll 已结束时提前退出 select loop，stdout管道
仍有尚未读取的退出/回收记录。补读进程结束后的实际尾部并保存原字节后，四类构造
失败的两种RAM全部通过；不放宽退出42或owner门禁。GNU runner原来另有原版QEMU
默认路径，新增TOY输入在该二进制不受支持；改为共用架构profile的模拟器路径，
五形态在派生模型上重新验证。二者是采集/工具路由修复，不记为缺失硬件能力被存根
绕过，也不覆盖尚未完成的恢复/整体审查门禁。

## 一次独立整体审查与集中修复（2026-10-07）

对 `1c4d715..b653966` 的一次fresh-context只读审查发现一个P2验收问题：
UDP/TCP小缓冲fault、null/zero边界与RT sysctl测例的mprotect已按目标页大小，
指针却仍在4092/4094/4095。LA两侧都没有碰到保护页，完整1366条匹配因此不能
证明这些原fault ID实际触发。原冻结记录保留该限制：UDP/TCP返回4、zero读8，
rtctl分别EINVAL/读2；不能把此前计数当作这些边界已验收。

集中修复先仅加入实际EFAULT/partial4与offset不改变的前置断言，旧LA两侧都在
UDP边界停止并退出99（BoarOS仍正常归还root owner）。再将这三类指针及初始化/
证据位置改为目标页尾，保持原案例ID、消息长度、文件offset4094和负指针-4096。
解析器没有归一化这些返回值；修复后LA四流与RV两流各1366条完整匹配，UDP/TCP均EFAULT、zero仅4字节、rtctl均EFAULT且offset0。审查后LA启动/SIMD三配置/RTC模型及真实Linux告警/环境/pipe，以及RV完整架构、userland/GNU/ABI/栈重新通过；两内核哈希与此前229项及存储/设备全回归的冻结产物一致。

审查未确认新的内核/owner阻塞。作者逐项裁定未升级项：AF_UNIX最后packet与destroy
的假设抢占窗口在当前实际syscall/reap关闭IRQ的入口契约下不可达，未来开放内核
抢占/SMP时需重新审计；net初始化明确要求新零对象，stop后全owner归还才能清零
复用，failed reset不允许覆盖；没有增加热插拔功能。任意恶意DMA隔离、guard的
DMW别名/跨越整个guard/boot-idle栈、全IEEE运算及完整信号组合、深调用链/新性能
均没有本轮承诺。LBT/SMP/实板/packed ring/外部IPv6/DNS/TLS/原生开发/更广程序/
完整Harness、BoarOS RTC告警与全TCG时钟迁移亦保持范围外。旧allocator/Virtqueue
历史归因没有新增现场证据，继续保留unknown。作者只做这一轮集中修复，不请求
第二位独立reviewer；最终判定依赖新反例和真实回归。

2026-10-07收口：review后生产kernel-la SHA-256
`e1e234c3dbf8a6d0448b857947ff85c31aaa66fda3041ef7cc592e7e37b88f98`，
kernel-rv `e4cc9dbe43c01b9d03b276350e92a8e2ff4250dd2bda42ae2422c4f5d087d11a`，
与完整清单/ABI/设备/存储门禁冻结内核一致。派生模拟器二进制为
`63dcacc82765ba4a18cfb623410d19fd462bd3755c06cc81cd73caaf226bc0b9`；RV本轮默认
模拟器实际为11.1.1，SHA-256 `a1cfcceb6c688f9b0a290d512211ed08cf465b92b26a04cfb032280a53625718`，
没有把两个架构的模拟器身份混称为同一个二进制。只读review确认的P2已用真实红/绿
和全量记录纠正，没有新增独立review。预览后执行 `make prune-build` 删除349项
临时目录/日志/镜像及旧缓存，复查预览0项；未保留临时plan、ledger或review package。
LP64D musl、两种GNU runtime、原版/派生QEMU、两套LA Linux及编译产物缓存保留，
清理后内容/模式/链接/构建配置身份再检查通过。提交沿main，未push或发布。

## 转入原评测镜像（2026-10-07）

最新main已单向合入兼容分支，本地分支随后改名oscomp-compat，旧远程引用保留。
下一阶段采用固定Harness原Docker入口与原judge，保留300秒LTP监督；原盘程序
与独立musl/GNU验收输入分别记录。固定pre-20250615 LA盘的loader为
`/glibc/lib/ld-linux-loongarch-lp64d.so.1`，原libc的PT_INTERP还引用
`/usr/lib64/ld-linux-loongarch-lp64d.so.1`；RV链接名不能机械迁移到LA。
官方容器和原盘结果尚未取得，本轮指定矩阵仍保持既有证据边界。

## Transitional PCI 的 modern 接口（2026-10-07）

固定Linux `references/linux@f4cdf7ca9a1fdcca413157df19753f388a5a224e` 的
`drivers/virtio/virtio_pci_modern_dev.c` 使用0x1000–0x107f范围；transitional
设备的kind来自subsystem device，而不是旧PCI ID算术。固定QEMU
`references/qemu@84f07211cc5b4fc6a371559bf8a5de4fb068e648` 的
`hw/virtio/virtio-pci.c` 可同时暴露legacy PIO BAR和modern capabilities。
因此“有legacy BAR”不等于“没有modern接口”。

host先复现同样capability内容因身份拒绝，修复后验证真实BAR claim、PIO持续关闭、
reset后配置恢复和错误subsystem/缺modern caps的失败回滚。根盘测例区分默认
transitional与`disable-modern=on`的纯legacy负例；不能以移除disable-legacy
作为legacy-only证据。此修复不增加legacy PCI寄存器协议或PIO资源分配。
