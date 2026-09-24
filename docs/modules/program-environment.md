# 用户程序构建环境

`tests/program-inventory/environment.py` 为完整上游程序准备 RISC-V Linux
UAPI 与现有 musl 1.2.5 工具链。它不修改 BusyBox 源码或关闭 applet，
也不把宿主 glibc 头加入交叉编译器搜索路径。

## 入口与固定来源

- `prepare()` 导出 `references/linux` 固定 commit
  `f4cdf7ca9a1fdcca413157df19753f388a5a224e` 的 `ARCH=riscv headers_install`。
  默认目录为 `build/program-environment/linux-uapi/include`，供 libc-test 等程序使用。
- `prepare(uapi='busybox')` 通过 `inputs.json` 的 `archive_reference` 引用 `references/sources.tsv` 的固定官方 Linux v6.6 archive
  导出完整旧 UAPI。v6.6 对应 commit
  `ffc253263a1375a65fa6c9f62a893e9767fbebfa`，archive SHA-256 为
  `d926a06c63dd8ac7df3f86ee1ffc2ce2a3b81a2d168484e76b5b389aba8e56d0`。
  来源为 `https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.6.tar.xz`；
  2026-09-16 核对官方 `sha256sums.asc` 与 Torvalds Git 的 peeled tag。
  该环境位于 `build/program-environment/busybox-environment/`。
- `build_busybox()` 从 `inputs.json` 选择固定 BusyBox 源码与配置，以 Git archive
  提取，再用 musl 静态链接。固定比赛 commit
  `b5ec6ef8497e1818cbdec3b54bb722f036e57972` 包含 BusyBox 1.33.1；
  原 `config/busybox-config-riscv64` 启用 398 applets。

两个 `prepare` 结果都返回 `include`、`cflags`、工具与源码身份、头文件树哈希。
调用方必须使用返回的完整 `cflags`；目前由 `-idirafter` 添加目标 UAPI，
并在编译器支持时使用 `-fno-link-libatomic`，避免发行版交叉 GCC 自动加入
不存在的 `atomic_asneeded`。musl 自带头仍先于 Linux UAPI。

Linux 7.2 UAPI 已移除 CBQ 定义，而固定 BusyBox 的 `tc` 仍引用这些定义。
完整 6.6 UAPI 是其编译环境；运行 Linux 内核继续使用固定 7.2 commit。
这里不混搭单个历史头，也不为通过构建裁剪 `tc`。这只建立编译兼容性，
不表示 BoarOS 已支持这些 applets 所需的全部系统调用。

## 失败、缓存和证据

固定源码 HEAD 或内容不符、archive 哈希错误、缺少工具、头文件导出或程序构建失败
均抛错。BusyBox `oldconfig` 若改变任一功能设置也失败；自动生成日期不参与此比较。
无 smoke 回退。`environment.json` 记录源码、工具及生成头的 SHA；只有身份与头文件树
均匹配才复用，重建旧 UAPI 时重新提取校验过的 archive。

构建期间 `full-busybox/` 保存 `source.tar`、原配置、`competition.config`、configure/build
日志、产物及 `build.json`；清理后只保留跨轮复用的源码构建树、配置、产物和身份文件。Linux 与 BusyBox 的源码许可证仍保留在各自固定来源或构建树中；
校验后的 archive 保存在 `references/linux-uapi/linux-6.6.tar.xz`；临时解包源码和构建产物位于忽略的 `build/`，不复制进内核源树。

## 验证

需要 `make`、宿主 GCC、RISC-V Linux GCC/binutils、`rsync`、`tar`、`curl` 与
已构建的 musl 工具链。首次准备旧 UAPI 需下载固定 archive。

```sh
make test-program-environment-host
make prepare-program-environment
make build-competition-busybox
```

host 测试保护源版本拒绝、头文件身份变化、配置功能不被裁剪及失败传播。
真实构建还编译含 musl、Linux 和 RISC-V `asm` 头的静态 probe；完整 BusyBox
产物必须交由 inventory 的 Linux/BoarOS 运行步骤继续验证。

## 双侧执行与失败所有权

`make inventory-userland-riscv` 调用 `run.py` 构建上述完整 BusyBox 和
`libc_build.py` 的固定 libc-test，再经 `suites.run_suite()` 逐例启动两侧 QEMU。
`--reuse-builds` 显式复用已核验的 revision 和 ELF/DSO 校验值；默认重新构建。
`--case`、`--suite` 缩小运行集合；`--require-pass` 对本次所选集合要求每项完成且通过，未选项保持历史结果或 `not-run`。未指定 `--case` 时全量 228 项均需完成且通过。执行状态保存本次 `selection`，恢复运行可改变集合而不把未选项伪装成通过；runner 中断、环境错误、参考侧失败与未知案例仍为失败。
`--output` 指定本次核对目录，默认 `build/program-inventory-full`。运行期间默认在每个案例结果、日志、输出及其目录完成 `fsync` 后删除已通过案例的三份可重建磁盘；失败案例磁盘和 suite 基础 fixture 暂存，以便当轮诊断。诊断时可用 `--keep-pass-images` 保留新执行案例的全部磁盘。核对后用 `python3 tests/prune-build.py` 预览、`make prune-build` 清理整个一次性运行目录和旧缓存。

suite identity 包含程序/驱动/内核/执行器/校验器身份、QEMU 与磁盘工具身份和超时配置。
镜像由同一 fixture 独立复制；每例持久化结果后才继续。中断留下显式未运行项；恢复前核对完整
身份，输入变化要求新输出目录。构建或执行器异常直接失败，没有精简程序的自动回退。

`/init` 驱动准备 proc、sysfs、tmpfs、mqueue 和 loopback，父进程监督真实子程序。
stdout/stderr 分开保留，子程序的文本不能伪造驱动边界。驱动分别记录 setup 与 exec errno，
以及 wait status、终止信号和超时；超时杀死并回收子进程，外层另有启动/客体退出超时。
Linux 环境准备失败使该例成为无效参考；BoarOS 缺能力的诊断保留，并继续执行不依赖它的程序。

`reports.py` 校验上游 BusyBox/libc 脚本的完整有序断言，shell 退出码不足以证明通过。
只有参考有效且双方退出状态、完整原始输出一致才记为 `pass`。suite 的 `complete` 仅表示
manifest 所有项目完成；清单本身不是必过测试。原始命令、日志、输出及失败案例磁盘保留；通过案例的磁盘可从固定输入和基础 fixture 重建。不能因
环境或参考失败把 BoarOS 标为通过。具体固定输入及证据边界见
[真实程序清单](../learning/user-program-inventory.md)。
