# 第三方开源组件与许可说明

BoarOS 内核本身采用 **GNU General Public License v2.0 only（GPL-2.0-only）** 开源协议（见 [LICENSE](../LICENSE)）。随仓库引入或辅助构建的第三方开源组件均完整保留上游版权与许可声明，绝不混用未标明来源的代码。

---

## 一、集成于仓库的第三方组件

| 组件名称 | 上游版本与来源 | 在 BoarOS 中的角色与本地工作边界 | 开源许可 |
| :--- | :--- | :--- | :--- |
| **lwIP** | [`STABLE-2_2_1_RELEASE`](https://github.com/lwip-tcpip/lwip/tree/STABLE-2_2_1_RELEASE) | 提供基础 TCP/IP 协议状态机。BoarOS 实现了 Linux 套接字层、阻塞等待、按连接分配处理器核、路由快照和链路层共享内存接收环，并在 IPv4 分片重组及 `TIME_WAIT` 回收路径中补充对象所有权管理。详见 [多核网络协议栈](modules/kernel-network.md)。 | [BSD-3-Clause](../third_party/lwip/COPYING) |
| **lwext4** | [上游提交 `58bcf89`](https://github.com/gkostka/lwext4/tree/58bcf89a121b72d4fb66334f1693d3b30e4cb9c5) | 提供 ext4 磁盘元数据与区段格式解析基础。BoarOS 在其上构建了完整 VFS 层、统一页缓存、延迟分配与元数据空间预留、JBD2 多核日志调度、有序直接 I/O、多核目录级并行锁与块组位置缓存（详见 [VFS 与 ext4](modules/vfs-ext4.md)）。 | [上游许可证](../third_party/lwext4/LICENSE) |
| **BLAKE2s** | [Linux 内核通用实现](https://github.com/torvalds/linux/blob/f4cdf7ca9a1fdcca413157df19753f388a5a224e/lib/crypto/blake2s.c) | 用作内核随机源的密码学混种压缩函数，仅以独立单文件集成。 | GPL-2.0 或 MIT（见 [kernel/blake2s.c](../kernel/blake2s.c)） |
| **pahole 1.28 & elfutils 0.192** | [dwarves](https://fedorapeople.org/~acme/dwarves/) · [elfutils](https://sourceware.org/elfutils/) | 作为宿主机离线构建工具保存于 `third_party/sources/`，用于将 BoarOS 内核 ELF 的 DWARF 调试信息转换为真实 BTF 类型信息，不链接进内核本体。 | 见 `third_party/sources/` 原始归档内许可文件 |

---

## 二、用户态根盘与辅助验证工具

下列组件不进入内核源码树，仅用于制作交互式根盘或在验证环境中作为独立用户态工具运行：

| 工具名称 | 来源与版本 | 用途 | 许可 |
| :--- | :--- | :--- | :--- |
| **BusyBox 静态包** | [Alpine Linux v3.23](https://dl-cdn.alpinelinux.org/alpine/v3.23/main/) | 制作本地交互式 ext4 根盘（`make shell-*`），提供串口登录与基础 Shell 命令。 | GPL-2.0-only |
| **strace 7.3** | [strace 官方发布](https://github.com/strace/strace/releases/tag/v7.3) | 由 [tools/build-strace.py](../tools/build-strace.py) 构建，用于在双架构上验证 `ptrace` 系统调用跟踪接口。 | LGPL-2.1-or-later |

开发与差分验证所参考的外部标准规范与上游源码清单（均按提交哈希固定，不复制到内核源树中），详见 [references/README.md](../references/README.md) 与 [references/sources.tsv](../references/sources.tsv)。
