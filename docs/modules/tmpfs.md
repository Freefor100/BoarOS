# 内存文件与 tmpfs

入口是 `mm/memory_object.c`、`fs/tmpfs.c`、`fs/open_file.c` 和
`arch/riscv/mm.c`。固定语义依据为 `references/linux/mm/shmem.c`、
`references/linux/Documentation/filesystems/tmpfs.rst`、`references/linux/fs/inode.c`
以及 `references/linux/lib/{cmdline,kstrtox}.c`，commit
`f4cdf7ca9a1fdcca413157df19753f388a5a224e`。

## 数据和所有权

共享匿名映射与 tmpfs 普通 inode 使用同一稀疏内存后备对象。对象拥有每个
驻留页的一份引用；PTE、fork 别名和临时请求另持引用，不重复计量容量。
普通文件仍保留 inode/OFD 身份，文件映射与执行文件沿原 inode–MM 登记。
没有第二份磁盘页缓存数据，也没有共享文件 futex 的隐式实现。

`find_page` 只查现有页；read/readv/pread 读取空洞时填零，不实例化数据页。
映射缺页按需实例化。文件缺页接口返回本次是否新建；失败先释放临时 pin，
仅撤销地址仍匹配且没有其他引用的新页；已有页不回滚。inode 锁 rank 30，
对象锁 rank 35；对象分配禁止递归进入可睡眠脏页回收。

截断先撤销越界的所有 MM 映射（含私有 COW），再释放对象的越界页并清零
末页尾部。私有 COW 仍属于 MM；文件尾页清零不改写独立私有副本。
关闭或删除一个名字不会释放仍被打开、映射或执行的 inode。

mode、UID/GID和时间属于inode；`fchown/fchownat`在node独占锁下更新真实
所有权与ctime，硬链接和已unlink的fd仍观察同一对象。`UINT32_MAX`保留字段；
非目录的set-ID清除遵循固定root契约。setgid父目录使新节点继承GID、子目录
继承setgid位，进程UID仍为root。这不是凭据变更或完整权限检查。只读挂载
拒绝元数据修改；tmpfs所有权与内容同样不跨重启持久保存。

驻留内存后备页计入 Shmem，并包含于 Cached；无 swap 时不可驱逐，不计入
MemAvailable 的文件回收估算，也不进入磁盘 Dirty/Writeback。短符号链接使用
inode 内存，长符号链接使用有配额的后备页。

## 实例与空间限制

每个挂载拥有独立 inode、目录项、页预算和 inode 预算。目录项与 inode 分离；
硬链接共享数据、链接计数、锁，但独立打开产生独立 OFD。目录继续位置是
稳定递增 cookie，修改期间不保证额外快照一致性。

用户态创建挂载点并调用 mount；内核不会自动覆盖 `/tmp` 或 `/dev/shm`。
支持 `size`、`nr_blocks`、`nr_inodes`、`mode`；默认页数和 inode 数分别为受管
页数的一半，默认根模式 01777。零取消相应显式限额。数字按固定 Linux
memparse 的 base 0、K/M/G/T/P/E、溢出和百分比规则解析，重复参数以后项为准；
不支持的选项明确失败。大数行为以固定版本为准，不将容量限额当作物理内存保证。

稀疏扩展不收费，页实例化才收费；额外硬链接占用 inode 预算。空间配额耗尽为
ENOSPC，真实内存分配失败为 ENOMEM，已写部分仍返回进展。映射超过 EOF 或
页配额用尽为用户 SIGBUS，不是内核 fatal。statfs、st_blocks 反映实际占用。
fsync/fdatasync/msync 完成内存文件契约，不提交磁盘 I/O，也不承诺重启后数据保留。

只读和忙卸载沿通用 VFS 契约。尚无 remount、bind、move、lazy/force unmount、
ACL、xattr、swap、SysV 信号量/消息队列、共享文件 futex 或完整权限模型；
SysV 共享内存已由共用内存后备对象实现。挂载扩展的剩余工作归[P1h](../goals.md#p1h-虚拟文件系统与多挂载)。

## 验证入口

```sh
make test-vma-riscv test-files-riscv
make test-diff-abi-riscv
make test-userland-riscv
make test-root-multi-block-riscv
make test-offline-c-tmpfs-riscv
make test-busybox-tmpfs-riscv
```

差分覆盖配额、稀疏读、实际占用、尾页/COW、映射 SIGBUS、删除后经 proc fd
重新打开并截断、所有权与setgid继承、硬链接限额和选项边界。musl 消费者实际调用 shm_open/shm_unlink，
通过管道握手验证 fork、不同地址别名、重建同名对象与最后映射的忙引用。
离线 GCC 的工作目录和 TMPDIR 都位于 tmpfs；最终产物复制到根盘仅用于主机
核验，不是 tmpfs 持久化证据。

共享映射的直接写缺页更新 mtime/ctime，私有 COW 不更新文件时间。读缺页可以
直接发布可写 PTE，之后的 store 不保证另一次时间通知；这与固定 Linux 的
`mm/memory.c: fault_dirty_shared_page`、`mm/vma.c: vma_wants_writenotify`
及 shmem 无 page_mkwrite 回调相符，不能为测试强制所有 tmpfs 首写陷入。
时间更新在成功发布后以单 hart IRQ 临界区完成，不升级 fault 持有的 inode 读锁。

命名FIFO使用真实tmpfs inode与目录项，但不创建稀疏文件数据对象；打开才取得活动pipe。
inode配额和只读创建检查与普通节点一致，传输不计入文件数据容量。打开/关闭及弱关联
契约见[文件模块](kernel-files.md#命名-fifo)，同ELF验证入口为`make test-fifo-riscv`。
