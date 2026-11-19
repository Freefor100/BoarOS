# 本地参考资料

`references/` 保存本地阅读资料和外部构建输入。需要哪一项时从上游取得，Git 源码直接用各自仓库的版本记录。

| 来源 | 用途 |
|---|---|
| [Linux](https://github.com/torvalds/linux)、[QEMU](https://gitlab.com/qemu-project/qemu)、[OpenSBI](https://github.com/riscv-software-src/opensbi) | 对照系统调用和内核机制，查模拟设备与固件交接 |
| [RISC-V 架构手册](https://docs.riscv.org/reference/isa/)、[LoongArch 架构手册](https://github.com/loongson/LoongArch-Documentation)、[VisionFive 2 板级文档](https://doc-en.rvspace.org/) | 查指令、页表、异常与板级设备 |
| [比赛规则](https://gitlab.eduxiji.net/csc1/csc-os/os2026)、[公开测例](https://github.com/oscomp/testsuits-for-oskernel)、[评测框架](https://github.com/oscomp/autotest-for-oskernel) | 查原始规则、程序和判分。原盘统一放仓库根目录的 `sdcard/` |
| [构建输入清单](sources.tsv) | 供使用固定源码或工具归档的构建脚本读取 |
| [第三方组件](../docs/third-party.md) | 查仓库中随源码分发的组件来源、用途和许可 |

若本地 Linux 工作树用了稀疏检出，先展开相关目录，再核对对应实现。
