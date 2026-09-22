# BoarOS

<p align="center">
  <img src="assets/boaros_header.png" alt="BoarOS 吉祥物与字标" width="100%">
</p>

BoarOS 是从零搭建、面向 OS Comp 能力建设的 C / 少量汇编内核，目标是运行未经 BoarOS 特改的 Linux 用户程序。Linux 是用户可观察契约和机制参考：在声明支持的范围内，返回值、对象身份、共享关系、错误、并发与生命周期必须正确；内部结构不必复制 Linux。

## 当前能力

当前生产路径为 **RV64、QEMU virt、单 hart、Sv39 / 4 KiB 页**。下表是已验证子集，具体接口与限制见模块文档。

| 范围 | 已有能力 | 主要边界 |
|---|---|---|
| 启动与内存 | OpenSBI、DTB、高半区/direct map、buddy/slab、连续页和引用回收 | 无 SMP；任务栈有 canary/高水位，没有未映射 guard page |
| 虚拟内存 | VMA、按需匿名页、文件私有映射、fork COW、权限和 fixed replace、跨 MM 截断撤映射 | 拒绝 `MAP_SHARED`；无共享后备对象、可写共享页或 `msync/mremap` |
| ELF / exec | 按需 ELF、PIE、`PT_INTERP`、初始栈/auxv、musl DSO/TLS、失败保持旧映像 | glibc 未独立验证；无 shebang、`getrandom` |
| 进程与等待 | fork/vfork、pthread clone、线程组退出、非组长 exec、wait/zombie/reparent、FIFO 抢占、时钟与睡眠 | 合法 clone 组合仍有限；无完整会话/TTY；单 hart 关中断不等于跨核同步 |
| futex / 信号 | WAIT/WAKE/REQUEUE、超时/重启、标准信号、用户 handler、`rt_sigtimedwait` | 无 robust-list、跨 MM 共享 key、实时信号队列和 `sigaltstack` |
| 文件与事件 | fd/OFD 分离、dup/CLOEXEC、共享 offset、阻塞 pin、部分/向量/定位 I/O、pipe、poll/select/epoll；ext4 节点按设备号接入 null、zero、console | 无 devfs、完整 TTY、socket 后端或记录锁；设备 mmap 未支持 |
| 路径与 ext4 | 持引用的 mount/inode/目录项路径对象，可写/只读根盘、符号链接、目录枚举、稀疏文件、活 inode 时间、打开后删除、私有映射截断 | cwd 固定 `/`；相对路径只支持 `AT_FDCWD`，无目录 fd 起点、硬链接、rename、多挂载 |
| 缓存与存储 | read/write/private fault 共用文件页、inode 脏范围与定向写回、OFD 错误观察、`fsync/fdatasync/O_SYNC/O_DSYNC`；VirtIO legacy/modern flush | 无后台写回线程；journal/replay 尚未接入生产，仍拒绝需 recovery 的 ext4 |
| 身份与资源 | 单用户 root 的 UID/GID 查询；线程组共享并执行 NOFILE/STACK，fork 继承、exec 保留 | 无凭据变更/完整权限；fd 硬容量 1024、栈硬容量 8 MiB；其他有效 limit 返回 `ENOTSUP` |
| 平台与网络 | RISC-V QEMU 真实根盘 `/init` 与 musl 用户态 | 无 socket 传输链、外部中断、LoongArch、实板或多核验证 |

文件层已有部分读写、OFD 生命周期、稀疏文件与私有映射截断的语义深度；cwd/dirfd、完整 TTY、共享文件页和持久化协议仍有结构性缺口。可写 ext4 不代表掉电可靠，动态 musl 通过不代表完整 glibc 兼容。

固定 BusyBox/libc-test 的最近全量记录为 `build/p1bc-full-final3`：228 个顶层案例全部完成，215 项双侧一致、10 个直接 entry 退出不符、3 个包装脚本断言失败。相对 `7971eedb` 基线，静态/动态 `stat` 与 `syscall_sign_extend` 共四项新增通过；剩余直接失败为 `daemon_failure`、`pthread_robust_detach`、`socket`、`statvfs`、`utime` 的静态/动态版本。包装脚本与 entry 有重叠，不能当成独立缺陷探针，也不能把清单生成成功当成全部通过。证据与复现见[程序清单](docs/learning/user-program-inventory.md)。

## 构建与验证

需要 RISC-V bare-metal GCC/binutils、GNU Make 和 QEMU；支持 `riscv64-unknown-elf-` 与 `riscv64-elf-` 前缀。真实用户态和 Linux 差分的额外工具见[工具链](docs/toolchain.md)及[差分模块](docs/modules/differential-abi.md)。

```sh
make all                       # kernel-rv
make test-riscv                 # 通用模块、架构与真实根启动
make test-userland-riscv        # 静态 musl、动态 pthread / TLS
make test-diff-abi-riscv        # 同一 ELF 对照固定 Linux
make test-stack-usage
make test-lwext4-host
make inventory-userland-riscv  # 能力清单，不是必过门禁
make test-references
```

聚焦测试只在对应[模块文档](docs/README.md)维护。`make run-riscv` 不附根盘，启动后停留 timer-idle，需人工退出；`make debug-riscv` 以 `-S -s` 等待 GDB。完整比赛 Harness 当前因缺少 `kernel-la` 等能力阻塞，不算已通过。

## 近期工作与文档

[TODO 与阶段依赖](docs/goals.md)集中维护下一步、阻塞与验收：先处理真实程序暴露的设备/路径/元数据和 robust 缺口；共享匿名对象可独立设计，文件共享依赖统一文件页与同步协议。glibc 试跑、LoongArch 最小入口和网络按各自依赖推进；SMP 先验证所有权、唤醒和 TLB 回收，再谈调度策略与性能。

- [文档导航](docs/README.md)：模块契约与可复用学习材料。
- [工程原则](docs/design.md)与[贡献说明](CONTRIBUTING.md)：技术取舍、验证与提交边界。
- [固定资料](references/README.md)与[第三方代码](docs/third-party.md)：版本、来源及许可。
