# 第三方代码

## lwIP

- 上游地址：<https://github.com/lwip-tcpip/lwip>（Savannah 上游的官方镜像）。
- 固定版本：`STABLE-2_2_1_RELEASE`，peeled commit `77dcd25a72509eb83f72b033d219b1d40cd8eb95`；`references/sources.tsv` 固定恢复身份，2026-09-27 核对。
- 导入路径：`third_party/lwip/`，保留 `src/core/`、`src/include/` 和 `COPYING`；未导入 socket/netconn API 实现、应用、PPP 和网卡驱动源码。
- 许可证：BSD-3-Clause，原有版权和许可声明保留在每个源码文件及 `COPYING`。
- 用途：向 BoarOS 自有 socket fd/OFD 与 Linux ABI 层提供单 hart IPv4 UDP/TCP 协议核心；本阶段先用 NO_SYS raw API 和 loopback，网卡后端另行验证。
- 本地修改：导入的上游文件未修改；BoarOS 的 lwIP 配置、端口和 socket 所有权适配位于 `net/` 与 `fs/`，不以 lwIP 的 socket fd 空间代替 BoarOS 文件表。

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

- lwext4 rename 扩展提供 parent-inode/name 接口，原子维护两侧目录项、HTree checksum、`..`、链接数和覆盖 orphan；空目录检查校验记录边界与 checksum。路径分量扫描补齐 255 字节名称的分隔符边界。验证入口为 `make test-lwext4-rename-host`，未升级上游固定 commit。

- 显式时间扩展提供 `ext4_file_set_times` 位掩码事务接口，沿用 inode 真实字段、错误 owner 和同步依赖；时间范围端点按 Linux 清零纳秒。统计扩展返回校验后的 metadata/journal overhead、保留块及 UUID，静态开销按 mount 缓存，动态分配数读真实 superblock。修正 first_data_block 组边界、META_BG 主描述符定位和 sparse_super2 备份组判断；`make test-lwext4-metadata-host` 验证几何、OOM、I/O、损坏和 e2fsck。

SQLite 3.53.4 作为固定外部测试构建输入使用：官方 amalgamation 压缩包保存在被忽略的 `references/sqlite/`，来源和 SHA-256 由 `references/sources.tsv` 固定；构建时在 `build/riscv/sqlite/` 解包，未把 SQLite 源码导入 Git，也未对上游源码作本地修改。其发布声明为 public domain；用途是验证原生 Unix VFS、记录锁与回滚日志持久性，见[程序环境](modules/program-environment.md)。

glibc 2.44 作为外部测试输入使用：官方源码归档保存在被忽略的 `references/glibc/`，来源和 SHA-256 由 `references/sources.tsv` 固定；RV64 loader/libc 直接取自宿主 GNU 交叉工具链，并由 `tests/userland/glibc/inputs.json` 的 SHA-256 固定，未导入 Git 或修改。相关源码按 LGPL-2.1-or-later 发布，归档含 `COPYING.LIB` 和 `LICENSES`；本仓库新增的探针只测试其原生启动、TLS、pthread、信号与退出路径，见[程序环境](modules/program-environment.md)。

- 单 hart 可睡眠扩展：mount 读写锁、任务 transaction owner、纯定位 `ext4_fpread`、同块 loading 等待、RELATIME 无副作用读预检；orphan/journal/恢复/卸载统一独占。生产 owner 检查不依赖可关闭的 debug assert。写事务仍单 owner 且保持日志格式与提交顺序，未升级上游版本；验证见 `test-io-sleep-riscv` 与 `test-lwext4-recovery-host`。
- 多实例扩展：锁回调显式携带 context，BoarOS 适配按实例登记名称、块设备、锁和错误 owner；公共堆绑定按引用计数维护。新增固定源 inode 的硬链接事务入口，同步目录项、链接数和时间戳；最后链接才进入 orphan 生命周期。验证为 `test-lwext4-instances-host` 与扩展的 `test-lwext4-rename-host`，上游 commit 不变。
- 异步日志扩展：操作级私有 undo/资源预留、mount running group、不可变 metadata/data/log 版本及独立 worker；有预算的日志映射、未提交 home writeback 禁止、释放块/inode quarantine、full/data 同步目标与版本 checkpoint。保持现有磁盘格式和 ordered/log/commit 屏障，未启用 ASYNC_COMMIT 特性，也未升级上游。旧“即时提交”描述仅适用于未启用组引擎的路径；生产 journal mount 使用组提交。实际 1/4KiB 引擎、RV64 IRQ/lifecycle、完整恢复及原消费者验收见 [VFS模块](modules/vfs-ext4.md) 和 [机制分析](learning/cost-baseline.md#异步日志与组提交验收2026-10-01)。

## BLAKE2s

- 固定来源：`references/linux/lib/crypto/blake2s.c`，Linux commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e`。
- 本地路径：`kernel/blake2s.c`、`include/kernel/blake2s.h`；保留 Jason A. Donenfeld 的版权与 `GPL-2.0 OR MIT` 声明，项目按 GPL-2.0 使用。
- 用途与修改：移植为无动态分配的便携 BLAKE2s-256 混种接口，去除 Linux 专用调用约定；中间状态显式清除。ChaCha20 fast-key-erasure 契约另对照同版本 `drivers/char/random.c`，密钥更新材料不返回给调用者。
- 验证：`make test-random-host` 比较已知向量、分块边界与旧输出预测下一密钥的回归；确定性向量不证明熵源质量。
