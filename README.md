# BoarOS

<img src="assets/logo-concept.png" alt="BoarOS 正面野猪 Logo" width="180">

BoarOS 是一个从零搭建，面向 OS Comp 能力建设的 C 语言（少量 Asm）的 类 Linux 内核。

## 当前实现

- `make all` 构建 RISC-V64 ELF `kernel-rv`。
- QEMU `virt` 加载默认 OpenSBI，随后以 S-mode 进入 BoarOS。
- 启动代码建立 `gp`、清零 BSS、创建单 hart 启动栈，并把 OpenSBI 的 hart ID 与 DTB 指针交给 C 入口。
- 启动代码安装 Direct-mode `stvec`；S-mode 入口在当前内核栈保存完整整数 Trap Frame，经过 C dispatcher 后可以验证并执行 `sret`。生产 dispatcher 已处理 supervisor timer interrupt；其他未处理事件仍输出 `scause`、`sepc`、`stval`、`sstatus` 后关机。
- 内核校验并扫描 DTB，读取第一段 RAM、静态保留区和 RISC-V `timebase-frequency`，排除固件、内核镜像与 DTB 自身占用后形成启动内存布局。
- 物理页分配器按 4 KiB 向内对齐可用区间，支持单页分配、释放、耗尽和重复释放诊断；进入高半区后一次性绑定物理地址访问函数，回收链节点通过该函数读写。
- RISC-V 内核 ELF 链接到 Sv39 高半区 `0xffffffff80000000`，QEMU 当前仍从物理地址 `0x80200000` 装载和进入；内核先用只覆盖切换所需低/高别名的过渡页表迁移 PC、栈、`gp` 和 `stvec`，再切换到只含高半区内核、从 `0xffffffc000000000` 开始的 128 GiB RAM direct map 和平台 MMIO 的最终页表。
- Sv39 建表器按条件组合 2 MiB 与 4 KiB 叶子；最终页表不映射低地址 RAM，QEMU `virt` UART 仍使用独立的低地址 RW MMIO 映射。
- 最终地址空间建立后，内核先把 boot context 初始化为 idle，再通过 SBI TIME 设置绝对 deadline，以 100 Hz 策略处理 supervisor timer interrupt；迟到时按原 deadline 相位一次补记 elapsed tick，并把同一 elapsed 交给 scheduler。
- RISC-V switch context 按 psABI 保存 `ra/sp/tp/s0..s11`，其中 `tp` 固定指向 current kernel thread。普通线程各使用一个私有 4 KiB 页承载控制块、canary 和向下增长的栈；单 hart FIFO scheduler 每 tick 最多抢占切换一次，线程入口返回后由 boot idle 在另一张栈上回收页。正常内核不创建演示线程，仍永久执行 `wfi`。
- 自动测试分别验证 DTB 与启动布局、物理页状态机、direct-map 地址边界、Sv39 编码/规模/失败语义、只读页写故障、最终地址空间拒绝低 RAM 访问、低地址/高半区 Trap Frame 返回、timer 状态/deadline/真实中断、context 保存集合、scheduler 状态/回滚、timer-only A -> B -> A 抢占、退出回收、非法返回拒绝和致命 trap，并在 512 MiB、1 GiB 和 16 GiB guest RAM 配置下通过 direct map 实际写读、释放和复用物理页。

当前只支持 RISC-V64 单 hart、QEMU `virt` 平台、无低 RAM 恒等别名的启动期高半区内核/direct map、S-mode 整数 Trap Frame/返回、SBI timer/100 Hz tick、单页内核线程与 FIFO 抢占调度、DTB 中第一段物理内存和静态保留区，以及最小物理页分配。除 timer 外的生产中断、用户态 trap/进程、阻塞与唤醒、运行期映射修改、SMP 和 LoongArch64 均未实现。完整比赛 Harness 仍会因缺少 `kernel-la` 失败。

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

下一步围绕第一个用户地址空间、U-mode trap 往返和最小系统调用边界设计可执行用户任务；RISC-V64 + OpenSBI 主路径稳定后，再接入 LoongArch64 16 KiB/三级页表和对应 context/trap 实现。

## 文档

- [RISC-V 启动模块](docs/modules/riscv-boot.md)
- [RISC-V Trap 模块](docs/modules/riscv-trap.md)
- [RISC-V Timer 与内核 Tick 模块](docs/modules/riscv-timer.md)
- [内核线程调度模块](docs/modules/kernel-scheduler.md)
- [DTB 与启动内存布局模块](docs/modules/dtb-memory.md)
- [物理页分配模块](docs/modules/physical-pages.md)
- [RISC-V Sv39 分页模块](docs/modules/riscv-sv39.md)
- [RISC-V 启动学习总结](docs/learning/riscv-boot.md)
- [RISC-V Trap 学习总结](docs/learning/riscv-traps.md)
- [RISC-V 时间与周期 Tick 学习总结](docs/learning/riscv-time.md)
- [内核线程与抢占调度学习总结](docs/learning/kernel-scheduling.md)
- [内存管理学习总结](docs/learning/memory-management.md)
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
