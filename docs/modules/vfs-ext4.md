# VFS 与只读 ext4 模块

本文描述当前根文件系统的稳定接口和 lwext4 私有适配。第三方版本与许可证见[第三方代码](../third-party.md)，存储知识见[存储与文件系统学习总结](../learning/storage-filesystems.md)。

## 通用边界

`include/kernel/block.h` 定义同步只读块设备，`include/kernel/vfs.h` 定义不透明 mount/file 对象以及根挂载、open、pread、close、unmount。VFS 对外返回负 Linux errno；lwext4 的结构、全局设备名和正值 errno 不泄漏到调用者。当前只有一个根挂载与一个 lwext4 heap binding，没有 fd table、mount namespace 或并发文件访问。

`kernel_vfs_file_read_source()` 把保持打开的文件导出为带 `size/context/read_at` 的精确随机读源。回调只有填满整个范围才返回零；EOF 以内的短读转成 `-EIO`。ELF parser 因而能复用内存和 VFS 来源，而不依赖文件系统类型。

## lwext4 配置和生命周期

内核只编译 lwext4 读取路径需要的源码，关闭 journaling、xattr、debug/assert 和 mkfs，并把 malloc/calloc/realloc/free 绑定到当前内核堆。根设备是 raw whole-disk ext4，物理块大小固定为 512 字节；当前不解析分区表。

挂载只读后额外检查 superblock `needs_recovery` incompat feature。因为当前没有 JBD2 replay 和写回能力，发现该位返回 `-EUCLEAN` 并完整撤销挂载，不能静默读取可能不一致的数据。打开文件记录大小和 mode；unmount 在仍有 open file 时返回 `-EBUSY`。close/unmount 的底层释放失败保留 CLEANUP 状态，调用者可以重试而不会重复关闭或丢失 heap owner。

当前 VFS 只形成生产 `/init` 所需的纵向闭环。它不是完整 Linux VFS：没有 inode/dentry cache、路径权限、symlink、目录遍历、写入、页缓存、fd/open-file-description、并发锁或用户 syscall。以后文件表和 syscall 应消费现有 mount/file 生命周期，而不是把 lwext4 handle 暴露给进程层。

## 验证

```sh
make test-lwext4-host
make test-vfs-riscv
make test-root-init-riscv
```

宿主测试核对 lwext4 metadata checksum seed。QEMU 测试建立真实 ext4 镜像，验证 `/init` mode、随机偏移、EOF、`-ENOENT`、open-file `-EBUSY`、dirty-journal `-EUCLEAN` 和全部页回收。生产测试再用同一 VFS read source 装载并执行磁盘中的静态 ELF。
