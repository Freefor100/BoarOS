# BoarOS

<p align="center">
  <img src="assets/boaros_header.png" alt="BoarOS 吉祥物与字标" width="100%">
</p>


<p align="center">
  <a href="docs/modules/riscv-boot.md"><img src="https://img.shields.io/badge/RISC--V-64--bit%20Sv39-blue?style=for-the-badge&logo=riscv&logoColor=white" alt="RISC-V 64"></a>
  <a href="docs/modules/loongarch-boot.md"><img src="https://img.shields.io/badge/LoongArch-64--bit%20LA464-red?style=for-the-badge" alt="LoongArch 64"></a>
  <a href="docs/design.md"><img src="https://img.shields.io/badge/Kernel-Monolithic-059669?style=for-the-badge" alt="Monolithic Kernel"></a>
  <a href="docs/modules/kernel-syscall.md"><img src="https://img.shields.io/badge/Compatibility-Linux%20ABI-D97706?style=for-the-badge&logo=linux&logoColor=white" alt="Linux ABI Compatible"></a>
  <a href="docs/modules/riscv-smp.md"><img src="https://img.shields.io/badge/SMP-1~8%20Cores-7C3AED?style=for-the-badge" alt="SMP 1~8 Cores"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-GPL--2.0-475569?style=for-the-badge" alt="License"></a>
</p>

---

## 📖 项目简介

**BoarOS** 是面向 RISC-V 64 位与 LoongArch 64 位的单体内核，以兼容 Linux 用户态 ABI 为目标。内核主要用 C 编写，启动、异常入口和上下文切换使用架构汇编。

目前在 QEMU virt 上验证，两种架构默认单核，多核版本可启动至八核。已运行基于 musl 和 glibc 的静态与动态 ELF，以及 BusyBox、SQLite 和 Lua 等程序。程序环境与运行方式见[程序环境](docs/modules/program-environment.md)；比赛适配在 `oscomp-compat` 分支维护，实板支持仍待验证。

---

## 🏛️ 内核架构

RISC-V 和 LoongArch 各自处理启动、异常入口、上下文切换和页表操作。系统调用进入内核后，由公共代码实现进程、内存、文件和网络接口。VirtIO 驱动通过各平台的传输接口访问设备；串口和 RTC 由平台代码接入。

![BoarOS 内核架构：双架构异常入口与共用系统调用层，每核调度与跨核 TLB 失效，以及 ext4 和网络在调用核与 CPU0 的执行分工。](assets/kernel-architecture.svg)

---

## 🧩 核心设计与子系统实现

### 1. 双架构抽象与平台适配
- **RISC-V 64**：运行于 S-Mode（Supervisor Mode），由 OpenSBI 提供 SBI 服务。采用 4 KiB 页与 **Sv39** 三级页表；平台通过设备树发现设备，VirtIO 使用 MMIO 传输。
- **LoongArch 64**：在 QEMU virt 的 **LA464** CPU 模型上运行，采用 16 KiB 页和三级页表。VirtIO 使用 PCI 传输；串口和 RTC 由平台代码接入。
- **公共抽象层**：两种架构分别处理异常入口、寄存器切换、页表和硬件定时器，系统调用语义、调度、VFS 和网络由公共代码实现。

### 2. 任务调度与多核 SMP
- **Per-CPU 独立就绪队列**：每个 CPU 核心拥有独立的就绪任务队列，普通任务的入队、领取和运行权交接由所在 CPU 的队列保护；跨核控制仍协调共享状态。
- **多核负载均衡与亲和性**：唤醒时按亲和性选择队列；空闲 CPU 可领取其他队列中允许迁移的就绪任务；已经忙碌的 CPU 也可按公平负载或实时优先级迁移候选。可用核数与具体负载的加速比是两回事。
- **Futex 与快速同步机制**：支持等待、唤醒、重排、多地址等待（`futex_waitv`）和优先级继承，供 pthread 互斥锁和条件变量等使用。
- **POSIX 信号体系**：支持实时信号与经典信号投递、专用信号栈 `sigaltstack` 以及通过 `sigreturn` 恢复被中断的执行现场。

### 3. 现代虚拟内存体系（VMM）
- **混合内存分配**：物理内存由页级伙伴系统（Buddy Allocator）统一管理，内核对象由细粒度 SLAB/堆分配器管理。
- **虚存管理（VMA）**：按起始地址排序保存虚拟内存区域，用二分查找定位范围；每个区域记录访问权限及后备对象。
- **按需调页与写时复制（CoW）**：ELF 和文件映射按需装页；`fork` 后，父子共享尚未修改的私有页，写入时再为写入方建立私有副本。
- **跨核 TLB 一致性**：在 SMP 多核环境下，当发生虚存撤回或权限降级时，RV 使用 SBI RFENCE，LA 使用核间中断；在相关 CPU 确认旧地址转换已失效后，才归还退休的页。

### 4. 虚拟文件系统（VFS）与存储持久化
- **标准分层抽象**：VFS 分开维护路径、inode 和打开文件对象。改名后，已打开的 FD 仍指向原文件；`dup` 得到的 FD 共享打开对象及其文件位置。
- **ext4 日志与页缓存**：文件数据经页缓存读写。带日志的 extent 文件在写入空洞时先预留空间，写回时再分配物理块。显式同步的数据写回可在调用核执行。多核时日志提交和 checkpoint 按在线核数选核，页缓存后台写回由 CPU0 推进；`fsync` 等待本次同步确定的持久化目标。
- **多文件系统与挂载**：支持同时挂载 ext4、FAT32/VFAT、内存文件系统 tmpfs、进程状态文件系统 procfs 以及伪终端 devpts；支持挂载、bind、延迟卸载和 `chroot`；私有挂载命名空间可以复制挂载视图并隔离挂载变化。

### 5. 网络与事件等待
- **网络协议**：使用 lwIP 处理 IPv4 TCP/UDP，与 IPv6 TCP/UDP，支持以太网链路本地地址、静态 IPv6 地址、SLAAC、默认路由和 MLDv1。用户数据复制和等待可在调用核执行。已建立的 TCP 连接由各归属核处理，UDP、监听和建连控制仍在 CPU0。IPv4 地址、掩码、MTU 和默认网关可在运行时配置。
- **Linux 兼容 Socket API**：提供常用 Linux 套接字接口。UNIX 套接字还支持文件描述符传递与消息凭据。
- **[BPF 过滤与跟踪](docs/modules/kernel-bpf.md)**：双架构共用校验器与解释器，提供 socket 过滤、map/bpffs/BTF、raw_syscalls tracepoint 和 ringbuf，供原版 bpftool、libbpf 与 bpftrace 使用。
- **事件驱动多路复用**：通过 `epoll`、`poll` 和 `pselect` 等接口等待套接字、管道及事件 FD 的就绪状态。

---

## 📊 平台支持

| 特性 | RISC-V 64 平台 | 龙芯 LoongArch 64 平台 |
| :--- | :--- | :--- |
| **处理器架构** | RV64GC (Sv39) | LoongArch 64 (LA464) |
| **运行级别** | S-Mode (Supervisor) | PLV0 (Kernel Privilege) |
| **固件 / 引导** | OpenSBI | QEMU `-kernel` 与启动固件 |
| **内存页大小** | 4 KiB | 16 KiB |
| **MMU 页表级数** | 3 级页表 (Sv39) | 3 级页表 (Direct Map + Page Walk) |
| **多核 SMP 支持** | 1 ~ 8 核 | 1 ~ 8 核 |
| **外设总线** | VirtIO-MMIO | PCI / PCIe 总线 |
| **浮点与 SIMD** | RV64D 双精度浮点，支持 V 的 CPU 提供 RV 向量寄存器上下文 | LA464 FR / LSX / LASX 向量扩展 |
| **时钟源** | 架构计时器，期限由 SBI 设置 | 龙芯片上恒频时钟 + 硬件 RTC |
| **运行与验证环境** | QEMU virt + OpenSBI（当前未适配实板） | QEMU virt + LA464（当前未适配实板） |

---

## 🚀 快速上手

### 构建并进入命令行

在 Linux 上准备交叉工具链和 QEMU，再按目标架构选择一条命令。命令会构建内核、制作根盘并启动虚拟机。首次制盘会用 curl 下载静态 BusyBox，并用 e2fsprogs 制作 ext4 镜像。 [安装依赖和配置工具链](docs/toolchain.md)

```sh
make shell-riscv
make shell-loongarch QEMU_LOONGARCH64=qemu-system-loongarch64
```

LoongArch 命令中的 `QEMU_LOONGARCH64` 指定要运行的模拟器，可写程序名或绝对路径。

pahole 用来为内核生成类型信息。构建默认从仓库附带的源码编译它，并缓存到 `build/tools/pahole/`。已有系统 pahole 时，可在上面的命令后加 `PAHOLE=pahole`，让构建从 `PATH` 调用它。 [pahole 的构建依赖](docs/toolchain.md#从随仓库的源码构建-pahole)

看到 `Please press Enter to activate this console.` 时按回车。进入 `/ #` 后可执行 `ls /`、`uname -a`，用 `poweroff -f` 关机。根盘存放在 `build/rootfs/`，后续启动会复用盘内改动。

### 只编译内核

```sh
make            # RISC-V，生成 kernel-rv
make kernel-la  # LoongArch，生成 kernel-la
```

只制作根盘时，使用 `make rootfs-riscv` 或 `make rootfs-loongarch`。

<details>
<summary>展开等价命令，指定工具路径、输出目录和虚拟机参数</summary>

在仓库根目录执行下列其中一组命令。可将工具名换成绝对路径。示例使用独立的对象目录和根盘，首次制盘后可直接重复运行最后一条 QEMU 命令。

**RISC-V**

```sh
# 改这里可以选择交叉工具链、模拟器、固件和客户机内存
cross=riscv64-elf-
rv_ld=riscv64-linux-gnu-ld
qemu=qemu-system-riscv64
firmware=default
ram=1G

# 对象文件、内核和根盘都放在这个目录下
out="$PWD/build/manual-riscv"
make -j"$(nproc)" CROSS_COMPILE="$cross" RV_VDSO_LD="$rv_ld" \
  INIT_CONFIG=config/init.json BUILD_DIR="$out/obj" KERNEL_RV="$out/kernel-rv" "$out/kernel-rv"
python3 -B tools/mkrootfs.py riscv64 "$out/root.img"

# 使用 VirtIO 内存映射 I/O 块设备，串口接到当前终端
"$qemu" -machine virt -bios "$firmware" -kernel "$out/kernel-rv" \
  -m "$ram" -smp 1 -nographic -no-reboot -nic none \
  -drive "file=$out/root.img,format=raw,if=none,id=root" \
  -device virtio-blk-device,drive=root
```

**LoongArch**

```sh
# 按本机安装位置调整交叉编译器前缀和 QEMU 路径
cross=loongarch64-unknown-linux-gnu-
qemu=qemu-system-loongarch64
ram=1G
out="$PWD/build/manual-loongarch"

# LA_BUILD 指定对象目录，KERNEL_LA 指定内核输出文件
make -j"$(nproc)" LA_CROSS_COMPILE="$cross" \
  INIT_CONFIG=config/init.json LA_BUILD="$out/obj" KERNEL_LA="$out/kernel-la" "$out/kernel-la"
python3 -B tools/mkrootfs.py loongarch64 "$out/root.img"

# 选择 LA464 处理器和现代 VirtIO PCI 块设备
"$qemu" -machine virt -cpu la464 -kernel "$out/kernel-la" \
  -m "$ram" -smp 1 -nographic -no-reboot -nic none \
  -drive "file=$out/root.img,format=raw,if=none,id=root" \
  -device virtio-blk-pci,drive=root,disable-legacy=on
```

以上命令使用默认单核内核。调整 `-smp` 前，RISC-V 需用 `SMP=1 USER_SMP=1 SMP_CPUS=4` 等参数编译对应容量的内核，LoongArch 需用 `LA_SMP=1 LA_SMP_CPUS=4` 等参数。给不同构建配置各自指定对象目录。

</details>

### 在 Docker 中构建和运行

[Dockerfile](Dockerfile) 采用本地开发使用的 x86-64 Arch Linux 软件包版本和 LoongArch 交叉工具链。构建镜像后，在同一仓库根目录挂载源码并启动。

```sh
docker build --platform linux/amd64 -t boaros-dev .

# 挂载当前仓库，以宿主用户身份写入内核和根盘
docker run --rm -it --user "$(id -u):$(id -g)" \
  --mount "type=bind,source=$PWD,target=/workspace" \
  boaros-dev make -j4 shell-riscv
```

启动 LoongArch 时将最后的 `shell-riscv` 换成 `shell-loongarch`。镜像已设置 `PAHOLE` 和 `QEMU_LOONGARCH64`。工具版本与宿主配置见[工具链指南](docs/toolchain.md)。

开发和回归检查见[贡献指南](CONTRIBUTING.md)。

---

## 📂 目录结构

```
BoarOS/
├── arch/            # 体系结构相关实现（启动汇编、Trap 陷入、上下文切换、MMU）
│   ├── riscv/       # RISC-V 64 专用源码（启动、异常入口、Sv39）
│   └── loongarch/   # LoongArch 64 专用源码 (boot.S, tlb_refill.S, mmu.c 等)
├── kernel/          # 通用启动、调度、系统调用、程序装载、信号、时钟、IPC 与物理页分配
│   ├── sched/       # SMP 抢占式调度器、就绪队列、Futex
│   └── syscall/     # Linux 系统调用统一分发与具体处理
├── mm/              # 地址空间、VMA、缺页处理、用户内存访问与内核堆
├── fs/              # 虚拟文件系统（VFS、ext4 事务日志实现、FAT32、tmpfs、procfs）
├── net/             # 网络子系统（Socket 接口、以太网适配、lwIP 移植接入）
├── drivers/         # 外设与总线驱动（VirtIO 块设备/网卡/RNG、PCI 总线、串口）
├── platform/        # 平台初始化（RISC-V virt 平台、LoongArch virt 平台）
├── include/         # 内核公共头文件与 Linux UAPI 兼容定义
├── lib/             # 独立通用的基础 C 运行库函数（字符串、排序等）
├── third_party/     # lwext4、lwIP 等第三方源码
├── tests/           # 单元测试、集成测试、差分对比测试套件
├── tools/           # 镜像制作、构建配置辅助脚本
├── references/      # 外部规范、源码与本地构建输入
├── sdcard/          # 本地评测原盘，按发布版本放置（Git 忽略）
└── docs/            # 系统设计文档、模块说明与技术记录
```

---

## 📚 文档索引

更深入的设计考量与实现细节，请参考以下专题文档：

- 📐 **共同设计契约**：[内核接口、并发与持久化约定](docs/design.md)
- 🧭 **文档中心与导航**：[完整文档索引目录](docs/README.md)
- 🛠️ **工具链与环境**：[交叉编译器与 QEMU 配置指南](docs/toolchain.md)
- 🎯 **开发路线**：[未完成工作](docs/goals.md)
- 📦 **第三方组件授权**：[第三方开源组件说明](docs/third-party.md)
- 🤝 **参与贡献**：[贡献指南](CONTRIBUTING.md)
