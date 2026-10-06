# LoongArch QEMU 启动与根盘

当前验收范围为 QEMU virt、LA464、单核、LA64、16 KiB/三级页表及整数用户态。
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
make test-loongarch             # 同时准备固定 Linux，对照同一个用户 ELF
make test-stack-usage-la
make test-signal-loongarch     # 同一静态 musl ELF 的整数信号/恢复/重启对照
make test-pthread-loongarch    # 静态 TLS/线程、原 BusyBox ash 非交互 trap/wait
make test-pthread-oom-loongarch # 两处 clone 构造 OOM、真实 pthread EAGAIN/重试
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
rt_sigaction/rt_sigreturn 已接入共用信号策略和 LA 整数帧；布局、故障与重启契约见
[信号模块](kernel-signal.md)。尚无 FP/SIMD 扩展帧和 sigaltstack。

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
原 BAR/command 并归还 claim。`drivers/virtio/pci_block.c` 通过共用块核心完成
现代 PCI transport；总线访问宽度、read-to-clear ISR 和通知地址由 PCI 实现，
ECAM、uncached 映射及 INTx 来源由 `platform/loongarch_pci.c` 提供。BAR 地址
不能假设由固件分配：无 BIOS QEMU 上先 sizing/分配，再确认 reset，最后打开
bus-master，销毁按 stop DMA→摘 IRQ→释放队列→恢复 BAR/command 的 owner 顺序。

QEMU 平台采用128个bus的ECAM、低PCI memory aperture、根bus现代端点及
PCH-PIC→EXTIOI→CPU0/HWI0。资源 claim 有64个槽，耗尽明确失败；无桥、热插拔、
legacy PCI、MSI-X 或实板验证。控制器为共享level pin在逐设备ISR之间保持mask，
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
block，不通过后续磁盘或内存 fixture 掩盖启动错误。legacy PCI 明确 ENOTSUP，
数量超限 ENOSPC，损坏 ext4 保留后端 EUCLEAN。默认 `/init` 的 path/argv/envp
使用与 RV 相同的 `INIT_CONFIG=config/init.json` 生成方式，LA 有独立生成依赖。
PT_INTERP 暂未接入，返回 ENOEXEC，不发布半成品任务；此限制可在后续扩展。

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
动态DSO、socket和RV的未支持PI marker门禁保留在原目录，不能把这一静态选择集
称为原pthread全量测试。Linux对照发现并修正测试的join/PID摘除时序，以及WAKE
后目标可能退出的假设；内核不按测试输入特判。

clone的任务页、栈分配两处故障分别在两种RAM注入，真实pthread_create返回EAGAIN、
未执行半成品worker，任务/栈页立即恢复，随后成功create/join。此四组是BoarOS注入
证据，与同ELF Linux语义对照分别报告。每次正常、故障、OOM根启动收口要求
heap live/pages为0，所有可回收物理页、根盘/cache/PCI owner回到预热基线。

本阶段RV信号/syscall、完整架构、原静态/动态用户态、五种固定glibc、1366条ABI
和栈门禁均通过。此次未改文件映射/COW或块核心，不把此前SQLite/存储恢复矩阵
记为本轮重跑。FP/SIMD、动态musl/DSO TLS、LA完整终端/网络和比赛Harness仍缺。
