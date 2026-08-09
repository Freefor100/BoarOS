# 工具链事实

本文记录 2026-08-10 在当前开发环境中实际观察到的工具与 Harness 行为。版本变化时重新验证，不把本机路径视为项目接口。

## 当前工具

| 工具 | 版本 | 用途 |
|---|---|---|
| `riscv64-unknown-elf-gcc` | 14.2.0 | RISC-V freestanding 内核编译 |
| RISC-V bare-metal binutils | 2.45.50.20251209 | 链接与 ELF 检查 |
| `riscv64-linux-gnu-gcc` | 15.2.0 | Linux ABI 用户程序候选 |
| `loongarch64-linux-gnu-gcc` | 15.2.0 | LoongArch freestanding 探针已通过 |
| LoongArch GNU binutils | 2.46 | 链接与 ELF 检查 |
| `qemu-system-riscv64` | 10.2.1 | RISC-V system emulator |
| `qemu-system-loongarch64` | 10.2.1 | LoongArch system emulator |
| GNU Make | 4.4.1 | 构建入口 |
| Python | 3.13.13 | 宿主脚本可用 |
| Docker CLI | 29.1.3 | CLI 已安装，daemon 尚未验证 |

当前 `PATH` 中没有 Clang、LLD、Podman、bare-metal LoongArch GCC 或 LoongArch musl GCC；它们目前不是项目依赖。

## Freestanding 证据

同一份最小 C 源码已经在不使用 libc 和启动文件的条件下为两个目标生成 ELF64 little-endian 对象：

```sh
riscv64-unknown-elf-gcc \
  -std=gnu11 -ffreestanding -fno-builtin -fno-stack-protector \
  -nostdlib -nostartfiles -march=rv64gc -mabi=lp64d -mcmodel=medany \
  -c smoke.c -o smoke-riscv64.o

loongarch64-linux-gnu-gcc \
  -std=gnu11 -ffreestanding -fno-builtin -fno-stack-protector \
  -nostdlib -nostartfiles -march=loongarch64 -mabi=lp64d \
  -c smoke.c -o smoke-loongarch64.o
```

这只证明编译器前端和对象生成可用，不证明链接脚本、固件启动、QEMU 入口、ABI 或内核功能正确。

## Harness 事实

本地 `autotest-for-oskernel` 快照在提交目录执行 `make all`，随后读取仓库根目录的 `kernel-rv` 和 `kernel-la`。当前 RISC-V 命令使用 `qemu-system-riscv64 -machine virt -kernel kernel-rv -bios default`，并挂载 virtio-mmio 块设备与网络；LoongArch 命令使用 `qemu-system-loongarch64 -kernel kernel-la` 和 virtio-pci 设备。

默认配置是 1 CPU、1 GiB、3600 秒，但比赛轮次可以覆盖。2026 BuildStorm 使用 8 CPU、8 GiB，guest 内构建目标为 RISC-V 或 LoongArch musl。这些是兼容性输入，不是永久项目 API；每次接入前都要以当前规则和 Harness 为准。
