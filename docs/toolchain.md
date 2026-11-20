# BoarOS 工具链与开发环境配置指南

本文档介绍构建与运行 BoarOS 所需的交叉编译工具链、模拟器及辅助工具的环境配置方法。

---

## 🛠️ 1. 环境依赖概览

内核使用 GNU Make 和目标架构的交叉 GCC 构建，脚本使用 Python 3 和 Git。运行内核需要对应架构的 QEMU 系统模拟器。交互启动首次制作根盘时还需要 curl 和 e2fsprogs。

| 目标 | 交叉编译器 | 模拟器 | 内核构建模式 |
| :--- | :--- | :--- | :--- |
| **RISC-V 64** | `riscv64-unknown-elf-gcc` 或 `riscv64-elf-gcc` | `qemu-system-riscv64` | GNU C11、freestanding |
| **LoongArch 64** | `loongarch64-unknown-linux-gnu-gcc` | `qemu-system-loongarch64` | GNU C11、freestanding、LP64S |

以下是仓库使用过的环境和构建入口。固定测试输入的版本与校验见 [`references/sources.tsv`](../references/sources.tsv)。

### 从随仓库的源码构建 pahole

直接执行 `make` 或 `make kernel-la`。构建会先用宿主 C 编译器从随仓库附带的归档编译 pahole，再用它为内核生成类型信息。内核构建可以离线执行。宿主需提供 C 编译器、binutils、CMake、pkg-config、patch，以及包含 `libz.a` 和 `libdl.a` 的开发库。 [类型信息构建说明](modules/kernel-bpf.md#构建与检查)

如果宿主已经安装了 pahole，也可以用 `make PAHOLE=pahole`。遇到缺少 `libz.a` 的提示时，可以安装提供静态库的 zlib 开发包，或使用已安装的 pahole。

---

## 🐧 2. RISC-V 64 开发环境配置

通过宿主软件源或交叉工具链发行包安装 GCC、binutils 和 QEMU，并将它们加入 `PATH`。Makefile 优先选择 `riscv64-unknown-elf-`，其次选择 `riscv64-elf-`。Arch Linux 已验证使用 `riscv64-elf-gcc` 15.2.0 与 binutils 2.45.1。也可显式指定工具前缀和模拟器：

```sh
make CROSS_COMPILE=riscv64-elf-
make shell-riscv QEMU_RISCV64=qemu-system-riscv64
```

构建时会检查编译器和汇编器是否接受 `rv64imac_zicsr_zifencei`。早期工具链使用
`rv64imac`，其指令集定义已包含控制与状态寄存器访问及 `FENCE.I` 指令。浮点和向量
现场代码也能使用旧汇编器生成同一组指令。旧编译器搭配新汇编器时，构建会向汇编器
单独传入显式的扩展名。

RV 的 vDSO 是用户态共享 ELF，链接时还需要 `riscv64-linux-gnu-ld` 或 `riscv64-linux-musl-ld`。可以通过 `RV_VDSO_LD=/path/to/ld` 选择。内核和 vDSO 对象仍由原交叉 GCC 编译。LA 默认使用其交叉工具链的链接器。

---

## 🐉 3. LoongArch 64 开发环境配置

### 3.1 交叉编译器

先安装交叉 GCC 和 binutils，并将它们加入 `PATH`。Makefile 优先选择 `loongarch64-unknown-linux-gnu-`，也识别 `loongarch64-linux-gnu-`。内核使用整数调用约定，构建探测编译器能否禁用两种向量扩展。较早的 GCC 通过关闭自动向量化编译。已有工具链也可显式指定前缀。

```sh
make kernel-la LA_CROSS_COMPILE=loongarch64-unknown-linux-gnu-
```

### 3.2 模拟器

`make prepare-la-tools` 使用固定 QEMU 源码及 RTC 补丁构建模拟器，输出为 `build/qemu-la-rtc/qemu-system-loongarch64`。它只准备模拟器。已有适配的 QEMU 时，可以通过 `QEMU_LOONGARCH64` 指定其路径；涉及 RTC 的测试使用仓库固定版本。

### 3.3 静态用户程序工具链

`make prepare-la-userland` 从固定归档构建 GCC 15.1.0 的 LP64S 用户态工具链、musl 1.2.5 和原配置 BusyBox。首次构建 GCC 需要宿主 C++ 编译器及 GMP、MPFR、MPC 开发库。安装结果保存在 `build/loongarch/`，后续按输入和产物清单核对后复用。

这套静态用户态工具与编译内核用的交叉 GCC 分开。LP64D 的运行库不能用于链接 LP64S 程序；程序使用 16 KiB 页的装载要求也要在 ELF 链接时满足。

---

## 🔍 4. 启动与调试

交互启动使用 `make shell-riscv` 或 `make shell-loongarch`。根盘制作需要 curl 和 e2fsprogs，首次启动方法见 [README](../README.md) 的快速上手一节。

开发时可以用 `make run-riscv` 检查无盘启动，用 `make debug-riscv` 启动并等待调试器连接。LoongArch 的 `make run-loongarch` 可检查生产内核在无盘时的启动诊断。测试入口见[贡献指南](../CONTRIBUTING.md#测试)。

## 5. Docker 开发环境

仓库根目录的 [Dockerfile](../Dockerfile) 以 x86-64 Arch Linux 为宿主环境，使用 Arch Linux Archive 的 `2026/11/20` 软件仓库快照。关键版本为 RISC-V 交叉 GCC 16.2.0、GNU Binutils 2.47、QEMU 11.1.3 和 pahole 1.32。LoongArch 使用本地开发同款的龙芯 GCC 15.1.0、Binutils 2.45 工具链。

交互启动使用 Arch 软件包中的两种 QEMU。需要仓库实时时钟（Real-Time Clock，RTC）模型补丁的 LoongArch 测试，仍按本页的模拟器准备步骤构建并显式指定 `QEMU_LOONGARCH64`。
