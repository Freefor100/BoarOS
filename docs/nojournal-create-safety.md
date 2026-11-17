# 无 journal create / O_TMPFILE：安全机制预审

状态：create/O_TMPFILE 尚未开放。本候选只修复无 journal 的重新挂载和错误状态保护；
发布三态仍是待实现设计，不是功能支持声明。
基线：`40e1bd99`。已有普通文件 O_DIRECT 保持原行为。

## 现有复用路径不能直接开放

- `namespace_create_execute` 在无 journal 时没有外层事务 undo；
  `ext4_create_child` 先发布目录项，VFS 随后才写最终 ACL、完成节点绑定。
  直接删除门禁可能在后续失败时暴露错误身份/权限或遗留名字。
- `ext4_tmpfile_child`、`ext4_tmpfile_link_child` 依赖 JBD orphan 事务。
  不能假造 journal，或把 orphan enrollment 改为成功的空操作就称支持完成。
- 目录添加失败可能已有目录块/extent/位图写入，之后 `nlink==0` 并不证明
  inode 未被目录引用。按失败返回回收 inode 会留下目录指向已释放 inode。
- `ext4_orphan_free` 有可用的无 journal 最后 owner 回收路径，但它只适用于
  **确定未发布**的 inode，不负责判断发布状态。

## 已执行的确定性恢复探针

修改前基线的独立探针在 1 KiB/4 KiB 块下观察到：

1. 外部预置已有普通文件，通过当前已支持的 open/write/unlink 留下仍由文件
   handle 持有的零链接 inode，不依赖尚未开放的 no-journal create/O_TMPFILE。
2. 写入 4 KiB，显式同步测试镜像，然后模拟掉电，不执行最后 owner 回收。
3. `e2fsck -fn` 返回 4，报告零 dtime、inode/block 位图和计数不一致。
4. 原实现重新可写挂载，`ext4_orphan_recover` 返回成功但没有发现该 inode。
   随后正常卸载把 superblock 标记为 clean；再次强制 fsck 仍报告同一泄漏。
5. 离线 `e2fsck -fy` 返回 1 完成修复，随后 `-fn` 返回 0。

本候选的 `sh tests/host/lwext4-metadata.sh nojournal-orphan-probe` 改为回归门禁：
原 dirty/error 镜像可写挂载返回 EUCLEAN 且没有设备写入；通过有效校验的只读挂载
可检查，但不能未经修复 remount 为可写，卸载不清除 dirty/error。真实 e2fsck 修复后
重新挂载、写入、DIRECT 故障清理及 fsck 均须通过。干净镜像的 RW/RO 往返仍可用。

当前挂载的 bcache 写回发生设备错误后锁存 `nojournal_error`，后续 mutation、
cache writeback、remount/umount 不得以重试把不确定元数据标 clean。最终 clean
superblock 写入失败时不发布 clean 内存状态。journal 会话不走这条保护；普通
DIRECT 设备数据错误若原有 extent 清理成功，并不自动等同于 metadata 错误。

这是一项保守兼容性变化：本实现不具备离线 fsck 的扫描修复能力，故不再允许
未修复 dirty/error no-journal 镜像重新可写。它不等同于 Linux 的全部挂载策略。
测试中显式同步仅控制故障镜像持久边界，没有修改生产 FLUSH 规则，也没有宣称
覆盖所有写入/掉电排列或完成真实 Linux 动态差分。

## 待实现且待独立审查的状态机

1. `unpublished`：分配无名 inode，准备最终 uid/gid/mode/ACL、时间和 VFS owner。
   发布前失败只可回收这个确切身份，不可通过路径 unlink 补偿。
2. `publishing-uncertain`：在第一次可能修改目录/目录 extent 之前进入。
   先具备保守 nlink/身份保护；之后的任何失败，包括 put_inode_ref 和 scope_end，
   均不得简单回退 nlink 或释放 inode。回读名字不存在及内存 undo 不能证明
   存储设备没有完成目录写前缀。
3. `published`：成功完成发布后的 owner/链接状态。O_TMPFILE linkat 也必须遵循
   同样的交接；lastclose、后台 cleanup 和 unmount 均需尊重不确定状态。

不确定状态需保留已分配 inode 并进入需修复/失败保护，不能允许后续 namespace
分配把它重用，也不能在普通卸载时把该状态清成 clean。宁可把未引用分配交给
fsck，也不能发生运行期误 free。

本候选已使重启路径拒绝未经修复的原始 dirty/error 无 journal 镜像重新可写；
仅在文档说“请 fsck”而继续自动挂载/标 clean 不满足恢复边界。

没有 journal 时，多块 metadata 持久化并非原子操作；无 barrier 时，marker 的
设备完成不等于可证明的掉电持久化顺序。上述状态机不得宣称无需 fsck、任意
断电无泄漏或具有日志式原子性。保留既有 FLUSH 策略，不用额外全局 fsync
掩盖未解决的发布/回收机制。

## 后续验证门禁

- 名字发布前的每个内存/ACL失败点：无名字、无错误权限、确切 owner 返回。
- 首次目录修改前后、put/scope_end 的设备失败：不误 free、不重用 inode，
  不确定状态不可被正常卸载清除。
- 匿名最后关闭、dup、linkat 成功/失败、O_EXCL、缓存/映射持有者和 unmount。
- 写故障与多处断电镜像的 fsck 修复；原 dirty/error 可写重挂拒绝及修复后恢复。
- 最终实现后再跑 host、RV/LA 和文件内容完整性检查；本阶段未声称这些功能测试通过。

参考 Linux v6.12 `fs/ext4/orphan.c:ext4_orphan_add`：无 journal 时直接返回，
不使用持久 orphan enrollment。该事实不替代本实现的 VFS owner/错误保护和
离线恢复设计。
