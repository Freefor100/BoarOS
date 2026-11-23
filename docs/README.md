# BoarOS 文档中心

本页面汇总 BoarOS 的全部设计文档、性能优化专题、验证体系说明与子系统技术手册。文档按阅读深度分为**推荐阅读路线**、**核心设计支柱**、**子系统技术手册**以及**专题剖析与实验工具**四个层次。

---

## 推荐阅读路线

- **通用内核与快速启动**见 [main 分支说明](https://github.com/Freefor100/BoarOS/tree/main)。
- **官方评测**见[分支主页](../README.md)、[评测环境适配](modules/oscomp-compat.md)与[测例问题](learning/oscomp-test-issues.md)。
- **核心设计与工程主线（推荐按序阅读）**：
  1. [系统总体架构与核心设计](design.md) — 双架构统一抽象、两级分层锁序、跨核回收与持久化契约
  2. [性能工程与关键优化](performance.md) — `COST_DIAGNOSTICS` 归因方法论与五大子系统代表性优化
  3. [正确性保证与四层验证体系](verification.md) — 宿主并发故障注入、Linux 差分验证、229 真实程序与离线自编译
- **环境搭建与参与开发**：阅读 [工具链与环境指南](toolchain.md) 与 [参与贡献指南](../CONTRIBUTING.md)。

---

## 核心支柱文档

| 文档 | 核心主题 |
| :--- | :--- |
| **[评测分支主页](../README.md)** | 双架构交付、评测镜像与本地运行入口 |
| **[系统总体架构与核心设计](design.md)** | 双架构构建期绑定、多核执行分工、两级锁序模型、对象生命周期与页缓存持久化边界 |
| **[性能工程与关键优化](performance.md)** | 内置成本归因体系与调度、虚拟内存、文件系统、多核网络和驱动层的核心优化 |
| **[正确性保证与四层验证体系](verification.md)** | 宿主并发测试、双架构客机回归、同 ELF 上游 Linux 差分验证与真实生态闭环 |
| **[工具链与环境指南](toolchain.md)** | RISC-V / LoongArch 交叉编译环境、QEMU 模拟器、离线 `pahole` 与 Docker 开发镜像 |
| **[演进路线与待完成项](goals.md)** | 当前多核深化方向、尚未实现的功能缺口与真实开发板移植计划 |
| **[第三方组件与许可](third-party.md)** | `lwIP`、`lwext4`、`BLAKE2s`、`pahole`、`strace` 等外部开源组件的来源、修改边界与协议 |

---

## 子系统技术手册

### 1. 架构引导、硬件抽象与外设驱动
- **[RISC-V 64 引导机制](modules/riscv-boot.md)**：OpenSBI 固件接管、早期 Sv39 页表建立与高半区内核跳转。
- **[RISC-V 64 根盘启动](modules/riscv-root-boot.md)**：VirtIO 块设备探测、根文件系统挂载与首个用户进程引导。
- **[RISC-V 64 多核 SMP 执行](modules/riscv-smp.md)**：从核唤醒（OpenSBI HSM）、核间中断（IPI）、跨核执行边界与压力组合验证。
- **[RISC-V 64 硬件定时器](modules/riscv-timer.md)**：每核架构计时器编程、SBI 期限设置与事件缓存复用。
- **[LoongArch 64 启动与多核](modules/loongarch-boot.md)**：LA464 16 KiB 页表、PCI/PCIe 枚举、从核 IOCSR 邮箱引导与根盘启动。
- **[扁平设备树（DTB）与物理内存探测](modules/dtb-memory.md)**：设备树解析、物理内存区间与保留内存处理。
- **[GUID 分区表（GPT）解析](modules/gpt.md)**：磁盘分区表校验、分区偏移与块设备分区视图映射。
- **[VirtIO 通用设备框架](modules/virtio-framework.md)**：虚拟队列（Virtqueue）环形缓冲、MMIO/PCI 传输抽象与中断通知。
- **[VirtIO 块设备驱动](modules/riscv-virtio-block.md)**：多核块请求提交、多扇区读写、`FLUSH` 持久化屏障与超时处理。
- **[VirtIO 网卡驱动](modules/riscv-virtio-net.md)**：收发双环管理、描述符批量发布、中断合并与链路配置。
- **[VirtIO 硬件随机数驱动](modules/riscv-virtio-rng.md)**：硬件熵源接入与内核随机池补熵机制。
- **[16550A 串口与终端传输](modules/riscv-uart-tty.md)**：串口中断驱动与控制台字节流收发。

### 2. 异常陷入、系统调用与可观测性
- **[RISC-V 陷入与异常分发](modules/riscv-trap.md)**：用户态与内核态陷入切换、寄存器现场保存恢复与异常分流。
- **[RISC-V 浮点与向量上下文](modules/riscv-fpu.md)**：F/D 双精度浮点延迟切换、RVV 向量扩展与信号浮点上下文。
- **[LoongArch 浮点与 SIMD 向量](modules/loongarch-fpu.md)**：LA464 浮点及 LSX/LASX 向量指令集状态切换。
- **[Linux 系统调用统一分派](modules/kernel-syscall.md)**：双架构系统调用号分发、参数规范、异步 I/O 与标量查询快路径。
- **[用户内存安全访问（uaccess）](modules/kernel-uaccess.md)**：内核读写用户态指针的安全边界、异常表恢复与页固定借用。
- **[eBPF 网络过滤与系统调用跟踪](modules/kernel-bpf.md)**：双架构共用校验器与解释器、Map/RingBuf/bpffs、自生成 BTF 与 `raw_syscalls` 跟踪。
- **[内核成本观测模块（COST_DIAGNOSTICS）](modules/kernel-cost.md)**：内置性能归因窗口控制、指标定义、调用链分析与直方图快照。
- **[内核日志系统](modules/kernel-log.md)**：`printk` 格式化输出、环形日志缓冲区与 `dmesg` 读取接口。

### 3. 内存管理体系（VMM）
- **[物理页帧伙伴分配器（Buddy）](modules/physical-pages.md)**：多阶连续物理页管理、合并拆分与内存压力回收钩子。
- **[内核细粒度堆分配器（Slab）](modules/kernel-heap.md)**：小对象档位缓存池、批量槽分配与对齐保障。
- **[RISC-V Sv39 三级分页映射](modules/riscv-sv39.md)**：三级页表结构、页表项权限编码与跨核 TLB 失效栅障。
- **[虚拟内存管理（VMM）](modules/kernel-mm.md)**：地址空间分层并发协议（`range_claim`）、按需调页、写时复制、批量页表发布与 `mlock`/`madvise`。
- **[虚拟内存区域（VMA）管理](modules/kernel-vma.md)**：有序地址区间维护、二分查找、`mmap`/`munmap`/`mprotect` 区间拆分与合并。
- **[原生大页与复合页（Hugepages）](modules/hugepages.md)**：RISC-V 2 MiB 与 LoongArch 32 MiB 大页映射、预留配额池与写时复制语义。

### 4. 进程模型、调度、同步与终端
- **[进程模型、SMP 调度与同步原语](modules/kernel-scheduler.md)**：每核就绪队列、公平/实时调度与带宽限流、等待令牌、可睡眠锁、Futex 与用户凭据。
- **[程序装载与映像替换（execve）](modules/kernel-exec.md)**：新地址空间构造、原子提交切换与旧资源清理。
- **[用户态 ELF 解析与装载](modules/user-elf.md)**：静态与动态 ELF 程序头解析、解释器加载、地址空间随机化与辅助向量构造。
- **[POSIX 信号处理](modules/kernel-signal.md)**：经典与实时信号投递、信号掩码、专用信号栈、蹦床返回与系统调用自动重启。
- **[进程调试跟踪（ptrace）](modules/kernel-ptrace.md)**：被跟踪进程停止与恢复、系统调用/信号拦截、原生寄存器与调试内存读写。
- **[PID 命名空间](modules/pid-namespace.md)**：层级进程标识映射、命名空间初始化进程与可见性隔离。
- **[时钟与高精度定时器](modules/kernel-time.md)**：单调/实时/粗粒度时钟源、`nanosleep`、POSIX 定时器与 `timerfd`。
- **[资源记账与限额（rlimit / rusage）](modules/kernel-accounting.md)**：进程/线程 CPU 与 I/O 统计、资源使用上限控制。
- **[TTY 与作业控制终端](modules/kernel-tty.md)**：会话管理、前台进程组、伪终端（PTY）与行规程处理。

### 5. 虚拟文件系统（VFS）与存储子系统
- **[文件描述符、打开文件对象与 I/O](modules/kernel-files.md)**：进程 FD 表、共享打开文件描述（OFD）、管道、`epoll`/`poll`、`fanotify` 与记录锁。
- **[VFS、ext4 文件系统与 OverlayFS](modules/vfs-ext4.md)**：目录项与页缓存、多核目录并行、JBD2 事务日志、延迟分配、直接 I/O、POSIX ACL、文件句柄与可写叠加文件系统。
- **[FAT32 / VFAT 文件系统](modules/fat32.md)**：簇链管理、长文件名编码与外部磁盘镜像兼容。
- **[内存文件系统（tmpfs）](modules/tmpfs.md)**：基于物理内存的临时文件存储、空间配额与稀疏空洞语义。
- **[进程状态文件系统（procfs）](modules/procfs.md)**：`/proc` 动态进程快照、内存映射视图、系统控制参数与文件描述符信息。

### 6. 网络协议栈与进程间通信（IPC）
- **[多核网络协议栈与套接字](modules/kernel-network.md)**：lwIP 深度集成、多核 TCP 连接分片、ACK 合并、路由快照、IPv4/IPv6 双栈、组播、`AF_UNIX` 与 `TPACKET_V3`。
- **[网络路由与配置接口（rtnetlink）](network-rtnetlink.md)**：`AF_NETLINK` 协议套接字、接口/地址查询与运行时网络参数配置。
- **[IPC 命名空间](modules/ipc-namespace.md)**：System V 与 POSIX 进程间通信对象的命名空间隔离与生命周期。
- **[System V 共享内存（shm）](modules/sysv-shm.md)**：跨进程共享内存段、内存锁定配额与大页支持。
- **[System V 信号量（sem）](modules/sysv-sem.md)**：信号量集合原子操作、阻塞等待与进程异常退出自动撤销（`SEM_UNDO`）。
- **[System V 消息队列（msg）](modules/sysv-msg.md)**：按类型筛选的内核消息队列、阻塞收发与容量限制。
- **[POSIX 消息队列（mqueue）](modules/posix-mqueue.md)**：按优先级投递的消息队列、文件描述符统一接口与异步通知。

---

## 专题剖析、验证与实验工具

- **[Linux 双架构 ABI 差分验证](modules/differential-abi.md)**：同 ELF 对比参考 Linux 的 1,744 项主清单系统调用差分框架与规范化协议。
- **[用户程序构建与离线自编译环境](modules/program-environment.md)**：完整上游应用（BusyBox、musl/glibc、Alpine 离线编译器）的构建与核验流程。
- **[epoll 对象与事件通知机制](learning/epoll-subsystem.md)**：监听对象登记身份、环形嵌套图检查与事件通知关系。
- **[epoll 事件交付与并发取消](learning/epoll-delivery.md)**：用户空间结果拷贝提交、并发扫描与对象生命周期协调。
- **[文件时间戳语义](learning/file-timestamps.md)**：`atime`/`mtime`/`ctime` 更新规则、`relatime` 策略与错误边界。
- **[proc 控制文件读写契约](learning/proc-controls.md)**：短读、文件定位（`seek`）、分散拷贝故障与参数原子提交。
- **[内核随机源设计](learning/random-source.md)**：可信硬件熵注入、BLAKE2s 混种与字符设备协议。
- **[CPU 记账测量方法](learning/cpu-accounting-cost.md)**：固定工作量下的定时器开销、CPU 记账成本与扩展性观察。
- **[存储与网络数据路径实验工具](learning/data-path-budget-experiments.md)**：固定工作量性能对照脚本、参数配置与完成边界口径。
