# 文档导航

文档只保存能减少后续理解和决策成本的事实。代码、测试和 Git 历史已经表达清楚的内容不再复制。

| 文档 | 职责 |
|---|---|
| [目标与边界](goals.md) | 项目方向、质量目标和暂不处理的事项 |
| [设计与工程原则](design.md) | 人—Agent 协作边界与默认技术原则 |
| [工具链事实](toolchain.md) | 已验证的本地工具、Harness 和构建输入 |
| [RISC-V 启动模块](modules/riscv-boot.md) | 当前启动代码的入口、契约、不变量和限制 |
| [RISC-V Trap 模块](modules/riscv-trap.md) | S/U-mode Trap Frame、换栈、返回、诊断契约和测试 |
| [RISC-V Timer 与内核 Tick 模块](modules/riscv-timer.md) | DTB timebase、SBI TIME、deadline、生产 timer trap 和 tick 契约 |
| [内核线程调度模块](modules/kernel-scheduler.md) | RISC-V switch context、单 hart FIFO 抢占、任务身份、MM 接管和退出回收契约 |
| [系统调用解码模块](modules/kernel-syscall.md) | 显式调用任务、exit/uname/getpid/gettid 与未知系统调用契约 |
| [DTB 与启动内存布局模块](modules/dtb-memory.md) | RAM、静态保留区与可用物理区间的接口和限制 |
| [物理页分配模块](modules/physical-pages.md) | 构建期页粒度、单页分配/释放契约和限制 |
| [RISC-V Sv39 分页模块](modules/riscv-sv39.md) | 启动建表、运行期用户根、SATP 切换、所有权和失败语义 |
| [内核 MM 模块](modules/kernel-mm.md) | 跨架构 MM 句柄、RISC-V Sv39 后端、共享引用和可重试回收契约 |
| [用户内存访问模块](modules/kernel-uaccess.md) | 用户范围、跨页 copy-to-user、错误分类、并发与性能边界 |
| [用户 ELF64 装载模块](modules/user-elf.md) | 有界 ELF64 解析、RISC-V 静态映像物化、权限和失败所有权契约 |
| [RISC-V 启动学习总结](learning/riscv-boot.md) | 启动知识、BoarOS 的应用方式、平台差异和调试经验 |
| [RISC-V Trap 学习总结](learning/riscv-traps.md) | Trap CSR、上下文保存、异常返回、中断确认和项目选择 |
| [RISC-V 时间与周期 Tick 学习总结](learning/riscv-time.md) | timebase、clockevent、SBI/Sstc、周期 tick、性能和平台事实 |
| [内核线程与抢占调度学习总结](learning/kernel-scheduling.md) | Trap/switch context、psABI、线程状态、栈所有权和 timer 抢占 |
| [内存管理学习总结](learning/memory-management.md) | 内存与分页知识、架构能力、项目选择和验证依据 |
| [RISC-V 用户态与系统调用学习总结](learning/riscv-user-mode.md) | 特权边界、首次进入、地址空间、系统调用 ABI 和故障隔离 |
| [ELF 用户程序装载学习总结](learning/elf-loading.md) | ELF 形态、装载段、BSS、页权限、用户初始栈和比赛输入依据 |
| [第三方代码](third-party.md) | 实际引入的外部源码、版本与许可 |

`modules/` 保存当前实现的稳定事实，接口或不变量变化时同步更新。`learning/` 保存开发过程中值得集中复习的知识、已经确定的项目选择及理由、架构或板级资料依据，以及可复用的验证和调试经验；它不保存未确认方案、TODO、临时 plan 或流水账。根目录 `README.md` 记录当前能力、运行入口和近期方向。每个可独立验证的子系统阶段结束时都要主动检查这三类文档，而不是等到人再次提问，也不是每个提交都追加流水账。

工程演进由测试和提交保存。Harness 或 skill 的临时 spec、plan 和会话材料默认不进入仓库；跨 Session 确实需要保留时，先由人确认具体用途和路径。

精确接口、限制和当前状态只在模块说明或 README 维护；learning 可以引用这些结论解释原理和理由，但不复制实现清单。过时内容直接修正或删除。
