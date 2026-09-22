# VFS 与 ext4 模块

本文描述当前根文件系统的稳定接口和 lwext4 私有适配。第三方版本与许可证见[第三方代码](../third-party.md)，存储知识见[存储与文件系统学习总结](../learning/storage-filesystems.md)。

## 通用边界

`include/kernel/block.h` 定义同步块设备（支持读与可选写），`include/kernel/vfs.h` 定义不透明 mount/file 对象以及根挂载、open/create、pread/pwrite、ftruncate、mkdir、unlink、rmdir、close、unmount、`kernel_vfs_fstat()` 与 `kernel_vfs_mount_is_readonly()` 查询。VFS 对外返回负 Linux errno；lwext4 的结构、全局设备名和正值 errno 不泄漏到调用者。当前只有一个根挂载与一个 lwext4 heap binding；进程 fd/open-file-description 位于独立的[文件资源层](kernel-files.md)，VFS 本身没有 mount namespace；当前调用依赖单 hart 不可调度的命名空间临界区。

`kernel_vfs_mount_root()` 根据传入块设备是否提供 `write` 回调决定只读还是读写挂载。读写 journal 挂载先 replay、校验 orphan 记录、启动日志并回收遗留 orphan，完成后才发布根路径。只读介质不能完成恢复时明确拒绝；未知必需特性、损坏日志或元数据也不能作为干净镜像继续访问。

`kernel_vfs_file_read_source()` 把保持打开的文件导出为带 `size/context/read_at` 的精确随机读源。回调只有填满整个范围才返回零；EOF 以内的短读转成 `-EIO`。ELF parser 因而能复用内存和 VFS 来源，而不依赖文件系统类型。

`kernel_vfs_open_executable()` 在普通 open 之上统一要求 regular file 和至少一个执行位；目录、非普通文件或无执行位返回 `-EACCES`。若目标 node 存在活动的写打开者（`node->write_openers > 0`），返回 `-ETXTBSY`；检查通过后累加 `node->exec_users` 并持有 `file->exec_lease`。对应地，以写权限打开普通文件需调用 `kernel_vfs_file_acquire_write()`，当 `node->exec_users > 0` 时准确返回 `-ETXTBSY`，否则累加 `node->write_openers` 并持有 `file->write_lease`。该租约在 `kernel_vfs_close()` 时释放，严格保障运行中二进制与写入者之间的互斥。

挂载为 lwext4 注册可选 realtime 时钟；尚未初始化时钟的纯模块环境保留原时间。`kernel_vfs_file_accessed()` 按活 inode 执行 relatime（包含页缓存命中的 read/pread），`kernel_vfs_file_modified()` 在非零写请求复制前更新时间并传播 metadata flush 错误。create、目录链接/删除和 truncate 在拥有 inode 引用的后端更新相应时间；同尺寸 truncate 也走后端，unlink 后仍打开的文件不依赖路径。时间字段编码、操作时机与失败 owner 的依据见[时间戳学习记录](../learning/file-timestamps.md)。

## 文件节点与页缓存

VFS 以挂载实例与 ext4 inode 为活节点身份，普通文件、目录和字符节点都持有引用计数 node；路径对象另持有父目录项身份和一份活 inode 引用。独立 open file description 各自保存 offset，但同一 inode 指向共享 node。文件大小通过 `kernel_vfs_file_size()` 实时查询所属 node 的实时大小，确保写入或截断后各共享描述符观察到一致的文件长度。

根启动建立一个挂载共享的 4 KiB 页缓存，键为 `(node, page_index)`：开放寻址哈希提供平均常数时间查找，双向 LRU 维护回收次序。缓存项持有 node 引用和一份物理页引用；命中时再给调用者一份临时引用，因此 `read`、不同 fd 和 file-private mmap 可以安全共享同一页。

普通写把已复制的字节写入同一缓存页并更新 node 的逻辑大小；不同 fd、VFS pread、ELF 读源与私有映射缺页立即看到该内容。缓存项按 inode 另建双向链表，写回和显式失效只遍历该 inode 的页面，成本为 O(目标 inode 缓存页数)。每项保留脏范围、修改代次与 writeback 状态；写回固定页面，只有完整提交且代次未变才清脏。失败保留缓存页/node owner，并记录 inode 的错误序列。lwext4 非 journal 写入/截断用操作范围固定本次修改的缓冲，建立完整的分配所有权后才提交该集合；不会全量 drain 历史 dirty list。底层最后引用的 flush 错误显式返回，失败缓冲仍在 mount dirty list。最后一个 fd 关闭不释放仍有脏页的 inode。

截断只写回新 EOF 之前的脏范围，再调用后端；成功后按实际结果更新逻辑长度、通知 MM、丢弃越界页并清零尾页。后端失败且磁盘 size 未变时保留原逻辑长度与脏 owner，截零不需要保存待丢弃数据。删除仍打开的文件保留缓存；只有 orphan 最后一个打开/执行 owner 退出时，才丢弃已不再可观察的缓存并回收 inode。

缓存索引失效本身不撤销用户 PTE。向下截断另通过稳定 node–MM 登记通知相关地址空间，撤销越过新 EOF 的整页（含 private COW 与 PROT_NONE），并按驻留来源处理非对齐尾页；普通写直接修改共享缓存，不撤销 private COW 页面。VMA 保留，后续访问重新缺页并检查 live inode size。实现与单 hart 同步边界见本文末尾及[MM 模块](kernel-mm.md)。

miss 路径先分配并清零页，再通过 node 的无 offset 副作用 `pread` 填充，有效字节数按 node 的逻辑大小计算，尚未写回的稀疏扩展区从清零页读取。读取整个越过 EOF 的页返回 `OUT_OF_RANGE`，尾页剩余字节保持为零。物理页分配器只有一个压力回收槽，当前由该缓存注册；分配首次耗尽时从 LRU 尾部扫描，对引用数为 1 的未固定脏页先尝试写回，成功后才驱逐，然后由分配器重试一次。被用户映射或正由 read 使用的页引用数大于 1，不会被回收。写回期间禁止递归压力回收；lwext4 分配回调内只允许回收干净 VFS 页，防止重入共享 handle 的 fpos 和 bcache 查找/分配间隙；当前没有后台线程。

缓存销毁和 mount 卸载有严格顺序：先释放所有进程 MM 和临时读者，再写回并 purge 该 mount 的缓存项、关闭最后的 node，之后才允许 lwext4 unmount 与设备 flush；缓存最后注销 reclaimer 并释放哈希表。物理页和堆对象的合法释放完成即返回，分配器不变量错误进入 fatal；只有真实 ext4/block I/O 清理错误保留 mount owner。

`kernel_vfs_sync()` 只主动提交目标 inode 的脏页和必要元数据事务，再执行块设备 flush；不会用 `ext4_cache_flush("/")` 排空无关文件数据。独立 open 各持错误观察位置，dup/fork 共用 OFD 的位置。`fsync/fdatasync` 支持普通文件与目录；当前 metadata 在修改时提交，handle 记录已提交事务号，两者均等待完整 inode 元数据依赖。共享事务可能连带提交其他元数据。`O_SYNC/O_DSYNC` 对已接受的写入前缀执行相同同步，失败返回 errno，但已接受字节与 offset 保留。journal 的关键写入、checkpoint 或屏障失败使 mount 持续拒绝修改和同步；OFD 错误游标不能清除该错误。

## lwext4 配置和生命周期

内核编译 lwext4 journal/replay、orphan 和分批截断路径，关闭 xattr、debug/assert 和 mkfs，并把 malloc/calloc/realloc/free 绑定到当前内核堆。根设备是 raw whole-disk ext4，物理块大小固定为 512 字节；当前不解析分区表。

事务接口 `ext4_transaction_begin/end/abort` 支持同一 mount 的嵌套修改；外层提交前保留 metadata 和数据缓冲的 before-image 与引用。明确发生在日志提交前的 OOM、空间不足或关联数据 I/O 失败可回滚内存并重试；已可能影响日志持久状态的错误由 mount 保留，不能清除后继续。外层 abort 后，调用者须重新打开在内层修改过的 lwext4 handle；VFS 的普通操作各自完成事务，不持有跨 syscall 的开放事务。

每次提交先预留全部日志空间、映射与缓冲，再依次完成关联文件数据及 flush、日志内容及 flush、commit 记录及 flush。预留失败不写当前事务的数据；预留缓冲由事务持有至提交或回滚，内存成本随本次 metadata 日志大小增长。checkpoint 把已提交内容写回原位置并 flush，再持久化日志起点，最后释放日志空间和缓冲 owner。数据不写入 metadata 日志。主 superblock 的分配计数、恢复位和校验和也属于事务；挂载/卸载不在日志之外直接覆盖它。512 字节原子扇区模型下若 superblock 校验失败，只允许根据合法几何信息进入受限恢复，必须重放有效 superblock 日志后才能访问文件。

`ext4_orphan.c` 支持传统 `last_orphan/i_dtime` 链和 `orphan_file`，校验范围、重复记录、循环、分配状态和 checksum。unlink 将最后一个链接摘除与持久 orphan 记录放在同一事务；缩小文件先提交最终 size 与 orphan，再由 `ext4_truncate.c` 每事务最多释放 32 个尾部数据块及已空的索引路径。恢复根据实际映射找到尾部，支持稀疏 extent 和三级间接块，不依赖已缩小的 size 推测待回收块。最后删除 orphan 记录与释放无链接 inode 同事务完成，重复恢复可继续前次进度。仍被打开的无链接 inode 在正常运行期间保留，重启才回收。

支持有界的 JBD2 checksum v2/v3、32/64-bit revoke；异步 commit、未知必需特性和旧 CRC32 journal 格式明确拒绝。已提交事务的损坏不能当作未提交尾部丢弃。只读脏日志、恢复 I/O 错误或损坏均拒绝开放用户访问。失败的日志/挂载 owner 保留到重启；普通合法内存释放不建立重试链。挂载准备阶段的 ENOMEM/ENOSPC 与关键 I/O 错误分开：资源不足保留可恢复的准备状态，后续 cleanup 可继续恢复并卸载，不能永久锁住 heap binding。
已有路径先逐分量取得目录项和 inode 身份，再用 `ext4_fopen_inode` 按 inode 打开普通文件、目录或字符节点；新建仍由 `ext4_fopen2` 提交。目录 handle 不在内核任务栈上分配完整 `ext4_dir` 结构。
普通文件随机写入与追加先进入页缓存；append 以共享 node 的逻辑 EOF 为起点并推进实际接收字节。定向写回通过 `ext4_fseek` + `ext4_fwrite` 提交脏范围。lwext4 的 `SEEK_SET` 允许定位到 EOF 后，磁盘逻辑块只在写回时按数据范围分配；新分配的部分块先清零，旧 EOF 块尾清零，完整中间 hole 保持未映射。底层部分写失败不缩小缓存已经接收的逻辑长度，整段脏范围由页缓存保留供重试。`st_blocks` 报实际已分配存储，不由逻辑大小猜测；检查磁盘块生命周期的测试需先同步。

截断统一通过 sparse-capable `ext4_ftruncate` 执行：缩小仍释放尾部块，扩大只清零已分配的旧 EOF 块尾并发布新 inode size，不为完整逻辑 gap 分配块，且不改变调用 OFD offset。文件打开时按实际 inode mapping 缓存可寻址 size 上限：extent inode 使用 `EXT_MAX_BLOCKS * block_size`；legacy block-map inode 取指针树容量、`EXT_MAX_BLOCKS` 个可安全计数的逻辑块和 inode `i_blocks` 容量（包含间接块开销）的最小值，再乘以 `block_size`。因此 legacy 最后可用逻辑块也是 `0xfffffffe`：即使 8 KiB 三级间接树容量超过 2^32，也不会让 `ext4_lblk_t` 在 2^45 字节处回绕到块 0。truncate/write 超界返回 `EFBIG`，seek 超界返回 `EINVAL`，在任何尾部清零、64-bit offset 缩窄为 `ext4_lblk_t` 或 inode size 更新前拒绝。lwext4 在写入或截断所有可能改变状态的返回路径上，让 handle 的 `fsize` 保持为当前 inode 中可知的最新 size。

新分配的数据块在 caller data 写入前整块初始化；初始化失败只撤销该 exact logical block 的 mapping，不回滚此前已经完成的块。对预置 unwritten extent 的初始化和 caller 部分写使用同一个 block-cache buffer，避免延迟的 zero buffer 在 direct I/O 之后覆盖用户数据；读取 mapped block 前先排空该物理块的 dirty cache，flush 失败则返回错误而不从旧磁盘内容读取。lwext4 在写入或截断所有可能改变状态的返回路径上，让 handle 的 `fsize` 保持为当前 inode 中可知的最新 size；VFS 在 truncate 后据此同步 node/file size 并失效缓存；writeback 期间逻辑长度仍由缓存接收进度决定。部分写返回依据固定 Linux `references/linux` commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e` 的 `mm/filemap.c::generic_perform_write()` 与 `fs/read_write.c::new_sync_write()`：正进度覆盖后续错误并只推进相同字节数的文件位置。

`kernel_vfs_fstat()` 把当前 open handle 指向的 raw ext4 inode 转换为统一 `kernel_vfs_stat`，不按路径重新查找。普通文件 size 来自共享 node 的逻辑大小，其余字段来自实际 inode。它读取 inode 的 ino/mode/nlink、Linux low/high uid/gid、64-bit size、以 512 字节为单位的 allocated block count，以及 filesystem block size；atime/mtime/ctime 按磁盘 inode size 与 `extra_isize` 共同判断 extra 字段是否存在，再解码 2-bit epoch 与纳秒。该编码依据固定 Linux commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e` 的 `references/linux/fs/ext4/ext4.h`。根 mount 保存稳定的内部 ID 1 作为 `dev`，不表达物理设备 major/minor。lwext4 的 `ext4_fraw_inode_fill()` 是最小 handle adapter，使最后一个 dentry 删除后仍能查询活着的 inode。
目录操作中，`kernel_vfs_mkdir` 调用 `ext4_dir_mk`；`kernel_vfs_unlink` 实现了真正的 Linux `unlink`-but-open 语义：
- `kernel_vfs_unlink` 在需要时先从挂载的 orphan 记录池预留一个回收记录，再调用 `ext4_funlink_dentry` 从父目录中立即移除目标目录项；后续对原路径的 `open` 立即返回 `-ENOENT`，并在同名路径重新创建时分配独立全新 inode。没有现存 node 的文件也使用这份预留记录，因而 orphan 回收失败时由 mount 单独持有。
- 若目标文件当前仍处于打开状态（`node->open_files > 0`），VFS 标记 `node->unlinked = 1`，旧 open 描述符（包括只读/读写 OFD 以及正在运行的源映射 ELF 可执行文件）保留底层 inode 数据与有效物理块，继续正常执行 `read/write/fstat` 与缺页加载（demand fault）；
- 只有当最后一个打开描述符与执行租约释放（`node->open_files == 0 && node->exec_users == 0`）时，VFS 才通过专有的 `kernel_vfs_try_release_orphan` 驱动物理存储释放。该过程先持有临时节点引用并安全使页缓存失效，再在扁平调用栈上调用 `ext4_orphan_free` 截断释放底层 inode，最后标记 `node->orphan_freed = 1`。通用的 `kernel_vfs_node_release` 仅负责内存节点对象的生命周期，绝不隐式或嵌套调用 `ext4_orphan_free`，避免双重释放与页缓存回收深度嵌套额外消耗任务栈；若底层释放失败，节点转移至 `adapter->cleanup_nodes`，由 mount 保留唯一重试 owner，绝不重新指向 file。若文件在 unlink 时没有现存 node，则使用预留记录直接调用 orphan free；失败后记录留在 mount 链，路径仍保持已删除。
由于 lwext4 的 `ext4_dir_rm` 会递归删除非空目录，`kernel_vfs_rmdir` 先用独立辅助函数 `check_directory_empty` 跳过 `.`/`..` 并拒绝非空目录，再通过 `ext4_fdir_unlink_dentry` 只摘目标目录项。被持有的目录 inode 延至最后引用释放才回收；同名重建得到不同 inode，旧路径对象仍可查询零链接状态。目录项 checksum 在设置 inode 字段后计算，卸载后的 `e2fsck -fn` 验证磁盘结构。
只读挂载下，所有上述修改操作直接返回 `-EROFS`。
unmount 在仍有 open file 或路径引用时返回 `-EBUSY`。末节点 `ext4_fclose` 失败把 node 转移到 mount cleanup 链，卸载重试同一个 handle；测试注入路径末引用和重复 inode 合并两种 close 失败并确认都被实际重试。非法引用或释放顺序触发 fatal，合法 heap/page 释放不返回可重试状态。

当前 VFS 同时服务 ELF 随机读、进程文件表和文件私有缺页，但仍不是完整 Linux VFS：没有负目录项缓存、逐分量权限检查、硬链接、后台 writeback、read-ahead、并发锁或多挂载。`kernel_vfs_path` 持有 mount/inode 与父目录项引用；`ext4_lookup_child` 按父目录 inode 查找。统一逐分量解析处理 `.`、`..`、相对/绝对符号链接、尾斜线和最多 40 次展开；open/stat 的尾斜线按目录查找，mkdir/unlink/rmdir/symlink 保留不跟随的最终目录项语义。创建允许缺失的最终分量，并把已解析父对象及最终名称转换为 lwext4 修改接口所需的临时路径。适配缓冲按真实祖先长度分配，用户输入/符号链接展开仍限制为 4096 字节；已存在的长父链不挤占短相对输入额度，255 字节组件可用于修改。路径对象释放不依赖原始绝对路径仍存在。该规则依据固定 Linux commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e` 的 [`fs/namei.c`](../../references/linux/fs/namei.c)。fs context 和目录 fd 直接持有解析起点；绝对路径忽略 dirfd，删除或改名不会把旧引用重定向到同名新 inode。目录支持打开与按后端 cookie 查询（`kernel_vfs_dir_entry`），会返回真实的 `.`/`..` 条目；每次查询从传入的 ext4 字节位置开始，而不是从目录起点重走，顺序枚举的条目访问为 O(N)。适配层把 lwext4 的正 errno 与 EOF 分开，再向文件资源层返回负 Linux errno。页缓存只保存普通文件内容；进程层只持有 VFS mount/file 抽象，lwext4 handle 没有泄露到 task 或 syscall ABI。

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
make test-lwext4-recovery-host
make test-vfs-riscv
make test-files-riscv
make test-files-partial-write-riscv
make test-exec-riscv
make test-root-init-riscv
```

恢复测试使用 `tests/host/block_fault.c` 的易失缓存与稳定镜像，逐个写入/flush 边界丢失未同步写，并另测最后一个 512 字节扇区先落盘。journal、ordered data、公共事务、持久 orphan 记录及实际回收分别测试；1 KiB/4 KiB、extent/legacy、orphan_file/传统链覆盖两次重启、分配和链接计数、空间回收及 `e2fsck -fn`。公共事务还逐个注入内存分配失败，验证命名状态完整回滚。此承诺限于该块模型；QEMU 正常退出和实板行为不能代替断电证据。普通数据原地覆盖不承诺整文件写入原子性，成功同步保证已提交字节持久；真实 musl 已覆盖临时文件写入→文件 fsync→跨目录 rename→两侧目录 fsync，rename 后端另用同一故障模型验证断电原子性。

宿主测试保留两种 lwext4 metadata checksum seed 只读探针，并在独立可写的 1 KiB-block extent、1 KiB legacy 与 8 KiB legacy (`^extent,^64bit`) 镜像上验证 aligned/unaligned hole、同块 gap、sparse truncate、allocated-block 上界、各自 exact maxbytes 以及大块 legacy 逻辑号不回绕，卸载后分别运行 `e2fsck -fn`。QEMU 测试建立真实 ext4 镜像，验证 `/init` mode、目录预检、随机偏移、EOF、越过 EOF 写入、sparse truncate、`-ENOENT`、open-file `-EBUSY`、只读 dirty-journal `-EUCLEAN`、缓存 miss/hit/LRU/pin、压力回收、mount purge、raw inode metadata 和全部页回收；VFS runner 注入一次 orphan free 失败，覆盖仍有打开 fd 与无现存 node 两条路径，确认路径不复现、mount 只保留一个 owner、重试后可卸载。文件资源测试核对 fstat/newfstatat metadata、unlink-but-open 的 `nlink == 0`，并证明不同 fd 与 mmap 共用 node/cache 而保持各自 offset；生产测试由静态和动态 musl 入口通过 VFS read source 读取真实根盘。

向下截断通过稳定 node–MM 登记通知相关地址空间，依据后端实际大小撤销越界整页
（含 private COW），并按驻留来源区分尾页清零与私有修改保留。通知不分配内存，
也不删除 VMA；O_TRUNC 和后端已变更再报错同样执行协调。关联由 MM 拥有，node
借用，末次 OFD 释放前必须解除。具体生命周期与失败回滚见 [MM 模块](kernel-mm.md)。

## 共享目录项与原子改名

挂载内以 `(parent identity, name)` 复用活路径对象，root 也只有一个活身份；inode 节点独立于名字。注册链只借用对象，OFD、fs context 和解析过程拥有引用，最后一个引用释放时摘除注册并迭代释放父链。路径内置的 inode handle 不再持有自身路径，避免循环引用；打开文件持路径引用且独立保留 node，因此关闭时可先释放路径再减少自身 open_files。

`kernel_vfs_*_at()` 使用 start/root 对象；只有仍使用 lwext4 路径接口的修改需要构造临时名字，解析与身份以已持有对象为准。活路径查找注册链成本 O(活路径数)，修改适配成本 O(祖先名称总字节数)，当前未引入负缓存或跨核锁。目录枚举每次刷新真实 inode 大小，打开后扩展目录不会漏掉新块。

`ext4_rename_child()` 以父 inode 和名称为入口。先检查类型、目标空目录、祖先关系和资源，再在一个事务里处理目录项、`..`、父目录 nlink、时间与覆盖目标的持久 orphan。VFS 在调用前预留新名和父引用；成功后不可调度地替换 source 的 parent/name 并断开 target。无失败后的内存分配，也不会将已打开的 target 换成 source。rmdir 同样用检查 checksum 和记录边界的空目录扫描，损坏目录不能被当作空目录删除。

聚焦验证：`make test-vfs-riscv test-files-riscv test-lwext4-rename-host`，组合验证为 `make test-userland-riscv test-diff-abi-riscv`。rename host 矩阵包含 1/4 KiB、linear/HTree、orphan_file/传统链、覆盖/插入/目录扩展，逐点 OOM 和断电后重复恢复及 `e2fsck -fn`；VFS 测试另覆盖改名后对象共享、活覆盖目标、删除 cwd、17 层 255 字节目录名的相对修改。

本阶段验证记录：`build/namespace-host-final.log`（32 组、620 次断电/重排、3004 个分配失败点）与 `build/namespace-final-regression.log`（RISC-V 全套、真实 musl/pthread、297 条 Linux 差分、988 个函数栈界；最大单函数 1952 字节）。
