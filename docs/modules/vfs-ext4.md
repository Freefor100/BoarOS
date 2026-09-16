# VFS 与 ext4 模块

本文描述当前根文件系统的稳定接口和 lwext4 私有适配。第三方版本与许可证见[第三方代码](../third-party.md)，存储知识见[存储与文件系统学习总结](../learning/storage-filesystems.md)。

## 通用边界

`include/kernel/block.h` 定义同步块设备（支持读与可选写），`include/kernel/vfs.h` 定义不透明 mount/file 对象以及根挂载、open/create、pread/pwrite、ftruncate、mkdir、unlink、rmdir、close、unmount、`kernel_vfs_fstat()` 与 `kernel_vfs_mount_is_readonly()` 查询。VFS 对外返回负 Linux errno；lwext4 的结构、全局设备名和正值 errno 不泄漏到调用者。当前只有一个根挂载与一个 lwext4 heap binding；进程 fd/open-file-description 位于独立的[文件资源层](kernel-files.md)，VFS 本身没有 mount namespace 或并发访问协议。

`kernel_vfs_mount_root()` 根据传入块设备是否提供 `write` 回调自动决定只读还是读写挂载：若底层设备 `write == 0`，以只读挂载且拒绝任何修改；若底层设备可写，则以读写模式挂载。若磁盘镜像需要 recovery（`needs_recovery` incompat feature），则返回 `-EUCLEAN`。

`kernel_vfs_file_read_source()` 把保持打开的文件导出为带 `size/context/read_at` 的精确随机读源。回调只有填满整个范围才返回零；EOF 以内的短读转成 `-EIO`。ELF parser 因而能复用内存和 VFS 来源，而不依赖文件系统类型。

`kernel_vfs_open_executable()` 在普通 open 之上统一要求 regular file 和至少一个执行位；目录、非普通文件或无执行位返回 `-EACCES`。若目标 node 存在活动的写打开者（`node->write_openers > 0`），返回 `-ETXTBSY`；检查通过后累加 `node->exec_users` 并持有 `file->exec_lease`。对应地，以写权限打开普通文件需调用 `kernel_vfs_file_acquire_write()`，当 `node->exec_users > 0` 时准确返回 `-ETXTBSY`，否则累加 `node->write_openers` 并持有 `file->write_lease`。该租约在 `kernel_vfs_close()` 时释放，严格保障运行中二进制与写入者之间的互斥。

挂载为 lwext4 注册可选 realtime 时钟；尚未初始化时钟的纯模块环境保留原时间。`kernel_vfs_file_accessed()` 按活 inode 执行 relatime（包含页缓存命中的 read/pread），`kernel_vfs_file_modified()` 在非零写请求复制前更新时间并传播 metadata flush 错误。create、目录链接/删除和 truncate 在拥有 inode 引用的后端更新相应时间；同尺寸 truncate 也走后端，unlink 后仍打开的文件不依赖路径。时间字段编码、操作时机与失败 owner 的依据见[时间戳学习记录](../learning/file-timestamps.md)。

## 文件节点与页缓存

VFS 为每个已解析普通文件维护引用计数 node；独立 open file description 各自保存 offset，但指向同一共享 node。文件大小通过 `kernel_vfs_file_size()` 实时查询所属 node 的实时大小，确保写入或截断后各共享描述符观察到一致的文件长度。

根启动建立一个挂载共享的 4 KiB 页缓存，键为 `(node, page_index)`：开放寻址哈希提供平均常数时间查找，双向 LRU 维护回收次序。缓存项持有 node 引用和一份物理页引用；命中时再给调用者一份临时引用，因此 `read`、不同 fd 和 file-private mmap 可以安全共享同一页。

写入与截断通过 `kernel_page_cache_invalidate_node()` 失效指定 node 的全部缓存项。它遍历整个 LRU 再筛选 node，查找成本为 O(缓存总页数)；对象选择准确不代表已有按 node 索引。释放缓存引用后，后续读取/重新缺页从介质加载；仍持有旧页的 private PTE 不是可写共享缓存协议。当前没有性能数据证明这段扫描是主要瓶颈。文件删除（`kernel_vfs_unlink`）在底层目录项移除后，遍历当前挂载的所有存活节点（`adapter->nodes`）进行保守失效。

缓存索引失效本身不撤销用户 PTE。向下截断另通过稳定 node–MM 登记通知相关地址空间，撤销越过新 EOF 的整页（含 private COW 与 PROT_NONE），并按驻留来源处理非对齐尾页；普通写导致的缓存索引失效不丢弃这些来源记录。VMA 保留，后续访问重新缺页并检查 live inode size。实现与单 hart 同步边界见本文末尾及[MM 模块](kernel-mm.md)。

miss 路径先分配并清零页，再通过 node 的无 offset 副作用 `pread` 填充，记录尾页有效字节数。读取整个越过 EOF 的页返回 `OUT_OF_RANGE`，尾页剩余字节保持为零。物理页分配器只有一个压力回收槽，当前由该缓存注册；分配首次耗尽时从 LRU 尾部扫描，仅驱逐引用数为 1 的未固定页，然后由分配器重试一次。被用户映射或正由 read 使用的页引用数大于 1，不会被回收。

缓存销毁和 mount 卸载有严格顺序：先释放所有进程 MM 和临时读者，再 purge 该 mount 的缓存项、关闭最后的 node，之后才允许 lwext4 unmount；缓存最后注销 reclaimer 并释放哈希表。物理页和堆对象的合法释放完成即返回，分配器不变量错误进入 fatal；只有真实 ext4/block I/O 清理错误保留 mount owner。

`kernel_block_device` 当前仅有 read/write，没有 flush。VFS 无 `fsync/fdatasync` 或目录同步承诺，open 拒绝 `O_SYNC/O_DSYNC`；同步块写完成、关闭后能读与掉电持久性是不同契约。

## lwext4 配置和生命周期

内核编译 lwext4 读写路径需要的源码，关闭 journaling、xattr、debug/assert 和 mkfs，并把 malloc/calloc/realloc/free 绑定到当前内核堆。根设备是 raw whole-disk ext4，物理块大小固定为 512 字节；当前不解析分区表。

挂载后额外检查 superblock `needs_recovery` incompat feature。发现该位返回 `-EUCLEAN` 并完整撤销挂载。
普通文件打开走 `ext4_fopen`，新建走 `ext4_fopen2`（`kernel_vfs_create`），目录打开走专有的 `ext4_dir_open_file` 直接绑定 `node->file`，免去在内核任务栈上分配 312 字节的完整 `ext4_dir` 结构。
普通文件随机写入通过 `kernel_vfs_pwrite`（`ext4_fseek` + `ext4_fwrite`）执行，追加写入通过 `kernel_vfs_append` 在底层原子解析当前 EOF 并写入，确保 `O_APPEND` 的原子推进。lwext4 的 `SEEK_SET` 允许定位到 EOF 之后而不改变 inode size；后续写入只为调用者数据相交的逻辑块分配物理块，新分配的部分写块先清零，已分配的旧 EOF 尾部在扩大 size 前清零，完整中间 hole 保持未映射。读取未映射逻辑块时直接向调用者缓冲写零，绝不把物理块号 0 当作数据块读取。若 lwext4 提交正字节前缀后报告后续错误，VFS 先消费该前缀、从 handle 刷新 live inode size 并失效 node 页缓存，再向文件资源层返回成功与正字节数；只有零进度时才返回 errno。

截断统一通过 sparse-capable `ext4_ftruncate` 执行：缩小仍释放尾部块，扩大只清零已分配的旧 EOF 块尾并发布新 inode size，不为完整逻辑 gap 分配块，且不改变调用 OFD offset。文件打开时按实际 inode mapping 缓存可寻址 size 上限：extent inode 使用 `EXT_MAX_BLOCKS * block_size`；legacy block-map inode 取指针树容量、`EXT_MAX_BLOCKS` 个可安全计数的逻辑块和 inode `i_blocks` 容量（包含间接块开销）的最小值，再乘以 `block_size`。因此 legacy 最后可用逻辑块也是 `0xfffffffe`：即使 8 KiB 三级间接树容量超过 2^32，也不会让 `ext4_lblk_t` 在 2^45 字节处回绕到块 0。truncate/write 超界返回 `EFBIG`，seek 超界返回 `EINVAL`，在任何尾部清零、64-bit offset 缩窄为 `ext4_lblk_t` 或 inode size 更新前拒绝。lwext4 在写入或截断所有可能改变状态的返回路径上，让 handle 的 `fsize` 保持为当前 inode 中可知的最新 size。

新分配的数据块在 caller data 写入前整块初始化；初始化失败只撤销该 exact logical block 的 mapping，不回滚此前已经完成的块。对预置 unwritten extent 的初始化和 caller 部分写使用同一个 block-cache buffer，避免延迟的 zero buffer 在 direct I/O 之后覆盖用户数据；读取 mapped block 前先排空该物理块的 dirty cache，flush 失败则返回错误而不从旧磁盘内容读取。lwext4 在写入或截断所有可能改变状态的返回路径上，让 handle 的 `fsize` 保持为当前 inode 中可知的最新 size；VFS 每次调用这两类 backend 操作后都据此同步 node/file size 并失效缓存，包括 truncate 已改变 inode 后才返回错误的路径。部分写返回依据固定 Linux `references/linux` commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e` 的 `mm/filemap.c::generic_perform_write()` 与 `fs/read_write.c::new_sync_write()`：正进度覆盖后续错误并只推进相同字节数的文件位置。

`kernel_vfs_fstat()` 把当前 open handle 指向的 raw ext4 inode 转换为统一 `kernel_vfs_stat`，不按路径重新查找，也不从 logical size 猜 metadata。它读取 inode 的 ino/mode/nlink、Linux low/high uid/gid、64-bit size、以 512 字节为单位的 allocated block count，以及 filesystem block size；atime/mtime/ctime 按磁盘 inode size 与 `extra_isize` 共同判断 extra 字段是否存在，再解码 2-bit epoch 与纳秒。该编码依据固定 Linux commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e` 的 `references/linux/fs/ext4/ext4.h`。根 mount 保存稳定的内部 ID 1 作为 `dev`，不表达物理设备 major/minor。lwext4 的 `ext4_fraw_inode_fill()` 是最小 handle adapter，使最后一个 dentry 删除后仍能查询活着的 inode。
目录操作中，`kernel_vfs_mkdir` 调用 `ext4_dir_mk`；`kernel_vfs_unlink` 实现了真正的 Linux `unlink`-but-open 语义：
- `kernel_vfs_unlink` 在需要时先从挂载的 orphan 记录池预留一个回收记录，再调用 `ext4_funlink_dentry` 从父目录中立即移除目标目录项；后续对原路径的 `open` 立即返回 `-ENOENT`，并在同名路径重新创建时分配独立全新 inode。没有现存 node 的文件也使用这份预留记录，因而 orphan 回收失败时由 mount 单独持有。
- 若目标文件当前仍处于打开状态（`node->open_files > 0`），VFS 标记 `node->unlinked = 1`，旧 open 描述符（包括只读/读写 OFD 以及正在运行的源映射 ELF 可执行文件）保留底层 inode 数据与有效物理块，继续正常执行 `read/write/fstat` 与缺页加载（demand fault）；
- 只有当最后一个打开描述符与执行租约释放（`node->open_files == 0 && node->exec_users == 0`）时，VFS 才通过专有的 `kernel_vfs_try_release_orphan` 驱动物理存储释放。该过程先持有临时节点引用并安全使页缓存失效，再在扁平调用栈上调用 `ext4_orphan_free` 截断释放底层 inode，最后标记 `node->orphan_freed = 1`。通用的 `kernel_vfs_node_release` 仅负责内存节点对象的生命周期，绝不隐式或嵌套调用 `ext4_orphan_free`，避免双重释放与页缓存回收深度嵌套额外消耗任务栈；若底层释放失败，节点转移至 `adapter->cleanup_nodes`，由 mount 保留唯一重试 owner，绝不重新指向 file。若文件在 unlink 时没有现存 node，则使用预留记录直接调用 orphan free；失败后记录留在 mount 链，路径仍保持已删除。
由于 lwext4 的 `ext4_dir_rm` 会递归删除非空目录，`kernel_vfs_rmdir` 将目录判空逻辑隔离在独立辅助函数 `check_directory_empty` 中（遍历目录项跳过 `.` 和 `..`，存在子条目返回 `-ENOTEMPTY`），使 312 字节的 `ext4_dir` 栈帧在调用 `ext4_dir_rm` 之前及时退栈，保证深层递归删除时调用栈扁平受控。
只读挂载下，所有上述修改操作直接返回 `-EROFS`。
unmount 在仍有 open file 时返回 `-EBUSY`。close/unmount 的真实 ext4/block I/O 释放失败保留 CLEANUP 状态，mount 或所属文件表可重试而不会重复关闭；合法 heap/page 释放不返回可重试状态。

当前 VFS 同时服务 ELF 随机读、进程文件表（支持读写与目录修改）和文件私有缺页，但仍不是完整 Linux VFS：没有通用 inode/dentry cache、逐分量权限检查、硬链接、writeback、read-ahead、并发锁或多挂载。逐分量解析在 `fs/vfs.c`，对每个现存分量查询 ext4 mode，处理相对/绝对符号链接与最多 40 次展开；最终分量是否跟随由 open/stat 或创建/删除入口决定。解析工作区由 VFS heap 临时持有，释放后不留下路径指针。fs context 的 cwd 当前固定 `/`，相对路径仅接受 `AT_FDCWD`，其他 dirfd 返回 `EBADF`；能打开和枚举目录不代表能以目录 fd 解析 openat。目录支持打开与按后端 cookie 查询（`kernel_vfs_dir_entry`），会返回真实的 `.`/`..` 条目；每次查询从传入的 ext4 字节位置开始，而不是从目录起点重走，顺序枚举的条目访问为 O(N)。适配层把 lwext4 的正 errno 与 EOF 分开，再向文件资源层返回负 Linux errno。页缓存只保存普通文件内容；进程层只持有 VFS mount/file 抽象，lwext4 handle 没有泄露到 task 或 syscall ABI。

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
make test-files-partial-write-riscv
make test-exec-riscv
make test-root-init-riscv
```

宿主测试保留两种 lwext4 metadata checksum seed 只读探针，并在独立可写的 1 KiB-block extent、1 KiB legacy 与 8 KiB legacy (`^extent,^64bit`) 镜像上验证 aligned/unaligned hole、同块 gap、sparse truncate、allocated-block 上界、各自 exact maxbytes 以及大块 legacy 逻辑号不回绕，卸载后分别运行 `e2fsck -fn`。QEMU 测试建立真实 ext4 镜像，验证 `/init` mode、目录预检、随机偏移、EOF、越过 EOF 写入、sparse truncate、`-ENOENT`、open-file `-EBUSY`、dirty-journal `-EUCLEAN`、缓存 miss/hit/LRU/pin、压力回收、mount purge、raw inode metadata 和全部页回收；VFS runner 注入一次 orphan free 失败，覆盖仍有打开 fd 与无现存 node 两条路径，确认路径不复现、mount 只保留一个 owner、重试后可卸载。文件资源测试核对 fstat/newfstatat metadata、unlink-but-open 的 `nlink == 0`，并证明不同 fd 与 mmap 共用 node/cache 而保持各自 offset；生产测试由静态和动态 musl 入口通过 VFS read source 读取真实根盘。

向下截断通过稳定 node–MM 登记通知相关地址空间，依据后端实际大小撤销越界整页
（含 private COW），并按驻留来源区分尾页清零与私有修改保留。通知不分配内存，
也不删除 VMA；O_TRUNC 和后端已变更再报错同样执行协调。关联由 MM 拥有，node
借用，末次 OFD 释放前必须解除。具体生命周期与失败回滚见 [MM 模块](kernel-mm.md)。
