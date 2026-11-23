# BoarOS 工具链与开发环境指南

本文介绍在 Linux 宿主机或 Docker 容器中搭建 BoarOS 构建与运行环境的方法，覆盖 RISC-V 64 与 LoongArch 64 两个目标架构。

---

## 一、环境依赖概览

构建内核需要 **GNU Make**、目标架构的 **交叉 GCC 工具链**、**Python 3** 与 **Git**；运行内核需要对应架构的 **QEMU 系统模拟器**；首次制作交互式根文件系统镜像时还需要 `curl` 与 `e2fsprogs`（提供 `mke2fs` / `debugfs`）。

| 目标架构 | 推荐交叉编译器 | 模拟器 | 内核编译模式 |
| :--- | :--- | :--- | :--- |
| **RISC-V 64** | `riscv64-unknown-elf-gcc` 或 `riscv64-elf-gcc` | `qemu-system-riscv64` | GNU C11 · 独立运行环境（freestanding） |
| **LoongArch 64** | `loongarch64-unknown-linux-gnu-gcc` | `qemu-system-loongarch64` | GNU C11 · 独立运行环境 · LP64S |

> **提示**：测试与对比验证所使用的固定上游源码版本及校验哈希统一记录在 [references/sources.tsv](../references/sources.tsv)。

### 内核类型信息工具（`pahole`）的离线构建

BoarOS 会在编译内核时调用 `pahole`，直接从内核自身的调试信息（DWARF）生成 **BPF 类型格式（BPF Type Format，BTF）** 并嵌入最终内核镜像，供 `bpftool` 与 `bpftrace` 等工具在运行时读取。

- **默认离线构建**：直接执行 `make` 或 `make kernel-la` 时，构建系统会自动用宿主机 C 编译器从仓库附带的归档离线编译 `pahole`，并缓存到 `build/tools/pahole/`（详见 [eBPF 构建说明](modules/kernel-bpf.md)）。宿主机需具备 C 编译器、`binutils`、`CMake`、`pkg-config`、`patch` 以及包含静态库 `libz.a` 和 `libdl.a` 的开发包。
- **使用系统已安装版本**：若宿主机已安装 `pahole`，只需在构建命令后追加 `PAHOLE=pahole` 即可跳过本地编译，直接调用系统版本。

---

## 二、RISC-V 64 开发环境配置

通过发行版软件源或预编译工具链包安装交叉 GCC、`binutils` 与 `qemu-system-riscv64` 并加入 `PATH`。构建脚本会优先自动探测 `riscv64-unknown-elf-`，其次探测 `riscv64-elf-`；也可以通过命令行参数显式指定：

```sh
make CROSS_COMPILE=riscv64-elf-
make shell-riscv INIT_CONFIG=config/init.json QEMU_RISCV64=qemu-system-riscv64
```

- **指令集扩展兼容**：构建脚本会自动探测工具链是否支持显式子扩展名 `rv64imac_zicsr_zifencei`，自动兼容新旧版本 GCC 与汇编器组合。
- **用户态虚拟动态共享对象（vDSO）链接器**：RISC-V 内嵌的 vDSO 是一个用户态共享 ELF，链接时需要 `riscv64-linux-gnu-ld` 或 `riscv64-linux-musl-ld`（可通过 `RV_VDSO_LD=/path/to/ld` 显式指定）。

---

## 三、LoongArch 64 开发环境配置

### 3.1 交叉编译器

安装龙芯交叉编译器并加入 `PATH`（构建脚本自动识别 `loongarch64-unknown-linux-gnu-` 或 `loongarch64-linux-gnu-`）：

```sh
make kernel-la LA_CROSS_COMPILE=loongarch64-unknown-linux-gnu-
```

内核态默认使用纯整数调用约定（关闭自动向量化与内核态向量指令生成），用户态的浮点与 LSX/LASX 向量上下文由内核按需保存和恢复。

### 3.2 QEMU 模拟器与硬件实时时钟（RTC）补丁

常规交互式启动可直接使用发行版自带的 `qemu-system-loongarch64`。如果希望运行涉及 LoongArch 硬件实时时钟（Real-Time Clock，RTC）模型的专项测试，可执行以下命令基于仓库固定源码与补丁自动构建配套模拟器：

```sh
make prepare-la-tools
# 产物位于 build/qemu-la-rtc/qemu-system-loongarch64
```

之后通过 `QEMU_LOONGARCH64=build/qemu-la-rtc/qemu-system-loongarch64` 指定使用该模拟器。

### 3.3 LoongArch 静态用户态工具链（可选）

如需在本地编译运行 LoongArch 静态用户态测试程序与 BusyBox，可执行：

```sh
make prepare-la-userland
```

该入口会从固定源码归档构建面向 16 KiB 页对齐的 LP64S 用户态 GCC、`musl` 运行库与 BusyBox，并缓存于 `build/loongarch/`（首次编译 GCC 需要宿主机 C++ 编译器及 GMP、MPFR、MPC 开发库）。

---

## 四、使用 Docker 零配置构建（推荐）

仓库根目录的 [Dockerfile](../Dockerfile) 完整封装了双架构交叉编译器、QEMU 与 `pahole`（基于 x86-64 Arch Linux Archive 快照与龙芯 GCC 15.1.0 工具链）：

```sh
# 1. 构建开发镜像
docker build --platform linux/amd64 -t boaros-dev .

# 2. 挂载源码并一键编译启动（将 shell-riscv 替换为 shell-loongarch 可启动龙芯平台）
docker run --rm -it --user "$(id -u):$(id -g)" \
  --mount "type=bind,source=$PWD,target=/workspace" \
  boaros-dev make -j4 shell-riscv INIT_CONFIG=config/init.json
```

镜像已预设 `PAHOLE` 与 `QEMU_LOONGARCH64` 环境变量，可直接用于构建内核与交互式体验。

---

## 五、常用开发与调试入口

| 命令 | 作用 |
| :--- | :--- |
| `make shell-riscv` / `make shell-loongarch` | 指定 `INIT_CONFIG=config/init.json` 后编译内核、制作交互根盘并进入串口命令行 |
| `make run-riscv` / `make run-loongarch` | 启动[本地评测环境](modules/oscomp-compat.md) |
| `make debug-riscv` | 以调试模式启动 RISC-V 本地评测环境，等待调试器连接 |
| `make test-riscv` / `make test-loongarch` | 运行对应架构的核心回归测试套件 |
| `make prune-build` | 清理中间对象文件与临时测试镜像（保留交互根盘与已编译的工具缓存） |
