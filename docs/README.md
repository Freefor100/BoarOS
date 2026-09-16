# 文档导航

| 入口 | 唯一职责 |
|---|---|
| [README](../README.md) | 已验证能力摘要、运行命令、近期方向 |
| [目标与 TODO](goals.md) | 优先队列、阶段依赖、阻塞、验收和待确认路线 |
| [设计原则](design.md) | 长期工程选择；协作/提交规则见 [AGENTS](../AGENTS.md)、[CONTRIBUTING](../CONTRIBUTING.md) |
| [工具链](toolchain.md)、[固定资料](../references/README.md)、[第三方](third-party.md) | 环境、版本、来源和许可 |

## 模块契约

记录入口、接口、不变量、所有权、限制和聚焦验证命令；源码与验证冲突时修正文档。

| 子系统 | 模块 |
|---|---|
| 启动 / 平台 | [启动](modules/riscv-boot.md)、[DTB](modules/dtb-memory.md)、[根启动](modules/riscv-root-boot.md) |
| Trap / 执行 | [Trap](modules/riscv-trap.md)、[浮点](modules/riscv-fpu.md)、[syscall](modules/kernel-syscall.md) |
| 内存 | [物理页](modules/physical-pages.md)、[堆](modules/kernel-heap.md)、[Sv39](modules/riscv-sv39.md)、[MM](modules/kernel-mm.md)、[VMA](modules/kernel-vma.md)、[uaccess](modules/kernel-uaccess.md) |
| 用户映像 | [ELF](modules/user-elf.md)、[exec](modules/kernel-exec.md) |
| 任务 / 时间 | [调度与生命周期](modules/kernel-scheduler.md)、[信号](modules/kernel-signal.md)、[时钟与睡眠](modules/kernel-time.md)、[硬件 timer](modules/riscv-timer.md) |
| 文件 / 存储 | [fd/OFD 与路径](modules/kernel-files.md)、[VFS/ext4/页缓存](modules/vfs-ext4.md)、[VirtIO block](modules/riscv-virtio-block.md) |
| 验证设施 | [Linux 差分](modules/differential-abi.md)、[真实程序环境](modules/program-environment.md) |

## 学习与证据

记录可复用背景、固定资料依据、已确认取舍和调试经验；不重复完整能力清单，不保存 TODO、临时计划或逐提交流水账。

| 主题 | 材料 |
|---|---|
| 架构基础 | [启动](learning/riscv-boot.md)、[Trap](learning/riscv-traps.md)、[用户态](learning/riscv-user-mode.md)、[时间](learning/riscv-time.md) |
| 内存 / ELF | [内存管理](learning/memory-management.md)、[ELF 装载](learning/elf-loading.md) |
| 进程 / 并发 | [调度](learning/kernel-scheduling.md)、[生命周期](learning/process-lifecycle.md)、[线程与 futex](learning/threads-and-futex.md) |
| 文件 / 事件 | [存储](learning/storage-filesystems.md)、[时间戳](learning/file-timestamps.md)、[I/O 多路复用](learning/io-multiplexing.md)、[epoll](learning/epoll-subsystem.md) |
| 真实程序 | [最近全量基线、阻塞与根因](learning/user-program-inventory.md) |

每个可独立验证的阶段收口时检查 README、模块、learning 三类文档：有新事实才更新，旧结论直接替换，细节用链接引用。运行产物留在忽略的 `build/`；临时 plan/spec 与会话材料不入库。
