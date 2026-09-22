# 文件时间戳：请求边界、live inode 与 relatime

## 固定证据

Linux 参考均为 `references/linux/` commit
`f4cdf7ca9a1fdcca413157df19753f388a5a224e`：

- `fs/ext4/file.c` 的 `ext4_file_read_iter()` 对零长度请求直接返回，跳过 atime；
  `ext4_write_checks()` 在 `file_modified()` 之前检查请求是否非零。
- `mm/filemap.c:filemap_read()` 在实际 read_iter 路径末尾调用 `file_accessed()`，
  因此 EOF、部分复制和首字节 user fault 不能统一视为“没有访问”。缓存命中同样经过这个路径。
- `fs/inode.c` 的 `relatime_need_update()` 在 `atime <= mtime`、`atime <= ctime` 或
  `now.tv_sec - atime.tv_sec >= 86400` 时允许更新；`touch_atime()` 不让 metadata I/O 错误
  覆盖 read 的结果。`file_modified()` 在用户数据复制之前修改 mtime/ctime。
- `fs/open.c:do_sys_ftruncate()` 请求 `ATTR_MTIME|ATTR_CTIME`，同长度 truncate 也更新；
  `fs/ext4/inode.c:ext4_setattr()` 保持 inode 的数据与 metadata 修改路径。
- `init/do_mounts.c:init_mount()` 和 `fs/namespace.c` 的普通 mount 路径默认使用 relatime。
  `rootflags=relatime` 会被传给 ext4 的 filesystem 参数解析器并被当前基线拒绝，不能用它
  代替 VFS 的 mount 选项。

lwext4 基线为仓库内 `third_party/lwext4/`，上游固定 commit
`58bcf89a121b72d4fb66334f1693d3b30e4cb9c5`，本地修改另见 `docs/third-party.md`。

## 所有权和更新位置

VFS 在 mount 上绑定可选 realtime 回调，未初始化 kernel clock 时返回“不可用”，保持 fixture 的
原时间。正常系统使用 `kernel_time_realtime_ns()`；回调结构保留 signed seconds 和 nanoseconds。
内部 `ext4_file_touch()` 通过 `ext4_file.inode` 取得 inode，不重新解析路径，因此 unlink 后的
仍打开文件继续更新自身时间。

文件层在完成 fd/模式/用户范围检查之后、非零 I/O 请求开始时更新：read/pread 走 relatime，
普通文件 write/writev 在 usercopy 前更新 mtime/ctime。零长度和访问模式拒绝不更新；
非零首字节用户 fault 写仍更新 mtime/ctime。写入会先按实际 append/普通 offset 校验 inode maxbytes，`-EFBIG` 不更新时间；pread 按原始 count 在 MAX_RW_COUNT 截断及 EOF 判定前检查完整用户范围，含非法高地址的零长度范围，`-EFAULT` 不更新时间。实际数据返回值和 OFD offset 继续只统计 backend
提交的字节数，metadata 更新不构成数据进度。

创建时直接初始化新 inode 的 atime/mtime/ctime；成功目录链接/移除更新父目录 mtime/ctime，
unlink 更新子 inode ctime。truncate 在活 inode 修改后更新 mtime/ctime，包括同长度请求；
已改变大小但后续错误的 inode 状态仍按原 VFS mutation reconciliation 保持可见。

扩展时间字段同时受磁盘 inode_size 和 extra_isize 限制。128-byte inode 只保存 signed32 seconds；
扩展 inode 使用 signed32 基数加两位 epoch，范围为 `[-2147483648, 15032385535]` 秒，
其余 30 位保存纳秒。写入超范围时间按 inode 支持范围截断，不把 2038 年之后时间解释成负数，
也不把不存在的 extra 字段写进下一 inode。

## I/O 失败与持久化

`ext4_fs_put_inode_ref()` 标记 inode 所属 metadata block 为 dirty；已修复上游 `ext4_bcache_free()` 即时 flush 忽略 errno 的问题，失败缓冲保留在 mount dirty list，引用 owner 完成转移后向调用层返回真实错误。touch 直接释放目标 inode 引用即可取得提交结果，不再用全量 cache write-back drain；零更新、无时钟和只读访问不为本次 touch 新增 flush。truncate 和普通写入在建立完整内存分配关系前固定本次修改的缓冲，收尾只提交本次集合，防止位图 I/O 失败中断在块尚无 inode owner 的位置。

写侧 touch 错误在数据提交之前返回；读侧 atime 错误按 Linux 规则不改变 read 返回值。
flush 失败后的 dirty block 由 mount block cache 持有，后续显式 cache flush/unmount 可恢复；
不把已经释放的 inode 临时引用或 fd 当成重试 owner。内存 inode 已更新不等于磁盘已持久化。
创建/链接的事务原子性仍需 journal 集成验证；即时错误传播不等于已有崩溃恢复。

## 验证

```sh
make test-files-partial-write-riscv
make test-lwext4-host
make test-userland-riscv
make test-diff-abi-riscv
```

聚焦文件测试向真实块写回调注入一次 I/O 错误：修改 touch 返回 `-EIO`、数据/offset/size 不推进，
mount 的 dirty inode 随后实际重新写出并可读回；同时验证零长度写不消费注入的错误。
该测试在修复前实测返回 1，修复后返回 `-EIO`；同长度 truncate 的 metadata 错误测试则从错误返回 0 修复为 `-EIO`。

host 测试使用真实 128/256-byte inode ext4 镜像和控制时钟，覆盖未初始化时钟、只读挂载、
relatime 相等/抑制/满 24 小时、负 epoch、2038/2106 边界、上限截断及纳秒编码；镜像最后经
`e2fsck -fn` 检查。

真实 musl 和固定 Linux 差分覆盖 create、write/read/pread、缓存命中、EOF、零长度、拒绝访问、
首字节/部分 fault、同长度/扩大/缩小 truncate、unlink 及 unlink 后继续读写，并检查父目录。额外五组真实差分验证 write maxbytes 拒绝、pread 高地址（含零长度和 EOF）及原始 count 溢出的前置错误时机。
原始观察保留全部时间字段及操作前后 CLOCK_REALTIME，不把时间戳归零后比较。
Linux `current_time()` 可以取 coarse clock，所以操作区间在一次 50ms 等待前开始，
比较实际变更字段是否落在完整区间内，以及字段相等/relatime 条件是否一致。
当前未覆盖 mmap 本身的 atime、utimensat、noatime/lazytime 可配置挂载或 SMP 时间更新锁。
