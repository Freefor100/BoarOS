# 第三方代码

## lwext4

- 上游地址：<https://github.com/gkostka/lwext4>
- 固定版本：`58bcf89a121b72d4fb66334f1693d3b30e4cb9c5`
- 导入路径：`third_party/lwext4/`
- 导入内容：`include/`、`src/`、`LICENSE`、`README.md`、`CHANGELOG`
- 许可证：`src/ext4_extent.c` 和 `src/ext4_xattr.c` 为 GPL-2.0-or-later，其余导入源码为 BSD-3-Clause；上游说明组合后的库受 GPLv2 约束。
- 用途：在 BoarOS 自有 VFS 与块设备接口之后提供 ext2/3/4 磁盘格式实现。
- 本地修改：初始导入提交不修改上游源码；后续补全现代 superblock 中 `s_checksum_seed` 等字段的磁盘布局，识别 `metadata_csum_seed`，并让 bitmap、group descriptor、inode、directory、extent 与 xattr 的 metadata checksum 使用规范选择的种子；目录适配另加 `ext4_dir_entry_next_status()` 和原始记录推进入口，以区分 EOF、inode-zero 尾记录与块/格式错误。保留 JBD2 journal 与 superblock 自身原有的独立校验算法。内核构建 profile 只编译读取所需源码并关闭 journal、xattr、mkfs、debug/assert；BoarOS 的 heap、块设备、VFS 和负 errno 适配均位于 `kernel/`，不暴露 lwext4 类型。

BoarOS 采用 GPL-2.0-only；仓库根目录 `LICENSE` 保存完整许可证文本，第三方文件保留各自的上游版权与许可声明。

后续引入第三方代码时继续记录名称、上游地址、版本、SPDX 许可证、导入路径、用途和本地修改。来源或许可证不清楚时不导入；外部源码与本地适配尽量分开提交。

比赛规则、公开测例、Harness、编译器、QEMU 和固件是外部构建或测试输入，不属于项目源码。影响复现时在相关文档中记录版本。
