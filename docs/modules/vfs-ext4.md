# VFS 与只读 ext4 模块

本文描述当前根文件系统的稳定接口和 lwext4 私有适配。第三方版本与许可证见[第三方代码](../third-party.md)，存储知识见[存储与文件系统学习总结](../learning/storage-filesystems.md)。

## 通用边界

`include/kernel/block.h` 定义同步只读块设备，`include/kernel/vfs.h` 定义不透明 mount/file 对象以及根挂载、open、pread、close、unmount。VFS 对外返回负 Linux errno；lwext4 的结构、全局设备名和正值 errno 不泄漏到调用者。当前只有一个根挂载与一个 lwext4 heap binding；进程 fd/open-file-description 位于独立的[文件资源层](kernel-files.md)，VFS 本身没有 mount namespace 或并发访问协议。

`kernel_vfs_file_read_source()` 把保持打开的文件导出为带 `size/context/read_at` 的精确随机读源。回调只有填满整个范围才返回零；EOF 以内的短读转成 `-EIO`。ELF parser 因而能复用内存和 VFS 来源，而不依赖文件系统类型。

`kernel_vfs_open_executable()` 在普通 open 之上统一要求 regular file 和至少一个执行位；目录、非普通文件或无执行位返回 `-EACCES`。生产 `/init` 与用户 `execve` 共用这一检查，权限拒绝时立即关闭临时 file；若清理需要重试，调用方仍保留 file owner。

## lwext4 配置和生命周期

内核只编译 lwext4 读取路径需要的源码，关闭 journaling、xattr、debug/assert 和 mkfs，并把 malloc/calloc/realloc/free 绑定到当前内核堆。根设备是 raw whole-disk ext4，物理块大小固定为 512 字节；当前不解析分区表。

挂载只读后额外检查 superblock `needs_recovery` incompat feature。因为当前没有 JBD2 replay 和写回能力，发现该位返回 `-EUCLEAN` 并完整撤销挂载，不能静默读取可能不一致的数据。打开前先读取 mode 并拒绝目录，避免为不支持的对象创建 lwext4 file handle；成功文件记录大小和 mode。unmount 在仍有 open file 时返回 `-EBUSY`。close/unmount 的底层释放失败保留 CLEANUP 状态，调用者可以重试而不会重复关闭或丢失 heap owner。

当前 VFS 同时服务 ELF 随机读和进程文件表，但仍不是完整 Linux VFS：没有 inode/dentry cache、路径权限、symlink、目录遍历、写入、页缓存、并发锁或多挂载。进程层只持有 VFS mount/file 抽象，lwext4 handle 没有泄露到 task 或 syscall ABI。

## 验证

```sh
make test-lwext4-host
make test-vfs-riscv
make test-files-riscv
make test-exec-riscv
make test-root-init-riscv
```

宿主测试核对 lwext4 metadata checksum seed。QEMU 测试建立真实 ext4 镜像，验证 `/init` mode、目录预检、随机偏移、EOF、`-ENOENT`、open-file `-EBUSY`、dirty-journal `-EUCLEAN` 和全部页回收。文件资源测试在其上验证用户路径与 fd 语义；生产测试既用 VFS read source 装载磁盘中的静态 ELF，也由 PID 1 通过 syscall 读取普通文件。
