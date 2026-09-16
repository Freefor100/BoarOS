# BoarOS

<p align="center">
  <img src="assets/boaros_header.png" alt="BoarOS 吉祥物与字标" width="100%">
</p>

BoarOS 是一个从零搭建、面向 OS Comp 能力建设，并以兼容 Linux 用户态 ABI 为最终功能目标的 C 语言（少量 Asm）内核。当前兼容子集以本文和模块文档列出的已验证能力为准。

## 当前实现

- `make all` 构建 RISC-V64 ELF `kernel-rv`。
- QEMU `virt` 加载默认 OpenSBI，随后以 S-mode 进入 BoarOS。
- 启动代码建立 `gp`、清零 BSS、创建单 hart 启动栈，并把 OpenSBI 的 hart ID 与 DTB 指针交给 C 入口。
- 启动代码安装 Direct-mode `stvec`；统一 Trap 入口让 S-mode 直接使用当前栈，并通过 `sscratch <-> tp` 为 U-mode 切到任务内核栈，保存完整整数 Trap Frame 后经过 C dispatcher 验证并执行 `sret`。生产 dispatcher 已处理 supervisor timer、U-mode ecall 和用户同步故障；instruction/load/store page fault 会先按当前 MM 的 VMA 策略解析，成功补页时保持 `sepc` 重试，其他未处理事件仍输出 CSR 现场后关机。
- 内核校验并扫描 DTB，读取第一段 RAM、静态保留区、RISC-V `timebase-frequency` 和经 `ranges` 翻译的可用 VirtIO MMIO transport，排除固件、内核镜像与 DTB 自身占用后形成启动内存布局。设备节点按物理地址排序，实际设备类型和 feature negotiation 留给驱动判断。
- 物理页分配器按 4 KiB 向内对齐可用区间，分页启动期使用顺序游标与回收链；进入高半区、绑定 direct-map 访问函数后，一次性导入既有所有权并切换到自托管 metadata 的 buddy 模式。现有单页接口在最终模式使用 order 0，另支持自然对齐的二次幂连续页，释放时按 buddy head 做常数时间摘链和逐级合并。内核堆在其上为 16..2048 字节对象提供 size-class slab，为大对象直接分配 buddy order，并统计 live bytes、分配次数和当前/峰值页数；空 slab 会归还物理页。
- RISC-V 内核 ELF 链接到 Sv39 高半区 `0xffffffff80000000`，QEMU 当前仍从物理地址 `0x80200000` 装载和进入；内核先用只覆盖切换所需低/高别名的过渡页表迁移 PC、栈、`gp` 和 `stvec`，再切换到只含高半区内核、从 `0xffffffc000000000` 开始的 128 GiB RAM direct map 和平台 MMIO 的最终页表。
- Sv39 建表器按条件组合 2 MiB 与 4 KiB 叶子；最终页表不保留低地址映射，QEMU `virt` UART 的物理 MMIO 通过 `0xffffffe000000000` 的 supervisor-only 高半区别名访问。运行期用户地址空间拥有低半区 4 KiB U 页和页表页、借用最终内核高半区根项，并以 ASID 0 切换 `satp`；MM 维护按地址排序的匿名与文件私有 VMA，支持 demand-zero 栈/heap/anonymous mmap 以及可读普通文件的按需映射，`O_WRONLY` OFD 返回 `-EACCES`。`fork` 共享物理叶子并把原可写页转为 COW，首次写 fault 在引用数为 1 时原地恢复，否则复制一页；`PROT_NONE` 也保留 exclusive/COW 所有权。`munmap`/fixed replace 按“PTE 失效、`SFENCE.VMA`、释放页”的顺序完成，不保留 retired PTE 或页回收计数。uaccess 与硬件 fault 共用匿名、文件和 COW 解析路径。
- 最终地址空间建立后，内核先把 boot context 初始化为 idle，再通过 SBI TIME 设置绝对 deadline，以 100 Hz 策略处理 supervisor timer interrupt；迟到时按原 deadline 相位一次补记 elapsed tick，并把同一 elapsed 交给 scheduler。
- RISC-V switch context 按 psABI 保存 `ra/sp/tp/s0..s11`，其中内核 `tp` 固定指向 current task。普通内核/用户任务各自拥有独立的 4 KiB 元数据页和 8 KiB 连续物理执行栈，保留 canary 与高水位观测；退出切换到可信栈后释放执行栈，zombie 只保留元数据；生产用户任务拥有 MM、文件表、fs context、身份和父子关系。单 hart FIFO scheduler 每 tick 最多抢占一次并把 elapsed 记为被中断任务的 user/kernel CPU 时间（idle 不记账），直接使用任务缓存的 `satp`，热路径不遍历进程树或执行资源生命周期操作。通用等待队列让任意任务阻塞在事件通道或 deadline 上：BLOCKED 任务都在全局 blocked 链，tick 路径先到期唤醒再抢占，`sched_yield(124)` 把当前任务排到 ready 队尾；wait4、console read、nanosleep 和 pipe read/write 可被信号唤醒，vfork 保持不可中断。wait4 经等待队列阻塞，子进程记账在回收时回卷；vfork 以 `kernel_mm_acquire` 共享父地址空间并挂起父进程直到子进程 exec/exit；已清掉重资源的子进程以 zombie 保留 PID、wait status 和任务页，父进程 wait 后最终回收。复合清理失败由 idle 按 owner 状态重试。
- Linux 风格系统调用边界显式接收当前调用任务，支持 `epoll_create1(20)/epoll_ctl(21)/epoll_pwait(22)`、`dup(23)/dup3(24)/fcntl(25)`、`mkdirat(34)`、`unlinkat(35)`、`ftruncate(46)`、`openat(56)`、`close(57)`、`pipe2(59)`、`getdents64(61)`、`lseek(62)`、`read(63)/readv(65)`、`write(64)/writev(66)`、`pselect6(72)`、`ppoll(73)`、`exit(93)`、`exit_group(94)`、`set_tid_address(96)`、`futex(98)`、`restart_syscall(128)`、`kill(129)/tkill(130)/tgkill(131)`、`rt_sigsuspend(133)`、`rt_sigaction(134)`、`rt_sigprocmask(135)`、`rt_sigpending(136)`、`rt_sigreturn(139)`、`uname(160)`、`getpid(172)`、`getppid(173)`、`gettid(178)`、`brk(214)`、`munmap(215)`、`clone(220)`、`execve(221)`、`mmap(222)`、`mprotect(226)`、`fstat(80)`、`newfstatat(79)` 和 `wait4(260)`，未知调用返回 `-ENOSYS`。mmap 当前覆盖 anonymous-private、regular-file private、hint/top-down、fixed/fixed-noreplace、`MAP_STACK` 和 `MAP_NORESERVE`；shared 与 populate 明确返回不支持。raw `brk` 保留 byte-granular 精确值，增长只登记 demand-zero heap VMA，缩小即使区间已打洞或分段也会撤销越界页。普通 fork/clone 子 MM 使用 COW，fd 表和 cwd 独立复制，OFD/offset 共享；pthread clone 共享 MM、fd 表、fs context 和信号 disposition。缺页越界分别产生 `SIGSEGV`/`SIGBUS` 形态终止状态，孙进程在中间父进程退出后 reparent 到 PID 1。TID/TGID 来自可回收的 1..32768 位图；标准信号和信号帧走真实用户态路径，已经接入 syscall restart 框架的 wait4、console、pipe 阻塞 I/O 与无超时 FUTEX_WAIT 支持 `SA_RESTART`；带超时 FUTEX_WAIT 遇用户 handler 返回 EINTR，没有 handler 时由 restart_syscall 保持原 deadline。
- 有界 ELF64 解析器通过带总长度的 `read_at` 来源一次解码并校验 ELF header 与 program header，内存和 VFS 文件共用同一语义；RISC-V 生产装载器固定 Sv39/4 KiB，接受非 PIE `ET_EXEC`、PIE/无解释器 `ET_DYN` 以及非递归 `PT_INTERP`，由不可变 source 保存解析结果和规范化 `PT_LOAD` 区间。ELF 页登记为专用 `ELF_PRIVATE` VMA：完整文件页从 page cache+COW 取得，边界页和 BSS 按需私有化、清零并在可执行时执行 `FENCE.I`。RISC-V image 同时建立随机或无种子确定性降级的 PIE/解释器、mmap、brk、栈和 vDSO 布局，以及含真实 `AT_PHDR`、`AT_ENTRY`、`AT_HWCAP`、可用时 `AT_RANDOM` 和 `AT_EXECFN` 的 Linux 形态 `argc/argv/envp/auxv` 初始栈。动态 musl 已在用户态完成重定位、额外 DSO 装载和 TLS；内核 `getrandom` 仍未实现。
- RISC-V QEMU 路径从 DTB transport 初始化第一个 VirtIO MMIO block device，同时支持 version 1 legacy 和 version 2 modern；两者共用一个 size 8 split queue、最多一个 outstanding request 和一秒轮询超时提供同步块 I/O（支持读与包含子扇区 bounce RMW 的写），并通过 feature 协商识别 `VIRTIO_BLK_F_RO` 只读降级。legacy 使用 4 KiB 对齐的连续 16 KiB 队列，modern 使用一页队列；默认 QEMU 和显式 `-global virtio-mmio.force-legacy=false` 都有测试入口。VFS 在 BoarOS 自有接口后私有接入 lwext4，把 raw whole-disk ext4 挂载为根（由底层块设备能力自动决定读写或只读挂载）；需要 journal recovery 的镜像以 `-EUCLEAN` 拒绝。挂载共享的 4 KiB 文件页缓存以 `(VFS node, page index)` 为键，哈希查找、LRU 回收，写入与截断执行 node 级精确失效；向下截断另以稳定 node–MM 登记撤销越界驻留 PTE（包含私有 COW），并按来源处理非对齐尾页，文件删除执行 mount 级保守失效以保证缓存一致性；物理页分配失败时进行一次有界回收重试；`read` 和 file-private fault 共用缓存页。进程文件表把 fd 槽与 open file description 分开，初始 32 槽、按倍数增长至 1024，保存独立 offset 与 `O_CLOEXEC`；支持 `openat` 标志解析（`O_RDONLY/O_WRONLY/O_RDWR/O_CREAT/O_EXCL/O_TRUNC/O_APPEND/O_NONBLOCK`），regular file/directory 保留 `O_NONBLOCK` 状态位但不伪造非阻塞磁盘 I/O，普通文件 `read/readv/pread` 对 `O_WRONLY` OFD 返回 `-EBADF`；支持常规文件写（`write/writev`）、新建、`mkdirat`、`unlinkat`（支持 `AT_REMOVEDIR`，非空目录返回 `-ENOTEMPTY`）与 `ftruncate`（支持不分配完整 gap 的向上 sparse 扩展），普通写可先 `lseek` 到 EOF 之后并只分配与调用者数据相交的逻辑块，所有 hole 读取为零；写打开与运行中可执行映像之间互斥拒绝（`-ETXTBSY`），只读挂载下修改操作准确返回 `-EROFS`。exec 提交时先逻辑摘除 CLOEXEC fd，普通 fd 与 offset 保持。root boot 把 console 绑定为 PID 1 的 fd 0/1/2，`write/writev` 经串口输出、console read 阻塞等待真实 UART 输入（tick 轮询唤醒），目录可打开并经 `getdents64` 枚举：lwext4 记录结束位置作为 `d_off` cookie，独立 open 的游标独立，dup/fork 共享 OFD 游标，顺序访问为 O(N)。`pipe2` 以两个 OFD 共享一个 64 KiB FIFO 环形缓冲，≤4 KiB 写保持原子，读者/写者耗尽分别产生 EOF/EPIPE+SIGPIPE，`O_NONBLOCK` 可由 pipe2 或 F_SETFL 切换。`dup/dup2/dup3/fcntl` 复用 OFD，`pselect6/ppoll` 提供多队列等待节点、钉住 OFD 生命周期与原子 sigmask 替换，`epoll_create1/epoll_ctl/epoll_pwait` 以 `(target_fd, description)` 键化监听项，拒绝常规文件与目录（`-EPERM`），有界 DFS 检测嵌套深度与环路（上限 4 层，`-ELOOP`），关中断安全注册就绪唤醒，支持 LT/ET/ONESHOT 触发、OFD 双向解链与 epfd 自身可组合 poll。`lseek/fstat/newfstatat` 覆盖常规查询以及 FIFO 的 `ESPIPE`/`S_IFIFO` 形态；filesystem stat 统一从活 inode 读取 dev/ino/mode/nlink/uid/gid/size/allocated blocks/blksize 和纳秒时间；create/read/write/truncate/unlink 按活 inode 更新，读取采用 relatime，unlink-but-open 可观察 `nlink=0`。fs context 当前借用根 mount、cwd 为 `/`。绝对路径忽略 dirfd，相对路径只支持 `AT_FDCWD`；offset 只提交实际复制到用户空间的字节。
- 生产内核打开并检查可执行普通文件 `/init`，从 VFS 来源装载为 PID 1，并把文件表、fs context 和 MM 一起交给任务。用户 `execve` 先在旧 MM 上完成路径打开、argv/envp 快照和新映像准备，再由 scheduler 切换 `satp`、替换 MM 和完整 Trap Frame；失败保持旧映像，成功不返回并保留 TGID、cwd 与非 CLOEXEC fd。非组长 exec 由调用线程接管原 TGID，释放旧 TID。根启动在发布 PID 1 前若发生真实文件系统、块 I/O 或对象清理错误，会把仍有 owner 的 root 状态保存在持久对象中；有限重试仍失败时报告并停止。分配器不变量错误直接 fatal。生产测试中的 PID 1 从真实根盘连续进入 `/init -> stage2 -> stage3`，第三段再创建并回收多组父子/孙进程；所有后代和 PID 1 回收后才卸载文件系统、复位设备、核对 heap/物理页基线并通过 SBI 关机。
- 自动测试除启动、物理页、分页、Trap、timer 和调度状态外，还验证物理页引用、页缓存命中/回收、fork COW 的复制与原地恢复、父子独立写、fd 表复制/OFD offset 共享、PID 分配、clone 双返回、PPID、wait selector、WNOHANG/阻塞唤醒、zombie、reparent、退出/故障 status 和 EFAULT 后回收。真实 ext4 `/init` 还验证两个 `MAP_PRIVATE` 别名、重复 fixed replace 后单一 OFD 来源释放、写隔离、文件尾页补零和越过 EOF 的 `SIGBUS`；真实静态 musl 用户程序额外验证 FP 抢占和 fork 继承、libc ucontext/sigreturn、sigsuspend mask 恢复、SIGCHLD 自动回收、vfork 一次性完成，以及 wait4/console/pipe、nanosleep 和 futex 各自承诺的 EINTR/SA_RESTART 语义、timed futex deadline、重启时重新比较 futex word、pipe 多等待者、EOF/EPIPE、writev 整体原子性、FIFO stat、动态 O_NONBLOCK 和 poll/select 多路复用。低内存 uaccess 入口验证缺页耗尽不升级为整机 fatal。释放不变量由 fatal-path 测试覆盖，真实 ext4 orphan/I/O 错误由 mount owner 覆盖；最终要求 exec/files/fs/MM/缓存页/用户页/任务页/TID、heap 和 mount 全部回到基线。

当前只支持 RISC-V64 单 hart、QEMU `virt`、Sv39/4 KiB 用户页、S/U 整数 Trap Frame、SBI timer/100 Hz tick、16 KiB boot 栈、独立双页任务内核栈与 FIFO 抢占、Linux 线程组、普通 fork/vfork 与共享资源线程 clone、标准信号/handler/信号帧、FP D 状态和 pipe，以及 raw whole-disk 可写/只读 ext4。时间子系统提供单调/实时时钟（启动时 goldfish RTC 墙钟）与 `clock_gettime/getres/gettimeofday/clock_nanosleep/nanosleep`，`sched_yield` 可用；首个编译器生成的真实用户程序（静态 musl）已作为 PID 1 在生产内核上运行 stdio、readdir、文件操作（含新建/写入/截断/创建删除目录）、时钟读取、真实睡眠、信号和 pipe（`make test-userland-riscv`）。root-init 链覆盖 `times`/`wait4` rusage/自定义栈 clone/vfork（共享地址空间 + exec 前后观察）。按需分页覆盖匿名栈、`brk` heap、private-anonymous mmap、可读普通文件的 `MAP_PRIVATE` 和 ELF source；RISC-V exec 的 PIE/解释器/无解释器 `ET_DYN`、独立布局随机化和无可信种子降级已进入生产路径，动态 musl PIE、额外 DSO、初始 TLS 和运行中 dlopen TLS 已通过真实入口；shebang、`getrandom` 与 glibc 运行时仍待实现或验证。尚无 shared mmap、内存承诺/`RLIMIT_DATA`、实时信号排队、`sigaltstack`、signalfd、SIGHUP 会话语义；文件层没有目录 fd、异步脏页写回（writeback）、read-ahead、symlink、分区表或并发访问；console read 阻塞等待 UART 输入（tick 轮询）。也没有 SMP 并发 unmap/COW、ASID 分配或 SUM 快路径。外部中断、SMP、开发板和 LoongArch64 仍未实现，完整比赛 Harness 仍会因缺少 `kernel-la` 失败。

线程/futex 当前边界：单 hart、256 桶 MM+地址 key，支持 WAIT/WAKE/REQUEUE；无 MAP_SHARED、PI futex、bitset、robust-list 回收或 clone3。exit 与 exit_group 已分离，线程组只向父进程产生一次最终 zombie；阻塞 I/O 持有 OFD，组终止沿原栈释放 owner。验证覆盖及成本见[调度模块](docs/modules/kernel-scheduler.md)和[线程组与 futex 学习总结](docs/learning/threads-and-futex.md)。

## 构建与运行

需要 RISC-V64 bare-metal GCC 与对应 binutils、GNU Make 和
`qemu-system-riscv64`。构建系统兼容 `riscv64-unknown-elf-` 与 Arch Linux
提供的 `riscv64-elf-` 工具前缀。

```sh
make all
make run-riscv
make test-riscv
make test-userland-riscv
make test-diff-abi-riscv
make test-elf-tail-riscv
make test-stack-usage
make inventory-userland-riscv
make test-lwext4-host
make test-references
```

`make test-diff-abi-riscv` 在固定 RISC-V Linux 与 BoarOS 上运行同一 raw-syscall 文件用例并严格比较，版本、协议、失败产物见[差分模块](docs/modules/differential-abi.md)。`make inventory-userland-riscv` 生成固定 BusyBox/libc-test 输入的[程序失败清单](docs/learning/user-program-inventory.md)，清单生成成功不表示所有程序通过。

`make test-riscv` 串行运行通用模块、RISC-V 架构边界、故障注入和真实 ext4 根启动回归；各模块的聚焦入口只在对应 `docs/modules/` 说明中维护。`make test-userland-riscv` 共用根盘构造和生产入口验证静态 musl 与动态 pthread 程序，`make test-lwext4-host` 验证宿主侧 lwext4 集成，`make test-references` 核对固定参考资料元数据。`make run-riscv` 不附加根盘，启动成功后持续在 timer-idle 中等待，需要由人退出 QEMU；`make debug-riscv` 使用 `-S -s` 在第一条 guest 指令前暂停并等待 GDB。

## 近期方向

静态 musl 和动态 PIE/解释器/DSO/TLS 已通过生产入口。当前线程组共享 MM/files/fs/disposition，支持 musl pthread create/join、竞争同步、取消、运行中 dlopen TLS，以及组长先退、非组长 exec 和 exit_group。可写 ext4 文件系统与 `mkdirat/unlinkat/ftruncate` 已完成闭环，文件部分写、跨 MM 截断驻留页和 relatime 时间更新已接入固定 RISC-V Linux 差分。真实程序环境已补齐：完整 398 applet BusyBox 与固定 `pre-2025` libc-test 均可构建，逐例双侧运行及原始脚本生成明确的失败清单；不能把有限基础命令的通过当作完整程序兼容。阶段仍以完整回归与生命周期审查收口为准，syscall 数量不是完成度。后续按内核能力依赖推进共享映射与同步扩展、设备抽象、devtmpfs、软硬链接以及 mount/procfs；不按测例选择路线。RISC-V64 + OpenSBI 主路径稳定后，实现 LoongArch64 16 KiB/三级页表和对应 context/trap；开发板到手后验证固件交接、DTB、设备、熵源与真实 TLB/中断性能。

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
- [VFS 与可写 ext4 模块](docs/modules/vfs-ext4.md)
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
- [Linux I/O 多路复用与等待架构学习总结](docs/learning/io-multiplexing.md)
- [Linux 线程组与 futex 学习总结](docs/learning/threads-and-futex.md)
- [Linux epoll 事件通知子系统学习总结](docs/learning/epoll-subsystem.md)
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
