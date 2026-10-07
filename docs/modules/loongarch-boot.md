# LoongArch QEMU 启动与根盘

当前验收范围为 QEMU virt、LA464、单核、LA64、16 KiB/三级页表，
整数/FPU/LSX/LASX状态与本轮指定用户程序、平台矩阵；完整Harness/原生开发另行验收。
入口为 `arch/loongarch/boot.S` 与 `main.c`，平台事实独立放在
`platform/loongarch_virt.c`。`kernel-la` 有现代 PCI block 时启动可配置根盘 PID 1，
无盘时运行内嵌独立 ELF 契约；平台存储生命周期在 `platform/loongarch_root.c`。
默认 `all` 和 RV 测试入口保持原路径。

```sh
make kernel-la                 # LP64S 整数内核和独立用户 ELF
make run-loongarch             # QEMU 无 BIOS 直接 ELF 启动，串口可交互
make test-root-loongarch        # PCI/ext4/LP64S musl/原 BusyBox 与 Linux 对照
make test-root-io-loongarch     # 真正的块写错误与持久 owner
make test-la-userland-host      # 缓存输入/完整安装树的9个拒绝反例
make test-loongarch-boot        # 512 MiB/1 GiB 启动与后续契约
make test-boot-random-loongarch # DTB材料可用、可信熵仍为零，两种RAM
make test-rng-loongarch        # 同ELF双侧、两种RAM、四种真实PCI RNG模式
make test-network-external-loongarch # 同ELF、实际PCI收发与原BusyBox HTTP
make test-net-failures-loongarch      # DMA/IRQ/worker/reset失败及BAR回收
make test-rng-failures-loongarch # DMA/任务/栈OOM、IRQ登记失败与回收
make test-loongarch             # 同时准备固定 Linux，对照同一个用户 ELF
make test-stack-usage-la
make test-signal-loongarch     # 同一静态 musl ELF 的整数信号/恢复/重启对照
make test-pthread-loongarch    # 静态 TLS/线程、原 BusyBox ash 非交互 trap/wait
make test-pthread-oom-loongarch # 两处 clone 构造 OOM、真实 pthread EAGAIN/重试
make test-permissions-loongarch # 冷/驻留 EXEC 页、fork、uaccess 与改权对照
```

`run-loongarch` 进入 console 探针后，依次在 poll/read ready 标记出现时输入 `g` 和 `r`（各回车），完成后关机。

`prepare-la-tools` 从固定 QEMU 加本地 RTC 补丁构建派生模拟器；`prepare-la-original-tools`
保留无补丁对照构建。`prepare-la-linux` 从固定 Linux defconfig 建立
16 KiB/三级页表、initramfs 对照。缓存分别位于 `build/qemu-la-rtc`、`build/qemu-la`、
`build/linux-la`、`build/loongarch`；身份变化时拒绝混用。`make prune-build`
保留工具、对象、ELF 和身份，删除运行日志及 initramfs。`QEMU_LOONGARCH64` 和
`LA_CROSS_COMPILE` 可覆盖工具位置，版本变化需要重新验收。

内核物理装载地址为2MiB，高地址为缓存DMW的`0x9000000000200000`。
开启分页前临时建立低地址DMW，跳到高地址后立即撤销；用户模式不可使用
内核PLV0的DMW。平台从直接启动传入的EFI system table定位DTB，检查签名、
表数与范围；按DTB中所有RAM bank排序，扣除启动信息、DTB、内核和保留区。
物理分配器复用通用buddy实现，页大小在构建期固定为16KiB。
完整启动布局验证成功后，将DTB的rng-seed交给共用随机核心作为不计熵材料，随后
擦除临时副本；原DTB仍属于固件。缺种子不伪造材料或ready；AT_RANDOM只在有材料
时构造。固定QEMU的32字节种子使GNU启动可用，可信初始化仍等待真实RNG设备输入。
根现从PCI身份发现device4，以共用RNG核心及joinable worker提供真实可信材料。
根退出在消费设备/BAR前stop/join并确认DMA停止，错误保留实际owner；未就绪实验
的固件输入与正常DTB策略分开，见[随机设备模块](riscv-virtio-rng.md)。

架构 MMU 设置 PWCL/PWCH、STLBPS 和独立 TLB refill 入口；根目录/中间目录缺失时
refill 写入无效 paired entry，交由普通用户缺页路径处理，不能读取物理地址零。
用户 PTE 的 PLV3、NR/NX、dirty 与软件 COW/PROT_NONE 状态由 LA 后端拥有。
非 NONE 用户 PTE 按固定 Linux LA 映射表保持可读；请求 VMA 权限不改写，
所以冷 EXEC 页的数据读取与合法取指后的驻留页行为分别验证。
撤映射先失效转换再归还页；地址空间切换使用 PGDL、ASID0 和全量失效。
用户页、目录页及 COW 引用均按通用 MM owner 协议转移或回滚，所有权损坏 fatal。
内核代码/数据继续使用PLV0 DMW；任务栈使用独立PGDH窗口，RW/NX叶仅允许PLV0，
每槽下方留一个16KiB未映射guard页。窗口骨架属于永久boot owner，任务栈页仍由
调度器持有；撤叶先刷新TLB，切回可信栈后再释放物理页。SP恰好为栈上界时以
其下方字节决定槽位；不能容纳304字节trap frame时改用永久16KiB emergency栈。
`make test-stack-guard-loongarch`验证实际guard/SP越界/NX/旧翻译故障、用户访问
拒绝、slot改映射复用及全部骨架分配OOM回滚，均覆盖512MiB/1GiB。

调度器通过 `arch/task.h`、`context.h`、`timer.h` 和 `mmu.h` 构建期选择实现。
LA 保存完整整数 trap frame（304 字节）、内核 ABI context、用户 SP/TP/ERA/PRMD；
KS0 用于区分并交换用户/内核 TP。timer 使用 CPUCFG 频率、RDTIME.D 和 one-shot
TCFG，接入共用 tick、deadline、预算与调度；未启用 UART 外部 IRQ，timer 轮询
就绪输入并唤醒阻塞 read/ppoll owner。未使用FP/SIMD的task保持EUEN关闭；已使用者
按实际宽度启用FPE/SXE/ASXE并保存内嵌状态，具体见[FP/SIMD模块](loongarch-fpu.md)。LBT未支持。

内存 reader 经共用 source、映像、MM、任务创建与 syscall dispatch 路径进入 PLV3。
LA syscall 从 a7/a0–a5 解码，返回 a0 并恢复 ERA+4。未知 syscall 返回 ENOSYS；
坏复制指针返回 EFAULT；权限/未映射故障和访问内核地址按实际用户故障处理。
合法缺页 OOM 使用 RESOURCE/NO_MEMORY 退出，不能误报为用户地址非法。
rt_sigaction/rt_sigreturn 已接入共用信号策略和 LA 整数帧；布局、故障与重启契约见
[信号模块](kernel-signal.md)。FPU/LSX/LASX扩展帧已接入，见[LA浮点/SIMD](loongarch-fpu.md)；sigaltstack未支持。

2026-10-06 在 512 MiB 和 1 GiB 下完成：

- 内核保留区/RAM 布局、真实页写读、16 KiB slab 位图及 MMU/COW/PROT_NONE 回收；非法/重复页释放与损坏页表 owner 在两种 RAM 各自独立启动中均 fatal。
- 两个不主动 yield 的内核任务、fork 出的两个用户计算任务均经 timer 前进；整数寄存器、SP、TP 和恢复 PC 保持。
- 正常段、整页 FILE、BSS 尾页、初始栈/AT_PAGESZ、跨页复制、PID/TID、yield、时钟/睡眠、匿名 mmap/mprotect/munmap、exit/exit_group。
- 只读、PROT_NONE、NX、撤映射后访问及内核 DMW 访问；9 组同 ELF 的 Linux/BoarOS 退出契约一致。
- 损坏 header、错误 machine、重叠布局、16 个构造分配失败边界、两处任务/栈构造 OOM 和缺页 OOM；失败不发布任务，页数回到预热基线，最后 heap live 为零。
- 首阶段可信 idle 栈回收 13 个任务栈，实测最小余量 31,016 字节、最大使用 1,736 字节；编译器 2,066 条函数记录（含 fatal fixture）最大单帧 3,392 字节。单帧检查不证明完整调用链上界。

本轮 RV allocator、MM/VMA、uaccess、context、scheduler、exec 及 `test-riscv`、
真实静态/动态用户程序、五种固定 glibc 形态、1,344 条 ABI 差分、栈检查与 SQLite
DELETE/WAL 恢复回归通过。上述结果不代表 LA 完整线程、信号、libc 或比赛 Harness。
下一阶段 PCI→VirtIO 块→ext4→静态 musl 已由人批准，采用共用块核心与独立 MMIO/PCI transport；
该阶段现已通过下述根盘与静态用户程序验收。SMP、实板与动态加载另行验收。

固定依据为`references/qemu` v11.1.0，commit
`84f07211cc5b4fc6a371559bf8a5de4fb068e648`的`hw/loongarch/boot.c`、
`include/hw/loongarch/virt.h`和FDT生成代码。工具为本机
`loongarch64-unknown-linux-gnu-gcc`15.1.0；内核使用LP64S、禁用浮点和LSX/LASX。
QEMU从该固定源码在`build/qemu-la`构建，未引入新的产品依赖。
Linux 对照 commit 为 `f4cdf7ca9a1fdcca413157df19753f388a5a224e`；CSR、PTE、
refill 和 syscall 核对路径及调试经验见[LA 学习记录](../learning/loongarch-bringup.md)。

## 第二阶段：PCI 块设备已验证

`kernel/pci.c` 有界解析 capability、按真实 BAR mask 分配不重叠资源，失败恢复
原 BAR/command 并归还 claim。`drivers/virtio/pci_block.c` 将共用块核心与
`drivers/virtio/pci.c`的现代PCI adapter绑定；[共用框架](virtio-framework.md)拥有
feature/status和split queue。总线访问宽度、read-to-clear ISR和通知地址由PCI实现，
ECAM、uncached 映射及 INTx 来源由 `platform/loongarch_pci.c` 提供。BAR 地址
不能假设由固件分配：无 BIOS QEMU 上先 sizing/分配，再确认 reset，最后打开
bus-master，销毁按 stop DMA→摘 IRQ→释放队列→恢复 BAR/command 的 owner 顺序。

QEMU 平台采用128个bus的ECAM、低PCI memory aperture、根bus现代端点及
PCH-PIC→EXTIOI→CPU0/HWI0。资源 claim 有64个槽，耗尽明确失败；无桥、热插拔、
纯legacy PCI传输、MSI-X 或实板验证；transitional身份的modern接口已接入。控制器为共享level pin在逐设备ISR之间保持mask，
unmask重新采样intirr；trap只分发ESTAT与ECFG交集，同时处理已启用的timer。

```sh
make test-pci-host
make test-block-loongarch
```

512MiB/1GiB各验证两个共享INTx pin的真实PCI盘、启动轮询和运行期IRQ、
八槽批量数据、CPU进展、只读与范围错误、写入/FLUSH后的宿主字节和完整
任务/队列/PCI claim回收。queue OOM在真实设备初始化中恢复BAR、command和
页基线。宿主模型额外覆盖capability循环/截断/溢出、BAR资源不足回滚，以及
不依赖handler遍历顺序的共享level迟到完成。掩蔽外设的pending状态不能在
timer trap中被分发，真实QEMU先复现失败后修复。

固定依据：QEMU上述commit的 `hw/pci-host/gpex.c`、`hw/intc/loongarch_{pch_pic,extioi}.c`
及 `include/standard-headers/linux/virtio_pci.h`；Linux上述commit的
`drivers/virtio/virtio_pci_modern_dev.c` 和 `drivers/irqchip/irq-loongson-{pch-pic,eiointc}.c`。


## 根盘、静态 musl 与回收

根 bus 按 BDF 顺序枚举 block 端点，最多8盘，首盘是 raw whole-disk ext4；一旦发现
block，不通过后续磁盘或内存 fixture 掩盖启动错误。仅有legacy接口的PCI明确ENOTSUP，
数量超限 ENOSPC，损坏 ext4 保留后端 EUCLEAN。默认 `/init` 的 path/argv/envp
使用与 RV 相同的 `INIT_CONFIG=config/init.json` 生成方式，LA 有独立生成依赖。
PT_INTERP已按fs context打开唯一解释器source并交给共用image，路径错误保留errno；
短64字节header为EIO，完整但损坏/错误架构为ELIBBAD，失败不发布PID1。
解释器含另一个PT_INTERP仍按现有明确子集拒绝，不声称Linux递归解释器全部行为。

永久可信 cleanup 栈在页基线前创建，启动构造也在该内核任务上执行。设备与
cache/mount/files/fs/source/image 的真实字段一直保存 owner；映像准备、I/O worker
就绪后才原子转交 MM/files/fs 并发布 PID 1。页缓存和 journal 使用原共用 worker，
运行期 I/O 通过 PCI IRQ 睡眠；idle 只负责调度。PID 1 completion 后先停止其他
用户 owner，回收用户任务，再 stop/join cache worker、卸载 journal/mount、销毁
cache、摘 block 注册并 stop DMA/IRQ、释放队列和 BAR claims。成功要求 heap live
和 heap pages 均为0，物理页回到排除永久 cleanup 栈后的精确基线；当前栈不提前释放。

真实 I/O 清理失败仍由原对象持有，最多三次清理后报告并关机，不打印资源回收成功。
`test-root-io-loongarch` 用原 NBD fault server 在真实现代 PCI 盘注入 write EIO，
确认用户失败、mount/cache/device/claim 仍存在且没有成功标记。非法 allocator
释放或元数据损坏继续 fatal，不进入该重试路径。

2026-10-06 的两种 RAM 验收均通过：正常/只读根盘（只读镜像整盘哈希不变）、
缺失/无 execute bit/错误 machine init、损坏 superblock、legacy PCI、用户 fault，
以及 image、task、cache worker 构造 OOM。失败不发布 PID 1；无 I/O 故障的各条路径
均恢复根页/堆/PCI基线。静态 musl probe 检查16KiB auxv/BSS、跨页文件读/私有和
共享映射、fd offset、msync/fsync 持久数据、clock/sleep、fork/exec/wait；原 BusyBox
完整配置未裁剪，执行cp/cmp/grep/cat/echo/uname/dd。相同 probe 和 BusyBox ELF 在
固定 Linux 的 PCI ext4 上分别运行，验证关系和持久字节，不比较偶然 PID/时间。
这是七个原 applet 与组合 ABI 的验收，未运行完整 BusyBox、RV 的 FP/signal/ucontext
用户探针或完整比赛 Harness。最新 RV完整架构、真实用户态、五种glibc、1366条ABI、
栈检查及共享驱动的SQLite DELETE/WAL/恢复与四种io-sleep门禁均通过。

LP64S 工具缓存由 `make prepare-la-userland` 重建并校验：固定 GCC15.1.0 archive
构建 `loongarch64-unknown-linux-gnusf` C 编译器和匹配 libgcc/CRT，使用固定 musl1.2.5
headers bootstrap runtime，再构建静态 libc；BusyBox 使用清单 commit 和原配置，
UAPI 从固定 Linux v6.6 `ARCH=loongarch headers` 导出后原样安装，不混宿主 glibc 头。
编译源码与可复用产物、工具/配置/ELF身份留在 `build/loongarch`，完整安装树包含helpers/specs/headers与链接目标；冷构建先生成子配置。运行目录、镜像、
日志及早期失败构建用 `make prune-build` 清除。ELF 使用 SOFT-FLOAT ABI，EUEN关闭；
没有用忽略 ABI mismatch 或成功存根绕过硬件限制。

## 整数信号与静态线程

2026-10-06 `test-signal-loongarch` 与 `test-pthread-loongarch` 在固定 Linux 和
BoarOS 的512MiB/1GiB运行相同LP64S静态ELF。信号覆盖真实musl布局、mask、嵌套、
SEGV/BUS/ILL/TRAP/FPE、用户映射修复与上下文返回、坏帧及pipe/nanosleep/sigsuspend。
线程覆盖11组：纯计算timer进展、整数寄存器/SP/TP、TLS data/BSS与errno隔离、
mutex/cond/barrier/超时、取消cleanup、阻塞fd pin、组长退出/非组长exec/exit_group、
futex重启及非PI robust注册/查询/owner死亡/raw exit/COW。原BusyBox完整配置的ash
另执行非交互trap、后台sleep和wait；不据此宣称LA UART控制终端或PTY作业控制。

`tests/loongarch/pthread.c` 直接调用已有RV用户测试的适用函数，没有裁剪原RV入口；
此静态入口不执行动态DSO；动态入口在下节另测。socket和RV的未支持PI marker
门禁保留在原目录，不能把这一静态选择集
称为原pthread全量测试。Linux对照发现并修正测试的join/PID摘除时序，以及WAKE
后目标可能退出的假设；内核不按测试输入特判。

clone的任务页、栈分配两处故障分别在两种RAM注入，真实pthread_create返回EAGAIN、
未执行半成品worker，任务/栈页立即恢复，随后成功create/join。此四组是BoarOS注入
证据，与同ELF Linux语义对照分别报告。每次正常、故障、OOM根启动收口要求
heap live/pages为0，所有可回收物理页、根盘/cache/PCI owner回到预热基线。

整数信号阶段的RV信号/syscall、完整架构、原静态/动态用户态、五种固定glibc、
1366条ABI和栈门禁通过；当时未改文件映射/COW或块核心，未重跑SQLite/存储
恢复矩阵。当时的标量FP、动态musl/DSO TLS限制已由下节解除，SIMD、LA完整
终端/网络和比赛Harness仍缺。

## 原版动态 musl、解释器与 DSO TLS

`make prepare-la-dynamic` 在独立dynamic-dp-v2缓存构建固定musl1.2.5原源码，
用户使用已安装GCC15.1.0 LP64D和匹配CRT/libgcc；内核仍LP64S。输入记录
compiler/frontend/内建headers、runtime、原archive/flags；完整安装树记录文件
内容、权限和链接目标。`test-la-dynamic-host` 的9个反例先验证合法命中，再
拒绝loader/header/specs/CRT/mode/link/ABI及compiler/runtime身份变化。原静态
SF缓存独立，`test-la-userland-host` 原9个反例继续通过。

`make test-dynamic-loongarch` 对相同PIE和非PIE ELF执行原late-dlopen TLS案例与
11组线程消费者，另有DT_NEEDED、ORIGIN RPATH、RELRO链接及初始DSO TLS。
均在Linux/BoarOS两种RAM实际运行；ldso/libc和DSO由用户运行时装载、重定位、
分配TLS，不在内核另写动态链接器。依赖库与程序在运行期间逐文件核对SHA。

`make test-exec-errors-loongarch` 双侧核对缺失、无执行位、错误架构、损坏完整
header和短header的errno；失败后旧PID、TLS、存活线程、CLOEXEC fd/offset、
信号handler保持。初始动态PID1的上述五条失败及image构造物理OOM，在两种
RAM共12次启动中不发布任务，主/解释器source、MM、页/堆与PCI/root owner
准确回收。真实VFS/block I/O清理失败仍按原owner契约保留。

```sh
make test-fpu-loongarch test-dynamic-loongarch test-exec-errors-loongarch
make test-la-dynamic-host
```

该动态musl阶段的FP限制已由标量FPU解除；当时SIMD、glibc、更广应用与TTY/网络
尚未验收。当前指定矩阵已完成，SMP/实板、完整Harness等仍在范围之外。

动态musl阶段RV完整架构、原静态/动态用户态、五种glibc、1366条ABI、栈和SQLite
DELETE/WAL多进程/重启通过；当时未跑NBD全恢复和LA原SQLite。后者已在当前
单核对齐轮完成，见下节及[SQLite](vfs-ext4.md#双架构-sqlite-deletewal-原程序)。

共用net核心和Ethernet已接入LA现代PCI。root枚举device1、持有BAR和device，
网络层借用它；missing NIC仍有timer worker保障loopback与AF_UNIX。块addr1、
net addr5、RNG addr9的共享INTx是测试输入，生产扫描不依赖槽位。真实TAP、
九类失败与资源证据见[net模块](riscv-virtio-net.md)。本轮TTY UART IRQ、RTC、
AF_UNIX sendfile计费及指定原程序均已验收；范围外应用仍不能由此声明全部等价。

## LS7A RTC 与派生模拟器

`platform/loongarch_rtc.c` 按 [时间模块](kernel-time.md#la-ls7a-rtc) 初始化和读取
真实 UTC。正常用户态仍复用共用只读 RTC 字符设备，运行期不持有 RTC IRQ owner。
原 BusyBox dmesg/hwclock/df、OFD 独占、dup/fork/exec、错误指针与最终根资源释放，
同一 ELF 已在 Linux/BoarOS、512 MiB/1 GiB 验证。日志探针与原脚本各自清空日志，
因此分别启动，不能以人为打印启动标记补偿已经消耗的日志内容。

用户选择 A 后，模拟器采用 `tests/loongarch/qemu-ls7a-rtc.patch` 的派生构建。
固定 `references/qemu` commit `84f07211cc5b4fc6a371559bf8a5de4fb068e648`
保持原样；补丁 SHA-256 为
`a2951479d2f45afed8ffc9e69ed4a44efd7174b00a688b03d5d786332515cf2d`。
`tests/loongarch/qemu_rtc.py` 导出固定 commit 与三个固定 Meson wrap commit，
校验源文件、构建配置、编译器及模拟器内容/权限/链接目标。派生缓存是
`build/qemu-la-rtc`；原版缓存 `build/qemu-la` 可由 `prepare-la-original-tools` 重建。
普通命中拒绝身份变化，已确认的补丁修改通过 `--rebuild` 重新导出和构建。

补丁补齐 Linux 使用的 RTC PM 子集：PM1_STS.RTC_STS 是真实锁存的 W1C 状态，
PM1_EN.RTC_EN 与 PM1_CNT.INT_EN 控制独立 SCI 输出。六个 TOY/RTC 比较器各自
持有 pending，重写只清除本比较器；match=0 的无效 TOY 日期清除直接 IRQ。
原 virt 聚合计时 IRQ 保持 pin6，PM SCI 与 GED 经 OR 共享 pin7，避免互相覆盖。
复位撤销定时器和 IRQ；迁移保存 pending/PM 状态并恢复尚未触发的比较器。
RTC counter 按 32 位回绕；六位年份的过去 TOY match 等待下一个 64 年周期。
`system/rtc.c` 日期差值改用实际 `rtc_clock`，修复 clock=vm 的宿主时钟错配。

直接 ELF 启动 profile 显式传入 `-global ls7a_rtc.toy-enabled=on`，代表已初始化的
电池时钟输入；设备默认复位值仍为关闭。无需改 Linux 驱动或 RTC compatible。
该 PM 子集不声明完整 ACPI 电源管理、休眠或调频支持；TOY 的当前模拟分辨率仍为
1 秒，不能引用芯片手册的 0.1 秒作为 QEMU 实测能力。

```sh
make test-rtc-loongarch-host
make test-rtc-model-loongarch    # 实际 MMIO、W1C、独立 IRQ、重编程、回绕、复位、迁移
make test-rtc-alarm-loongarch    # 原版 Linux 两次告警/跨年，混合 block/RNG/UART 与实际退出
make test-environment-loongarch  # 两个独立清日志场景，各在双侧、两种 RAM
```

2026-10-07原Harness设备调查发现默认transitional PCI同时提供modern接口。
此前按PCI ID提前拒绝并把PIO BAR当作整设备不支持，导致原盘尚未启动PID1就ENOTSUP。
现按subsystem device枚举，保持未使用PIO关闭；host反例及真实根盘transitional/modern
在512MiB和1GiB验证正常PID1退出、页/堆/任务栈/BAR基线，纯legacy负例仍ENOTSUP。
同一个静态root ELF在固定Linux对照两个设备形态；命令仍为`make test-root-loongarch`。
