# BoarOS 文档中心

欢迎查阅 BoarOS 内核技术文档。本文档中心旨在系统化呈现 BoarOS 的系统架构、核心设计准则、子系统模块实现细节以及深度工程研究记录。

---

## 📌 核心指南

| 文档 | 说明 | 重点内容 |
| :--- | :--- | :--- |
| **[项目主页](../README.md)** | 内核总览与快速上手 | 内核调用路径、子系统设计、平台与启动命令 |
| **[设计与工程原则](design.md)** | 内核顶层设计哲学 | Linux ABI 兼容原则、内存与所有权模型、并发与锁策略、持久化边界 |
| **[工具链与环境](toolchain.md)** | 交叉编译器与模拟器环境 | GCC 工具链版本、QEMU 模拟器环境、目标依赖准备指南 |
| **[开发路线与能力现状](goals.md)** | 内核演进与功能支持 | 系统调用覆盖、应用生态支持、正在进行中的特性与已知瓶颈 |
| **[持续集成与验证](modules/continuous-integration.md)** | 自动化测试与质量门禁 | 测试分层、真实 Linux 应用验证环境与测试入口 |
| **[第三方组件说明](third-party.md)** | 依赖引入与开源许可 | 外部开源组件（lwIP、lwext4 等）的来源与授权协议 |

---

## 🧩 子系统设计手册

各子系统模块文档详细记录了核心数据结构、对外接口、关键执行流与边界约束：

### 1. 架构引导与硬件驱动
- **[RISC-V 引导机制](modules/riscv-boot.md)**：OpenSBI 固件接口、S-Mode 早期页表建立与内核入口接管。
- **[RISC-V 根盘启动](modules/riscv-root-boot.md)**：VirtIO 块设备初始化、根文件系统探测与首个用户进程 `init` 引导。
- **[RISC-V 多核 SMP](modules/riscv-smp.md)**：从核启动唤醒（HSM）、核间中断（IPI）及任务并行调度。
- **[LoongArch 启动与根盘](modules/loongarch-boot.md)**：LA464 16 KiB 页表初始化、PCI 总线探测与 ext4 根盘挂载。
- **[设备树（DTB）解析](modules/dtb-memory.md)**：扁平设备树解析、物理内存范围探测与保留区处理。
- **[VirtIO 设备框架](modules/virtio-framework.md)**：VirtIO 队列管理、MMIO 传输协议与中断通知机制。
- **[VirtIO 块设备驱动](modules/riscv-virtio-block.md)**：磁盘 I/O 调度、多扇区读写与异步请求完成处理。
- **[VirtIO 硬件随机数](modules/riscv-virtio-rng.md)**：高熵随机源接入与内核随机数池管理。
- **[串口与终端传输](modules/riscv-uart-tty.md)**：16550A 串口驱动与终端收发。

### 2. 异常控制与系统调用
- **[Trap 陷入处理机制](modules/riscv-trap.md)**：用户态到内核态异常切换、寄存器上下文保存与恢复。
- **[RISC-V 浮点支持](modules/riscv-fpu.md)**：FPU 状态延迟保存（Lazy Context Switch）与信号浮点上下文恢复。
- **[LoongArch 浮点与 SIMD](modules/loongarch-fpu.md)**：LA464 浮点及 LSX/LASX 向量指令集支持与动态切换。
- **[Linux 系统调用分发](modules/kernel-syscall.md)**：系统调用编号转换、参数校验、分发器实现与返回值规范。
- **[用户内存安全访问 (uaccess)](modules/kernel-uaccess.md)**：内核访问用户空间指针的安全边界、缺页保护与地址检查。
- **[BPF 网络过滤与系统调用跟踪](modules/kernel-bpf.md)**：共同校验器与解释器、map/bpffs、socket 过滤、tracepoint/perf 和原版工具。
- **[内核日志系统](modules/kernel-log.md)**：printk 实现、环形日志缓冲区与 `dmesg` 接口支持。

### 3. 内存管理体系
- **[物理页分配器 (Buddy)](modules/physical-pages.md)**：伙伴算法实现、页帧分配/释放与内存碎片合并。
- **[内核堆管理 (SLAB)](modules/kernel-heap.md)**：小对象缓存池、动态内存分配器与内存对齐保障。
- **[Sv39 分页映射](modules/riscv-sv39.md)**：三级页表结构、TLB 刷新栅障与地址翻译加速。
- **[虚拟内存管理 (VMM)](modules/kernel-mm.md)**：进程地址空间抽象、缺页中断处理与写时复制（CoW）。
- **[虚拟内存区域 (VMA)](modules/kernel-vma.md)**：`mmap` / `munmap` / `mprotect` 区间维护与权限控制。

### 4. 任务、调度与同步
- **[进程模型与 SMP 调度](modules/kernel-scheduler.md)**：Per-CPU 就绪队列、优先级时间片调度与进程状态流转。
- **[任务生命周期与执行](modules/kernel-exec.md)**：ELF 装载、`execve` 提交与旧资源回收。
- **[Futex 同步原语](learning/threads-and-futex.md)**：快速用户态互斥量、哈希等待队列与优先级继承。
- **[POSIX 信号处理](modules/kernel-signal.md)**：信号产生、掩码屏蔽、信号递送与蹦床回跳（Trampoline）。
- **[时钟与高精度定时器](modules/kernel-time.md)**：系统滴答时钟、软硬件定时器与 `nanosleep` / `clock_nanosleep`。
- **[资源记账与限制](modules/kernel-accounting.md)**：`getrusage` 统计、时间片计算与进程资源限额（rlimit）。
- **[TTY 与控制终端](modules/kernel-tty.md)**：会话管理、前台进程组、作业控制与标准输入输出重定向。

### 5. 虚拟文件系统与存储
- **[文件描述符与抽象](modules/kernel-files.md)**：FD 表、打开文件对象（OFD）、偏移量维护与路径查找。
- **[VFS 与 ext4 文件系统](modules/vfs-ext4.md)**：ext4 目录树、Extent 块映射、JBD2 事务日志与 Page Cache 页缓存。
- **[FAT32 / VFAT 文件系统](modules/fat32.md)**：FAT 表遍历、长文件名支持与磁盘镜像兼容。
- **[临时文件系统 (tmpfs)](modules/tmpfs.md)**：基于物理内存的轻量级文件系统实现。
- **[进程状态系统 (procfs)](modules/procfs.md)**：`/proc` 动态文件生成、进程状态查询与系统统计。

### 6. 网络与进程间通信 (IPC)
- **[内核网络协议栈](modules/kernel-network.md)**：lwIP 协议栈内核集成、以太网包收发与 Socket 状态机。
- **[VirtIO 网卡驱动](modules/riscv-virtio-net.md)**：VirtQueue 双环通信、收发缓冲池与链路状态管理。
- **[SysV 共享内存 (shm)](modules/sysv-shm.md)**：进程间大容量内存共享、权限控制与引用计数。
- **[SysV 信号量 (sem)](modules/sysv-sem.md)**：信号量集合、原子增减操作与撤销记录（SEM_UNDO）。
- **[SysV 消息队列 (msg)](modules/sysv-msg.md)**：类型化消息队列、阻塞收发与系统限额。
- **[POSIX 消息队列 (mqueue)](modules/posix-mqueue.md)**：优先级消息队列与文件描述符统一接口。

---

## 🔬 专题研究与深度分析

在 `docs/learning/` 目录下，记录了开发过程中的关键机制研究、真实应用适配经验与性能测量分析：

- **体系结构与硬件交互**：
  - [RISC-V 启动全流程解析](learning/riscv-boot.md)
  - [Trap 异常与系统调用现场分析](learning/riscv-traps.md)
  - [RISC-V 用户态隔离机制](learning/riscv-user-mode.md)
  - [LoongArch 平台 Bringup 记录](learning/loongarch-bringup.md)
- **并发、调度与性能基线**：
  - [内核调度器演进与设计](learning/kernel-scheduling.md)
  - [多核 SMP 任务交接与失效分析](learning/smp-handoff.md)
  - [Futex 机制与多线程同步](learning/threads-and-futex.md)
  - [CPU 资源记账与性能基准](learning/cpu-accounting-cost.md)
  - [单核规模扩展与瓶颈排查](learning/single-hart-scale.md)
- **存储、VFS 与持久化**：
  - [存储子系统与文件系统架构](learning/storage-filesystems.md)
  - [SQLite 记录锁与崩溃恢复测试](learning/record-lock-sqlite-recovery.md)
  - [内存文件系统多挂载验证](learning/memory-backed-mounts.md)
  - [文件时间戳精度与更新时机](learning/file-timestamps.md)
- **I/O 多路复用与网络**：
  - [I/O 多路复用机制演进](learning/io-multiplexing.md)
  - [epoll 子系统深度解析](learning/epoll-subsystem.md)
  - [网络对象所有权与多核并发模型](learning/network-ownership.md)
  - [数据路径性能预算与吞吐调优](learning/data-path-budget-experiments.md)
- **真实软件生态适配**：
  - [真实程序清单、测试基准与已知兼容性](learning/user-program-inventory.md)
  - [ELF 动态装载与重定位技术](learning/elf-loading.md)
  - [客体内原生编译工具链探针](learning/offline-toolchain-probe.md)
  - [原生 CI 运行环境配置](learning/ci-native-environment.md)
