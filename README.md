# BoarOS

<img src="assets/logo-concept.png" alt="BoarOS 正面野猪 Logo" width="180">

BoarOS 是一个从零搭建，面向 OS Comp 能力建设的 C 语言（少量 Asm）的 类 Linux 内核。

## 当前实现

- `make all` 构建 RISC-V64 ELF `kernel-rv`。
- QEMU `virt` 加载默认 OpenSBI，随后以 S-mode 进入 BoarOS。
- 启动代码建立 `gp`、清零 BSS、创建单 hart 启动栈，并把 OpenSBI 的 hart ID 与 DTB 指针交给 C 入口。
- 启动代码安装 Direct-mode `stvec`；统一 Trap 入口让 S-mode 直接使用当前栈，并通过 `sscratch <-> tp` 为 U-mode 切到任务内核栈，保存完整整数 Trap Frame 后经过 C dispatcher 验证并执行 `sret`。生产 dispatcher 已处理 supervisor timer、U-mode ecall 和用户同步故障；其他未处理事件仍输出 CSR 现场后关机。
- 内核校验并扫描 DTB，读取第一段 RAM、静态保留区和 RISC-V `timebase-frequency`，排除固件、内核镜像与 DTB 自身占用后形成启动内存布局。
- 物理页分配器按 4 KiB 向内对齐可用区间，支持单页分配、释放、耗尽和重复释放诊断；进入高半区后一次性绑定物理地址访问函数，回收链节点通过该函数读写。
- RISC-V 内核 ELF 链接到 Sv39 高半区 `0xffffffff80000000`，QEMU 当前仍从物理地址 `0x80200000` 装载和进入；内核先用只覆盖切换所需低/高别名的过渡页表迁移 PC、栈、`gp` 和 `stvec`，再切换到只含高半区内核、从 `0xffffffc000000000` 开始的 128 GiB RAM direct map 和平台 MMIO 的最终页表。
- Sv39 建表器按条件组合 2 MiB 与 4 KiB 叶子；最终页表不保留低地址映射，QEMU `virt` UART 的物理 MMIO 通过 `0xffffffe000000000` 的 supervisor-only 高半区别名访问。运行期用户地址空间拥有低半区 4 KiB U 页和页表页、借用包含 UART 在内的最终内核高半区根项，并以 ASID 0 全局刷新方式切换 `satp`。
- 最终地址空间建立后，内核先把 boot context 初始化为 idle，再通过 SBI TIME 设置绝对 deadline，以 100 Hz 策略处理 supervisor timer interrupt；迟到时按原 deadline 相位一次补记 elapsed tick，并把同一 elapsed 交给 scheduler。
- RISC-V switch context 按 psABI 保存 `ra/sp/tp/s0..s11`，其中内核 `tp` 固定指向 current task。普通内核/用户任务各使用一个私有 4 KiB 页承载控制块、canary 和内核栈；用户任务持有带引用计数的通用 MM 句柄，RISC-V 后端用一个 4 KiB 记录页拥有 Sv39 地址空间。单 hart FIFO scheduler 每 tick 最多抢占切换一次，直接使用任务缓存的 `satp`，不在切换热路径解析 MM。退出后由 boot idle 逐条返回完成记录，并按 MM 引用、TID、任务页的顺序回收；复合失败保留可移动、可重试的 owner。正常内核不创建演示任务，仍永久执行 `wfi`。
- Linux 风格系统调用边界显式接收当前调用任务，支持 `exit(93)`、`uname(160)`、`getpid(172)` 和 `gettid(178)`，未知调用返回 `-ENOSYS`；`uname` 通过任务只读借用的 MM 把 Linux 390 字节 `new_utsname` 复制到用户页，非法、未映射或不可写目标返回 `-EFAULT`。当前首线程的 TID 与 TGID 相同，取值来自可回收的 1..32768 位图分配器。用户同步故障只终止当前任务并保留 `scause/stval` 完成记录，S-mode 未处理故障仍为内核 fatal。
- 有界 ELF64 解析器从只读内核内存缓冲区解码并校验 ELF header 与 program header；RISC-V 装载器支持静态、小端 ELF64 `ET_EXEC`，把 `PT_LOAD` 复制到独立 Sv39 用户地址空间，按页合并权限并执行 W^X 检查、补零 BSS，再根据带长度的参数请求建立 Linux 形态的 `argc/argv/envp/auxv` 初始栈。用户栈预留低半区顶端 8 MiB 虚拟区间，初次只提交容纳至多 128 KiB 栈镜像并额外向下留出 64 KiB 的页，预留区下方保持一页永久 guard；动态链接、TLS、随机数和按需扩栈尚未支持。
- 仓库固定保存 lwext4 源码快照；宿主集成测试会分别建立使用 superblock checksum seed 与 UUID 派生 seed 的真实 128 MiB ext4 镜像，经 lwext4 只读挂载并精确读取文件。该依赖尚未接入内核分配器、块设备、VFS 或启动路径。
- 自动测试除启动、物理页、分页、Trap、timer 和内核任务状态外，还验证共享 MM 引用、位图 PID 分配，以及记录页访问/释放与页表回收的复合失败，要求状态可重试且页所有权不丢失；uaccess 聚焦测试覆盖跨页前缀复制、整体用户范围、权限、未映射页和内核状态错误。两个共享 MM 但使用不同用户栈和身份的任务会真实进入 U-mode，经 timer 抢占到内核 worker 后恢复，核对 `gp/sp/tp/s0..s11`、不同的 `getpid/gettid`、跨页 `uname`、三类 `-EFAULT`、未知 syscall 和 `exit(93)`，另一独立 MM 任务触发 load page fault。另一组测试独立链接完整静态 ELF，验证 `.data`、BSS、参数/环境字符串、auxv、栈对齐、初始提交边界、RX 文本写故障和永久栈 guard 故障。测试最终要求完成记录正确且全部 MM/用户/任务页与 TID 回收，另在用户根下破坏返回凭据验证 fatal 诊断。

当前只支持 RISC-V64 单 hart、QEMU `virt` 平台、Sv39/4 KiB 用户页、S/U 整数 Trap Frame、SBI timer/100 Hz tick、单页内核栈与 FIFO 抢占、每个用户任务为单成员线程组，以及从完整内存缓冲区装载静态 `ET_EXEC` 用户程序。调用者已经能把 acquire 得到的 MM 引用交给多个任务，但尚无从 current task 派生子任务的 `clone` 接口、同组多线程、父子关系、文件表或完整 Linux 进程生命周期；用户内存访问当前只有已映射页上的 `copy_to_user`，还没有 `copy_from_user`、按需缺页或并发 unmap/COW。文件来源与 `exec`、动态链接、`fork/wait`、阻塞与唤醒、文件系统、外部中断、SMP 和 LoongArch64 也尚未实现。生产启动仍不创建用户任务，完整比赛 Harness 仍会因缺少 `kernel-la` 失败。

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
make test-context-riscv
make test-scheduler-cases-riscv
make test-scheduler-riscv
make test-syscall-riscv
make test-uaccess-riscv
make test-elf64-riscv
make test-lwext4-host
make test-user-elf-cases-riscv
make test-user-elf-riscv
make test-mm-riscv
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

`make run-riscv` 启动成功后持续在 `wfi` 中等待中断，需要由人退出 QEMU。`make debug-riscv` 使用 `-S -s` 启动 QEMU：虚拟 CPU 会暂停并在宿主 TCP 端口 1234 等待 GDB，因此命令也不会自行返回。

## 近期方向

下一步从文件系统提供可执行文件来源，建立 `copy_from_user` 的首个真实消费者并形成 `exec` 生命周期，再逐步接入比赛所需系统调用；RISC-V64 + OpenSBI 主路径稳定后，实现 LoongArch64 16 KiB/三级页表和对应 context/trap。

## 文档

- [RISC-V 启动模块](docs/modules/riscv-boot.md)
- [RISC-V Trap 模块](docs/modules/riscv-trap.md)
- [RISC-V Timer 与内核 Tick 模块](docs/modules/riscv-timer.md)
- [内核线程调度模块](docs/modules/kernel-scheduler.md)
- [系统调用解码模块](docs/modules/kernel-syscall.md)
- [DTB 与启动内存布局模块](docs/modules/dtb-memory.md)
- [物理页分配模块](docs/modules/physical-pages.md)
- [RISC-V Sv39 分页模块](docs/modules/riscv-sv39.md)
- [内核 MM 模块](docs/modules/kernel-mm.md)
- [用户内存访问模块](docs/modules/kernel-uaccess.md)
- [用户 ELF64 装载模块](docs/modules/user-elf.md)
- [RISC-V 启动学习总结](docs/learning/riscv-boot.md)
- [RISC-V Trap 学习总结](docs/learning/riscv-traps.md)
- [RISC-V 时间与周期 Tick 学习总结](docs/learning/riscv-time.md)
- [内核线程与抢占调度学习总结](docs/learning/kernel-scheduling.md)
- [内存管理学习总结](docs/learning/memory-management.md)
- [RISC-V 用户态与系统调用学习总结](docs/learning/riscv-user-mode.md)
- [ELF 用户程序装载学习总结](docs/learning/elf-loading.md)
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
