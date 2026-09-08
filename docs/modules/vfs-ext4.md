# VFS 与只读 ext4 模块

本文描述当前根文件系统的稳定接口和 lwext4 私有适配。第三方版本与许可证见[第三方代码](../third-party.md)，存储知识见[存储与文件系统学习总结](../learning/storage-filesystems.md)。

## 通用边界

`include/kernel/block.h` 定义同步只读块设备，`include/kernel/vfs.h` 定义不透明 mount/file 对象以及根挂载、open、pread、close、unmount。VFS 对外返回负 Linux errno；lwext4 的结构、全局设备名和正值 errno 不泄漏到调用者。当前只有一个根挂载与一个 lwext4 heap binding；进程 fd/open-file-description 位于独立的[文件资源层](kernel-files.md)，VFS 本身没有 mount namespace 或并发访问协议。

`kernel_vfs_file_read_source()` 把保持打开的文件导出为带 `size/context/read_at` 的精确随机读源。回调只有填满整个范围才返回零；EOF 以内的短读转成 `-EIO`。ELF parser 因而能复用内存和 VFS 来源，而不依赖文件系统类型。

`kernel_vfs_open_executable()` 在普通 open 之上统一要求 regular file 和至少一个执行位；目录、非普通文件或无执行位返回 `-EACCES`。生产 `/init` 与用户 `execve` 共用这一检查，权限拒绝时立即关闭临时 file；若清理需要重试，调用方仍保留 file owner。

## 文件节点与页缓存

VFS 为每个已解析普通文件维护引用计数 node；独立 open file description 各自保存 offset，
但可指向同一 node。根启动建立一个挂载共享的 4 KiB 页缓存，键为 `(node, page_index)`：
开放寻址哈希提供平均常数时间查找，双向 LRU 维护回收次序。缓存项持有 node 引用和一份
物理页引用；命中时再给调用者一份临时引用，因此 `read`、不同 fd 和 file-private mmap
可以安全共享同一只读页。

miss 路径先分配并清零页，再通过 node 的无 offset 副作用 `pread` 填充，记录尾页有效字节数。
读取整个越过 EOF 的页返回 `OUT_OF_RANGE`，尾页剩余字节保持为零。物理页分配器只有一个
压力回收槽，当前由该缓存注册；分配首次耗尽时从 LRU 尾部扫描，仅驱逐引用数为 1 的未固定页，
然后由分配器重试一次。被用户映射或正由 read 使用的页引用数大于 1，不会被回收。

缓存销毁和 mount 卸载有严格顺序：先释放所有进程 MM 和临时读者，再 purge 该 mount 的缓存
项、关闭最后的 node，之后才允许 lwext4 unmount；缓存最后注销 reclaimer 并释放哈希表。
页/node/堆对象释放失败进入各自 cleanup 链，后续 destroy 重试而不重新发布已经驱逐的项。

## lwext4 配置和生命周期

内核只编译 lwext4 读取路径需要的源码，关闭 journaling、xattr、debug/assert 和 mkfs，并把 malloc/calloc/realloc/free 绑定到当前内核堆。根设备是 raw whole-disk ext4，物理块大小固定为 512 字节；当前不解析分区表。

挂载只读后额外检查 superblock `needs_recovery` incompat feature。因为当前没有 JBD2 replay 和写回能力，发现该位返回 `-EUCLEAN` 并完整撤销挂载，不能静默读取可能不一致的数据。打开前先读取 mode；普通文件走 `ext4_fopen`，目录走 `ext4_dir_open`（`ext4_fopen` 自身拒绝目录 inode），成功文件记录大小和 mode。unmount 在仍有 open file 时返回 `-EBUSY`。close/unmount 的底层释放失败保留 CLEANUP 状态，调用者可以重试而不会重复关闭或丢失 heap owner。

当前 VFS 同时服务 ELF 随机读、进程文件表和文件私有缺页，但仍不是完整 Linux VFS：没有通用 inode/dentry cache、路径权限、symlink、写入/writeback、read-ahead、并发锁或多挂载。目录支持打开与按后端 cookie 查询（`kernel_vfs_dir_entry`），会返回真实的 `.`/`..` 条目；每次查询从传入的 ext4 字节位置开始，而不是从目录起点重走，顺序枚举的条目访问为 O(N)。适配层把 lwext4 的正 errno 与 EOF 分开，再向文件资源层返回负 Linux errno。页缓存只保存只读普通文件内容；进程层只持有 VFS mount/file 抽象，lwext4 handle 没有泄露到 task 或 syscall ABI。

目录游标设计依据固定 Linux 快照 `f4cdf7ca9a1f`：[`fs/readdir.c`](../../references/linux/fs/readdir.c)
的 `iterate_dir()` 在每次枚举前后同步 open file 的 `f_pos` 与 `dir_context.pos`，`filldir64()`
把继续位置写入 `d_off`；[`fs/ext4/dir.c`](../../references/linux/fs/ext4/dir.c) 的
`ext4_readdir()`、`ext4_dx_readdir()` 和 `ext4_dir_llseek()` 表明 ext4 cookie 既可能是字节位置，
也可能是目录 hash 编码的位置，并会在 seek 后重建迭代状态。因此 VFS 把它当作底层提供的
不透明恢复值，而不能固化为条目序号。BoarOS 当前线性 ext4 适配器返回记录结束的字节位置；
文件资源层将该值放进 OFD offset 和 `linux_dirent64.d_off`，只有完整 usercopy 后才提交。

`ext4_dir_entry_next_status()` 是当前适配器的错误保留入口：一次调用从 `next_off` 定位并推进
一个或多个物理记录，inode 为零的目录尾记录不会被误判为 EOF；返回 `UINT64_MAX` 只表示真实结束。
`kernel_vfs_dir_entry()` 对任意 seek 位置向前对齐到 4 字节边界并规范化到下一个记录，因而保存的
cookie 可以交给 `lseek`/`telldir`/`seekdir` 恢复。OFD 持有位置，所以独立 open 的游标独立，dup/fork
共享游标；目录节点本身不保存可变遍历状态。

## 验证

```sh
make test-lwext4-host
make test-vfs-riscv
make test-files-riscv
make test-exec-riscv
make test-root-init-riscv
```

宿主测试核对 lwext4 metadata checksum seed。QEMU 测试建立真实 ext4 镜像，验证 `/init` mode、目录预检、随机偏移、EOF、`-ENOENT`、open-file `-EBUSY`、dirty-journal `-EUCLEAN`、缓存 miss/hit/LRU/pin、压力回收、mount purge 和全部页回收。文件资源测试证明不同 fd 与 mmap 共用 node/cache 而保持各自 offset；生产测试既用 VFS read source 装载磁盘中的静态 ELF，也由 PID 1 通过 syscall 读取和私有映射普通文件。
