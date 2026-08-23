# BoarOS

<img src="assets/logo-concept.png" alt="BoarOS 正面野猪 Logo" width="180">

BoarOS 是一个从零学习并面向 OS Comp 能力建设的 C + 汇编类 Linux 内核，也是一次长期的人—Agent 协作开发实践。

## 当前实现

- `make all` 构建 RISC-V64 ELF `kernel-rv`。
- QEMU `virt` 加载默认 OpenSBI，随后以 S-mode 进入 BoarOS。
- 启动代码建立 `gp`、清零 BSS、创建单 hart 启动栈，并把 OpenSBI 的 hart ID 与 DTB 指针交给 C 入口。
- 启动代码安装 Direct-mode `stvec`；同步 trap 会输出 `scause`、`sepc`、`stval`、`sstatus` 后关机。
- 内核校验并扫描 DTB，读取第一段 RAM 和静态保留区，排除固件、内核镜像与 DTB 自身占用后形成启动内存布局。
- 物理页分配器按 4 KiB 向内对齐可用区间，支持单页分配、释放、耗尽和重复释放诊断。
- 自动测试分别验证 DTB 与启动布局、物理页状态机和致命 trap，并在 512 MiB 和 1 GiB 两种 guest RAM 配置下验证完整启动链。

当前只支持 RISC-V64 单 hart 启动、致命 trap 诊断、DTB 中第一段物理内存和静态保留区的启动布局，以及最小物理页分配；trap 恢复、分页、中断、用户态和 LoongArch64 均未实现。完整比赛 Harness 仍会因缺少 `kernel-la` 失败。

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
make test-trap-riscv
make test-references
```

`make debug-riscv` 使用 `-S -s` 启动 QEMU：虚拟 CPU 会暂停并在宿主 TCP 端口 1234 等待 GDB，因此命令不会自行返回。

## 近期方向

下一步使用物理页分配器建立 RISC-V64 Sv39/4 KiB 的最小内核映射。RISC-V64 + OpenSBI 优先，LoongArch64 16 KiB/三级页表随后接入。

## 文档

- [RISC-V 启动模块](docs/modules/riscv-boot.md)
- [RISC-V 致命 Trap 模块](docs/modules/riscv-trap.md)
- [DTB 与启动内存布局模块](docs/modules/dtb-memory.md)
- [物理页分配模块](docs/modules/physical-pages.md)
- [RISC-V 启动学习总结](docs/learning/riscv-boot.md)
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
