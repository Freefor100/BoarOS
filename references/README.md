# 本地参考资料

本目录保存不会纳入 BoarOS Git 历史的外部资料。新工作区执行
`make references`，恢复器会完整克隆需要分析历史的仓库、把新恢复的 Git
工作区以 detached HEAD 固定到清单 commit，并校验下载文件的 SHA-256。重复执行只校验现有
内容；来源不符、固定版本不符、校验失败或 Git 工作区有修改时立即失败，
不会覆盖本地分析。

## 恢复、校验与阅读

```sh
make references
make test-references
```

`sources.tsv` 是来源、固定 revision 和文件校验值的唯一清单。`full` 项保留上游历史，适合
比赛规则、Harness 和公开测例的分支分析；`snapshot` 项只保证清单 commit 的源码，不保证父提交
或完整历史；`file` 项只保证下载文件的 SHA-256。引用结论时写明本地路径和清单中的 commit、
tag、文档版本或 SHA-256。完整仓库若临时切到其他分支，还要用
`git -C references/<name> rev-parse HEAD` 记录实际 commit；分析后切回清单 commit 才能再次通过校验。

源码先用 `rg` 在本地路径搜索，再打开命中的上下文。Linux 工作区可能同时是 shallow、partial
clone 和 sparse checkout；可分别用以下命令确认：

```sh
git -C references/linux rev-parse HEAD
git -C references/linux rev-parse --is-shallow-repository
git -C references/linux config --get remote.origin.partialclonefilter
git -C references/linux sparse-checkout list
```

工作树里没有路径或对象、`git log` 看不到父提交，都不能作为 Linux 不存在该实现或历史的证据。
先用 `git -C references/linux sparse-checkout add <目录>` 展开固定 commit 所需目录；读取缺失 blob
时让 Git 从清单 origin 补取该对象，不切换 HEAD。只有固定 commit 本身不含所需主题，或对象确实
无法恢复时，才查 Linux 官方文档或上游仓库，并明确它不是当前固定基线。

musl 以校验过的 `musl/musl-1.2.5.tar.gz` 保存，不在 `references/` 另建源码快照。列出和读取
单个成员可直接使用：

```sh
tar -tzf references/musl/musl-1.2.5.tar.gz
tar -xOf references/musl/musl-1.2.5.tar.gz musl-1.2.5/src/dirent/seekdir.c
```

确需整树阅读时解包到仓库外的临时目录或已忽略的 `build/`，结束后删除。不要 `git add -f`
恢复出的仓库、PDF、压缩包或解包树，也不要把手工下载的网页副本留在仓库其他位置成为未跟踪
快照；要长期固定的新输入应加入 `sources.tsv`，由恢复器校验。

若本地输入缺少当前问题必需的官方新规则、勘误或板级资料，可回退到标准组织、上游项目或厂商
官网。记录 URL、版本和访问日期；若网页内容与固定输入冲突，先保留固定输入作为可复现基线，
说明差异并确认是否更新清单，而不是混用两个版本。

## 比赛输入

| 本地路径 | 上游 | 保存方式 |
|---|---|---|
| `os2026/` | [OS Comp 2026 规则](https://gitlab.eduxiji.net/csc1/csc-os/os2026) | 完整仓库 |
| `oscomp-testsuits/` | [内核公开测例](https://github.com/oscomp/testsuits-for-oskernel) | 完整仓库，保留全部历史、分支和 tags |
| `oscomp-autotest/` | [自动测试 Harness](https://github.com/oscomp/autotest-for-oskernel) | 完整仓库 |

恢复只取得资料，不执行任何外部测例或 Harness。分析
`oscomp-testsuits` 的某个分支时应记录实际 commit；允许手动切换分支，
但切离清单 commit 或产生本地修改后不会通过下一次恢复校验。分析结束后
切回 `sources.tsv` 记录的 commit 即可重新验证。

`tests/program-inventory/inputs.json` 是执行 profile：BusyBox 源码和配置取自
清单的 `b5ec6ef8497e1818cbdec3b54bb722f036e57972`，libc-test 和比赛脚本取自
同一个完整 object store 的附加 commit
`8b58dd16d26d30f7c74d48d5832d870d3051b703`（选择时分支名为 `pre-2025`）。
profile 使用明确 commit 做 `git archive`，不切换 `references/oscomp-testsuits`
的 HEAD，也不把易变分支名当作运行输入。它固定测试树选择，仓库 URL 与默认
HEAD 仍只由 `sources.tsv` 管理。

## 用户程序 UAPI 构建输入

`linux-uapi/linux-6.6.tar.xz` 是官方 Linux v6.6 源码 archive，下载 URL 与
SHA-256 唯一记录在 `sources.tsv`，由 `fetch.sh` 验证；环境脚本通过
`inputs.json` 中的 `archive_reference` 引用该项，不维护第二份 URL/校验值。
官方 tag v6.6 的 peeled commit 为
`ffc253263a1375a65fa6c9f62a893e9767fbebfa`，2026-09-16 从
[Linux 官方 Git](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git)
核对，并与 kernel.org 发布的 `sha256sums.asc` 核对 archive 哈希。

固定 `references/linux/Makefile` 的版本为 **7.2**；commit 标题提到的
子系统 tag 并不是 Linux 版本。它仍用于运行内核和默认 UAPI。
完整 BusyBox 1.33.1 的 `tc` 引用较新 Linux 已移除的 CBQ 定义，因此其
构建单独使用从官方 v6.6 archive 执行 `ARCH=riscv headers_install` 导出的
完整 UAPI。该选择不修改 BusyBox 源码、不裁剪 applet、不改变运行内核。
解包树及两套 UAPI 产物均在忽略的 `build/` 中；不混入宿主 glibc 头。

## 架构、模拟器与固件

| 本地路径 | 上游/版本 | 用途 |
|---|---|---|
| `riscv/` | [RISC-V Privileged Architecture 20260120](https://docs.riscv.org/reference/isa/v20260120/priv/priv-index.html) | Sv39、PTE、`satp` 与 `SFENCE.VMA` |
| `qemu/` | [QEMU v11.1.0](https://gitlab.com/qemu-project/qemu/-/tree/v11.1.0) | RISC-V 与 LoongArch 模拟器实现 |
| `opensbi/` | [OpenSBI v1.8.1](https://github.com/riscv-software-src/opensbi/tree/v1.8.1) | RISC-V 固件交接 |
| `linux/` | [Linux `f4cdf7ca9a1f`](https://github.com/torvalds/linux/tree/f4cdf7ca9a1fdcca413157df19753f388a5a224e) | 两种架构的成熟实现和板级 DTS |
| `loongarch-documentation/` | [LoongArch Documentation](https://github.com/loongson/LoongArch-Documentation) | 架构手册源码快照 |

BoarOS 的分页基线固定为 RISC-V64 Sv39/4 KiB 与 LoongArch64
16 KiB/三级页表。页大小是架构构建期常量；架构页表与 TLB 操作不在板级
代码中复用。

## 开发板

`visionfive2/` 保存 [StarFive JH7110 文档中心](https://doc-en.rvspace.org/Doc_Center/jh7110_hardware.html)、
[VisionFive 2 文档中心](https://www.starfivetech.com/en/index.php?c=category&id=28&s=docs)
发布的芯片数据手册、板级软件 TRM、启动指南和 SiFive U74 Core Complex
手册；`visionfive2-sdk/` 固定到官方 SDK `JH7110_VF2_6.12_v6.0.0`，但不递归
下载约 9 GiB 的 submodules。

`loongarch/` 保存以下龙芯官方发布资料的固定镜像：

- [LoongArch 架构参考手册卷一 v1.11](https://www.loongson.cn/uploads/images/2025032109191292796.%E9%BE%99%E6%9E%B6%E6%9E%84%E5%8F%82%E8%80%83%E6%89%8B%E5%86%8C%E5%8D%B7%E4%B8%80_r1p11.pdf)
- [2K1000LA 处理器用户手册 v1.0](https://www.loongson.cn/uploads/images/2022090113542571398.%E9%BE%99%E8%8A%AF2K1000LA%E5%A4%84%E7%90%86%E5%99%A8%E7%94%A8%E6%88%B7%E6%89%8B%E5%86%8C.pdf)
- [2K1000LA 处理器数据手册 v1.1](https://www.loongson.cn/uploads/images/2023053015362144181.%E9%BE%99%E8%8A%AF2K1000LA%E5%A4%84%E7%90%86%E5%99%A8%E6%95%B0%E6%8D%AE%E6%89%8B%E5%86%8C_V1.1.pdf)
- [LA 嵌入式统一系统架构规范 v1.1](https://www.loongson.cn/uploads/images/2025011014243453502.%E9%BE%99%E8%8A%AFCPU%E7%BB%9F%E4%B8%80%E7%B3%BB%E7%BB%9F%E6%9E%B6%E6%9E%84%E8%A7%84%E8%8C%83%28LA%E6%9E%B6%E6%9E%84%E5%B5%8C%E5%85%A5%E5%BC%8F%E7%B3%BB%E5%88%97%29V1.1.pdf)
- [2K1000LA 星云板资料入口](https://gitee.com/loongarch_community/2k1000la-doc)

龙芯 PDF 从固定提交的公开文档镜像恢复，以避开厂商下载站的临时连接问题；
README 保留官方发布地址。星云板仓库只公开了指向网盘资料的 README，网盘
内容无法做匿名、稳定、可校验的自动恢复，因此不伪造为已纳管资料。

确切 URL、Git commit 与 SHA-256 以 [`sources.tsv`](sources.tsv) 为准。
更新任何固定输入都应单独提交并重新运行 `make test-references`。
