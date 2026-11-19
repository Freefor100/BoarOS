# 第三方组件

BoarOS 采用 GPL-2.0-only。随仓库分发的第三方代码和归档保留上游版权及许可声明。

| 组件 | 来源与版本 | 用途 | 许可位置 |
|---|---|---|---|
| lwIP | [STABLE-2_2_1_RELEASE](https://github.com/lwip-tcpip/lwip/tree/STABLE-2_2_1_RELEASE) | 提供网络协议实现，BoarOS 维护套接字、等待和多核接入，见[网络模块](modules/kernel-network.md) | [BSD-3-Clause](../third_party/lwip/COPYING) |
| lwext4 | [上游版本 `58bcf89`](https://github.com/gkostka/lwext4/tree/58bcf89a121b72d4fb66334f1693d3b30e4cb9c5) | 提供 ext4 磁盘格式基础，BoarOS 在其上实现事务、缓存和并发适配，见[文件系统模块](modules/vfs-ext4.md) | [上游许可证](../third_party/lwext4/LICENSE) |
| BLAKE2s | [Linux 的通用 BLAKE2s 实现](https://github.com/torvalds/linux/blob/f4cdf7ca9a1fdcca413157df19753f388a5a224e/lib/crypto/blake2s.c) | 内核随机数发生器使用的混种函数 | [源码中的 GPL-2.0 或 MIT 声明](../kernel/blake2s.c) |
| pahole 1.28、elfutils 0.192 | [dwarves 发布页](https://fedorapeople.org/~acme/dwarves/)、[elfutils 项目](https://sourceware.org/elfutils/) | 离线生成 BPF 类型格式（BPF Type Format，BTF），见[构建说明](modules/kernel-bpf.md#构建与检查) | `third_party/sources/` 原始归档内的许可文件 |

外部规范和用于开发的上游源码见[参考资料](../references/README.md)。模块文档解释本地适配的设计，变更沿 Git 历史追查。

## strace 验证工具

[strace 7.3](https://github.com/strace/strace/releases/tag/v7.3) 用于验证 RISC-V 和 LoongArch 的进程跟踪接口。
构建入口为 [tools/build-strace.py](../tools/build-strace.py)，许可见原发布包。
