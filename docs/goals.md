# 目标与边界

## 项目目标

BoarOS 是一个从零学习并面向 OS Comp 能力建设的 C + 汇编内核，首要功能目标是兼容 Linux 用户态 ABI。开发过程同时检验一种轻量的人—Agent 协作方式：人掌握路线和关键实现，Agent 提供调查、解释、候选方案和有边界的执行。

内核以 RISC-V64 和 LoongArch64 为主要架构方向，目标是在已支持的平台配置上运行未经 BoarOS 特改的 Linux 用户态程序。兼容范围是用户能够观察到的契约：系统调用号和调用约定、返回值与 errno、部分成功、ABI 数据结构和对齐、ELF 进程初始状态，以及内存、文件、进程、线程、信号、时间、并发和资源生命周期语义。内部源码结构、内核模块 ABI、Linux 驱动接口及没有用户态契约的实现细节不属于兼容目标。

Linux ABI 兼容是最终功能方向，不是对当前完成度的声明；README 和模块文档只能列出已经实现并验证的子集。双架构、开发板与多核正确性、可调试性和有竞争力的性能是并列质量目标，不能以通过少量测例代替。项目接受 OS Comp 公开测例、LTP 和真实组合负载的检验；比赛资料用于观察通用能力趋势，不用于猜测未来赛题或编写测试特判。

## 质量目标

- **语义真实**：实现可解释的内核机制，不只产生期望输出。
- **简单可读**：选择当前足够的设计，避免提前泛化和多层转发。
- **可验证**：行为有最小可重复证据，失败结果也被准确记录。
- **可演进**：边界清楚，需求出现时能局部调整；可演进不等于提前实现。
- **可理解**：开发者能解释关键代码路径、不变量和调试方法。

## 已确认的能力与依赖清单

能力按可复用的纵向闭环推进。每个能力组都要有真实入口、正常与失败语义、所有权和回收证据；syscall 数量和单个测例通过仅作辅助统计。

| 能力组 | 已确认范围与依赖 | 当前状态 |
|---|---|---|
| 启动与内存 | DTB 内存/保留区和设备发现、启动布局、物理页分配、内核堆、RISC-V Sv39/4 KiB、direct map | RISC-V QEMU 已验证 |
| 文件与根启动 | VirtIO MMIO version 1 legacy 与 version 2 modern、块读写 I/O（含子扇区 bounce RMW 与 VIRTIO_BLK_F_RO 只读协商）、lwext4 读写适配、VFS、文件页缓存及失效、可写/只读根挂载、source-backed ELF `/init`（ET_EXEC/ET_DYN/非递归 `PT_INTERP`） | RISC-V QEMU 已验证可写与只读真实入口；未完成的 ext4 orphan 回收由 mount owner 保留，真实 I/O 清理错误有独立失败路径 |
| 进程与基础 syscall | `epoll_create1/epoll_ctl/epoll_pwait`、`mkdirat/unlinkat/ftruncate`、`openat/read/write/writev/close`、`pselect6/ppoll`、`lseek/fstat/newfstatat/getdents64`、`dup/dup3/fcntl`、`mmap/mprotect/munmap/brk`、`clone`（自定义子栈、vfork 共享地址空间和线程资源共享）/`execve`/`wait4`（rusage）/`exit_group`/`set_tid_address`、`times`、`sched_yield`、COW、阻塞 wait、zombie/reparent、`uname` | 已实现并按模块和真实根盘链路验证，覆盖仍是明确子集；内部 dup2 操作不代表 RISC-V 存在独立 dup2 syscall |
| 阻塞唤醒与时间 | 通用等待队列（事件通道 + deadline）、全局 blocked 链不变量、`clock_gettime/clock_getres/gettimeofday`（REALTIME/MONOTONIC）、`clock_nanosleep/nanosleep`、goldfish RTC 启动墙钟、console read 阻塞等待 UART 输入、标准信号驱动唤醒与按 syscall 分类的重启 | 已实现并经 scheduler 单测与 musl 真实睡眠/时钟/信号闭环验证；`times` 与每任务记账已落地，PLIC 驱动接收仍未实现 |
| 目录枚举成本修正 | OFD 拥有可继续游标，保持独立 open、dup/fork 共享、cookie/seek、部分复制及关闭回收语义 | 已实现并由真实 ext4/QEMU 文件测试验证；当前线性适配器顺序条目访问为 O(N)，开发板吞吐基线待测，见[文件模块](modules/kernel-files.md#lseekfstatnewfstatat-与-getdents64) |
| 用户态映像 | Linux 形态初始栈和 auxv、source-backed `ELF_PRIVATE` demand paging、`ET_EXEC`/`ET_DYN`/`PT_INTERP`、Sv39 ASLR/W^X/指令同步 | RISC-V 构造和静态入口已验证；真实动态 musl PIE、解释器、额外 DSO、初始 TLS 和运行中 dlopen TLS 已由用户态入口验证，其他动态 libc/DSO 矩阵仍待补 |
| 其他架构与平台 | LoongArch64 2K1000LA 的 16 KiB/三级页表；VisionFive 2 的 RISC-V 板级启动与设备/DMA 边界 | 目标已确认，开发板实机验证和 LoongArch 物化器尚未完成 |
| 后续 Linux 能力组 | 更广的动态 libc/DSO 兼容、设备文件系统（devtmpfs）、软硬链接（linkat/symlinkat）、实时信号排队与 `sigaltstack`/signalfd、线程组与共享资源的进一步扩展、外部中断、SMP、异步脏页写回及更多设备 transport | 属于长期 ABI 路线，按真实用户程序和依赖推进，不能伪装成当前已支持 |

“所有 syscall”必须绑定架构、内核版本和功能范围；未实现或仅返回 `ENOSYS` 的调用不计作完成。官方测例、未特改用户程序和 LTP 用来发现缺口和防止回归，不反向定义内核的全部语义。

## 下一阶段顺序

- differential ABI harness MVP 已落地：`make test-diff-abi-riscv` 将同一 raw-syscall ELF 分别运行于固定 commit 构建的 RISC-V Linux 与 BoarOS，严格比较返回值、errno、数据、长度、偏移与 wait status；缺失结果、超时或差异返回非零。CI 独立运行并保留日志与失败镜像。后续按 OFD/open/stat、poll/epoll、signal/futex/time、mmap/truncate 扩展，不在 C 源码硬编码 reference 输出。
- 用 differential 证据决定 open/openat 对任意 unknown flag bits 的兼容策略；固定 Linux 会忽略一部分未知 bit，不能把“一律拒绝”直接写成 ABI 要求。
- ext4 timestamp mutation 仍是独立目标，需要定义 create/read/write/truncate/unlink 的 atime/mtime/ctime 更新。sparse ftruncate 与越过 EOF 写入已经用 inode-size/hole 能力替换逐段写零增长，并由真实 `st_blocks`、aligned/unaligned zero read 和卸载后 `e2fsck` 验证。
- truncate-to-resident-mapping invalidation 已用稳定 node–MM 登记和驻留页来源记录闭环：涵盖 private COW、PROT_NONE、非当前 MM、尾页、fork、关闭 fd/unlink、O_TRUNC 和重新增长；真实 RISC-V Linux 差分与分配失败回滚回归共同验收。仍不包含 MAP_SHARED 或 SMP 同步。
- 真实 userspace 按 musl libc-test failure inventory、BusyBox、pthread stress、SQLite、Git 推进。每轮按 failure cluster、最小复现、Linux differential、kernel primitive regression、重跑 workload 闭环；SQLite 的验收包含事务、journal、并发 reader/writer、重开和 integrity_check。
- task metadata 与 kernel stack 分离为 SMP 前置目标，迁移现有 canary 并增加 high-water 和 frame/stack usage 检查，栈大小依据测量决定。SMP 只在 differential ABI、真实 userspace、task/stack 分离及同步协议准备完成后开始；multi-hart boot 只是第一个检查点。

## 规划假设与边界

- `final-2025` 作为明年初赛沿用的规划假设，除非组委会正式公布，不写成已确认规则；`final-2026` 和本地保存的测例仓库用于观察今年真实负载与接口趋势。
- QEMU、VisionFive 2 和 LoongArch 2K1000LA 的 RAM、DTB、MMIO、DMA 一致性和中断拓扑属于平台事实。通用 VFS、文件、ELF 和 syscall 语义不因平台复制；架构页表、trap、context 和设备后端在真实第二个实现点出现时隔离。
- 当前单 hart、同步轮询和动态兼容矩阵仍是已记录的边界，不是长期目标的替代品。RISC-V source-backed `ET_EXEC`/`ET_DYN`、`PT_INTERP`、ASLR 构造，以及真实动态 musl 的重定位、额外 DSO、TLS 入口已经验证；更广的动态 libc、共享 MM、SMP 和写回仍需补齐同步、引用和失败协议。可写与只读 ext4 都已有真实入口，分配器释放不变量错误按 fatal 处理，真实文件系统 I/O 清理错误才保留 owner。
- 官方完整 Harness 在缺少 `kernel-la` 或其他未实现能力时可以阻塞；记录阻塞原因，不把预期缺口报告成已验证回归。

## 工程与资料边界

- 不建立自研 Agent 编排平台、会话数据库、Hook 或文档生成系统。
- 不保存原始 Agent 对话来代替代码、测试和必要记录；Harness/skill 的临时 plan 默认不进入仓库。
- 不因“以后可能有用”引入框架、依赖或第三方源码。需要第三方实现时，必须记录来源、版本、许可证和实际消费者。

许可证和正式发布策略仍待决定。
