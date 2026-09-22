# 第三方代码

## lwext4

- 上游地址：<https://github.com/gkostka/lwext4>
- 固定版本：`58bcf89a121b72d4fb66334f1693d3b30e4cb9c5`
- 导入路径：`third_party/lwext4/`
- 导入内容：`include/`、`src/`、`LICENSE`、`README.md`、`CHANGELOG`
- 许可证：`src/ext4_extent.c` 和 `src/ext4_xattr.c` 为 GPL-2.0-or-later，其余导入源码为 BSD-3-Clause；上游说明组合后的库受 GPLv2 约束。
- 用途：在 BoarOS 自有 VFS 与块设备接口之后提供 ext2/3/4 磁盘格式实现。
- 本地修改：初始导入提交不修改上游源码；后续补全现代 superblock 中 `s_checksum_seed` 等字段的磁盘布局，识别 `metadata_csum_seed`，并让 bitmap、group descriptor、inode、directory、extent 与 xattr 的 metadata checksum 使用规范选择的种子；目录适配另加 `ext4_dir_entry_next_status()` 和原始记录推进入口，以区分 EOF、inode-zero 尾记录与块/格式错误。文件适配允许在按 inode extent/legacy mapping 计算的 maxbytes 内 seek 到 EOF 之后、精确分配逻辑块、对 hole 合成零，并以 sparse 方式执行写入和向上截断；legacy maxbytes 取间接指针树、`ext4_lblk_t`/哨兵及 inode 块计数容量的交集，不让大块文件系统的 64-bit offset 缩窄回绕到低逻辑块。新块初始化失败撤销 exact mapping，unwritten conversion 与 caller data 共享缓存路径。时间戳适配新增 mount 可选 realtime callback 和 live-inode touch，支持受 inode_size/extra_isize 约束的 signed epoch/纳秒编码、relatime、创建与父目录修改；即时 block-cache flush 传播 errno，失败缓冲由 mount 保留 dirty owner；文件写入/截断按操作固定并提交修改缓冲集合，防止分配中途 I/O 失败失去块 owner，也避免排空无关历史脏数据。保留 JBD2 journal 与 superblock 自身原有的独立校验算法。内核构建 profile 启用 journal/replay，并关闭 xattr、mkfs、debug/assert；BoarOS 的 heap、块设备、VFS 和负 errno 适配位于 `kernel/` 与 `fs/`，不暴露 lwext4 类型。

- 日志与恢复修改：加入设备 flush 回调、ordered data、修改前缓冲引用与 before-image、嵌套事务错误传播、checksum v2/v3/revoke 校验、checkpoint 后持久回收日志空间和 sticky journal 错误。superblock 更新进入日志，受限启动允许由合法 redo 修复扇区撕裂的 superblock。新增 `ext4_orphan.c` 支持 orphan_file/传统链，`ext4_truncate.c` 按实际 extent/间接块映射分批回收；公开修改入口接入持久 orphan 生命周期。块组读取不再隐式初始化位图，分配器才初始化；bitmap/inode/directory/extent 校验失败明确返回错误，索引目录不会静默降级后改写损坏元数据。恢复测试与承诺范围见 [VFS 模块](modules/vfs-ext4.md)。

BoarOS 采用 GPL-2.0-only；仓库根目录 `LICENSE` 保存完整许可证文本，第三方文件保留各自的上游版权与许可声明。

后续引入第三方代码时继续记录名称、上游地址、版本、SPDX 许可证、导入路径、用途和本地修改。来源或许可证不清楚时不导入；外部源码与本地适配尽量分开提交。

比赛规则、公开测例、Harness、编译器、QEMU 和固件是外部构建或测试输入，不属于项目源码。影响复现时在相关文档中记录版本。
