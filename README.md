# BoarOS

<img src="assets/logo-concept.png" alt="BoarOS 正面野猪 Logo" width="180">

BoarOS 是一个从零学习并面向 OS Comp 能力建设的 C + 汇编类 Linux 内核，也是一次长期的人—Agent 协作开发实践。

## 当前实现

- `make all` 构建 RISC-V64 ELF `kernel-rv`。
- QEMU `virt` 加载默认 OpenSBI，随后以 S-mode 进入 BoarOS。
- 启动代码建立 `gp`、清零 BSS、创建单 hart 启动栈，并把 OpenSBI 的 hart ID 与 DTB 指针交给 C 入口。
- 启动代码安装 Direct-mode `stvec`；同步 trap 会输出 `scause`、`sepc`、`stval`、`sstatus` 后关机。
- 内核校验并扫描 DTB，读取第一个物理内存范围，通过轮询 UART 输出启动信息，再以 SBI SRST 关闭虚拟机。
- 自动测试验证致命 trap 路径，并在 512 MiB 和 1 GiB 两种 guest RAM 配置下验证完整启动链、DTB 交接与物理内存范围。

当前只支持 RISC-V64 单 hart 启动、致命 trap 诊断和 DTB 中第一段物理内存的发现；保留区处理、trap 恢复、分页、中断、内存管理、用户态和 LoongArch64 均未实现。完整比赛 Harness 仍会因缺少 `kernel-la` 失败。

## 构建与运行

需要 RISC-V64 bare-metal GCC 与对应 binutils、GNU Make 和
`qemu-system-riscv64`。构建系统兼容 `riscv64-unknown-elf-` 与 Arch Linux
提供的 `riscv64-elf-` 工具前缀。

```sh
make all
make run-riscv
make test-riscv
make test-dtb-riscv
make test-trap-riscv
```

`make debug-riscv` 使用 `-S -s` 启动 QEMU：虚拟 CPU 会暂停并在宿主 TCP 端口 1234 等待 GDB，因此命令不会自行返回。

## 近期方向

在已经获得的 DTB 物理内存范围上，下一步识别并排除固件、内核镜像、DTB 与设备树保留区，再设计物理页管理。RISC-V64 + OpenSBI 优先，LoongArch64 随后接入。

## 文档

- [RISC-V 启动模块](docs/modules/riscv-boot.md)
- [RISC-V 致命 Trap 模块](docs/modules/riscv-trap.md)
- [DTB 物理内存发现模块](docs/modules/dtb-memory.md)
- [RISC-V 启动学习记录](docs/learning/riscv-boot.md)
- [目标与边界](docs/goals.md)
- [设计与工程原则](docs/design.md)
- [工具链事实](docs/toolchain.md)
- [文档导航](docs/README.md)
- [参与开发](CONTRIBUTING.md)

`references/` 保存本地规则、Harness 和公开测例快照，不纳入版本控制。

## 公开参考

- [OS Comp 2026 规则](https://gitlab.eduxiji.net/csc1/csc-os/os2026)
- [OS Comp 内核公开测例](https://github.com/oscomp/testsuits-for-oskernel)
- [OS Comp 自动测试 Harness](https://github.com/oscomp/autotest-for-oskernel)
