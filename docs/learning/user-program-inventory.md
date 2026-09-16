# 固定真实程序的失败清单

本入口用于收集外部真实程序的构建、启动和语义阻塞，不是比赛成绩，也不代表完整 BusyBox/libc-test 已通过。执行器只保存观察结果，不修改内核或外部源码来绕过失败。

## 输入与许可证

输入固定到 `references/sources.tsv` 中的 `references/oscomp-testsuits/` commit
`b5ec6ef8497e1818cbdec3b54bb722f036e57972`。该 commit 的 `README.md` 是决赛
CAgent/BuildStorm 说明；`Makefile.sub` 的 BusyBox 配方使用
`config/busybox-config-riscv64` 和静态编译器。

此 commit 根目录没有 `libc-test`：`git ls-tree --name-only <commit>` 的原始输出保存在
`source-tree.log`。因此 libc-test 标为 `build-blocked`，原因是固定输入缺失；不切换到另一分支，
也不把 BoarOS 自有 libc 测试当成上游 libc-test。

BusyBox 的 `busybox/README`、`busybox/INSTALL` 和 `busybox/LICENSE` 是使用与构建入口。
该版本采用 GPL-2.0-only，许可证原文和源码留在固定外部资料中；本仓库仅维护驱动和配置，
构建得到的二进制不入库。后续若分发该二进制，应一并履行该许可证的源码提供要求。

## 复现

先准备本仓库现有 RISC-V musl 工具链、生产 `kernel-rv` 与差分 Linux 基线，然后运行：

```sh
make inventory-userland-riscv
make test-program-inventory-host
```

依赖与差分入口相同，包含宿主 `bc`；本地临时解包的工具可通过 PATH 提供，不属于仓库依赖安装方式。

默认结果在忽略目录 `build/program-inventory/`；`--output` 可保留不同实验目录，
`--timeout` 设置每个内核的启动/运行超时。重新运行会覆盖同一输出目录中的生成源码与日志；
需保留旧结果时指定新目录。脚本不下载外部依赖，不修改 `references/oscomp-testsuits/`。
BusyBox 源码与原配置经 `git archive` 从固定 commit 复制到构建目录。

先真实尝试原比赛配置；若构建失败，保留 `busybox-competition` 的 `build-blocked` 记录，
再用仓库中的 `busybox-smoke.config` 构建有限 applet 子集。最小配置只改变构建选项，
不修改 BusyBox 源码；它的结果单独记为 `busybox-smoke`，不能代替比赛配置结果。
静态 musl 编译沿用本仓库工具链；若编译器支持，使用与现有用户态构建一致的
`-fno-link-libatomic`，避免工具链自动请求不存在的 `libatomic_asneeded`。

Linux 使用 `tests/diff-abi/harness.py` 的固定源码/配置/工具身份缓存；fixture 与 QEMU
启动形态沿用同一入口：512 MiB、单 hart、VirtIO MMIO ext4 根盘。两个内核各取得相同初始
磁盘副本。驱动对同一 BusyBox ELF 执行 `true`、`false`、`echo`、`cat`、`ls` 与一条
调用外部 `cat` 的 shell 命令，收集完整 stdout/stderr 字节及真实 `waitpid` 状态。
`false` 的退出码 1 是该命令的正确行为，不作为执行失败。

## 记录和判定

`inventory.json` 保存固定 revision、源码归档和许可证 SHA-256、原始及解析后配置 SHA-256、
编译器/binutils/make/QEMU/mkfs 身份、musl archive/specs 身份、完整执行命令、构建和串口日志路径、
Linux 构建身份、BusyBox/驱动/BoarOS ELF 和 fixture 校验值。`linux.log`、`boaros.log` 保留完整
原始输出；测试输出另以十六进制编码，防止程序文本被误认为结构化结果。

- `build-blocked`：固定源码或工具缺失、配置或编译失败；明确保留原因和日志。
- `missing-capability`：已有直接证据，例如驱动记录 `execve errno=38`（ENOSYS）；不凭非零退出猜测缺失 syscall。
- `semantic-mismatch`：Linux 基线本身未按命令契约退出，或 BoarOS 的 wait status/输出与有效 Linux 观察不一致。
- `crash`：子程序被信号终止，或 QEMU 已退出但没有完整程序记录。
- `timeout`：QEMU 到期且对应程序没有完整记录；之前已完成的程序保留独立结果。
- `protocol-error`：运行记录缺失、重复、乱序或格式不合法；不能当作完整执行。
- `pass`：Linux 命令退出符合契约，BoarOS 与该有效基线的退出状态和原始输出均相同。

`guest_runs` 单独记录 QEMU 退出、严格 BEGIN/有序结果/END 协议及 BoarOS 最终回收状态；`runtime_status` 只有双侧完整退出且语义一致时才是 `passed`。超时前已经完成的用例可保留独立结果，但不能把不完整运行标为整体通过。

这些状态是清单，不是自动放宽的回归预期。清单生成成功时脚本退出 0，即使其中有构建阻塞或
程序失败；执行器自身异常退出非零。审查时必须读取每项状态和原始日志，不能把脚本退出码当成
所有程序已通过。

## 2026-09-16 的首轮事实

按上述固定输入真实执行后，原比赛配置构建在 `console-tools/kbd_mode.c` 包含
`linux/kd.h` 时失败：当前 musl 工具链安装树没有该 Linux UAPI 头。
日志为 `build/program-inventory/busybox-build.log`；这属于构建环境阻塞，不能归因于 BoarOS syscall。
最小配置完整构建成功，六个 applet 用例在固定 Linux 和 BoarOS 上的输出、wait status 全部一致，
包括 `false` 的退出码 1 以及 shell 通过 `/busybox cat` 执行外部程序。
BoarOS 最终记录 PID 1 状态 `0x2a`、`heap-live=0x0`。
该轮未覆盖网络、procfs、复杂 shell/job control 或完整比赛脚本。
