# BoarOS 工具链与开发环境配置指南

本文档介绍构建与运行 BoarOS 所需的交叉编译工具链、模拟器及辅助工具的环境配置方法。

---

## 🛠️ 1. 环境依赖概览

内核使用 GNU Make 和目标架构的交叉 GCC 构建，脚本使用 Python 3 和 Git。运行或测试内核需要对应架构的 QEMU 系统模拟器。

| 目标 | 交叉编译器 | 模拟器 | 内核构建模式 |
| :--- | :--- | :--- | :--- |
| **RISC-V 64** | `riscv64-unknown-elf-gcc` 或 `riscv64-elf-gcc` | `qemu-system-riscv64` | GNU C11、freestanding |
| **LoongArch 64** | `loongarch64-unknown-linux-gnu-gcc` | `qemu-system-loongarch64` | GNU C11、freestanding、LP64S |

以下是仓库使用过的环境和构建入口。固定测试输入的版本与校验见 [`references/sources.tsv`](../references/sources.tsv)。

---

## 🐧 2. RISC-V 64 开发环境配置

通过宿主软件源或交叉工具链发行包安装 GCC、binutils 和 QEMU，并将它们加入 `PATH`。Makefile 优先选择 `riscv64-unknown-elf-`，其次选择 `riscv64-elf-`。Arch Linux 已验证使用 `riscv64-elf-gcc` 15.2.0 与 binutils 2.45.1。也可显式指定工具前缀和模拟器：

```sh
make CROSS_COMPILE=riscv64-elf-
make run-riscv QEMU_RISCV64=qemu-system-riscv64
```

RV 的 vDSO 是用户态共享 ELF，链接时还需要 `riscv64-linux-gnu-ld` 或 `riscv64-linux-musl-ld`。可以通过 `RV_VDSO_LD=/path/to/ld` 选择。内核和 vDSO 对象仍由原交叉 GCC 编译。LA 默认使用其交叉工具链的链接器。

---

## 🐉 3. LoongArch 64 开发环境配置

### 3.1 交叉编译器

先安装交叉 GCC 和 binutils，并将它们加入 `PATH`。已验证的交叉 GCC 为 15.1.0。默认前缀为 `loongarch64-unknown-linux-gnu-`，可在编译时覆盖：

```sh
make kernel-la LA_CROSS_COMPILE=loongarch64-unknown-linux-gnu-
```

### 3.2 模拟器

`make prepare-la-tools` 使用固定 QEMU 源码及 RTC 补丁构建模拟器，输出为 `build/qemu-la-rtc/qemu-system-loongarch64`。它只准备模拟器。已有适配的 QEMU 时，可以通过 `QEMU_LOONGARCH64` 指定其路径；涉及 RTC 的测试使用仓库固定版本。

### 3.3 静态用户程序工具链

`make prepare-la-userland` 从固定归档构建 GCC 15.1.0 的 LP64S 用户态工具链、musl 1.2.5 和原配置 BusyBox。首次构建 GCC 需要宿主 C++ 编译器及 GMP、MPFR、MPC 开发库。安装结果保存在 `build/loongarch/`，后续按输入和产物清单核对后复用。

这套静态用户态工具与编译内核用的交叉 GCC 分开。LP64D 的运行库不能用于链接 LP64S 程序；程序使用 16 KiB 页的装载要求也要在 ELF 链接时满足。

---

## 🔍 4. 环境验证与快速测试

下列目标可检查内核构建与 QEMU 启动。无根盘时，RV 进入空闲循环，LA 生产内核报告根盘缺失。完整用户态启动见 [RV 根盘](modules/riscv-root-boot.md)和 [LA 根盘](modules/loongarch-boot.md)。

```bash
# 1. 编译并启动 RISC-V 内核（通过串口查看日志，Ctrl+A X 退出）
make run-riscv

# 2. 编译并启动 LoongArch 内核
make run-loongarch

# 3. 运行 RISC-V 架构与内核回归
make test-riscv
```
