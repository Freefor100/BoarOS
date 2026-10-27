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
对象引用采用原子非零/溢出检查，末次引用唯一负责销毁；每次对象操作必须由实际owner覆盖。
跨对象预算及allocator共享页计数在后备页发布时原子收费，失败准备归还自己的页/metadata，
PTE、fork及pin不重复收费。驻留数/statfs使用值读取，不承诺跨对象全局原子快照。
对象索引仍使用原mutex；预算绑定/上限在私有构造期确定。此基础同步不开放tmpfs业务跨核。
受控来源验证同时核对实际 tmpfs 普通 inode、对象、heap/allocator 和所属挂载的预算身份；
共享匿名对象的域验证仍要求无 budget。该检查借用已有 OFD owner，不额外增加对象引用。

截断先撤销越界的所有 MM 映射（含私有 COW），再释放对象的越界页并清零
末页尾部。私有 COW 仍属于 MM；文件尾页清零不改写独立私有副本。
关闭或删除一个名字不会释放仍被打开、映射或执行的 inode。

mode、UID/GID和时间属于inode，硬链接和已unlink的fd仍观察同一对象。
`fchown/fchownat`检查实际调用者身份；`UINT32_MAX`保留字段。非目录先按旧
所属组决定set-ID清除，产生模式变更时再按新所属组过滤SGID；目录保留set-ID。
创建在目录发布时设置fsuid/fsgid，SGID父目录决定新节点的GID及子目录SGID。
属性与写入的互斥顺序沿用[共同VFS规则](vfs-ext4.md#文件节点与页缓存)。
只读挂载拒绝元数据修改；tmpfs所有权与内容同样不跨重启持久保存。

驻留内存后备页计入 Shmem，并包含于 Cached；无 swap 时不可驱逐，不计入
MemAvailable 的文件回收估算，也不进入磁盘 Dirty/Writeback。短符号链接使用
inode 内存，长符号链接使用有配额的后备页。

## 实例与空间限制

每个挂载拥有独立 inode、目录项、页预算和 inode 预算。目录项与 inode 分离；
硬链接共享数据、链接计数、锁，但独立打开产生独立 OFD。目录继续位置是
稳定递增 cookie，修改期间不保证额外快照一致性。

挂载按 inode 号维护红黑树，每个目录分别按名字和 cookie 维护目录项红黑树；
这些索引同时承担对象登记与卸载遍历，不保留另一份全挂载链表。inode 查找为
O(log I)，名字查找和 cookie 继续定位为 O(log D)，其中 I 为挂载 inode 数，
D 为当前目录的项数。读取一个目录不会扫描其他目录。目录项借用仍在索引中
存活的父 inode，直接维护其大小；卸载先删除全部目录项，再释放 inode。
创建先准备名字、inode 与后备对象，重命名先分配新名字；提交索引不再分配。
重命名取得新 cookie，cookie 耗尽返回 EOVERFLOW 并保留原名字与替换目标。

用户态创建挂载点并调用 mount；内核不会自动覆盖 `/tmp` 或 `/dev/shm`。
设备节点可由 mknod 创建字符或块设备类型，inode 保留真实 `rdev`；它们不分配文件数据对象和数据页，stat 的大小与块数为 0。打开时由通用设备路径按 `rdev` 分派。

支持 `size`、`nr_blocks`、`nr_inodes`、`mode`；默认页数和 inode 数分别为受管
页数的一半，默认根模式 01777，根目录所属身份取创建者 fsuid/fsgid。零取消相应显式限额。数字按固定 Linux
memparse 的 base 0、K/M/G/T/P/E、溢出和百分比规则解析，重复参数以后项为准；
不支持的选项明确失败。大数行为以固定版本为准，不将容量限额当作物理内存保证。

稀疏扩展不收费，页实例化才收费；额外硬链接占用 inode 预算。空间配额耗尽为
ENOSPC，真实内存分配失败为 ENOMEM，已写部分仍返回进展。映射超过 EOF 或
页配额用尽为用户 SIGBUS，不是内核 fatal。statfs、st_blocks 反映实际占用。
fsync/fdatasync/msync 完成内存文件契约，不提交磁盘 I/O，也不承诺重启后数据保留。

只读和忙卸载沿通用 VFS 契约。尚无 remount、bind、move、lazy/force unmount、
ACL和swap。SysV IPC由共同IPC模块提供。挂载扩展的剩余工作归[P1h](../goals.md#p1h-虚拟文件系统与多挂载)。

扩展属性归inode所有，使用不可变的名称／值记录，在inode资格内替换、删除和查询；
打开对象及硬链接保留它们，最后一个链接和使用者结束后释放。capability记录另有实际指针，
普通写入无属性时只作空判断；设置、chown和截断按共同文件权限规则维护它。
名称空间、Linux ABI和filecap查询格式见[共同文件接口](kernel-files.md#扩展属性)。

## 验证入口

```sh
make test-vma-riscv test-files-riscv
make test-memory-object-concurrency-host
make test-tmpfs-host
make test-file-mapping-concurrency-host
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

宿主 tmpfs 用真实后端、内存对象、heap 和页分配器检查混合大目录的查找与
枚举、cookie seek/继续、插入删除与跨目录重命名、替换和硬链接、分配失败回滚、
inode/页预算及最终资源归还；VFS 发布由宿主适配，完整路径集成由文件与客户机测试覆盖。

共享映射的直接写缺页更新 mtime/ctime，私有 COW 不更新文件时间。读缺页可以
直接发布可写 PTE，之后的 store 不保证另一次时间通知；这与固定 Linux 的
`mm/memory.c: fault_dirty_shared_page`、`mm/vma.c: vma_wants_writenotify`
及 shmem 无 page_mkwrite 回调相符，不能为测试强制所有 tmpfs 首写陷入。
时间更新在成功发布后使用已有 inode metadata raw 保护，不升级 fault 持有的 inode 读锁。stat 和 CPU0 显式时间修改使用同一保护。

命名FIFO使用真实tmpfs inode与目录项，但不创建稀疏文件数据对象；打开才取得活动pipe。
inode配额和只读创建检查与普通节点一致，传输不计入文件数据容量。打开/关闭及弱关联
契约见[文件模块](kernel-files.md#命名-fifo)，同ELF验证入口为`make test-fifo-riscv`。

`st_blocks` 以 512 字节为单位，驻留页数乘 `PAGE_SIZE / 512`；RV 4 KiB 和 LA 16 KiB 的页大小不改变该 ABI 单位。`tests/diff-abi/tmpfs.c` 的 `tmpfs.allocated-block-units` 使用同一目标 ELF 与固定 Linux 对照。时间字段更新和 stat 的读取使用已有 inode metadata 保护，不升级缺页的 I/O 读锁；这不开放命名空间跨核修改。
