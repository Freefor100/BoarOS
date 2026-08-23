# 本地参考资料

本目录保存不会纳入 BoarOS Git 历史的外部资料。新工作区执行
`make references`，恢复器会完整克隆需要分析历史的仓库、把新恢复的 Git
工作区以 detached HEAD 固定到清单 commit，并校验下载文件的 SHA-256。重复执行只校验现有
内容；来源不符、固定版本不符、校验失败或 Git 工作区有修改时立即失败，
不会覆盖本地分析。

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
