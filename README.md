# BoarOS

<img src="assets/logo-concept.png" alt="BoarOS 正面野猪 Logo" width="180">

BoarOS 是一个从零搭建、面向 OS Comp 能力建设，并以兼容 Linux 用户态 ABI 为最终功能目标的 C 语言（少量 Asm）内核。当前兼容子集以本文和模块文档列出的已验证能力为准。

## 当前实现

- `make all` 构建 RISC-V64 ELF `kernel-rv`。
- QEMU `virt` 加载默认 OpenSBI，随后以 S-mode 进入 BoarOS。
- 启动代码建立 `gp`、清零 BSS、创建单 hart 启动栈，并把 OpenSBI 的 hart ID 与 DTB 指针交给 C 入口。
- 启动代码安装 Direct-mode `stvec`；统一 Trap 入口让 S-mode 直接使用当前栈，并通过 `sscratch <-> tp` 为 U-mode 切到任务内核栈，保存完整整数 Trap Frame 后经过 C dispatcher 验证并执行 `sret`。生产 dispatcher 已处理 supervisor timer、U-mode ecall 和用户同步故障；instruction/load/store page fault 会先按当前 MM 的 VMA 策略解析，成功补页时保持 `sepc` 重试，其他未处理事件仍输出 CSR 现场后关机。
- 内核校验并扫描 DTB，读取第一段 RAM、静态保留区、RISC-V `timebase-frequency` 和经 `ranges` 翻译的可用 VirtIO MMIO transport，排除固件、内核镜像与 DTB 自身占用后形成启动内存布局。设备节点按物理地址排序，实际设备类型和 feature negotiation 留给驱动判断。
- 物理页分配器按 4 KiB 向内对齐可用区间，分页启动期使用顺序游标与回收链；进入高半区、绑定 direct-map 访问函数后，一次性导入既有所有权并切换到自托管 metadata 的 buddy 模式。现有单页接口在最终模式使用 order 0，另支持自然对齐的二次幂连续页，释放时按 buddy head 做常数时间摘链和逐级合并。内核堆在其上为 16..2048 字节对象提供 size-class slab，为大对象直接分配 buddy order，并统计 live bytes、分配次数和当前/峰值页数；空 slab 会归还物理页。
- RISC-V 内核 ELF 链接到 Sv39 高半区 `0xffffffff80000000`，QEMU 当前仍从物理地址 `0x80200000` 装载和进入；内核先用只覆盖切换所需低/高别名的过渡页表迁移 PC、栈、`gp` 和 `stvec`，再切换到只含高半区内核、从 `0xffffffc000000000` 开始的 128 GiB RAM direct map 和平台 MMIO 的最终页表。
- Sv39 建表器按条件组合 2 MiB 与 4 KiB 叶子；最终页表不保留低地址映射，QEMU `virt` UART 的物理 MMIO 通过 `0xffffffe000000000` 的 supervisor-only 高半区别名访问。运行期用户地址空间拥有低半区 4 KiB U 页和页表页、借用最终内核高半区根项，并以 ASID 0 切换 `satp`；MM 维护按地址排序的匿名与文件私有 VMA，支持 demand-zero 栈/heap/anonymous mmap 以及只读普通文件的按需映射。`fork` 共享物理叶子并把原可写页转为 COW，首次写 fault 在引用数为 1 时原地恢复，否则复制一页；`PROT_NONE` 也保留 exclusive/COW 所有权。`munmap`/fixed replace 先撤销 PTE 并刷新 TLB，再释放页；释放暂时失败的页由 RSW retired PTE 保留 owner。uaccess 与硬件 fault 共用匿名、文件和 COW 解析路径。
- 最终地址空间建立后，内核先把 boot context 初始化为 idle，再通过 SBI TIME 设置绝对 deadline，以 100 Hz 策略处理 supervisor timer interrupt；迟到时按原 deadline 相位一次补记 elapsed tick，并把同一 elapsed 交给 scheduler。
- RISC-V switch context 按 psABI 保存 `ra/sp/tp/s0..s11`，其中内核 `tp` 固定指向 current task。普通内核/用户任务各使用一个私有 4 KiB 页承载控制块、canary 和内核栈；生产用户任务拥有 MM、文件表、fs context、身份和父子关系。单 hart FIFO scheduler 每 tick 最多抢占一次并把 elapsed 记为被中断任务的 user/kernel CPU 时间（idle 不记账），直接使用任务缓存的 `satp`，热路径不遍历进程树或执行资源生命周期操作。通用等待队列让任意任务阻塞在事件通道或 deadline 上：BLOCKED 任务都在全局 blocked 链，tick 路径先到期唤醒再抢占，`sched_yield(124)` 把当前任务排到 ready 队尾；wait4、console read、nanosleep 和 pipe read/write 可被信号唤醒，vfork 保持不可中断。wait4 经等待队列阻塞，子进程记账在回收时回卷；vfork 以 `kernel_mm_acquire` 共享父地址空间并挂起父进程直到子进程 exec/exit；已清掉重资源的子进程以 zombie 保留 PID、wait status 和任务页，父进程 wait 后最终回收。复合清理失败由 idle 按 owner 状态重试。
- Linux 风格系统调用边界显式接收当前调用任务，支持 `dup(23)/dup3(24)/fcntl(25)`、`openat(56)`、`close(57)`、`pipe2(59)`、`getdents64(61)`、`lseek(62)`、`read(63)`、`write(64)/writev(66)`、`exit(93)`、`exit_group(94)`、`set_tid_address(96)`、`restart_syscall(128)`、`kill(129)/tkill(130)/tgkill(131)`、`rt_sigsuspend(133)`、`rt_sigaction(134)`、`rt_sigprocmask(135)`、`rt_sigpending(136)`、`rt_sigreturn(139)`、`uname(160)`、`getpid(172)`、`getppid(173)`、`gettid(178)`、`brk(214)`、`munmap(215)`、普通进程 `clone(220)`、`execve(221)`、`mmap(222)`、`mprotect(226)`、`fstat(80)`、`newfstatat(79)` 和 `wait4(260)`，未知调用返回 `-ENOSYS`。mmap 当前覆盖 anonymous-private、regular-file private、hint/top-down、fixed/fixed-noreplace、`MAP_STACK` 和 `MAP_NORESERVE`；shared 与 populate 明确返回不支持。raw `brk` 保留 byte-granular 精确值，增长只登记 demand-zero heap VMA，缩小即使区间已打洞或分段也会撤销越界页。clone 当前只接受 `SIGCHLD` 进程形态：子 MM 使用 COW，fd 表和 cwd 独立复制，open file description/offset 共享；缺页越界分别产生 `SIGSEGV`/`SIGBUS` 形态终止状态，孙进程在中间父进程退出后 reparent 到 PID 1。每个进程仍是单成员线程组，TID/TGID 来自可回收的 1..32768 位图；标准信号、信号帧、`SA_RESTART` 和 pipe 的 EOF/EPIPE/O_NONBLOCK 均走真实用户态路径。
- 有界 ELF64 解析器通过带总长度的 `read_at` 来源解码并校验 ELF header 与 program header，内存和 VFS 文件共用同一语义；RISC-V 装载器支持静态、小端 ELF64 `ET_EXEC`，把 `PT_LOAD` 按页直接读入独立 Sv39 用户地址空间，按页合并权限并执行 W^X 检查、补零 BSS，再根据带长度的文件名和参数请求建立含 `AT_EXECFN` 的 Linux 形态 `argc/argv/envp/auxv` 初始栈。空间移入 MM 后，已物化的 ELF 页登记为 resident-required 匿名 VMA；最高 `PT_LOAD` 内存末端向上按页对齐后成为新映像的初始 program break，heap 上界为固定 RX VDSO 页下方；栈登记完整 8 MiB demand-zero reserve，初始映射参数区和 64 KiB headroom，其余页按首次用户访问提交，下方永久 guard 保持无 VMA/PTE。VDSO 页提供 RISC-V signal return 的 `li a7,139; ecall` 序列。动态链接、TLS 和随机数尚未支持。
- RISC-V QEMU 路径从 DTB transport 初始化第一个 VirtIO MMIO block device，同时支持 version 1 legacy 和 version 2 modern；两者共用一个 size 8 split queue、最多一个 outstanding request 和一秒轮询超时提供同步只读块 I/O。legacy 使用 4 KiB 对齐的连续 16 KiB 队列，modern 使用一页队列；默认 QEMU 和显式 `-global virtio-mmio.force-legacy=false` 都有测试入口。VFS 在 BoarOS 自有接口后私有接入 lwext4，把 raw whole-disk ext4 只读挂载为根；需要 journal recovery 的镜像以 `-EUCLEAN` 拒绝。挂载共享的 4 KiB 文件页缓存以 `(VFS node, page index)` 为键，哈希查找、LRU 回收，并在物理页分配失败时进行一次有界回收重试；`read` 和 file-private fault 共用缓存页。进程文件表把 fd 槽与 open file description 分开，初始 32 槽、按倍数增长至 1024，保存独立 offset 与 `O_CLOEXEC`；exec 提交时先逻辑摘除 CLOEXEC fd，普通 fd 与 offset 保持。root boot 把 console 绑定为 PID 1 的 fd 0/1/2，`write/writev` 经串口输出、console read 阻塞等待真实 UART 输入（tick 轮询唤醒），目录可打开并经 `getdents64` 枚举；`pipe2` 以两个 OFD 共享一个 64 KiB FIFO 环形缓冲，≤4 KiB 写保持原子，读者/写者耗尽分别产生 EOF/EPIPE+SIGPIPE，`O_NONBLOCK` 可由 pipe2 或 F_SETFL 切换。`dup/dup2/dup3/fcntl` 复用 OFD，`lseek/fstat/newfstatat` 覆盖常规查询以及 FIFO 的 `ESPIPE`/`S_IFIFO` 形态。fs context 当前借用根 mount、cwd 为 `/`。绝对路径忽略 dirfd，相对路径只支持 `AT_FDCWD`；offset 只提交实际复制到用户空间的字节。
- 生产内核打开并检查可执行普通文件 `/init`，从 VFS 来源装载为 PID 1，并把文件表、fs context 和 MM 一起交给任务。用户 `execve` 先在旧 MM 上完成路径打开、argv/envp 快照和新映像准备，再由 scheduler 切换 `satp`、替换 MM 和完整 Trap Frame；失败保持旧映像，成功不返回并保留 PID/TID、cwd 与非 CLOEXEC fd。根启动在发布 PID 1 前若发生可重试的 files/fs/MM/VMA 清理失败，会把未发布 owner 保存在持久 root 对象中并在关机前重试，而不是遗失栈上的 MM。生产测试中的 PID 1 从真实根盘连续进入 `/init -> stage2 -> stage3`，第三段再创建并回收多组父子/孙进程；所有后代和 PID 1 回收后才卸载文件系统、复位设备、核对 heap/物理页基线并通过 SBI 关机。
- 自动测试除启动、物理页、分页、Trap、timer 和调度状态外，还验证物理页引用、页缓存命中/回收、fork COW 的复制与原地恢复、父子独立写、fd 表复制/OFD offset 共享、PID 分配、clone 双返回、PPID、wait selector、WNOHANG/阻塞唤醒、zombie、reparent、退出/故障 status 和 EFAULT 后回收。真实 ext4 `/init` 还验证两个 `MAP_PRIVATE` 别名、重复 fixed replace 后单一 OFD 来源释放、写隔离、文件尾页补零和越过 EOF 的 `SIGBUS`；真实静态 musl 用户程序额外验证 FP 抢占和 fork 继承、libc ucontext/sigreturn、sigsuspend mask 恢复、SIGCHLD 自动回收、vfork 一次性完成，以及按 syscall 类别区分的 EINTR/SA_RESTART，以及 pipe 多等待者、EOF/EPIPE、writev 整体原子性、FIFO stat 和动态 O_NONBLOCK。低内存 uaccess 入口验证缺页耗尽不升级为整机 fatal。各资源层的访问/释放失败注入要求状态可重试且 owner 不丢失；最终要求 exec/files/fs/MM/缓存页/用户页/任务页/TID、heap 和 mount 全部回到基线。

当前只支持 RISC-V64 单 hart、QEMU `virt`、Sv39/4 KiB 用户页、S/U 整数 Trap Frame、SBI timer/100 Hz tick、16 KiB boot 栈、单页任务内核栈与 FIFO 抢占、单成员线程组、普通 `SIGCHLD` clone、标准信号/handler/信号帧、FP D 状态和 pipe，以及 raw whole-disk 只读 ext4 上的静态 `ET_EXEC`。时间子系统提供单调/实时时钟（启动时 goldfish RTC 墙钟）与 `clock_gettime/getres/gettimeofday/clock_nanosleep/nanosleep`，`sched_yield` 可用；首个编译器生成的真实用户程序（静态 musl）已作为 PID 1 在生产内核上运行 stdio、readdir、文件操作、时钟读取、真实睡眠、信号和 pipe（`make test-userland-riscv`）。root-init 链覆盖 `times`/`wait4` rusage/自定义栈 clone/vfork（共享地址空间 + exec 前后观察）。按需分页覆盖匿名栈、`brk` heap、private-anonymous mmap 和只读普通文件的 `MAP_PRIVATE`；fork 与文件缓存映射使用 COW。尚无 shared mmap、ASLR、内存承诺/`RLIMIT_DATA`、`CLONE_VM/CLONE_FILES/CLONE_THREAD`、futex、实时信号排队、`sigaltstack`、signalfd/epoll、SIGHUP 会话语义或多线程 exec 收拢；ELF 暂不支持 PIE/动态解释器、shebang、TLS 或 `execveat`。文件层没有目录 fd、共享整表、可写文件/writeback、read-ahead、symlink、分区表或并发访问；console read 阻塞等待 UART 输入（tick 轮询）。也没有 SMP 并发 unmap/COW、ASID 分配或 SUM 快路径。外部中断、SMP、开发板和 LoongArch64 也尚未实现，完整比赛 Harness 仍会因缺少 `kernel-la` 失败。

## 构建与运行

需要 RISC-V64 bare-metal GCC 与对应 binutils、GNU Make 和
`qemu-system-riscv64`。构建系统兼容 `riscv64-unknown-elf-` 与 Arch Linux
提供的 `riscv64-elf-` 工具前缀。

```sh
make all
make run-riscv
make test-riscv
make test-dtb-riscv
make test-page-riscv
make test-heap-riscv
make test-block-riscv
make test-vfs-riscv
make test-files-riscv
make test-context-riscv
make test-scheduler-cases-riscv
make test-scheduler-riscv
make test-syscall-riscv
make test-signal-riscv
make test-brk-riscv
make test-mmap-riscv
make test-uaccess-riscv
make test-elf64-riscv
make test-lwext4-host
make test-user-elf-cases-riscv
make test-user-elf-riscv
make test-root-init-riscv
make test-demand-page-riscv
make test-exec-riscv
make test-root-boot-cleanup-riscv
make test-mm-riscv
make test-vma-riscv
make test-user-riscv
make test-user-fatal-riscv
make test-sv39-riscv
make test-sv39-fault-riscv
make test-timer-riscv
make test-idle-riscv
make test-high-half-trap-riscv
make test-no-identity-riscv
make test-trap-riscv
make test-trap-return-riscv
make test-references
```

`make run-riscv` 不附加根盘，启动成功后持续在 timer-idle 中等待，需要由人退出 QEMU。`make test-root-init-riscv` 会建立真实 ext4 镜像，分别用默认 legacy 和显式 modern VirtIO MMIO 启动，把三个测试 ELF 和数据文件写入根盘，并验证连续 exec、参数栈、文件 syscall、file-private mmap/COW/SIGBUS、退出状态、资源回收和 SBI 关机；`make test-uaccess-oom-riscv` 在 64 MiB 真实根盘入口下验证 uaccess 缺页耗尽返回 `EFAULT` 而不 fatal，`make test-icache-riscv` 做装载/按需执行路径的 `FENCE.I` 对象检查。`make test-brk-riscv` 汇总 heap 生命周期验证，`make test-mmap-riscv` 汇总 Sv39/VMA/syscall 并让真实 `/init` ELF 验证匿名与文件私有映射、改权、替换、打洞和释放；`make test-vma-riscv` 还输出 1/64/1024 VMA 的 QEMU lookup 周期基线。`make test-demand-page-riscv` 让 PID 1 在初始栈提交区以下实际触发 load/store 缺页，并用测试内核注入 OOM；`make test-exec-riscv` 运行提交后旧 MM 首次释放失败的故障注入版本；`make test-root-boot-cleanup-riscv` 验证 VMA teardown 在根启动失败中可由持久 owner 重试。`make debug-riscv` 使用 `-S -s` 启动 QEMU：虚拟 CPU 会暂停并在宿主 TCP 端口 1234 等待 GDB，因此命令也不会自行返回。

## 近期方向

真实静态 musl 程序已在生产内核上闭环，文件、stdio、FP、信号与 pipe ABI 面已达到静态启动和基础 applet 的下限。近期方向按内核自身能力域推进：进程模型完备（clone 全语义、per-task 记账）、设备抽象与 devtmpfs、可写 ext4 与 mount/procfs；动态 ELF 依赖和多线程共享语义按后续真实用户程序需求排序。RISC-V64 + OpenSBI 主路径稳定后，实现 LoongArch64 16 KiB/三级页表和对应 context/trap；开发板到手后验证固件交接、DTB、设备与真实 TLB/中断性能。

## 文档

- [RISC-V 启动模块](docs/modules/riscv-boot.md)
- [RISC-V Trap 模块](docs/modules/riscv-trap.md)
- [RISC-V Timer 与内核 Tick 模块](docs/modules/riscv-timer.md)
- [内核调度与进程生命周期模块](docs/modules/kernel-scheduler.md)
- [内核信号模块](docs/modules/kernel-signal.md)
- [RISC-V 浮点状态模块](docs/modules/riscv-fpu.md)
- [系统调用解码模块](docs/modules/kernel-syscall.md)
- [DTB 与启动内存布局模块](docs/modules/dtb-memory.md)
- [物理页分配模块](docs/modules/physical-pages.md)
- [内核堆模块](docs/modules/kernel-heap.md)
- [RISC-V VirtIO MMIO 块设备模块](docs/modules/riscv-virtio-block.md)
- [VFS 与只读 ext4 模块](docs/modules/vfs-ext4.md)
- [进程文件资源模块](docs/modules/kernel-files.md)
- [RISC-V 根启动模块](docs/modules/riscv-root-boot.md)
- [RISC-V Sv39 分页模块](docs/modules/riscv-sv39.md)
- [内核 MM 模块](docs/modules/kernel-mm.md)
- [虚拟内存区域（VMA）模块](docs/modules/kernel-vma.md)
- [用户内存访问模块](docs/modules/kernel-uaccess.md)
- [用户 ELF64 装载模块](docs/modules/user-elf.md)
- [进程映像替换模块](docs/modules/kernel-exec.md)
- [RISC-V 启动学习总结](docs/learning/riscv-boot.md)
- [RISC-V Trap 学习总结](docs/learning/riscv-traps.md)
- [RISC-V 时间与周期 Tick 学习总结](docs/learning/riscv-time.md)
- [内核线程与抢占调度学习总结](docs/learning/kernel-scheduling.md)
- [进程生命周期学习总结](docs/learning/process-lifecycle.md)
- [内存管理学习总结](docs/learning/memory-management.md)
- [RISC-V 用户态与系统调用学习总结](docs/learning/riscv-user-mode.md)
- [ELF 用户程序装载学习总结](docs/learning/elf-loading.md)
- [存储与文件系统学习总结](docs/learning/storage-filesystems.md)
- [目标与边界](docs/goals.md)
- [设计与工程原则](docs/design.md)
- [工具链事实](docs/toolchain.md)
- [文档导航](docs/README.md)
- [参与开发](CONTRIBUTING.md)

`make references` 按固定版本恢复架构规范、QEMU/Linux/OpenSBI 源码、开发板
资料、比赛规则、Harness 和公开测例；实际快照不纳入版本控制，来源与校验值见
[本地参考资料](references/README.md)。恢复过程不会执行任何外部脚本。

## 公开参考

- [OS Comp 2026 规则](https://gitlab.eduxiji.net/csc1/csc-os/os2026)
- [OS Comp 内核公开测例](https://github.com/oscomp/testsuits-for-oskernel)
- [OS Comp 自动测试 Harness](https://github.com/oscomp/autotest-for-oskernel)
