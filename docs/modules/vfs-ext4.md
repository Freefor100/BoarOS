# VFS 与 ext4 模块

本文描述当前根文件系统的稳定接口和 lwext4 私有适配。第三方版本与许可证见[第三方代码](../third-party.md)，存储知识见[存储与文件系统学习总结](../learning/storage-filesystems.md)。

## 通用边界

`include/kernel/block.h` 定义同步块设备（支持读与可选写），`include/kernel/vfs.h` 定义 mount/file 对象以及根挂载、open/create、pread/pwrite、ftruncate、mkdir、unlink、rmdir、close、unmount、`kernel_vfs_fstat()` 与 `kernel_vfs_mount_is_readonly()` 查询。VFS 对外返回负 Linux errno；lwext4 的结构、全局设备名和正值 errno 不泄漏到调用者。当前有独立的 ext4、FAT32/VFAT、tmpfs、proc 和 devpts 实例；lwext4 共用 heap binding 按实例引用计数维护，每实例携带自己的锁上下文；进程 fd/open-file-description 位于独立的[文件资源层](kernel-files.md)，所有任务共享一棵挂载树，没有 mount namespace 隔离；命名空间修改使用可睡眠 mutex，inode、OFD 与后端各自同步。

`fs/vfs_objects.h` 定义通用实例、inode 节点、路径与后端操作表；
`fs/vfs.c` 管理路径身份、引用、执行/写租约、记录锁和缓存；
`fs/file_mapping.c` 管理 inode–MM 登记、借用游标和文件代次。

映射登记由 MM 持有，来源 OFD 维持 inode 寿命。inode 的 rank30 raw 仅保护登记、
代次、关闭状态和借用；注销先停止新借用并摘链，再在 MM gate、inode I/O 和 raw 锁外 drain。
退休后继持有真实借用，归还采用迭代分段，每段最多16项；记录及 MM 存储在 drain 前不能释放。
受控内存及磁盘文件的截断通知在 inode I/O 和 raw 锁外同步执行，回调可以睡眠取得一个 MM gate；普通CPU0缓存保留原调用契约。
它与禁止阻塞的等待节点回调是不同接口，不使用其抢占禁止上下文。
内存文件缩短先关闭旧代次，通知映射，再截断后备并提交大小/时间，最后开放新代次；
完整文件 MM 并发接纳仍由 MM 域验证和缺页提交重查保护，不因此开放一般文件业务跨核。
`make test-file-mapping-concurrency-host` 链接真实登记、VFS/tmpfs、OFD、对象及同步实现，
在4/16KiB、2/4宿主线程下核对1/8/32登记、退休借用、关闭等待、来源引用和最终资源。
映射登记的代次、短锁和等待状态由 inode 独占，首次映射时在发布 VMA 前准备；分配失败返回而不发布登记。未映射节点只有一个状态指针，不提前创建独立映射状态。独立状态为 88 字节、使用 128 字节 slab，最终随 inode 释放。首次并发准备的失败候选在锁外归还，预热后的准备不分配；没有状态的截断保持 inode 写保护直到后备提交，防止首次登记越过尺寸更新。

`fs/ext4_backend.c` 持有 lwext4 handle、块适配、事务、orphan 与 mount 错误，
通用层不再包含 lwext4 头文件。实例内按后端 inode 标识合并活 node，
不同实例不共享节点；后端准备私有 handle 后交给 `kernel_vfs_publish_node()`，
由通用层发布或复用已有节点。后端包装对象把通用 node 放在首部，末引用由
实例所属堆释放；`close_node` 只释放私有 handle，不等待 I/O，确保分配压力下
干净页回收不会递归进入存储等待。orphan、日志或块 I/O 错误仍由 ext4 实例持有。

生产路径仍只有一个 ext4 根实例；proc 是首个非磁盘后端，内容范围见[procfs 模块](procfs.md)。
普通文件继续使用同一页缓存与 inode 同步，pipe/socket/epoll 维持文件层独立 owner。
后端操作表在挂载前逐项初始化，支持分页前物理地址测试与高半区生产入口。
对象符号链接可由后端 `follow_link` 返回带引用的目标路径。proc 的 pipe fd
没有 VFS 路径，普通打开得到 `ENOENT` 后可对同一个最终链接调用可选
`reopen_link`；VFS 只核对链接类型和保留解析后的节点引用，后端在文件层
创建新的 OFD。其他后端未实现该回调时返回 `ENOTSUP`，不根据展示文本
猜测 pseudo 对象。
跟随式 `stat` 若最终链接没有可解析的路径，可调用后端 `stat_link` 取得
被链接对象的元数据；普通路径与悬空链接继续沿原解析结果返回。proc fd
的伪对象回调只在短关中断区内取得当前槽的元数据，不分配、不等待。

root/cwd 由共享 FS 记录持有；路径使用者取得带引用的成对快照，快照覆盖路径解析、等待和用户结果准备。同一次操作解析起点和绝对路径根共用这一对快照，起点助手直接借用它，不再次采样共享 cwd。`CLONE_FS` 共享 cwd 更新，fork 复制路径引用；上下文移动、退出与快照发布使用同一 VFS 状态短保护。存活路径按父路径身份和规范名称建立索引，rename 在目录事务成功后同步移动索引及父引用。最后引用先摘除弱索引，再在保护外关闭后端和归还存储。短保护内仅操作索引、引用、挂载连接及有长度上限的路径文本快照。

通用路径已有内部挂载树：挂载持有新实例根路径与被遮蔽路径的引用，父挂载记录
子挂载数；进入、同点覆盖、`..` 和 `getcwd` 都沿对象身份遍历。普通卸载须先
排除子挂载、其他打开对象及根路径引用，再摘挂载边；失败保留原挂载树。
跨挂载路径解析逐分量取得对应实例的命名空间锁。创建、删除、mkdir、symlink 和 mknod 直接把已解析父 inode 与单个名称交给后端；link/rename 沿用相同身份，不再把目录链拼回绝对路径后重新查找。ext4 的父 inode 创建与删除复用原事务、目录校验及持久 orphan 实现。修改操作只在目标实例锁下
完成后端事务；记录锁、OFD 和页缓存仍按各自 owner 同步。ext4 inode 仍是
32 位，但通用 inode 标识已扩展至 64 位；ext4 适配层拒绝越界标识。
RISC-V VFS 测试的内存后端覆盖内部边界；用户态 `mount(2)`/`umount2(2)`
已接入 proc、tmpfs、devpts 和块设备节点识别的 ext4/vfat。磁盘挂载按请求类型选择后端及其 worker，FAT32/VFAT 的名称、簇链与同步机制见[对应模块](fat32.md)。普通挂载、`MS_RDONLY`、`MS_SILENT`、同点覆盖、cwd 忙引用与
卸载恢复均有真实 U-mode 验证。`kernel/syscall/mount.c` 当前只接受这两个 mount
flags 和 flags=0 的 `umount2`；其他 flags 返回 `ENOTSUP`。bind/rbind、remount、
move、传播、noexec/nodev/nosuid 等其他挂载属性，以及 lazy/force/expire/nofollow
卸载未实现；所有任务共享挂载树，没有 mount namespace。剩余工作归[P1h](../goals.md#p1h-虚拟文件系统与多挂载)。
普通卸载当前针对非根挂载；绝对根挂载没有 parent/covered 边，用户态
`umount2("/", 0)` 返回 `EINVAL`。固定 Linux 对该情形尝试只读重配置，
这条根挂载路径须随 remount 独立实现，内核关机的根盘收口不代替它。
卸载先检查忙引用并标记 quiescing，阻止新操作，排空页缓存和在途 I/O。遗留的持久 orphan 意图在 journal worker 仍存活时恢复；恢复可能接受新日志组，因此先完成最终 checkpoint drain，再停止 journal worker 和卸载 lwext4。失败时挂载树、设备 claim 和错误 owner 保持可达；可重试卸载。成功后才摘树、释放最后 root handle 和实例，最终释放不再产生 I/O。`FALLOCATE_SYNC_RESIDUAL=1 sh tests/fallocate-sync-host.sh` 检查已关闭外层截断留下的恢复工作、worker 生命周期、重挂载内容与块归还。

`kernel_vfs_mount_root()` 根据传入块设备是否提供 `write` 回调决定只读还是读写挂载。读写 journal 挂载先 replay、校验 orphan 记录、启动日志并回收遗留 orphan，完成后才发布根路径。只读介质不能完成恢复时明确拒绝；未知必需特性、损坏日志或元数据也不能作为干净镜像继续访问。

`kernel_vfs_file_read_source()` 把保持打开的文件导出为带 `size/context/read_at` 的精确随机读源。回调只有填满整个范围才返回零；EOF 以内的短读转成 `-EIO`。ELF parser 因而能复用内存和 VFS 来源，而不依赖文件系统类型。

`kernel_vfs_open_executable()` 在普通 open 之上统一要求 regular file 和至少一个执行位；目录、非普通文件或无执行位返回 `-EACCES`。若目标 node 存在活动的写打开者（`node->write_openers > 0`），返回 `-ETXTBSY`；检查通过后累加 `node->exec_users` 并持有 `file->exec_lease`。对应地，以写权限打开普通文件需调用 `kernel_vfs_file_acquire_write()`，当 `node->exec_users > 0` 时准确返回 `-ETXTBSY`，否则累加 `node->write_openers` 并持有 `file->write_lease`。该租约在 `kernel_vfs_close()` 时释放，严格保障运行中二进制与写入者之间的互斥。

挂载为 lwext4 注册可选 realtime 时钟；尚未初始化时钟的纯模块环境保留原时间。普通文件的共享 inode 节点持有 atime/mtime/ctime 与脏字段位，首次打开从实际 inode 解码，硬链接和多个描述符共用该对象。`kernel_vfs_file_accessed()` 直接按这组运行时值判断 relatime，`kernel_vfs_file_modified()` 在非零写请求复制前更新 mtime/ctime；这两个时间更新入口不读取磁盘 inode 或创建时间事务，首字节 EFAULT 仍保留此前的时间更新。只读挂载与 inode 的 NOATIME 标志禁止访问时间更新；O_NOATIME open 选项仍明确返回 ENOTSUP。已知 mount 错误在写侧当场传播，读侧时间错误不改变数据结果。

脏时间字段随数据写回的同一事务编码；无脏数据页时由现有挂载后台线程在首次变脏约 100ms 后、显式同步或卸载编码。该线程在所有可写 ext4 上启动；没有 journal 时只处理脏时间，有 journal 时同时推进组提交。脏字段持有节点引用，最后描述符关闭不会丢失时间。脏时间队列的节点数在入队和摘链时维护，每轮直接用该计数限定处理量，不先遍历队列计数。worker 使用非阻塞事务准入，空间不足时先推进 journal，避免等待自身。namespace、truncate、chmod/chown 和显式 utimens 仍在原 metadata 事务修改时间；挂载回调暂存规范化值，外层事务成功后才发布到共享节点。已打开文件的回调直接借用 handle 的 `time_owner`；文件引用覆盖整个事务及 staged pin 归还。namespace 回调在共同 VFS 状态短保护内查找并取得节点引用，再进入时间戳保护；引用覆盖暂存及事务完成。每个字段的版本检查保留操作等待期间发生的更新，abort 丢弃暂存值，写回仅清除仍等于已编码快照的脏字段。运行时字段与脏时间链由挂载点时间戳 raw 锁保护；锁外通知及归还节点。前台元数据与显式同步的时间编码在发起 CPU 完成；后台时间写回、journal 提交和 checkpoint 由 CPU0 worker 推进。操作时机、编码及错误语义的依据见[时间戳学习记录](../learning/file-timestamps.md)。

共享查询的 GDT/inode checksum 校验不临时清零 cache 字段。`ext4_crc32c_zeroed()`
以一个或两个本地零字段计算同一 CRC，不复制整个 inode、不分配或等待；真正的修改路径
另写入计算结果。CRC32C共用4 KiB只读表的 slicing-by-4 实现，对齐后的四字节
一起更新，首尾逐字节处理，不越过输入区间；种子、分段更新和校验比较保持原格式。
RV 将小型 CRC32C 函数按 256 字节对齐，使热循环保持在同一 4 KiB 指令页，
减少 QEMU 的跨页翻译块查找；`make test-lwext4-crc-layout-riscv` 检查实际链接后的函数范围。
普通 inode 与 orphan 的计算共用归一化原语，128-byte/扩展 inode
字段规则保持原语义。布局由挂载几何验证保护，helper 的非法内部布局触发 fatal，
损坏镜像仍按原验证返回格式/校验错误。`make test-lwext4-checksum-host` 以独立逐位
Castagnoli参考覆盖CRC32C的种子、对齐、分段与守卫页边界，并覆盖只读输入、
非相邻字段、非法布局和真实镜像中的重入读者；引用归还、heap owner 和独立 e2fsck
共同验证。`make test-root-checksum-loongarch` 在512 MiB/1 GiB的带校验和根盘上
验证真实用户程序、持久数据、正常退出和e2fsck，保留普通错误fixture的原格式。
来源及反例见时间戳学习记录。

extent 树在读取节点时校验当前字节；实际修改通过 `ext4_ext_dirty()` 同步更新
该节点的 checksum，新建分裂节点使用同一入口。路径释放只归还引用：缓冲的
`BC_DIRTY` 表示仍待日志／落盘，不能据此给一次只读查询重复重写 checksum。
`sh tests/lwext4-metadata-host.sh extent-lookup` 检查脏节点的只读查找成本、
缓存字节损坏仍返回 EUCLEAN，以及恢复后的映射、资源与磁盘一致性。

`kernel_vfs_file_set_mode()` 通过持有的 inode handle 事务修改低 12 个权限位，
保留类型和其余位并更新 ctime；路径入口复用同一 handle。后端
`ext4_file_set_mode()` 不用 pathname，因此 fd 已 unlink 时仍可修改原 inode，
挂载只读或已有日志错误先返回真实错误。活 node 的 mode 缓存同步更新，
exec 权限检查可立即观察修改。该接口沿用现有
`ext4_file_set_times()` 的事务、sync_tid 和错误所有权边界。

`kernel_vfs_file_set_owner()`复用活inode handle和后端独占锁；路径形式另持
namespace锁，fd形式无需重新查路径。`ext4_file_set_owner()`将UID/GID、mode、
ctime与非目录的capability属性删除放在同一undo/日志事务中；成功后更新node的mode
缓存和同步目标，后续I/O失败仍由原mount owner保留。UID/GID按磁盘low/high 16位
合成32位。新建节点在父inode仍被引用时继承setgid目录的GID，初始目录权限设置
保留继承的setgid；普通chmod仍可明确清除它。匿名pipe元数据不经过ext4。inode最后释放时，外部EA块先归还引用，只有
最后引用才释放块；共享块的refcount、校验和与各inode占有的扇区数同事务更新。

已有活inode的按身份打开在namespace保护下先检查mount错误，再取得该节点引用与
新的file资格，保留实时size/mode；不先分配/打开临时后端对象再合并。节点registry
仍为弱链，closed/retired节点不复用，没有新增长期缓存owner。重新取得节点时，
先持有独立引用再等待inode锁；最后缓存页可在锁交接后立即回收，namespace锁
不能替代该生命周期引用。发布路径的候选合并也遵守这一边界。普通文件创建后
用已取得的lwext4 handle设置mode，避免重新走pathname，外层操作undo与错误归属不变。
`make test-vfs-riscv`以独立后端计数保护这两项可省工作及真实最后关闭的错误owner。

## 文件节点与页缓存

VFS 以文件系统实例与后端 inode 标识为活节点身份，普通文件、目录和字符节点都持有引用计数 node；路径对象另持有父目录项身份和一份活 inode 引用。实例按 inode 建立 AVL 索引，链接嵌在 node 内，查找与增删为 O(log N)，无需另分配索引对象。注册链保留全实例枚举顺序，末引用通过反向链接直接摘链；索引和注册链只借用节点，不增加引用。查询在短 IRQ 保护内完成，需跨等待的调用者在离开保护前取得自己的节点引用。磁盘 inode 复用只替换索引中的退休或已关闭节点，旧节点继续由其实际文件和缓存 owner 持有。独立 open file description 各自保存 offset，但同一 inode 指向共享 node。文件大小通过 `kernel_vfs_file_size()` 实时查询所属 node 的实时大小，确保写入或截断后各共享描述符观察到一致的文件长度。

活 node 还嵌入记录锁区间树与等待队列，因此独立 open 必须在同一 inode 上冲突；unlink 后仍打开的旧 inode 保持原锁身份，同名重建使用新 inode 和新锁状态。末节点释放要求树与等待队列都为空；锁 owner、close/退出释放与阻塞 pin 契约见[文件资源模块](kernel-files.md)。该状态属于单 hart 临界区，不代表已经具备跨核并发锁。

每个 ext4 实例建立独立的架构页缓存（RV64 为 4 KiB，LA64 为 16 KiB），键为 `(node, page_index)`：开放寻址哈希提供平均常数时间查找，双向 LRU 维护回收次序。删除产生的墓碑通过同容量重排清除；只有下一项存活元素使占用超过 75% 时才扩容，候选表分配后再次按当前占用判断发布。缓存项持有 node 引用和一份物理页引用；命中时再给调用者一份临时引用，因此 `read`、不同 fd 与文件私有/共享映射可以安全引用同一页。共享 PTE 另持物理引用；MM 拥有的别名记录由缓存项反向索引借用，含别名的缓存项不能失效或回收。

普通写在文件写操作互斥内，先为有限窗口准备实际磁盘空间，再在 inode 数据锁外解析并固定来源用户页，直接复制到同一缓存页并更新 node 的逻辑大小；不同 fd、VFS pread、ELF 读源与映射缺页立即看到该内容。跨 EOF 的普通写在复制后清零将要暴露的空隙，避免来源映射恰好位于旧 EOF 尾部时提前破坏输入；显式扩展亦清零新暴露字节，并重新保护共享别名，防止页仍可写时漏记脏。缓存项按 inode 另建全页链供截断/失效使用，脏页则由按页号排序的红黑树索引，计数随标脏和清脏增减。范围写回从范围起点查树，依次取得范围内的有序脏页窗口；按范围 `msync` 只提交目标文件页。每项保留脏范围、修改代次与 writeback 状态；写回前重新保护本页共享别名，再固定页面，只有完整提交且代次未变才清脏。失败保留缓存页/node owner；VFS 的后端提交入口统一检查短写并发布一次 inode 错误，后台、文件同步和挂载同步只传递同一结果。lwext4 非 journal 写入/截断用操作范围固定本次修改的缓冲，建立完整的分配所有权后才提交该集合；不会全量 drain 历史 dirty list。底层最后引用的 flush 错误显式返回，失败缓冲仍在 mount dirty list。最后一个 fd 关闭不释放仍有脏页的已链接 inode。最后链接已删除且文件使用者全部结束时，inode writer 先排空已提交的后端读写，再沿截断共用路径退休缓存页；尚未取得 inode reader 的写回快照看到退休状态后放弃提交，随后释放持久 orphan 和磁盘空间。

空间准备由后端维护真实映射：已有 initialized 或 unwritten 块直接复用，hole 才分配 unwritten extent；tmpfs 不需要此步骤。每次后端操作最多处理 32 个文件系统块，已提交的连续字节数只属于本次请求，不另建预留缓存。空间不足时只复制已准备的前缀，拒绝部分不改变 EOF；普通分配 ENOSPC 不登记写回错误，先前已接纳的数据仍须正常 fsync 和读回。真正的写回 ENOSPC／EIO 保留脏数据与错误游标。CPU0 执行后端准备时短暂持 inode writer，在来源缺页或 pin 前归还；外层文件写操作互斥继续固定本次写资格，避免等待同文件来源缺页时自锁。

legacy 间接指针格式没有 unwritten 状态，新分配块先清零。若映射越过持久 EOF，journal 操作同时保留已有 orphan 意图但暂不回收；复制结束后先写回已接纳字节，再将意图设为可回收并按实际磁盘 EOF 删除未用尾部。写回或清理失败分别由脏缓存和持久 orphan 保持恢复责任。无 journal 时节点在创建映射前持有完成标记，只在成功完成后清除；最后关闭不递归存储 I/O，而由既有 `cleanup_nodes` 保留节点供卸载重试。清理失败不释放节点或 mapping 对象，成功后一起销毁；inode 真正释放时退休对应旧清理节点，避免 inode 复用后误截断新文件。同尺寸 truncate 和 unlink 也遍历实际映射及空的间接树，能回收大小仍为零的尾部。来源首字节 EFAULT 不增长 EOF；extent 格式可以留下仍读零的真实预分配，legacy 未用尾部由上述完成路径回收。

共享映射写缺页在授予可写 PTE 前调用同一空间准备，要求整页中所有有效 EOF 字节都有存储；16 KiB 页只准备出部分 1/4 KiB 块时返回 BUS fault，不能开放其余 hole。等待期间释放 MM gate 和 inode 读资格，返回后先重新检查 VMA 代次、映射代次与 EOF，再解释后端错误并核对驻留页；若地址已被 MAP_FIXED 替换，重试新映射，不把旧文件的 ENOSPC 施加到新地址。private COW 不需要后端存储，仍可在磁盘满时生成私有副本。

`FALLOCATE_HOST_CASES='admission-full admission-legacy admission-prefix admission-prefix-legacy admission-fault admission-mmap' FALLOCATE_HOST_IMAGE_SIZE=8M sh tests/fallocate-host.sh` 检查磁盘满前的接纳与准确短写、fsync 和失效后读回、来源 EFAULT、legacy 清理失败后的 truncate／unlink／仅关闭及卸载重试、真实写回错误，以及共享映射和 private COW。默认组合为 4/16 KiB 页、1/4 KiB 文件系统块和有／无 journal，共 48 个场景，每个场景检查资源归还及 e2fsck。

截断只写回新 EOF 之前的脏范围，再调用后端；成功后按实际结果更新逻辑长度、通知 MM、丢弃越界页并清零尾页。后端失败且磁盘 size 未变时保留原逻辑长度与脏 owner，截零不需要保存待丢弃数据。删除仍打开的文件保留缓存；只有 orphan 最后一个打开/执行 owner 退出时，才丢弃已不再可观察的缓存并回收 inode。

截断与打洞在调用者退休缓存页，远端排空请求只通知现有 CPU0 worker。共享退休队列可能持有其他 inode 的最后引用；弱索引查找并取引用与末引用摘除使用共同 VFS 状态短保护，后端 close 与堆释放在保护外由实际归还者执行。未发布的私有候选仍由拥有文件引用的调用者就地释放。

缓存索引失效本身不撤销用户 PTE。向下截断另通过稳定 node–MM 登记通知相关地址空间，撤销越过新 EOF 的整页（含 private COW 与 PROT_NONE），并按驻留来源处理非对齐尾页；普通写直接修改共享缓存，不撤销 private COW 页面。VMA 保留，后续访问重新缺页并检查 live inode size。实现与单 hart 同步边界见本文末尾及[MM 模块](kernel-mm.md)。

miss 路径私有分配未清零页，通过 node 的无 offset 副作用批量 `pread` 初始化有效内容；稀疏区由后端合成零，成功后只清 EOF 尾部。有效字节数按 node 的逻辑大小计算。读取整个越过 EOF 的页返回 `OUT_OF_RANGE`，尾页剩余字节保持为零。物理页分配器的压力入口由多缓存共同 owner 注册，按实例轮转；分配首次耗尽时从 LRU 尾部仅回收引用数为 1 的未固定干净页，安全的普通任务可等待后台一轮后重试一次。物理引用数为 1 仍可能有等待回写的 entry 借用；后台完成 I/O 后，在缓存索引保护内归还本批借用，重新检查其他借用、loading、writeback、脏范围和映射，再决定退休。批量选择与干净页退休共用该保护，实际 I/O 和资源释放放在保护外。映射/请求固定页不驱逐；lwext4、堆与 IRQ 不等待后台写回，防止重入共享 handle、后端锁和 bcache 分配。阈值 worker 的生命周期见下文。

普通写入还在接纳边界处理脏页压力：每最多 32 个来源页，在取得 inode 数据锁和用户页 pin 之前检查一次水位；脏页达到全局高水位或可用页低于保留水位时，推进一批已有脏页，再回收可释放的干净页。整次 write 的 OFD 位置与文件写操作互斥保持不变。若写入正在缓存执行者内运行，就地使用其独占工作区推进，不能排队等待自己的后台线程；其他调用者只提交这批写回，执行者不取得原调用者的 OFD／写操作锁，也不为回收内存另分配前景工作区。写回错误记录在实际 inode；无关文件的 ENOSPC／EIO 不归给当前覆盖写，失败队首保留脏数据并让出后续可回收工作。分配器内的重入、锁和 I/O 限制保持不变，真实 ENOSPC、EIO、ENOMEM 继续返回；已有成功前缀仍按 Linux 短写规则交给调用者。`sh tests/page-cache-concurrency-host.sh pressure` 在生产分组日志与 16 MiB 页池内检查大于内存的完整写入、磁盘满、写回 EIO、真实无空闲页，以及另一 inode 失败后的独立覆盖写，覆盖 4/16 KiB 页与 1/4 KiB 文件系统块，并检查读回、资源归还和 e2fsck。

缓存销毁和 mount 卸载有严格顺序：先释放所有进程 MM 和临时读者，停止并 join 页缓存 worker，再写回并 purge 该 mount 的缓存项、关闭最后的 node；日志 worker 继续服务这些交接，排空到 checkpoint 后才停止并 join，之后允许 lwext4 unmount 与设备 flush；缓存最后注销 reclaimer 并释放哈希表。物理页和堆对象的合法释放完成即返回，分配器不变量错误进入 fatal；只有真实 ext4/block I/O 清理错误保留 mount owner。

`kernel_vfs_sync()` 先分批交接目标 inode 脏页，再按后端 `file_metadata_execution` 处理元数据并捕获一次 full/data durable 目标；ext4 在发起 CPU 准备并登记。原调用者等待自己的目标。达到目标或发生错误时，完成者在实例短保护内记录元数据错误、消费共享 OFD 的错误游标，再在原请求的 raw 保护内发布 result/done 并通知调用者；调用者取走结果直接返回，不为错误观察再次排队。数据交接失败只观察既有错误记录，不重复递增写回错误序号；挂载同步的时间戳失败由后端记在实际 inode，根元数据与最终 flush 失败记在根 inode，VFS 只传递结果。只读或内存后端即时完成，已经完成的请求无需建立等待队列。

等待请求位于调用者栈上，借用实际 file，外层 OFD 或映射来源引用覆盖整个等待，挂载仍有同步请求时不能停止日志 worker。登记端和 worker 在同一挂载 `journal_control` 短保护内检查已发布的 durable/error 并登记等待，已满足的目标直接完成，其余按 (目标序号，请求地址) 加入侵入式 AVL 索引；登记不重扫其他请求。journal 推进只查最早目标，每次在控制保护内摘除一个可完成请求，锁外发布结果和通知调用者；通知返回后不再访问该栈请求。checkpoint 只在出现错误时处理这些等待者；正常 checkpoint 不推进 durable，也不扫描等待索引。正常 journal 同步只交接一次 prepare，由原调用者等待完成，不以文件位置锁覆盖独立同步等待。

journal 路径已包含持久化屏障，不额外发设备 flush；无 journal 路径仍显式 flush。独立 open 各持错误观察位置，dup/fork 共用 OFD 的位置。`fsync/fdatasync` 支持普通文件与目录；共享 inode 的后端 handle 记录完整和数据序号。full 同步为涵盖经其他 handle 的 namespace 修改，捕获当前 mount 序号一次；data 同步等待数据/检索依赖序号，纯时间修改不推进它。同组时间字段可以顺带持久化；新修改不延长已捕获的目标。`O_SYNC/O_DSYNC` 分别对已接受的写入前缀执行 full/data 同步，失败返回 errno，但已接受字节与 offset 保留。journal 的关键写入、checkpoint 或屏障失败使 mount 持续拒绝修改和同步；OFD 错误游标不能清除该错误。commit 与 checkpoint 仍沿用既有独立推进、稳定版本和屏障顺序。

摘出 inode 弱索引后，最终关闭与 heap 释放继续以 `retiring_nodes` 占有挂载实例；卸载在短保护内检查索引和这些执行者，等待其结束后再释放后端。rename 可替换名称内存，因此 comm 等消费者在同一短保护内复制 basename。lwext4 的全局挂载槽在短保护下查找、预留和发布，初始化与设备 I/O 在保护外进行；操作所持的挂载引用覆盖返回槽的使用。

日志请求、checkpoint 请求和进度等待队列随挂载实例一起初始化，同步推进日志与后台 worker 共用这些队列。后台线程启动只建立执行者，不重置等待关系；受限日志预算在前台同步推进旧组后也可正常发布进度。

lwext4 块缓存保留目标作用于闲置缓冲，默认 8 项；正在装入、被调用者或日志借用的块另持真实引用。miss 在短保护下按闲置 LRU 选择释放者，日志版本的 home 缓冲仍计入原日志内存预算。批量借用归还和并发 miss 可短时超过闲置目标，内存压力沿现有回收入口处理。将日志借用计入闲置目标会使其占满名额，导致根目录和父目录反复驱逐、读盘。

## 挂载范围与全局同步

`kernel_vfs_sync_mount()`先取得根路径owner，按注册时增减的节点计数准备快照容量，
再在短发布保护区遍历一次并引用全部活节点；容量不足时先重新准备，不能漏掉新增节点。
节点在取得inode锁和睡眠前已被钉住，调用结束统一归还。逐个交接普通文件页缓存后，
后端捕获挂载durable目标一次；并发新修改不不断推进该目标。journal保留ordered-data、
日志与commit的既有屏障，syncfs不要求checkpoint全部完成；无journal则写回全部块缓存
并执行设备FLUSH。文件被unlink或最后fd关闭，不会从本次已捕获的节点集合消失。

全局sync先捕获已发布挂载的根路径引用，释放发布保护后进行I/O；额外引用阻止卸载。
挂载数组OOM时，按入口时的挂载身份上界逐个重新查找并取得引用，跨等待不保留裸树指针。
新挂载不无限延长调用，已卸载者由卸载路径排空；一个挂载失败不阻止尝试其他挂载。
节点／挂载数组只属于当前请求，不建立长期索引或后台引用。页与对象原有回收协议不变。

挂载记录写回错误序号和最近EIO/ENOSPC，OFD从打开时的序号开始观察；syncfs原子更新
该OFD的游标，dup/fork共享它，fsync仍使用独立的inode游标。内存不足返回资源错误并
保留脏状态，不能当成设备写回失败；不可确定的journal失败仍冻结原mount。
依据为`references/linux` v7.2的`fs/sync.c`、`fs/file_table.c`和`lib/errseq.c`。
验证入口是文件／VFS fixture、`tests/diff-abi/sync.c`及真实musl的`tests/userland/sync.h`。

## lwext4 配置和生命周期

内核编译 lwext4 journal/replay、orphan 和分批截断路径，启用内部xattr操作供chown删除capability；关闭debug/assert和mkfs，并把 malloc/calloc/realloc/free 绑定到当前内核堆。尚未接入用户xattr/ACL syscall或capability执行权限。生产根盘使用 raw whole-disk ext4。块层在整盘登记后扫描 MBR 四主分区，ext4 后端直接访问所选设备的相对偏移；分区平移、父盘引用和物理 I/O 锁由块层维护。

事务接口 `ext4_transaction_begin/end/abort` 支持同一 mount 的嵌套修改。元数据修改前保留原内容，供 abort 恢复仍被引用的元数据缓冲；普通文件数据在私有新版本上修改，abort 只丢弃新版本，不再备份或改写旧数据缓存。有 journal 时，发生在日志提交前的分配或关联数据 I/O 失败完整回滚本次操作并返回实际错误；已可能影响日志持久状态的错误由 mount 保留，不能清除后继续。外层 abort 后，调用者须重新打开在内层修改过的 lwext4 handle；VFS 的普通操作各自完成事务，不持有跨 syscall 的开放事务。

namespace 的创建、mkdir、mknod、rename、link、symlink、unlink 和 rmdir 通过
`ext4_metadata_execute` 执行完整原子操作；chown 删除外部 capability EA 也复用同一
执行核心。开始时的固定 credits 只保证基本准入，目录增长或位图等后续修改仍可能
遇到硬预算。只有外层操作返回 ENOMEM、本次成功开始之后预算拒绝计数增加、私有
事务已完整 abort 且 journal 没有持久错误时，才释放后端锁等待旧组的 checkpoint，
并从整个操作起点重做。没有待回收旧组或 checkpoint 未能前进时保留错误；单次操作
自身超过预算、真实物理 OOM、ENOSPC 和设备 EIO 不因此变成无限重试。嵌套操作不
自行等待或重做，由仍持有修改的外层决定回滚；事务开始失败沿用原准入错误。

每次重做前重新初始化未发布的临时 handle 和输出，创建同时完成 mode 设置；只有
最终成功才发布 VFS 节点、rename 结果、时间值和 live orphan 责任。chown 的文件
同步序号在同一 mount guard 内、外层成功接纳后发布，等待时只保留执行入口的锁层。
无 journal 时没有上述回滚重做；各操作保留原边界，原本依赖显式 journal 事务的
create、mkdir、mknod 仍返回 EOPNOTSUPP。

`FALLOCATE_HOST_CASE=namespace-budget FALLOCATE_HOST_JOURNALS=journal sh tests/fallocate-host.sh`
在 4/16 KiB 页和 1/4 KiB 块上让上述操作实际遭遇硬预算，检查 checkpoint 后成功、
mode/身份/链接与已打开 orphan 内容、chown capability 和同步依赖；另检查单操作
超预算、显式外层嵌套、真实物理 OOM 和设备读 EIO 的错误与回滚、旧内容、资源归还
及 e2fsck。`sh tests/lwext4-metadata-host.sh ownership` 另覆盖共享 EA、所有权更改的
分配失败和持久化错误。

`ext4_journal_group_enable/commit/checkpoint/drain` 由可写 journal mount 的两个 joinable 线程驱动：
commit 线程处理封口与日志提交，checkpoint 线程处理已持久前缀的 home 写入和日志回收。
二者只在选择与退休 owner 时持 mount 锁，设备 I/O 可独立等待；
宿主 fixture 的 `service` 接口仍可同步推进同一引擎。私有操作保留元数据撤销内容，成功后合入挂载点运行组并释放本次撤销内容。接纳只处理新增或替换的记录，不重新扫描累计集合。文件数据从接纳时起就借用稳定来源；封口只复制元数据到已预留的不可变版本。提交准备使用预留日志资源和挂载期固定映射，设备提交和 checkpoint 不再读取可变 bcache。`journal_pending` 阻止元数据提前回写；释放的块和 inode 到 checkpoint 完成后才能重新分配。释放区间仍由所属事务链持有，块与 inode 各有一个引用同一节点的区间索引，查询直接给出覆盖区间末端或下一个区间起点，分配器可整段跳过。可回收块数、inode 数随区间增删维护，不再反复扫描所有待提交事务；接纳只移动事务链，abort 或持久化 checkpoint 尾推进后才撤销索引。重复或交叠释放返回 EUCLEAN。

软封口条件是首次脏化后 100 ms，或运行组预留的元数据日志空间达到 256 KiB；普通文件数据块数不参与这个阈值。同步目标、显式请求和资源回收也会推动封口。文件版本按实际保留的内存控制：每个外部来源只取得一份引用并计一次完整分配，块借用只增加控制记录；未提供稳定覆盖的部分块才计入私有数据镜像。每个元数据 home buffer 在第一个版本 pin 时计费，最后一个版本释放时撤销计费。空闲镜像和控制记录池最多保留 min(256 KiB, 预算/4)，不足时先释放空闲资源；固定日志映射也计入挂载点预算，运行时上限为 min(4 MiB, RAM/32)。

lazy 块组的查询只读取和校验；数据分配按需初始化块位图，inode 分配再初始化 inode
位图，并在同一事务内清零新选中的 inode、修改分配位和 `itable_unused`。inode 分配
也准备该组的块位图，与固定 Linux `fs/ext4/ialloc.c` 的组元数据规则一致。
未使用的 inode table 尾部保持 lazy，`ITABLE_ZEROED` 不被提前置位；不会把整张表的
清零加入一个私有事务。大型块组的表本身即可超过 journal 预算，等待旧组 checkpoint
不能使这种操作变得可接纳。`sh tests/lwext4-lazy-groups-host.sh` 用显式关闭 discard 的
384 MiB lazy 镜像检查跨组分配、预留块写入、新 inode 清零、abort/OOM、正常卸载和
e2fsck；`LAZY_HOST_CUTS=1` 另检查 inode 创建的逐 write/flush 边界断电恢复。

运行组之外最多两组封口 FIFO 和一组提交中；commit 屏障后发布 durable，metadata 版本、日志空间与 quarantine 保留到 checkpoint 完成。commit 与 checkpoint 各自维护实际在途 owner，checkpoint 的设备等待不阻止后续提交或 fsync 完成。
checkpoint 积累到四组、活动内存超过预算一半，或实际空间等待和排空请求指定了目标序号时立即推进；低压力的小批次最多等待首个待回写组出现后的 100 ms，后来提交的组不延长这个期限。单纯没有待提交的 sealed 组不能作为回写条件，commit 此时仍可能在等待设备。每次选择最多八组已持久的连续前缀，目标序号保留到各批都已回收，释放的预算通知 commit 线程继续。选择阶段复用现有物理块索引，只为本次已提交前缀的同一 home 地址保留最后一个版本；revoke 去掉旧的块用途，后续元数据可以发布新版本，不借用尚未选中的事务。home 数据和 checkpoint 尾部持久化后，才释放所选前缀的全部版本与 quarantine。
任一线程遇到持久化错误都保留第一个错误；另一线程即使随后完成 I/O，也保留其版本和恢复 owner，不再发布进度或回收空间。等待区分 sealed、durable 和 checkpoint：封口 FIFO 满才等 sealed，实际内存、日志空间或复用不足才等 checkpoint。写入接纳前分别估算数据记录、所需私有镜像和新增来源；映射增长若仍触及硬预算，先结束私有操作并完整回滚，再等待旧组回收后重做。普通分配失败不会因此重试，嵌套原子操作也不会中途提交或释放修改锁等待。

同步返回保证提交记录及其先决数据已经持久化。历史存储流水线及 SQLite 恢复结果见[2026-10-01 验收记录](../learning/cost-baseline.md#s9-存储流水线验收2026-10-01)。

`make test-lwext4-group-host` 使用实际引擎与独立设备计数，覆盖 32 次时间修改合成一批、嵌套 abort、后操作各预留点 OOM、提交期间同块新修改、冻结后禁止新分配、1/4 KiB 文件系统的 WRITE/FLUSH 失败与重启恢复。新增交错检查把 checkpoint 停在实际 home write 内，后续真实 fsync 仍必须达到更晚的 durable 序号；
双向注入另一路写入或 flush 失败，检查两个 owner 均保留，并逐边界断电、两次恢复及 fsck。
checkpoint 合并另检查 1–3 组不产生 I/O、第四组合并同一 home 版本，十组请求按 8+2 保留目标，
实际来源内存越过阈值时优先回收旧前缀，以及后台首个期限不被后来的组延长。
命名空间回归暂扣提交进度，核对正常容量下的 truncate、空文件/数据文件删除、仍打开的无链接文件及 inode 复用隔离。`sh tests/lwext4-reclaim-host.sh` 在同组回收与意图先 durable、checkpoint 暂未执行的跨组回收上，逐实际 I/O 边界检查丢失/重排、两次恢复和文件系统一致性。默认同步路径的 metadata/几何/错误原子性回归继续由 `make test-lwext4-metadata-host` 保护。

同一提交阶段使用稳定数据和日志控制 owner 内嵌的范围节点，组成 `ext4_blocks_set_phase()` 输入；不再每 32 个区间等待整批完成。块层先检查整个只读范围游标，再用预留的 32 项工作区持续补充空出的成员，直到整个阶段结束。磁盘相邻、内存分散的区间沿 SG 路径提交；没有 phase 设备接口时按范围顺序写入。journal 的物理块索引保证同一阶段范围不重叠，公共 write_batch 仍保留原有重叠检查和部分扇区语义。日志 magic 转义只为实际命中的载荷预留独立首扇区，余部借用原版本，避免复用 descriptor 缓冲而被迫等待它的 DMA。数据来源一直保留到实际设备使用结束，失败挂载还会保留读取当前文件内容所需的版本。

数据和日志正文位于同一设备。提交先完成它们的全部写入，共用一次提交前 flush；随后写 commit 记录，再 flush。只有不生成日志记录的即时数据事务单独执行数据 flush。日志资源在数据写入前预留，预留失败不写当前事务的数据。checkpoint 将已提交元数据写回原位置并 flush，再持久化日志起点，最后释放日志空间和版本。文件数据不写进元数据日志。主 superblock 的分配计数、恢复位和校验和也属于事务，挂载和卸载不在日志之外覆盖它。512 字节原子扇区模型下，superblock 校验失败只允许根据合法几何信息进入受限恢复，重放有效日志后才能访问文件。

`ext4_orphan.c` 支持传统 `last_orphan/i_dtime` 链和 `orphan_file`，校验范围、重复记录、循环、分配状态和 checksum。完整加载或显式校验时建立挂载持有的成员索引，记录 inode 对应的传统链前后节点或 orphan_file 块与槽位；正常 contains/peek 直接查询该索引。orphan_file 用空槽位图与非满块链选择位置，增删只读取并校验实际修改的槽或链接。修改前预留私有事务 undo，abort 逆序恢复索引和空槽，不分配或执行 I/O；接纳或同步提交成功后释放 undo，不确定提交错误仍由失败事务持有。显式 validate 重新完整校验，不能在尚有私有 undo 时替换索引。unlink 将最后一个链接摘除与持久 orphan 记录放在同一事务；缩小文件先在操作事务中登记最终 size 与 orphan，再由 `ext4_truncate.c` 每事务最多释放 32 个尾部数据块及已空的索引路径。意图与回收可以合入同一运行组，也可以按 journal FIFO 跨组提交；普通 truncate/unlink 不为每次回收等待挂载点 checkpoint。释放范围仍由 quarantine 保留，实际预算、日志空间或复用压力才等待 checkpoint，显式同步及卸载保持各自的 durable／排空边界。无 orphan 的新建、同尺寸截断和扩展不启动回收事务。恢复根据实际映射找到尾部，支持稀疏 extent 和三级间接块，不依赖已缩小的 size 推测待回收块。最后删除 orphan 记录与释放无链接 inode 同事务完成；删除使用索引维护的成员数，在集合为空时同事务清除 ORPHAN_PRESENT，不重复扫描来判断是否为空。后续失败仍回滚本操作的 superblock 和记录修改，重复恢复可继续前次进度。仍被打开的无链接 inode 在正常运行期间保留，重启才回收。

支持有界的 JBD2 checksum v2/v3、32/64-bit revoke；异步 commit、未知必需特性和旧 CRC32 journal 格式明确拒绝。已提交事务的损坏不能当作未提交尾部丢弃。只读脏日志、恢复 I/O 错误或损坏均拒绝开放用户访问。失败的日志/挂载 owner 保留到重启；普通合法内存释放不建立重试链。挂载准备阶段的 ENOMEM/ENOSPC 与关键 I/O 错误分开：资源不足保留可恢复的准备状态，后续 cleanup 可继续恢复并卸载，不能永久锁住 heap binding。
已有路径先逐分量取得目录项和 inode 身份，再用 `ext4_fopen_inode` 按 inode 打开普通文件、目录或字符节点；新建仍由 `ext4_fopen2` 提交。目录 handle 不在内核任务栈上分配完整 `ext4_dir` 结构。
普通文件随机写入与追加先进入页缓存；append 以共享 node 的逻辑 EOF 为起点并推进实际接收字节。定向写回通过 `ext4_fpwrite_batch` 的独立区间提交脏范围，等待事务预算时不会被其他任务改变共享 handle 的 offset。lwext4 的 `SEEK_SET` 允许定位到 EOF 后，磁盘逻辑块只在写回时按数据范围分配；新分配的部分块先清零，旧 EOF 块尾清零，完整中间 hole 保持未映射。底层部分写失败不缩小缓存已经接收的逻辑长度，整段脏范围由页缓存保留供重试。`st_blocks` 报实际已分配存储，不由逻辑大小猜测；检查磁盘块生命周期的测试需先同步。

截断统一通过 sparse-capable `ext4_ftruncate` 执行：缩小仍释放尾部块，扩大只清零已分配的旧 EOF 块尾并发布新 inode size，不为完整逻辑 gap 分配块，且不改变调用 OFD offset。文件打开时按实际 inode mapping 缓存可寻址 size 上限：extent inode 使用 `EXT_MAX_BLOCKS * block_size`；legacy block-map inode 取指针树容量、`EXT_MAX_BLOCKS` 个可安全计数的逻辑块和 inode `i_blocks` 容量（包含间接块开销）的最小值，再乘以 `block_size`。因此 legacy 最后可用逻辑块也是 `0xfffffffe`：即使 8 KiB 三级间接树容量超过 2^32，也不会让 `ext4_lblk_t` 在 2^45 字节处回绕到块 0。truncate/write 超界返回 `EFBIG`，seek 超界返回 `EINVAL`，在任何尾部清零、64-bit offset 缩窄为 `ext4_lblk_t` 或 inode size 更新前拒绝。lwext4 在写入或截断所有可能改变状态的返回路径上，让 handle 的 `fsize` 保持为当前 inode 中可知的最新 size。

新分配的数据块在 caller data 写入前整块初始化；初始化失败只撤销该 exact logical block 的 mapping，不回滚此前已经完成的块。unwritten extent 的初始化和 caller 写入版本规则见[文件空间预留与打洞](#文件空间预留与打洞)。lwext4 在写入或截断所有可能改变状态的返回路径上，让 handle 的 `fsize` 保持为当前 inode 中可知的最新 size；VFS 在 truncate 后据此同步 node/file size 并失效缓存；writeback 期间逻辑长度仍由缓存接收进度决定。部分写返回依据固定 Linux `references/linux` commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e` 的 `mm/filemap.c::generic_perform_write()` 与 `fs/read_write.c::new_sync_write()`：正进度覆盖后续错误并只推进相同字节数的文件位置。

extent 向左合并相邻新分配区间时，`ext4_ext_insert_leaf()` 同时移动逻辑起点与物理起点，扩大长度，并通过 `ext4_ext_correct_indexes()` 更新首叶对应的祖先分隔键。只移动逻辑起点会使原右侧数据映射错位，新分配的左块失去引用，右端还可能指向其他 extent 的物理块；随后截断会重复释放该块，由 journal quarantine 返回 `EUCLEAN`。物理起点更新与固定 Linux 的 `fs/ext4/extents.c::ext4_ext_insert_extent()` 一致。`sh tests/lwext4-metadata-host.sh prepend` 验证 inode 内及外置叶的内容、同步及组提交截断、正常卸载和 `e2fsck -fn`。

`kernel_vfs_fstat()` 把当前 open handle 指向的 raw ext4 inode 转换为统一 `kernel_vfs_stat`，不按路径重新查找。普通文件 size 与 atime/mtime/ctime 来自共享 node 的当前值，其余字段来自实际 inode。它读取 inode 的 ino/mode/nlink、Linux low/high uid/gid、64-bit size、以 512 字节为单位的 allocated block count，以及 filesystem block size；atime/mtime/ctime 按磁盘 inode size 与 `extra_isize` 共同判断 extra 字段是否存在，再解码 2-bit epoch 与纳秒。该编码依据固定 Linux commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e` 的 `references/linux/fs/ext4/ext4.h`。根 mount 保存稳定的内部 ID 1 作为 `dev`，不表达物理设备 major/minor。lwext4 的 `ext4_fraw_inode_fill()` 是最小 handle adapter，使最后一个 dentry 删除后仍能查询活着的 inode。
目录修改由父 inode 和单个名称定位；`kernel_vfs_unlink` 保留 Linux 打开后删除的对象语义：
- `kernel_vfs_unlink` 在需要时先从挂载的 orphan 记录池预留一个回收记录，再调用 `ext4_unlink_child` 从父目录中立即移除目标目录项；后续对原路径的 `open` 立即返回 `-ENOENT`，并在同名路径重新创建时分配独立全新 inode。没有现存 node 的文件也使用这份预留记录，因而 orphan 回收失败时由 mount 单独持有。
- 若目标文件当前仍处于打开状态（`node->open_files > 0`），VFS 标记 `node->unlinked = 1`，旧 open 描述符（包括只读/读写 OFD 以及正在运行的源映射 ELF 可执行文件）保留底层 inode 数据与有效物理块，继续正常执行 `read/write/fstat` 与缺页加载（demand fault）；
- 只有当最后一个打开描述符与执行租约释放（`node->open_files == 0 && node->exec_users == 0`）时，VFS 才通过专有的 `kernel_vfs_try_release_orphan` 驱动物理存储释放。该过程先持有临时节点引用并安全使页缓存失效，再在扁平调用栈上调用 `ext4_orphan_free` 截断释放底层 inode，最后标记 `node->retired = 1`。通用的 `kernel_vfs_node_release` 仅负责内存节点对象的生命周期，绝不隐式或嵌套调用 `ext4_orphan_free`，避免双重释放与页缓存回收深度嵌套额外消耗任务栈；若底层释放失败，节点转移至 `adapter->cleanup_nodes`，由 mount 保留唯一重试 owner，绝不重新指向 file。若文件在 unlink 时没有现存 node，则使用预留记录直接调用 orphan free；失败后记录留在 mount 链，路径仍保持已删除。
由于 lwext4 的 `ext4_dir_rm` 会递归删除非空目录，`kernel_vfs_rmdir` 通过 `ext4_fdir_unlink_dentry` 只摘目标目录项；后端 `ext4_dir_check_empty` 校验目录记录和 checksum、跳过 `.`/`..` 并拒绝非空目录。被持有的目录 inode 延至最后引用释放才回收；同名重建得到不同 inode，旧路径对象仍可查询零链接状态。目录项 checksum 在设置 inode 字段后计算，卸载后的 `e2fsck -fn` 验证磁盘结构。
只读挂载下，所有上述修改操作直接返回 `-EROFS`。
unmount 在仍有 open file 或路径引用时返回 `-EBUSY`。末节点 `ext4_fclose` 失败把 node 转移到 mount cleanup 链，卸载重试同一个 handle；测试注入路径末引用和重复 inode 合并两种 close 失败并确认都被实际重试。非法引用或释放顺序触发 fatal，合法 heap/page 释放不返回可重试状态。

当前 VFS 同时服务 ELF 随机读、进程文件表和文件私有缺页，但仍不是完整 Linux VFS：已支持硬链接、tmpfs 和多 ext4 挂载；仍没有负目录项缓存与逐分量权限检查；后台写回按阈值和压力触发，顺序预读有独立候选实现，默认关闭。`kernel_vfs_path` 持有 mount/inode 与父目录项引用；`ext4_lookup_child` 按父目录 inode 查找。统一逐分量解析处理 `.`、`..`、相对/绝对符号链接、尾斜线和最多 40 次展开；open/stat 的尾斜线按目录查找，mkdir/unlink/rmdir/symlink 保留不跟随的最终目录项语义。创建允许缺失的最终分量，直接把已持有的父 inode 与名称交给后端。用户输入/符号链接展开限制为 4096 字节；已存在的长父链不占短相对输入额度，255 字节组件可用于修改。路径对象释放不依赖原始绝对路径仍存在。该规则依据固定 Linux commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e` 的 [`fs/namei.c`](../../references/linux/fs/namei.c)。fs context 和目录 fd 直接持有解析起点；绝对路径忽略 dirfd，删除或改名不会把旧引用重定向到同名新 inode。目录支持打开与按后端 cookie 查询（`kernel_vfs_dir_entry`），会返回真实的 `.`/`..` 条目；每次查询从传入的 ext4 字节位置开始，而不是从目录起点重走，顺序枚举的条目访问为 O(N)。适配层把 lwext4 的正 errno 与 EOF 分开，再向文件资源层返回负 Linux errno。页缓存只保存普通文件内容；进程层只持有 VFS mount/file 抽象，lwext4 handle 没有泄露到 task 或 syscall ABI。

目录游标设计依据固定 Linux 快照 `f4cdf7ca9a1f`：[`fs/readdir.c`](../../references/linux/fs/readdir.c)
的 `iterate_dir()` 在每次枚举前后同步 open file 的 `f_pos` 与 `dir_context.pos`，`filldir64()`
把继续位置写入 `d_off`；[`fs/ext4/dir.c`](../../references/linux/fs/ext4/dir.c) 的
`ext4_readdir()`、`ext4_dx_readdir()` 和 `ext4_dir_llseek()` 表明 ext4 cookie 既可能是字节位置，
也可能是目录 hash 编码的位置，并会在 seek 后重建迭代状态。因此 VFS 把它当作底层提供的
不透明恢复值，而不能固化为条目序号。BoarOS 当前线性 ext4 适配器返回记录结束的字节位置；
文件资源层将该值放进 OFD offset 和 `linux_dirent64.d_off`，只有完整 usercopy 后才提交。

`ext4_dir_entry_next_status()` 是当前适配器的错误保留入口：一次调用从 `next_off` 定位并推进
一个或多个物理记录，同一次 inode 读取同时取得实际目录尺寸并发布给适配层，省去每条记录前的重复 stat。inode 为零的目录尾记录不会被误判为 EOF；返回 `UINT64_MAX` 只表示真实结束。
`kernel_vfs_dir_entry()` 对任意 seek 位置向前对齐到 4 字节边界并规范化到下一个记录，因而保存的
cookie 可以交给 `lseek`/`telldir`/`seekdir` 恢复。OFD 持有位置，所以独立 open 的游标独立，dup/fork
共享游标；目录节点本身不保存可变遍历状态。

## 当前成本边界

增长与缩小通过 `kernel_page_cache_resize_prepare/finish` 管理非对齐 EOF 尾页，缩小另由 `kernel_page_cache_truncate` 退休整页。
增长只查询旧 EOF 的非对齐尾页，清零新暴露字节并重新保护别名，再发布长度；
页对齐或同长度不访问 inode 页链，也不排空历史 cleanup。真正缩小仍在撤映射后
处理越界页、尾页脏范围和 owner。该优化不改变私有 COW 副本，哈希探测与尾页
实际别名工作单独计量。`make test-cache-growth-riscv` 在 COST 构建检查
1/4/16/64 MiB × 1/4/64 KiB 写入的增长访问界及全文件内容，见[规模成本](../learning/single-hart-scale.md)。

截断后端修改映射后，VFS 保持同一次 inode 写保护，发布实际 EOF、退休越界缓存页并完成尾页处理；不能先解锁再更新缓存，否则排队的写回仍会按旧 EOF 重建已删除的映射，扩展时读出旧内容。后端部分修改后报错时，也先发布实际尺寸与退休整页，再释放保护重取对应尾页资格；无变化失败保留原脏数据。`sh tests/page-cache-concurrency-host.sh truncate` 用明确握手让旧尾页写回排在后端截断之后，检查截零、部分截断及部分修改后 EIO 的前缀保留、扩展补零、资源归还和 `e2fsck -fn`，覆盖 4/16 KiB 页和 1/4 KiB 文件系统块。

普通文件的当前内容由 VFS 页缓存持有。`ext4_fpread` 和 `ext4_fpread_batch`
先查私有操作、运行组、已封口组和提交中的最新数据版本；命中时从该版本读取。
其余文件数据直接读入调用者提供的页，不分配第二份 lwext4 数据缓存。
未对齐物理扇区的片段使用当前读取独占的临时缓冲，避免睡眠期间复用共享 scratch。
显式使用 lwext4 非日志延迟写接口的独立调用者仍可读到自己的脏 buffer。

`kernel_page_cache_writeback_range()` 从 inode 的脏页偏移树直接取得有序窗口，
不再收集整个脏页集合或排序。只借用当前窗口的页面，I/O 期间保留下一页号，
并发截断可以回收尚未选中的尾页。多页冷读与前台多页写回借用同一种有界堆工作区，
首次需要时分配，每个 cache 最多保留一个归还后的空闲区，后续调用复用它并跨窗口使用。
工作区借出期间，另一调用独立分配，不持有 inode I/O 资格或 loading 页借用去等待它归还；
多余工作区归还时释放，cache 销毁时释放保留区。读取工作区包含页缓存规划及其借给 VFS、lwext4 的同一组描述符；
后两层不再分配或转换数组。lwext4 规划区为 2560 字节，RV 前台联合工作区
最多占两个基页，LA 最多一个基页；实际堆占用按分配器粒度计。
单页使用标量描述符，全热批读不借工作区；后台复用启动时准备的独立工作区，与前台使用同一索引。
每页保存自己的 dirty 范围和
修改代次，稳定版本只复制覆盖本次脏范围的文件系统块。所有缓存都先关闭写借用、重新保护映射并排空
已有写者，之后才取得 inode 读资格并复制；元数据保护不跨越撤权回调、等待或复制。
复制结束就重新开放写入，后端接纳旧版本不能清掉后续新修改。

写回传递最多 32 个 `{offset, buffer, size, owner}` 区间，调用一次
`ext4_fpwrite_batch`，复用一次 inode 引用和一项私有操作。页缓存、VFS、后端和 lwext4 使用同一份 `file_write_span` 数组；后端直接借用描述符，不再分配转换数组。区间可有干净间隙，
文件页和内存都不必相邻；按索引顺序在 128 KiB 窗口内形成一批，块层再按实际磁盘
位置组织请求。快照范围按挂载的文件系统块大小向两端对齐，逻辑脏范围单独保留；
文件末尾只复制有效字节，其余覆盖范围补零。
逻辑长度已由缓存写扩展、后端尚未接纳时，后端读取在同一次读锁内固定已接纳 EOF；
仅补齐该 EOF 到逻辑长度之间的已知零洞，已接纳范围内的短读仍返回错误。完整文件系统块借用同一稳定页，ordered DMA 完成并越过
持久化屏障后才归还引用；不再经过普通数据 bcache、数据 undo 和封口 after 副本。
逻辑部分块写也借用完整块覆盖，无需在 lwext4 再分配数据镜像、读取旧块并合并；
只按逻辑脏字节更新写入完成量和文件长度。普通 lwext4 调用者未提供完整稳定覆盖时，
才组装事务私有版本；文件系统块大于缓存页时沿用此路径。失败丢弃当前事务版本，早先接纳的版本不变。
页缓存清脏表示后端已接纳，durable 仍由同步接口等待。
无 journal 的挂载同样把稳定完整块组成最多 128 KiB 的同步向量写；设备成功后才
更新逻辑完成量和 inode 长度。失败批次释放新分配映射，早先成功批次保留真实前缀；
已有显式 bcache 借用在成功后更新为相同内容，普通文件不因此创建第二份数据缓存。

提交失败后，挂载仍持有已接纳的当前数据版本，供尚存的读取者使用；DMA 已结束只
代表设备借用结束，不代表这些文件内容可以释放。新写入与同步继续返回日志错误，
未修改的文件冷读仍可正常装入，不能因为一次写入失败就禁止执行代码页的读取。

`BOAROS_PAGE_CACHE_WRITEBACK_PAGES` 允许从 1 到架构页大小对应的 128 KiB 上限，
默认采用该上限（4 KiB 页为 32，16 KiB 页为 8）。
前后台从同一空闲版本池取得稳定存储；只有最后一个实际读者、日志或 DMA 借用结束后，
版本对象及物理页才一起归池。每 cache 最多保留 256 KiB 空闲数据页，超过上限即释放；
worker 启动时准备一个实际回写窗口，内存压力先释放窗口以上的空闲版本，再回收文件缓存。
空闲版本不足时正常分配，运行组大小不受单窗口限制。后台分配耗尽时先提交已准备的前缀、
结束其页面写回资格，之后才能等待实际版本归还；不持未提交版本或页面资格等待，避免与前台互等。
停止 worker 唤醒版本等待并结束预留。池单独引用计数，不保存 cache 指针；cache 销毁时关闭池并释放
空闲项，仍在使用的版本晚归还只访问尚存的池，最后一个 owner 释放它。一个来源被多个文件系统块或事务借用时，共享同一个来源 owner；
日志只取得一份外部引用并计一次整页与控制对象容量，最后一个块归还后才释放。
这避免 16 KiB 页只脏一个 4 KiB 块时少计内存，也避免完整页拆块后重复计费。
控制工作区只保存本批描述符，版本页继续由真实借用者持有，归还工作区不缩短版本生命周期。
设备错误不会改成拆批重试。块层把磁盘相接、内存不相接的区间
组成一条 SG 链；不相接的请求一起发布后只通知队列一次，详见
[VirtIO 块设备](riscv-virtio-block.md#请求和-dma)。

`python3 -B tests/writeback-batch-riscv.py` 检查 1/2/4/8/32 页与 1/4 KiB ext4，
覆盖部分首尾、范围裁剪、干净间隙、无需连续大块分配、OOM、并发重脏、旧版本稳定、
短写/EIO 脏页保留、读回及 e2fsck。`make test-lwext4-group-host` 另外检查外部稳定页
在接纳、替换、abort 和 commit 时的实际引用归还，以及冷读没有第二份文件数据缓存。
cache 宿主测试另检查版本对象/物理页零分配复用、保留旧内容、空闲容量和压力回收、
前后台争用最后版本时的 OOM 交错，以及 cache 销毁后最后来源引用归还。
元数据日志的普通 payload 与 checkpoint 共享同一 after 版本，只有 descriptor、revoke、
commit 分配日志镜像。日志空间按 payload 块数、按 tag 容量装箱的 descriptor 块数、
按 32/64 位记录容量装箱的 revoke 块数和一个 commit 块预留，控制镜像与 payload credit 分别计数。
私有操作预留自己的新增 payload，并只补合并后控制块数量的增量；running 继续持有已有预留。
只读访问或撤销 revoke 留下的多余控制镜像暂留到 accept，合并去重后按最终记录数裁剪；
abort 只归还私有预留，不动已接纳组。磁盘空间与实际控制镜像内存分别计量。escape 格式仅复制
需改 magic 的第一个物理扇区，余下数据借用原版本，不临时修改共享输入。
这些检查说明内容、生命周期和请求形态；应用吞吐与延迟需要在对应负载上测量。

2026-10-16 的固定工作量对照使用同一份 `data-path` ELF，比较通用源码 `4ea3a3e`、
`5155e6f` 与试验提交 `d96ba78`。该提交去除一次性 uaccess 的 cursor 包装，随后因实际性能下降撤回。
最终内核继续使用 `5155e6f` 的复制实现。普通文件读取按已取得的缓存批次
持有 cursor，不按单页请求增加模式分支。QEMU 11.1.2、512 MiB、单 hart、modern VirtIO、
writeback，关闭诊断和预读；父进程与 QEMU 线程允许运行在宿主 CPU0/2。
每个客户机磁盘先完整写入独立、非稀疏文件并同步，准备不计时。冷读为新启动后的客户机
首次读取，宿主缓存未清空；热读先完整读取文件。复用 `tests/io-budget-experiment.py` 的
内容、完成条件及资源退出检查，轮换全部六种版本顺序，一次只运行一个客户机，
计时期间没有构建或其他测试。162 次启动全部通过；两种 4 KiB 热读窗口每版十二次，
其余每版六次。下表为中位吞吐，单位 MiB/s。默认单 hart 与 `USER_SMP` 本地执行的区别见
[RV 两核](riscv-smp.md#普通文件io的执行位置)。

| 负载与完成条件 | `4ea3a3e` | `5155e6f` | `d96ba78`（已撤回） |
|---|---:|---:|---:|
| 16 MiB 热读，4 KiB 请求 | 126.97 | 121.32 | 125.01 |
| 64 MiB 热读，4 KiB 请求 | 126.11 | 128.54 | 129.23 |
| 16 MiB 热读，1 KiB 请求 | 67.77 | 64.21 | 65.61 |
| 64 MiB 追加，4 KiB 请求，包含最终 fsync | 45.45 | 48.84 | 49.07 |
| 64 MiB 追加，64 KiB 请求，包含最终 fsync | 54.38 | 54.35 | 51.82 |
| 16 MiB 冷读，4 KiB 请求 | 22.33 | 22.69 | 22.22 |
| 16 MiB 冷读，64 KiB 请求 | 70.56 | 87.51 | 93.63 |

试验版 4 KiB 热读相对 `5155e6f` 的两组中位数分别高约 3.0% 和 0.5%；按对应轮次先算比值再取中位数，
分别为 1.039 和 1.022。这不是可推广的同幅加速：历史版与当前版的差值也随窗口和轮次改变。
另外两轮对照将完全相同的当前内核标为 A/B，各自的 16/64 MiB 热读每组十二次；
B/A 的逐轮比值中位数分别为 0.999/0.988 与 1.007/1.018。
原先 1%–3% 的下降没有稳定复现，不能全部归因于 cursor。
尝试为单页选择一次性复制时，同一实验 ELF、相同用户缓冲区地址，只改变计时前的预热
请求大小就会改变数个百分点的结果；按 4 KiB 预热没有消除差距，故不能把该分支的负差
解释为首次执行一次性复制的翻译成本。
正式表格仍使用原 ELF，实验开关未进入工作负载。

保留的 `5155e6f` 路径在 64 KiB 冷读中相对历史版中位数高约 24.0%，实际请求形态由需求读取测试检查。
追加在最终 fsync 返回后结束。表中 64 KiB 追加的负差主要出现在同步等待，不能省略：
随后单独交错运行三组各十二次，其中 A/B 使用相同的当前内核，吞吐中位数为
52.06/51.56，试验版为 52.87 MiB/s；逐轮试验版/A 中位数为 1.003，B/A 为 0.989。
前一组约 5% 的下降没有重复，先前历史对照中约 8%–10% 的追加收益本轮也没有稳定重现。
本次试验未改变写回、事务或设备请求组织，不把同步等待的波动记为 uaccess 加速。

源码与确定性指令对照回答另一类问题：缓存热命中没有新增查询、锁或物理页引用，
本组普通 MM 也没有新增 operation gate。原 ELF 的用户缓冲区为 `0x22618`，
一次 4 KiB 读取跨两个用户页，两个 pin 都是必要的。保持其他当前实现不变、只恢复旧的
逐来源复制策略，每次这样的热 `pread` 少 22 条客体指令，约为完整文件 API 的 0.17%；
这是 cursor 的执行工作差异，不能直接换算为吞吐百分比；它没有增加查询或 pin。

同一完整 `kernel_files_pread()` probe 每种布局执行 32 次，检查内容、偏移、精确 pin 数、
最终归还及零设备读取。QEMU 使用 `-icount shift=0,align=off,sleep=off` 计数，
每次启动以 1000/2000/1000 次 NOP 循环得到 3001/6001/3001，验证计数含义。
下表为每次 API 的平均客体指令数；该设置只检查执行工作，不用于吞吐表。

| 4 KiB 目标布局 | 完整 `4ea3a3e` | `5155e6f` | `d96ba78`（已撤回） | pin/次 |
|---|---:|---:|---:|---:|
| 页对齐 | 8587.06 | 8507.06 | 8502.06 | 1 |
| 页内偏移 `0x618` | 12735.06 | 12645.06 | 12642.06 | 2 |

当前完整热读已比历史版少执行指令，不能将吞吐负差说成同幅新增遍历或 pin 工作。
试验版公开的一次性复制在对齐/跨页 4 KiB 下分别少 32/38 条指令，但普通 TCG 的实际执行更慢。
长时间复制实验每组先预热 64 次，再计时 32768 次，用户目标固定偏移 `0x618`，只用两次
`rdtime` 读经过时间，禁用 icount。每个版本六次交错启动，所有内容、精确 pin 数、最终归还
及 MM 页数恢复均通过。16 B/1 KiB/4 KiB 的一次性复制配对耗时中位数分别增加
6.65%/6.59%/6.72%，每次新建 cursor 并 finish 的路径分别增加 7.55%/8.12%/7.23%；
各窗口为 137–342 ms，全部配对方向一致。更少的客体指令没有转化为更低的运行成本。

另一项隔离保持原 cursor 核心及其入口不变，只恢复明确的旧逐页一次性复制函数，
一次性复制仍慢约 1.6%–2.9%，原 cursor 路径也慢约 2.2%–3.1%；MM records 和 page_pool
地址相同。这个结果不能归因于 cursor 新增检查，也不足以确定某个宿主缓存或 TCG 机制。
新增复制主体及其编译布局没有带来有效收益，故撤回这项生产代码修改，保留原有共享复制核心。
RV 原 iozone 的 glibc/musl 三轮分数中位数也分别下降约 1.9%/1.7%；详细数据见
兼容分支 `docs/learning/oscomp-compat-baseline.md`；主线仅保留实验目录引用。
文件读取保持批次内 pin 复用，没有新增单页模式分支。

运行记录保存在 `build/hot-copy-20261016/{final,append-aa,compare,compare-v2,warm-compare,original}`。
指令对照为 `fs-final-summary.json`、`fs-legacy-policy-summary.json`，实际复制时间为
`wall-summary.json`、`wall-legacypublic-summary.json` 及同目录对应日志。

减少的工作由相关测试另行检查：传统单间接 32 块安装只借用一次叶，双重与三重间接同样每叶一次；
冷页准备的 cache guard 次数单页从 5 降为 4，八页从 21 降为 6。前台跨层读取稳态不分配描述符，
单页用标量区，空闲稳定版本复用时对象与物理页分配均为零；最后借用者归还前不会重用内容。
RV 前台联合区为 4352 字节，实际占 8 KiB；LA 为 3008 字节，实际占 4 KiB slab slot。
每 cache 最多保留一个空闲前台区，竞争者借独立区，归还多余区即释放。
这些结构变化不能直接换算成应用吞吐收益。

以下原 iozone 诊断在本轮源码改动前完成，说明测量条件和较早 Parent read 差异的原因。
原 iozone 3.506 的四进程 read 使用 `-t 4 -i 0 -i 1 -r 1k -s 1m`。
默认 stonewall 在首个进程完成时停止其余进程，实际传输量不固定；
`Parent = 各子进程计入的 KiB 之和 / 父进程区间`，该区间还包括读循环之后的
四次 `fsync`、关闭与退出回收。`Children` 是各子进程独立速率之和，分母不同，
不能当作同一时间区间的聚合吞吐。`Min xfer` 使用零作为初始化哨兵，可能不显示
某个零进度子进程；只有完整 `-C` 行才能重建总计入字节，缺失时保持未知。
比赛原 judge 使用每进程最大速率，未采用 Parent；不能用某一条 Parent 替代整组成绩。

2026-10-15 对兼容分支修改前的 `3189b00` 与修改后的 `ff9ceea` 原内核及原盘
作六次独立启动，顺序为前、后、后、前、前、后；每次完整执行 glibc/musl 原脚本。
两侧均为 QEMU 11.1.2、1 GiB、单 CPU、legacy MMIO，诊断关闭，
计时期间不构建或运行其他测试。RV musl 四进程 Parent read 的中位数分别为
34594.75 与 38629.35 KiB/s，范围为 25425.75–38340.37 与 25560.85–45571.46；
Children 中位数为 80955.28 与 124837.63 KiB/s。此前单次 Parent 下降没有形成
稳定的版本差异，原有单次结果仍保留在兼容分支记录中。

随后仍使用原 iozone ELF，对 `-C` 与 `-C -x` 各做每版本六次对照；
`-x` 仅关闭提前停止，每次确实计入四份 1 MiB，不以另写的固定字节数程序替代。
下表是中位值，时间由完整计入量与打印的 Parent 速率估算，受打印精度影响：

| 原程序模式 | 修改前计入 KiB / Parent ms | 修改后计入 KiB / Parent ms | 修改前 / 后 Parent KiB/s |
|---|---:|---:|---:|
| `-C`，提前停止 | 2983 / 89.48 | 3375.5 / 86.32 | 34280.77 / 38107.15 |
| `-C -x`，全部完成 | 4096 / 104.72 | 4096 / 94.85 | 39113.58 / 43238.03 |

各列分别取中位数，不能直接用表中两个中位数相除重建另一个中位数。
关闭提前停止后 Parent 仍会波动，说明差异不能只归给计入字节。
读取边界记录将较慢运行定位到循环结束后的 `fsync`，关闭与退出回收只占较小部分。
完整成本诊断会改变 100 ms 时间戳写回交错，未用它的耗时替代上述正式结果。

进一步用关闭成本诊断、仅记录提交边界的内核对照原实现、当前实现及恢复旧收割行为
的实验版本，三者都出现较晚轮次的等待增长。每次 reader 提交仍是 4 个设备请求、
2 次 flush、4 次 park，等待区间没有插入 checkpoint；恢复旧 MMIO 收割行为未消除增长。
两版本第二轮到第三轮完整读取，四次 commit 的 park 外总时间变化不到 0.1 ms，
宿主八次 `fdatasync` 的总时间却分别增加约 41.2、42.0 ms，与 guest park 的增长相符。

这项增长来自稀疏 raw backing 的首次分配，不能仅称为随机波动。
原盘 guest journal inode 8 已分配连续 64 MiB，但该范围在宿主仅前 558 个 4 KiB 块
有实际 backing，之后仍是 hole。旧版本第三轮提前停止读的 commit 位于日志块
486–495，尚未进入 hole；新版本对应 commit 已到 594–603，旧版下一次完整读取亦到
572–581。commit record 之后的宿主同步在已分配区约 2.6 ms，进入原 hole 后约 8 ms；
同阶段 checkpoint 覆盖既有 home 块，其两次同步仍合计约 5 ms。

为验证这个地址关系，保持镜像字节与原 iozone ELF 相同，仅将 raw 镜像改为独立、
非稀疏的写实副本并在 boot 前同步。两版本各一启动、每启动仍执行六次原 `-C` / `-C -x`
调用。第三轮完整读取的八次宿主同步总时间从 70.9 / 72.6 ms 回到 29.9 / 22.4 ms，
对应 Parent 从 29271.98 / 29036.88 回到 38884.33 / 44173.18 KiB/s。
这些带边界记录的结果用于归因，不与前面稀疏镜像下的正式多轮吞吐合并。
未修改内核读写通路、SG 合批或持久化顺序，也不为宿主分配成本添加 guest 轮询。

持久化性能比较使用 raw 镜像时，准备期须完成 backing 的实际写入与同步，再启动 guest；
只增加文件长度或 `fallocate` 留下的 unwritten extent 仍可能把初始化转换带入后续 flush。
本地兼容分支运行器采用 `cp --reflink=never --sparse=never` 后 `fsync`，原盘内容和
guest 测试不变。每份 4 GiB 工作镜像因此实际占用约 4 GiB，空间与准备时间计在运行前。
结果解析由 `tests/cost_consumer.py` 保留原值、完整子进程传输量与可推导区间，
`tests/test-cost-report.py` 覆盖提前停止、完整读取、零进度、缺失明细和打印舍入。


普通单 hart 路径用一个完整片段覆盖冷缓存页时，先取得输入访问资格，再初始化最终页；页面完成
初始化、标脏和长度更新前保持 loading，不先读取将被完全替换的旧页。部分页保留
未修改内容，用户 fault 只接纳已经复制的前缀。元数据定位仍可能读取设备；
普通 lwext4 部分写没有完整稳定块来源时也需要读取未修改内容。
小范围同步测试用 64 MiB 文件的 16384 个实际常驻页：单页范围 1 次候选访问，
3 个脏页全范围同步 3 次访问，16 个脏页中选一页仍 1 次；
1 字节脏范围复制覆盖它的文件系统块，后端接纳量仍为 1 字节。
规模准备采用稀疏零页，另独立验证第 0 页已存在且已同步的真实旧内容被完整覆盖。
重建 `make test-scale-riscv test-cache-growth-riscv`；这是成本界验证，吞吐结果另行测量。

`kernel_vfs_sync_range()` 分批交接数据，捕获 full 同步序号后由调用者等待；
`kernel_vfs_sync()` 按 datasync 选择 full/data，journal 提交线程负责屏障。
元数据随修改提交，频繁小写与逐次同步的事务/flush 放大尚需单独计量。

| 串行边界 | 当前必要契约与限制 |
|---|---|
| OFD offset | dup/fork 共享位置的操作互斥；独立 open 不共享同一 OFD 锁 |
| 命名空间 | 路径解析/修改按相应挂载锁保护；部分修改适配仍需遍历祖先构造完整路径，无负目录项缓存 |
| inode | 内容与 truncate/失效/孤儿回收相互保护；同页 miss 合并，不同页可各自等待 |
| ext4 实例 | 纯定位读共享进入，写事务及维护操作独占；不同盘有独立锁与错误 owner |
| 块队列 | 每设备最多八个在途槽；一次同步调用等待自己的请求，不能由八槽推断单次写回会填满队列；flush 排空此前请求并挡住后继 |

每盘后台 worker 从稳定版本池取得快照，按阈值和压力触发；不是周期刷盘，也不代替
fsync 的持久化与错误观察。全局内存快照 O(总缓存项数)、压力通知 O(实例数)。
等待期间 CPU、无关缓存及另一磁盘均有进展。后续工作见[路线](../goals.md)。

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

orphan 宿主测试另外检查 258 条混合记录、热查询零分配和零设备读取、已持 inode 引用的回滚、实际槽位损坏及分组接纳后另一私有事务 abort。
恢复测试使用 `tests/host/block_fault.c` 的易失缓存与稳定镜像，逐个写入/flush 边界丢失未同步写，并另测最后一个 512 字节扇区先落盘。journal、ordered data、公共事务、持久 orphan 记录及实际回收分别测试；1 KiB/4 KiB、extent/legacy、orphan_file/传统链覆盖两次重启、分配和链接计数、空间回收及 `e2fsck -fn`。公共事务还逐个注入内存分配失败，验证命名状态完整回滚。此承诺限于该块模型；QEMU 正常退出和实板行为不能代替断电证据。 NBD 断电切点冻结磁盘执行和回复，收到切点后停止 guest，不以先断连接后的 I/O 错误代替同时掉电。异步批次的事件数可变，执行器分别记录实际序号切断和提交后控制切断；未到达的故障序号不计为已注入。成功提交后的控制切断必须恢复新内容，每例仍检查两次恢复和 fsck。普通数据原地覆盖不承诺整文件写入原子性，成功同步保证已提交字节持久；真实 musl 已覆盖临时文件写入→文件 fsync→跨目录 rename→两侧目录 fsync，rename 后端另用同一故障模型验证断电原子性。

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

本阶段历史验证记录曾位于 `build/namespace-host-final.log`（32 组、620 次断电/重排、3004 个分配失败点）与 `build/namespace-final-regression.log`（RISC-V 全套、真实 musl/pthread、297 条 Linux 差分、988 个函数栈界；最大单函数 1952 字节）；日志已按本地构建清理策略删除。

## 显式元数据与统计

`kernel_vfs_file_set_times/path_set_times` 以活 inode 为目标，`ext4_file_set_times` 在单个 journal 事务内修改选定字段并记录同步依赖；成功发布仅覆盖所选运行时字段，OMIT 保留尚未写回的值。两项 OMIT 不产生 I/O；其他修改要求可用 realtime，时间未初始化明确返回 `EIO`。128 字节 inode 仅秒精度；扩展 inode 保存纳秒，超出或等于时间范围端点时按 Linux `timestamp_truncate` 将纳秒置零。事务准备 ENOMEM 可回滚，关键 journal/write/flush 失败由 mount 保留并停止修改。

`kernel_vfs_mount_statfs` 使用 `ext4_mount_point_stats` 的 superblock 分配计数、UUID 和真实 overhead。首次成功统计校验块组及 journal inode，计算首数据块、super/GDT/预留 GDT、两种 bitmap、inode table 和 journal 长度，缓存静态开销，成本 O(块组数)；后续查询 O(1) 读取动态计数。当前无在线 resize，故无几何缓存失效协议；BIGALLOC 与外部 journal 不在支持范围。META_BG 主描述符取所在 meta group 的首组，sparse_super2 按备份组字段处理，组数扣除 first_data_block。

`make test-lwext4-metadata-host` 覆盖 1/4 KiB 块、128/256 字节 inode、60 个 OOM 点、写与 flush 失败的 sticky owner、可重试读失败、损坏拒绝、无关脏数据不写回、稀疏/截断/删除计数、META_BG/sparse_super2/GDT_CSUM/无 journal 和精确组边界，并运行 `e2fsck -fn`。用户层契约与固定资料见[文件模块](kernel-files.md)和[时间学习记录](../learning/file-timestamps.md)。

`make test-lwext4-extent-host` 单独保护冷缓存 extent 树读取的失败契约：
块号在缓存分配前填写，不能作为已取得引用的证明；`ext4_trans_block_get()`
失败时调用者直接传播错误，只有成功取得的块才能在格式检查失败后归还。
测试在 1/4 KiB 块、128/256 字节 inode 下枚举实际路径的所有分配失败点，
另注入设备读错误，验证同一 inode owner 可重试、文件内容保持、正常卸载、
堆对象清零及 `e2fsck -fn`。非法释放仍触发原有 fatal 契约。

元数据收口历史验证：当时 `build/recoverable-fs-host-final.log` 的全部 host/断电矩阵通过；`build/recoverable-fs-regression-final.log` 的 RISC-V 全套、真实 musl/pthread、320 条固定 Linux 差分、1000 个函数栈界及工具自测通过。日志已清理；最大单函数当时为 kernel_main 的 1952 字节，assembly trap 288、保留 1024。这不是实板性能或 SMP 验证。

## 文件空间预留与打洞

`kernel_vfs_fallocate()` 将范围操作交给后端，ext4 支持 mode 0、`KEEP_SIZE` 和
`PUNCH_HOLE | KEEP_SIZE`。预留通过真实 unwritten extent 占用块并维护位图、inode
扇区数和空闲计数；读取这些块合成零，首次写入在同一事务内分割、转换相应范围。
只有逻辑、物理相邻且初始化状态一致的 extent 才能合并。unwritten 转换的零数据使用
ordered data 版本，允许调用者随后在同一事务内用稳定写入版本替换；不能把这些文件
数据块登记为 journal 元数据。磁盘满而 extent 分割需要新的索引块时，转换改为初始化
当前 unwritten extent：用一个文件系统块的缓冲逐块清零并 flush，再发布初始化状态，
请求内的新数据仍使用原 ordered data 版本。这样写入已预留块不再额外消耗磁盘空间，
也不需要为整个 extent 保留零数据镜像；I/O 失败保留 unwritten 状态并返回实际错误。
完整打洞块释放映射，两个部分块仅清零请求内的字节，
不为洞或 unwritten 边界分配或初始化块。未实现该能力的后端与其他模式返回
`EOPNOTSUPP`，超出 maxbytes 返回 `EFBIG`。

写操作互斥覆盖整个操作。通用层先关闭 mmap 发布代次，打洞时撤销相交页的文件别名、
保留私有 COW，然后写回脏页，使后端看见缓冲写已经发布的 EOF，并保存边界页的范围外
字节。后端结果、实际 EOF 和相交缓存页退休在同一 inode 写锁内发布，再开放新缺页。
等待 inode 读锁的旧写回候选在捕获前看到缓存页退休，不能重建已打掉的映射。日志事务
失败恢复本次位图、extent、部分块数据与 EOF；无日志后端已完成的部分变化仍按实际
结果更新缓存，错误不改成成功。相同 EOF 的截断也回收 EOF 外预留；orphan 索引的
运行时截断意图参加事务 undo，区分真正待回收的缩短映射与仍打开的无链接文件。
后者保留合法预留，最后 owner 释放时才完整回收；重启恢复仍按持久 EOF 检查。
即时提交和组提交的 journal 都保留释放块的 quarantine，直到 abort 或 checkpoint
归还；即时提交路径的新操作在低空间时先完成旧 checkpoint。外层事务仍拥有的旧块
不能提前分给新的 unwritten 范围，否则直接清零会破坏回滚后的原内容。
此操作不承诺无日志文件系统的断电原子性。

`sh tests/fallocate-host.sh` 使用 1/4 KiB ext4 块与 4/16 KiB 页检查预留及空闲计数、
混合 extent、部分块、缓存重读、真实共享 mmap/私有 COW、握手控制的活动写回、范围
中途失败回滚、无 journal 的部分失败和所有者归还；每个镜像卸载后运行 `e2fsck -fn`。
同一入口的 `FALLOCATE_HOST_CASE=reserved-full` 检查磁盘满时的已预留写入；
`reserved-middle` 检查分割到一半后的空间失败，`reserved-large` 用 512 KiB 组预算
转换 8 MiB 的 unwritten extent，`reserved-zero-error` 检查清零 I/O 失败和重试，
`reserved-abort-reuse` 检查外层事务的原内容归还。`reserved-large` 和
`reserved-abort-reuse` 需设置 `FALLOCATE_HOST_JOURNALS=journal`；除 large 外的磁盘满
场景可用 `FALLOCATE_HOST_IMAGE_SIZE=8M` 限定填盘和清理工作量，large 保留默认 32 MiB。

## 可睡眠存储与跨 CPU 读取

锁序为 OFD offset（10）→ inode 写操作门闩（15）→ 命名空间（20）→ 按稳定 node 身份排序的 inode → 后端读写锁。VFS 缓存命中不需取得后端锁；纯定位读 `ext4_fpread` 使用独立游标和共享后端锁，写入、事务提交/回滚、孤儿回收、恢复、journal start/stop 和卸载持独占锁。事务只允许同一任务 owner 嵌套；写者排队后不再放入新读者。RELATIME 先在读阶段判断，确需更新时间才在释放读锁后进入独占阶段，不允许读锁升级。

日志请求、停止标志、完成进度和同步等待索引由挂载级任务短锁保护。
后端最外层写锁释放前发布 sealed/durable/checkpoint 及粘性错误，等待者
读取该稳定快照并在同一保护内登记；释放短锁后休眠，提前完成由原等待
token 保留。实际事务仍由后端锁保护，日志空间等待先归还后端锁，恢复后
重新检查。同步请求从索引摘除后在锁外完成，通知后结束访问调用者的栈。
VFS 的错误发布和共享 OFD 错误游标消费另由实例任务短锁保护，覆盖来自
不同 CPU 的立即完成与日志完成；每个打开实例继续观察自己的错误代次。

同页 miss 先发布 loading 项，等待者合并到该项；不同页可各自等待设备。失败保留所属 errno，ENOMEM 不映射成磁盘错误。缓存索引、LRU 与借用由同一 metadata mutex 保护，跨等待时持有实际 entry、版本或 alias 引用；块缓存的 loading 与 wait/wake 同样合并同块读取。后端表的 read_execution 决定冷装入的位置：ext4 为 CALLER，其他磁盘后端默认 CPU0；调用者的数据复制使用共同路径。`file_metadata_execution` 独立决定已打开文件的空间准备、完成、截断、预分配和同步准备：ext4 为 CALLER。`namespace_execution` 决定路径查找、目录操作和路径元数据的位置，ext4 为 CALLER，其他后端为 CPU0。同一挂载的 namespace mutex 仍串行修改目录及事务；普通 ext4 文件、目录和 O_PATH 的最后关闭可由发起者完成。涉及设备、epoll、记录锁或生成内容的关闭按其消费者处理。日志、后台数据写回和挂载管理由 CPU0 推进。

写回固定本批页快照、脏范围与 generation；等待期间映射的新修改属于下一代，旧完成不清除它。范围写回只借用当前窗口的精确页集合，归还时使用同一集合；跨窗口只保留页号与范围，续批重查当前脏页索引，不长期借用链表后继。truncate/失效/最后 orphan close 取得 inode 写锁并复查 owner，排除 loading/writeback；最后 close 不在活动写回上强行失效。

每 inode 的写操作门闩覆盖 write/writev/pwrite、append 的数据接纳过程以及 truncate/fallocate；独立 OFD 也不能在页/iovec 边界交错同次写。数据接纳后先归还写操作门，再执行 O_SYNC/O_DSYNC 同步；位置资格和独立同步的保护范围见[文件接口](kernel-files.md)。内层 pwrite/append 借用外层门闩，不递归加锁。来源缺页与 pin 在目标 inode 数据锁和页写资格之外完成，实际复制及脏范围发布在取得目标资格后进行；缺页、读和写回不取得操作门闩，允许用户源缓冲映射同一 inode。普通文件写通过用户 iovec 直接填充页缓存或 tmpfs 后备页，没有请求中转页，保留短写及 EFAULT/EIO；这里的操作互斥不承诺断电原子性。持锁分配、堆和 MM 元数据操作只回收干净页，可按正向顺序等待元数据资格，不能等待脏页或设备 I/O；脏页回收在外层取得 inode try-read 后执行，完成后复查页引用与别名。干净页的物理引用归还后，node 与 entry 的释放在可进入后端的现场完成；固定版本 `ext4_fclose` 只清理私有 handle，不取后端锁或执行 I/O。只有未完成 orphan 或真实关闭错误交给 mount 清理链，不让健康节点滞留至卸载。

验证入口：`make test-io-sleep-riscv test-files-riscv test-files-partial-write-riscv test-lwext4-recovery-host`。设备握手、同页合并、快照、并发插入缓存页、最后 close/写回及完整恢复的证据见[可睡眠存储](../learning/sleepable-storage.md)。

`kernel_vfs_mknod_at` 在 namespace 锁内定位未存在的末级名字，普通文件、字符/块设备及FIFO
创建与 mode 设置放在同一个 lwext4 写事务内。失败回滚由原 mount/日志 owner 处理；
该入口不实现 devfs，不增加新的设备后端。只读根返回 EROFS，创建不占用进程 fd。

## 后台写回与水位回收

普通与受控 worker 都从脏 inode 队列选择写回：首个脏页把 inode 加入队尾，
最后一个脏页清除或退休时摘除。队列只借用脏页已经持有的 inode，不新增引用。
每轮以开始时的脏页数作为检查预算，按 inode 的脏页红黑树选择同一文件的有界批次。
缓存 record 保存当前脏 inode、下一页号与剩余预算，从不小于页号游标的首项继续沿树
向后选择；每批最多检查 64 个脏项后让出 CPU。干净页和空哈希槽不参与脏页选择，
繁忙或失败项也会推进游标。inode 摘除时同步推进队列游标并将页号归零。批次在离开
索引保护前借用缓存项，按已有页号顺序提交，完成后归还；期间新增或重新标脏的数据
通过实际发布事件请求后续轮次。普通实例的干净页压力回收仍独立检查缓存引用与别名。
受控 worker 为冷读让路时保留当前游标和剩余预算，恢复后继续选择。

所有缓存共用 metadata mutex、entry 借用、alias 排空和稳定版本协议，普通与受控构造不再切换保护算法。heap/allocator 绑定仍需一致；普通系统压力挂钩可以保留，回调与卸载使用实际组引用。
其索引由 rank45 mutex 保护，候选分配、用户复制、后端 I/O、回调与实际资源归还在保护外进行。
普通写的 entry 借用直接覆盖目标数据页寿命，不再重复取得物理页引用。写入结束在一次缓存保护内标脏、结束写借用并归还 entry；已有文件写操作门固定逻辑尺寸，准备用户来源前不再加一次 inode 锁。索引容量在首次查询时判断，只在实际不足时整理或扩容。一次写入按实际接纳的范围发布一次脏工作，不逐页重复查询全组压力；部分成功后出错同样通知，分配期间的真实低水位通知仍然保留。
回写关闭目标写入时同时固定 alias 游标；若关闭前没有实际写入者，回调后无需再取得缓存锁检查空等待条件。后端批次结束后，一次保护发布本批完成状态和 entry 借用归还，再在保护外释放稳定版本。每个版本仍按真实引用覆盖 journal 与 DMA 使用。
ext4 冷读由发起任务取得 inode 读资格，在缓存保护内复查并发布 loading，随后就地解析映射、装入和提交块请求。CPU1 与 CPU0 中断共享设备状态保护，设备完成通知实际等待者。候选分配前沿既有请求借用计数固定 cache，发布、等待和工作区归还后才结束借用，销毁可识别尚未进入索引的读取。计数在缓存接纳保护内增加，末次归还使用 release 原子减法，此后不再访问缓存记录；销毁停止接纳并排空请求后以 acquire 观察零。归还计数无需再次获取缓存 mutex，entry 的完成发布和引用归还仍由原缓存保护负责。全热路径仍只取得一次缓存保护。其他后端按 CPU0 策略借用原等待现场与文件 owner，排队阶段保持候选私有。ext4 的空间准备和同步准备直接使用原任务的事务现场；需要先刷脏数据时，向原写回队列借用实际 offset/写操作 guard 链，原任务等待全部窗口结束。
前景范围写回复用同一队列：一次调用只建立一个栈上 `cache_request`，每次最多推进
128 KiB 窗口，尚未完成就把原请求接回队尾。调用者等待整个请求完成，不逐批醒来重新
提交；工作区在开始前借用一次，跨窗口复用，结束后归还。最后关闭、其他 inode 的同步准备及采用 CPU0 策略的后端读取可排在续批前；ext4 调用者冷读独立推进。窗口内继续复用既有 SG 与稳定数据版本。
不占执行队列的直接调用者只在批末开放中断安全点，由正常定时器抢占，
不逐窗口强制轮转；尚未启动调度器的启动调用继续同步推进。队列 worker 在请求或
清理批次之间让出。
worker 停止后不再执行未开始的续批，唤醒请求并返回错误；已经接纳的版本不回滚，
尚未提交或失败的数据保留原脏状态和 owner。退休清理每轮最多释放 16 项后让出 CPU，
未完成清理的链由 worker 持有到后续轮次；仍有借用的项归回缓存，等待实际归还事件。
普通标量取页与批量取页都先在缓存索引保护内检查 inode 映射是否 closing、页是否 CLOSED，
并按当前 EOF 确定有效长度；已完成的热页在同一保护内取得物理引用，无需先取得 inode 读资格或排队。
不满足资格时转入既有 inode 读保护路径；loading 页仍由 entry 借用跨等待持有。
MM 取得来源页前后仍核对映射 epoch，最终提交 PTE 前重新取得 inode 资格，复查来源 epoch、VMA 身份和 EOF。
普通及受控任务共用后端执行策略，ext4 在发起 CPU 完成冷页装入，普通 read/readv/pread 在调用 hart 消费结果并复制用户数据；共享offset在调用者取得，需要位置或写操作门的后端回调只借用已有资格。独立fsync不取得offset锁，分批交接与prepare之后由原调用者等待固定目标，完成事件同时处理错误观察。SMP 运行时时间戳的共享值、dirty/staged链及deadline由一把挂载raw锁维护；单 hart 构建的这些短更新不阻塞，只需 IRQ 保护，不维护跨核锁。前台时间编码使用调用者的后端写资格，journal 提交仍归 CPU0；可选预读队列亦由缓存保护，取消后的节点由CPU0 worker锁外释放。预读复用普通候选分配和索引发布，取得 inode 读资格后先检查映射关闭状态；截断准备至 MM 通知期间取消预测，不能按旧 EOF 装入边界页。
worker 停止拒绝新排队请求并结束实际在途窗口；CALLER 读取由自己的缓存、inode 和文件引用覆盖，继续完成。cache 销毁检查这些借用及页引用。
`make test-page-cache-concurrency-host` 链接真实 VFS/lwext4 和镜像块读写，在4/16 KiB、
2/4宿主线程下检查冷读合并、64页扩容/预热命中、读失败、停止交接、1/8/32别名、
旧写入借用、快照后新代次、真实前缀短写/EIO、稀疏后台脏页选择、等待期间新增/删除脏 inode、墓碑反复插入删除的容量稳定及最终资源。
热读与截断通过明确握手覆盖 batch/scalar 的 closing/CLOSED 等待、mixed hit/miss 交接前截断再增长、
旧页 pin 存活、新 EOF 和重新增长后的页面身份；客户机证据另见[两核模块](riscv-smp.md)。

共享文件别名在注销时停止新借用并摘链，借用游标固定退休后继；锁外drain完成后才释放
resident/MM存储。共享磁盘叶初始只读，写缺页先取得页写许可并登记脏状态，才发布W。
可写uaccess pin同时持有entry及页写入借用，归还不取得MM gate。写回只关闭目标页，
锁外逐MM撤W并确认RFENCE，排空该页写借用，复制脏范围后立即重开；后端I/O由CPU0
在inode读保护下执行，新generation不能被旧完成清除。后台worker遇旧pin或忙MM gate
会延期，由实际pin归还/gate释放通知继续，避免它与同一worker的冷读请求互等。
强制写回若跳过前台正在提交的页，前台成功完成且新代次仍脏时会重新发布work；
I/O失败与停止不建立重试循环。宿主反例用已完成后台轮次的受保护值快照确认请求已消费，
再放行前台，检查无需额外请求即可从真实后端读回新代次。

截断保留文件写操作门，先同步保留范围，再关闭代次并锁外撤映射；后端实际尺寸
及changed明确后，在inode写保护下收敛尾零、缓存退休和尺寸发布。无变化失败不丢脏页；
部分生效按真实尺寸处理。已退休整页的旧pin/快照借用继续固定存储，旧写回不再提交。
purge/destroy由CPU0独占生命周期owner执行，stop/join不替代外部来源、MM或pin的结束。
`msync` 固定来源及范围后先释放 MM gate；ext4 数据写回由既有 CPU0 worker 推进，元数据按后端策略在调用者准备，持久化目标由原调用者等待。需要共享 offset 或文件写操作门的操作在调用者取得资格，由执行者借用，不能在队首等待调用者的锁。接口与关闭身份见[文件模块](kernel-files.md)。

```sh
make test-page-cache-concurrency-host test-mm-disk-concurrency-host
make test-mm-disk-smp-riscv QEMU_RISCV_FIRMWARE=build/data-path-baselines/sync-b3/firmware.bin
make test-mm-disk-smp-userland-riscv INIT_CONFIG=config/init.json \
  QEMU_RISCV_FIRMWARE=build/data-path-baselines/sync-b3/firmware.bin
```

每个当前页缓存实例拥有一个 joinable worker；worker 借用 cache/record，调用者必须保持它们有效直到 stop/join 返回。根盘运行期启动、用户磁盘挂载时创建，所属实例卸载前停止并回收。空闲低/高水位为受管页 2%/4%，脏页启动/停止为 10%/5%，小配置至少一页且阈值严格有序。没有周期清脏。分配器和持锁路径只为脏页发布回收请求；低水位先回收无别名/额外引用的干净页，worker 再写回合格脏页；仅脏阈值触发则保留清洁缓存。

干净回收在缓存 mutex 内检查现有 LRU 候选，退休后在锁外归还页，跳过仍有数据借用或别名的对象。注册表遍历锁只保护缓存寿命，锁序为后端 40 → 聚合遍历 41 → 缓存元数据 45；元数据锁忙时等待其完成，不能将竞争当成 OOM。已持同级或更高资格时跳过，避免自身重入。持聚合锁时只归还页，不执行 entry/node 的最终关闭；退休元数据由 worker 在离开该保护后释放，避免回收路径反向进入后端。脏页按本节开头的 inode 队列选择。

后台写回、前景 fsync/legacy 完成和写入接纳共用批次前后的干净回收：固定稳定版本及取得 inode 前先回收，归还本批 entry 借用后再次恢复原高水位余量。前景工作区申请亦在此前回收，不让连续批次积累分配压力后才在后端内部失败。水位未改变；不能回收的页面和真实内存耗尽仍返回 ENOMEM。启动时预留写回窗口内的稳定页与控制工作区，其借用和归还见[当前成本边界](#当前成本边界)。连续页分配按真实回收进展重试的规则见[物理页](physical-pages.md)。

`FALLOCATE_HOST_CASES='namespace-reclaim namespace-pressure' FALLOCATE_HOST_IMAGE_SIZE=256M FALLOCATE_HOST_TIMEOUT=60 sh tests/fallocate-host.sh` 使用同一个 16 MiB 物理池承载页缓存、VFS 堆、块缓存和 journal；长场景创建 48 个 4 MiB 文件，各自 fsync、重开并完整读回。握手场景在真实后端锁内耗尽空闲页，检查 cache 元数据资格忙时等待并取得干净页，同锁或更高资格重入时跳过，随后无干净页时返回 EMPTY；设备入口断言禁止该线程递归 I/O。两种页大小、两种文件系统块大小及有／无 journal 共 16 个组合，最终检查堆、物理页及 e2fsck。真实写回 EIO、ENOSPC 和不可回收页耗尽仍由前述 pressure 场景保护。
后端元数据仍可能分配失败。映射重新设保护与 generation 协议保持不变，旧完成不能
清除新修改。写回错误留在 inode/mount，当前失败页在成功前不计入 MemAvailable；
同一轮继续处理其他对象。后台完成不替代 fsync/fdatasync 的错误观察与 flush。

统计入口同时记录 worker 扫描、写回、失败、批次和实际释放量；内存快照单次 O(驻留页数)，分配通知为 O(1)。`test-io-sleep-riscv` 在 4 MiB 测试池上触发默认比例，覆盖单次 EIO 后继续进展、错误观察、再次修改仍排除失败页、2% 到 4% 回收、固定页不被驱逐、启动 OOM、暂扣在途写回时 stop/join 等待且不继续提交、退出资源基线；共享映射睡眠期间修改、孤儿关闭和设备延迟使用同一写回协议的既有聚焦测试。测试边界为单 hart，不推出 SMP 或实板持久性。

## 硬链接与第二磁盘

RV64 linkat 由 files 层固定源 inode/OFD，VFS 在目标父目录锁下提交后端操作；
flags 0、AT_SYMLINK_FOLLOW、AT_EMPTY_PATH 不重新解析 fd 的旧名字。目标已存在、
跨挂载、目录源、只读和零链接边界按固定 Linux 差分覆盖。不同名字共享 inode、
页与记录锁，但保留独立路径身份及独立打开的 OFD。unlink/rename 替换只在最后
链接消失后进入 orphan 生命周期。ext4 在同一日志事务更新目录项、nlink 和时间；
非 journal 修改仍明确不支持，不缩减原恢复契约。

DTB 枚举的每块设备独立维护队列、IRQ、DMA 和超时。设备登记、文件系统实例、
挂载树节点分别拥有生命周期；mknodat 块节点按 st_rdev 同时支持挂载和[裸设备 OFD](riscv-virtio-block.md#用户态块设备)。同一设备重复独占挂载为 EBUSY。lwext4 注册名及内部
路径按实例生成，生成配置容纳八实例；root 单盘入口仍可使用。

## 文件后备 loop

`fs/loop.c` 登记 Linux major 7 的 loop0～loop7，10:237 的 loop-control 提供
GET_FREE、ADD 和 REMOVE；节点由用户态 mknodat 创建。块 OFD 使用原生 RV64/LA64
Linux loop_info（160 字节）、loop_info64（232 字节）和 loop_config（304 字节），
支持 SET_FD、SET/GET_STATUS、SET/GET_STATUS64、CONFIGURE、SET_CAPACITY 和 CLR_FD。
配置支持只读、字节 offset、sizelimit、AUTOCLEAR 及 512 到架构页大小的逻辑扇区；
文件系统块必须是逻辑扇区的整数倍。分区扫描、加密、direct I/O、CHANGE_FD 和
SET_BLOCK_SIZE 返回不支持。配置与挂载／O_EXCL 竞争真实设备 claim。

绑定持有真实后备 OFD，因此原 fd 关闭和后备文件 unlink 不改变绑定。每个绑定拥有
一个独立 I/O worker；上层文件系统提交的请求借用调用者缓冲直到完成，worker 通过
后备 OFD 的 VFS／页缓存读写，flush 同步后备文件。独立栈不继承上层文件系统锁，
避免 ext4 调用块层时倒序获取后备 inode 的锁。依赖从 loop 指向后备文件所在块设备；
绑定在控制锁内检查整个有向链，只接受已绑定且无环的祖先，不接纳空闲或 rundown 父设备。

普通打开和 mount claim 都是真实 owner；CLR_FD 在仍有其他打开／挂载时设置
AUTOCLEAR，唯一打开者则进入 rundown，拒绝新打开和 I/O，到最后关闭才解绑。
worker 只在全部设备引用与在途请求归还后同步并释放后备 OFD；失败保留实际 owner
和错误，不报告为空闲。正常退出交错卸载可卸的叶挂载与清理无人使用的 loop，
让后备所在挂载在依赖者之后释放，最后停止根缓存并卸载根盘。

`sh tests/loop-host.sh` 使用真实 journal ext4 后备与两层文件系统，覆盖绑定／unlink、
原生结构布局、配置、只读、rundown、占用、循环拒绝、持久化读回和退出释放；
保存实际两层后备镜像以 e2fsck 检查。4 KiB 文件系统可另设
`BOAROS_LOOP_SECTOR=4096 LOOP_HOST_BLOCKS=4096` 检查非默认逻辑扇区。

`fs/disk_mount.c` 拥有动态 mount/cache/source 名称。构造中的实例不进入可清理
状态；真正 I/O 清理失败才移交待清理注册表。关机重试逐个认领 owner，不跨睡眠
借用链表 next，不在同轮反复重试同一失败。没有 I/O owner 的分配失败立即回滚。
proc mounts 保留用户给出的磁盘来源名，并正确标识 tmpfs。

全局缓存快照 O(总项数)，压力通知和脏阈值判断 O(实例数)。水位仍以全局受管页
计算，只扣一次低水位预算。安全分配失败至多等待共同一轮的首次实际释放，或所有
参与 worker 完成，不逐盘串行等待。测试入口包括 test-io-sleep-riscv 的两个非空
缓存、test-root-multi-block-riscv 的真实双盘与重启，以及 test-lwext4-instances-host。

成本观测分别记录缓存范围遍历、实际脏区间快照和逻辑后端写回；设备请求按真实提交阶段归因，未分类请求保留 unknown，不由后端入口推断 data/metadata/journal，见[成本观测](kernel-cost.md)。

成本隔离探针：`make test-lwext4-cost-host` 使用实际 lwext4 的公开 touch/fwrite 接口、
独立 block_fault 设备计数和三个新生成的 journal 镜像，对比128次纯时间更新、
4KiB热覆盖及组合操作；核对时间戳、读回内容及fsck。它测量后端请求数量，
不经过VFS页缓存，不包含QEMU设备延迟，也不证明原iozone的CPU或总耗时占比。
当前成本及可重建配置见[成本基线](../learning/cost-baseline.md)。

`make test-journal-group-riscv` 在实际 IRQ 块 I/O 下验证 128 页内容、预算、100ms 期限、full/data 同步、truncate、unlink-but-open、最终卸载；观测构建亦检查后台来源明确的数据请求。`orphan_file` 的 slot 更新必须通过 `ext4_trans_block_get` 在修改前保留 undo/version，不能直接 get 后补记 dirty。

异步准备事务会改变故障执行器握手后的 I/O 前缀，SQLite 恢复 fixture 在 `mutation armed` 前同步根目录，排空 loader/control/SQLite 准备元数据，再开始本次数据库变更的故障计数。页缓存短写/重脏注入拦截实际 `ext4_fpwrite_batch` 入口，覆盖当前向量写回。

接近满盘时，若释放位图中的空闲块/inode 仍归运行或提交事务的 checkpoint 所有，下一操作先释放后端锁等待已接受序号，而不是返回暂时性的 ENOSPC。近满盘宿主 fixture 在回收 checkpoint I/O 中尝试另一文件扩展，旧实现未走等待边界，修正后明确等待、checkpoint 后重试成功，随后 e2fsck 正确；正常 RV64 组提交/回收亦通过。

## lwext4 块缓存命中与回收

`ext4_block_get_noread()` 在设备/LBA检查后先 `ext4_bcache_find_get()`。命中只取得引用，
不为不存在的新分配提前回收；未命中才 shake，候选分配后在缓存短锁内重新查找，复用竞争者先发布的同块对象。按 LBA 查找保留红黑树，零引用缓冲按最后归还顺序串成双向队列：首次借出直接摘链，末次归还直接接尾，回收取队首，均为 O(1)，不使用年龄序号或第二棵树。缓存索引、队列、引用、dirty 链和 loading/error 发布共用每挂载 bcache 的任务短锁；UP 只禁止抢占，SMP 使用完整 raw，详见[短对象保护](kernel-scheduler.md)。设备中断仅修改设备请求状态。分配、释放、设备 I/O、数据复制与休眠位于锁外。shake 在保护内摘除干净的零引用候选，脏候选先固定引用再锁外刷回；只读上下文保留脏块。

单块与批量读取将取得引用和 READY/LOAD/WAIT 判断放在同一次保护内：热块直接使用，生产者装入后发布自己的结果，实际等待者才复查 loading/error。批次先完成自己负责的全部装入，再等待其他生产者，避免反向重叠批次互等。等待回调在同一保护内重查 loading 并登记，再锁外休眠，覆盖完成早于登记或 park 的交错。完成通知持续到生产者归还引用前；只有实际存在等待节点时，UP 通知才局部关闭中断。有效内容、加载错误、dirty 及 journal_pending 沿实际引用处理；归还干净、共享或延后写回的引用无需查询调用者读上下文。共享后端读取不刷脏回收。生产目标保持8块（4KiB时32KiB），是回收目标，
不是引用块驻留上限。`make test-lwext4-cache-host`保护循环热块、固定引用、加载/回收/OOM
失败与最终释放；真实I/O等待和恢复仍由io-sleep、journal及SQLite回归保护。

根挂载从已登记块设备的device_number生成`/dev/block/<major>:<minor>`来源（boot首盘252:0）。
字符串在mount adapter内由mount持有，不借用启动栈；卸载清空来源指针后释放adapter。
动态挂载仍由自己的source owner管理。公共程序镜像创建相应块节点，proc mount快照
沿现有转义规则输出，原df根盘行与statfs容量/空闲/类型另做内容核对。

COST 构建在成功封口时记录条件和 sealed/durable/checkpoint 等待时间；普通构建不做这些记录，重叠条件不能相加推导组数。历史 64 次操作封口策略及测量保存在[2026-10-02 对照记录](../learning/cost-baseline.md#版本量封口与socket纠错对照2026-10-02)，当前策略见上文事务说明。

宿主组提交测试在固定时钟下检查：128 次同块修改不重复封口或分配组载荷；384 KiB 文件数据不会触发旧的 256 KiB 切组，继续写到 6 MiB 时由真实内存预算推动回收，全部内容正确。年龄在 100 ms 触发，封口 FIFO、近满盘复用、环绕、旧版本、OOM 和 WRITE/FLUSH 失败继续覆盖。追加分配的提交还检查两个 flush 的精确顺序，并在屏障失败后分别丢弃、撕裂和反序持久化提交记录，重启两次后核对文件长度、内容、时间戳及 e2fsck。

## FIFO 节点与传输 owner

mknodat的FIFO使用lwext4已有EXT4_DE_FIFO格式，不改变日志、写回或持久化顺序。
VFS node按mount/inode共享，fifo_pipe是短IRQ区保护的弱关联；候选缓冲在发布前准备，
重查并复用竞争者已发布的pipe。打开会合不持VFS锁睡眠，节点引用保持到端点释放后。
类型/umask、目录项、hardlink/rename/unlink及重启见`make test-fifo-riscv`；
相关创建/元数据入口由`make test-lwext4-metadata-host`覆盖。

路径truncate持有解析后的path并直接调用共同截断入口，不安装临时用户fd。
rank15整次操作锁、inode锁、缓存写回、尾页清零和跨MM失效沿原路径；
目录返回EISDIR，非普通节点EINVAL，负长度在访问pathname前EINVAL。
共同ext4截断在原操作undo内删除capability，包含同长度请求；固定root的
CAP_FSETID语义保留set-ID，不能套用chown清位规则。共享外部EA块继续按引用计数
复制或释放，失败只回滚本操作；日志、orphan和持久化顺序不变。

## 元数据成本复建

`tests/cost-riscv.py --case metadata`复用既有统计，提供固定4096次stat/fstat/
open窗口、2048次空文件创建删除，以及原lmbench的三种文件syscall和lat_fs、
原iozone四进程(0,1)。open窗口逐次fstat校验身份，故包含校验成本；纯open/close
成本由原lmbench对应项给出。路径持有与未持有分别测量，fstat本身必须持有fd。
程序完成、显式同步和`ROOT_DRAIN_FIXTURE=1`的内部最终卸载分别记录。
`--platform-config official`使用本地固定平台的1GiB、单hart、默认VirtIO、网卡和
UTC RTC，不添加RNG；不是完整比赛Harness。原旧glibc使用兼容分支配置。
关闭观测的重复启动与单次定点COST分别解释；原始输出和机器身份只进build。

热路径查询与数据带宽应分开解释：弱path registry只共享仍活着的路径，普通分量仍先
进行后端lookup；解析还使用有界临时缓冲。复用活inode减少后端owner准备，不消除整个
路径遍历。定点计数按固定工作量核对；原lmbench自适应文件数，不能只比较整命令耗时。
原旧glibc的完整metadata选择需要兼容分支内核，可通过`--kernel`指定，不改变main身份。

## devpts 节点与挂载

`fs/devpts.c` 管理独立的终端编号空间、目录枚举和节点元数据，`fs/pty.c` 管理真实
配对与传输。挂载接受 uid/gid/mode/ptmxmode/max/newinstance，默认 slave 为 0600、
ptmx 为 0000、max=32；公共用户环境显式使用
`mount -t devpts -o mode=0620,gid=0,ptmxmode=0666 devpts /dev/pts`，并将 `/dev/ptmx`
链接到 `/dev/pts/ptmx`。外部 5:2 节点按其父目录下的 pts 挂载选择实例。

编号与 inode 身份分开：master 最后关闭立即撤销 slave 的可见名字和弱配对绑定，
worker 回收配对后才释放编号。已 pin 的旧节点保留元数据，编号复用创建新 inode；
旧 O_PATH/proc 路径打开不能进入新配对。创建节点不预分配全部终端。普通用户不能
直接增删这些动态节点，不支持的 namespace 修改返回明确 errno。

OFD 和控制终端保活配对，配对持有挂载 root/path 引用；挂载拥有可 join worker，
而 worker 不永久 pin 自己的 root。卸载须先确认外部引用消失、停止并 join worker，
再释放后端。TTY 的内部 base 引用不形成配对引用环。仅支持单 hart 的发布契约，
不能把此实现作为跨核路径与 TTY 同步已经成立的证据。

双盘暂扣/错误隔离门禁由guest显式配置单字节控制终端，避免规范输入阻塞握手；
`make test-multi-disk-io-riscv test-multi-disk-rt-riscv`覆盖原故障/重启和实时负载。
runner超时保存token及guest/NBD边界现场，CI已接入原双盘目标，见[可睡眠存储](../learning/sleepable-storage.md)。
正常及实时负载 runner 在进程退出后仍将 guest/NBD 管道读到 EOF，再检查退出状态、
持久数据及回收基线；进程退出不意味着其输出管道已经读空。CI 归档退出状态 JSON。


## 缓存版本内的批量读

普通 `read/readv/pread` 与内核缓冲的 `kernel_vfs_pread` 根据本次请求范围，
通过 `kernel_page_cache_get_batch` 一次取得最多 128 KiB 的基页：RISC-V 为 32 页，
LoongArch 为 8 页。调用者先在一次 cache guard 内按当前 EOF 检查可交付页，同时检查
inode 的 closing 状态和所检查页的 CLOSED 状态；本地在首个未就绪页后交给慢路，远端继续
收集后部热页。热页当场取得物理引用，不借用 entry。
全热请求不取得 inode 读资格，也不进入冷装入器、建立加载区段或进入禁止回收 I/O 的分配范围。
普通 miss 保留已经取得的成功页，仅把未完成项交给慢路；调用 CPU 在缓存保护下分类并准备私有候选，后端执行时取得 inode 读资格重查，
不重复 pin 成功页。若 closing/CLOSED 使取页资格失效，先归还本批暂取的全部物理引用，再走慢路重新查询。
冷装入器禁止内联，避免热路径也预留整批加载工作区。
已有 loading 的订阅者跨等待持有 entry 借用。多页读取在后续请求尚未完成或还需准备下一窗口时，才在调用任务中提前发布已完成页；已经排空的批次直接通过同步结果交付，块层不再逐层补发最终通知。同一 SG 请求内的页共同完成，不拆小请求制造通知。提前发布通过 release/acquire 保证页内容可见；等待者登记后重查，完成先于登记也不睡眠。发布不逐页取得缓存 mutex，生产者借用仍覆盖整批排空，最后一次缓存保护统一归还借用并清除提前发布状态。之后的热命中沿缓存 mutex 的可见性规则读取，不额外执行 acquire fence。缺页所需索引容量、entry 和未清零物理页在调用 CPU 的缓存及 inode 保护外准备；候选尚未发布，不阻塞先入队的截断。按后端策略执行的生产者取得 inode 读资格后复查竞争者和 EOF，在缓存保护内发布 loading，锁外归还竞争失败候选，再提交后端批次。自有加载完成后才等待其他 producer，反向重叠批次不会互等未提交的加载。读页有效内容由设备读、洞合成或稳定版本复制初始化，成功后只清 EOF 尾部。
只初始化本次请求使用的 entry 槽，
成功结果在交付时填写，失败结果明确清空地址和有效长度。分配中途失败仍完成
已准备的页，并逐页返回状态。
调用者在复制前结束存储锁，持有全部成功页的物理引用；引用只固定页的存储寿命，不冻结页面内容。
遇到 EOF、I/O 错误或用户 fault 时也归还未消费的成功项。普通读只推进交付前缀的 OFD offset，定位读
不推进它；需求范围之外不装入额外页，也不创建新的 worker。

页缓存多页慢路的 entry 借用表与装入器共用已有读取工作区，单页只在栈上保留一个 entry 指针。
慢路独立成帧，热命中不预留这些数组；普通 read 的用户复制触发 ELF 缺页、再嵌套读文件时，也不叠加整批借用表。
内部 `kernel_vfs_node_pread_batch` 接受最多 32 个独立输出区段及本次借用的读取工作区。caller 在整个调用期间
持有有效 inode/read gate 和所有输出，保证地址与范围有效、区段互不重叠。
VFS 直接分派后端，不再重复检查缓存调用者已经保证的地址与别名条件；lwext4 对外
入口保留参数检查。结果逐项返回连续成功前缀与负 errno，无批量后端时执行独立标量
回退，不隐含完整批次成功或原子性。
lwext4 按输出维护有序片段游标，复用本次请求的跨层工作区；同一页的磁盘、洞和稳定版本片段全部完成后才发布。DMA 排空后用一次片段遍历归并剩余结果，跳过提前回调已经消费的前缀，不再逐页进入提前发布函数。页缓存交付时只取得一次有效长度，用于检查短读、清 EOF 尾部和填写输出。后端在通知前转换错误并补齐逻辑 EOF，通知后不再改写该页内容。
ext4 适配器只填写读取区段的输入，完成长度和错误由 lwext4 初始化并返回，
不再在上下两层重复初始化。`ext4_fpread_batch` 持 mount read lock。 该读资格也是待提交日志版本的借用：查找和锁外复制全程持有，commit/checkpoint 的设备阶段只读封口来源，回收必须取得 mount writer，等待读者复制完成。读取共享缓存版本以只读归还接口释放引用，不再修改挂载的 cache_write_back 计数。
lwext4 与块层的公开入口用已见目标的地址边界排除重叠；目标按地址递增或递减时，检查为线性工作。目标进入已有范围的间隙时再逐项检查，乱序缓冲区、重叠拒绝及溢出规则不变。

文件后端通过范围接口返回连续物理块、洞或未写入 extent 的长度。游标跨连续页缓存 span 和设备窗口复用；连续分配在一次位图借用中统一修改位、计数与校验，不按页重做路径查询和 CRC。
legacy 查询与创建共用一次间接链遍历，在同一指针叶内批量安装连续物理指针；分配止于当前叶边界，
纯查询可返回整棵缺失子树的洞。新间接链由本次操作持有，所需分配与写访问登记全部成功后才发布到已有父块或 inode，
发布前失败保持旧树，私有链与未发布的数据分配随操作清理或 abort 归还；路径最多保留三级块引用，工作数组在栈上。
gap 和同块 partial 重入结束分配窗口，出错释放所有尚未消费的后续范围。按 extent/传统间接块的边界处理洞、EOF 和块内片段；文件版本与直接读取的归属
见[当前成本边界](#当前成本边界)。需要设备读取的区间经 `ext4_blockdev_read_batch` 一起提交，块层合并磁盘相接的区间。
同一输出中 RAM 与磁盘都相接的小文件系统块先扩展为一个区间。磁盘片段容量与输出
数量分开：单输出复用栈上八项规划项，多输出复用调用者提供的 32 项工作区，不在下层为每次
请求分配堆内存。VFS/lwext4 共用文件区段，lwext4/块层共用字节区段；
原始请求长度不改写，可见 EOF 单独传递，完成量只表示已初始化的连续前缀。连续洞合并为一个完成区间，其前后的磁盘片段可以在同一轮提交；洞、
待提交版本和实际碎片边界仍按各自来源处理。对齐且磁盘相接的 64 KiB 冷读形成一个
64 KiB SG 请求，128 KiB 形成一个 128 KiB 请求；LoongArch 的窗口按自己的
16 KiB 基页计算。实际请求仍受设备段数、长度和队列容量约束。
元数据继续使用 `ext4_block_get_batch` 的共享 loading；本批自有加载先完成并唤醒，
再等待其他 owner 的既有加载，避免交叉等待尚未发布的缓冲。各项保留自己的错误与
成功前缀，读失败不记录为 inode 写回错误。设备持有输出至全部完成或确认 reset。

`make test-lwext4-cache-host test-lwext4-batch-read-host` 覆盖共享加载、旧缓存版本保护、
OOM、独立错误与重试；后者运行 1/4/8 KiB ext4、extent/传统映射的洞、EOF 和片段。
`make test-io-sleep-riscv` 另检查八个独立块请求一并发布，以及真实八页 VFS batch
合成一个 32 KiB SG 请求；暂扣最后一次 DMA 时取消不能提前返回。
`make test-demand-read-riscv` 固定 RA0，使用真实 `kernel_files_read/pread/readv`，
在 1/4 KiB ext4、legacy/modern 设备上检查 256 KiB 需求的实际 READ 区间和两次
128 KiB 请求，
另检查单页 `read/pread` 只发出一次 4 KiB READ、单页热读没有设备读取。
用户目标对齐的 4 KiB 复制登记一次写 pin，偏移 `0x618` 时跨两页，登记两次；
64 KiB 的 read/pread 登记 16 次，碎 iovec 不增加数据页 pin，另有一次导入向量的读 pin。
批次内的 pin 复用及一次性复制接口见[MM 模块](kernel-mm.md#匿名-mm-的并发与回收)。
覆盖热命中、冷热混合、未对齐、洞、EOF、用户 fault、真实 READ EIO 前缀与重试，
以及暂扣 DMA 时 close/reclaim/取消的引用寿命。`make test-page-cache-concurrency-host`
在 4/16 KiB 基页检查重叠 loading、逐页错误、各分配位置 OOM、短 EOF、排队取消、
截断和最终引用归还，并记录调用线程实际取得的缓存锁、inode 锁和物理页引用。
后端无错误却未填满 inode 有效范围时，标量和批量读取均返回 I/O 错误，移除失败
loading 并允许重试；不能把清零的未读尾部交付为成功数据。
在无索引扩容、无等待的本地 CPU0 请求中，缓存锁次数如下；两种基页大小结果相同：

| 请求 | 首版需求合批 `2d0a08d` | 当前实现 |
|---|---:|---:|
| 单页冷读 | 7 | 5 |
| 八页冷读 | 49 | 7 |
| 单页热读 | 3 | 1 |
| 八页热读 | 24 | 1 |

全热批次不取得 inode 读资格，每个成功页仍取得和归还一次物理引用。冷请求先做一次
缓存资格检查并在慢路保留实际 cache 借用，首个缺页之后分类并私有准备未完成项，在后端 inode 读资格下重查，归还时结束 cache 借用；多页冷读
还用两次短缓存保护借出和归还可复用工作区。单页不借工作区。单页和多页冷读都不再逐页预留索引、重复保护发布，miss 统计并入整批发布。旧标量路径的热命中同样每页进入
三次缓存保护，因此热页重复借用和归还早于需求合批。远端全热请求一批只进一次缓存
保护。ext4 缺页在调用 CPU 装入，缺失批次保留后部已取得的热页和独立成功前缀。
操作次数用于检查实际执行路径，不换算成吞吐收益。默认预读仍为零，独立的顺序预读
候选见下一节。

下面记录 `b6ab571` 的 4 KiB 读取准备开销检查，早于 128 KiB 窗口改造。
需求合批前的 `2e3f728`、修改前的 `7cfcce5` 与 `b6ab571` 使用相同的
[`data-path` 程序](../../tests/workloads/io/data-path.c)，复用
[`io-budget-experiment.py`](../../tests/io-budget-experiment.py) 的预置镜像、启动及
内容、完成与退出检查。QEMU 11.1.2、512 MiB、单 hart、modern VirtIO、writeback、
RA0/WB8，关闭诊断，计时期间不编译或运行其他测试。各项三个版本轮换顺序，
每版六次；90 次启动全部通过。热读先完整读过一次，冷读为新启动后首次读取目标
数据，宿主存储缓存未清空。下面工作量均为 16 MiB，中位吞吐单位 MiB/s：

| 宿主线程位置 | 读取方式和请求大小 | `2e3f728` | `7cfcce5` | `b6ab571` |
|---|---|---:|---:|---:|
| 默认调度 | 冷读 4 KiB | 21.72 | 19.74 | 21.36 |
| 默认调度 | 冷读 64 KiB | 25.65 | 54.35 | 58.49 |
| 默认调度 | 热读 4 KiB | 110.03 | 104.86 | 109.65 |
| vCPU 在 CPU14，主线程在 CPU15 | 冷读 4 KiB | 12.70 | 13.37 | 12.62 |
| vCPU 在 CPU14，主线程在 CPU15 | 热读 4 KiB | 59.68 | 58.76 | 60.34 |

CPU14/15 是本机两个独立的低频核；通过 QMP 的 `query-cpus-fast` 取得 vCPU
线程后设置亲和性，再开始运行。默认调度允许迁移到不同频率和 SMT 条件的核，
两组绝对吞吐不能直接比较。固定线程位置也没有排除其他宿主任务的竞争。
默认 4 KiB 冷读的六次范围分别为 20.32–22.47、11.06–21.43、18.83–22.59 MiB/s；
固定线程后，合批前与 `b6ab571` 的范围仍为 11.68–13.49、8.37–13.69 MiB/s。
`b6ab571` 相对合批前的中位数差为默认调度低 1.7%、固定线程低 0.6%；这些样本波动
大于差值，该次检查不据此宣称 4 KiB 吞吐提高。64 KiB 冷读为合批前的 2.28 倍；
当时的窗口为八个 4 KiB 页，64 KiB 分成两次 32 KiB SG 请求。

剩余差距需要区分执行动作和设备完成时机。轻量观测在读取窗口内记录到三个版本
相同的 4,106 次请求发布调用、4,105 次通知；元数据也包含在内，没有新增逐页 I/O。
默认调度时，旧标量和修改前合批分别 park 4,125–4,132 与 4,130–4,132 次，
上下文切换约 8,230 次；将 vCPU 与主线程分核时，两者均 park 4,129–4,130 次。
将 QEMU 全部线程都绑到 CPU14 时，旧标量却 park 1,897–2,056 次，修改前合批
park 2,146–2,169 次。同核的重复计时可以稳定出现约 4% 差值，说明线程争用
可以改变设备完成与睡眠的相对时机；它不代表默认运行中合批新增同等次数的睡眠。
这些计数另行采集，不使用带观测的运行作为上表的吞吐结果。

源码中确认的额外工作是准备页时重复进入禁止回收状态，以及重复初始化 entry 槽
和区段结果；现已删除。inode 读锁仍禁止分配等待回收 I/O，每批少两次 I/O 上下文
查询与对应的状态增减。成功与失败结果各在负责交付它们的位置写入，未增加锁、
物理引用或数据复制。单页与多页继续复用同一需求读取和 SG 路径。


深度至少 2 的 extent 树截断会修改非根内部索引；每次路径上移归还该引用前，必须
通过 `ext4_ext_drop_refs` 重算已修改块的校验和。直接归还并清空块号会绕过最终
checksum 更新，使下一轮合法查找返回 EUCLEAN。`make test-lwext4-deep-truncate-host`
构造深层稀疏树，验证 1/4 KiB 文件系统部分截断、跨进程重启读回、截零和 e2fsck。


## 有界顺序预读候选

`BOAROS_PAGE_CACHE_READAHEAD_PAGES` 默认 0，配置上限为 128 KiB 对应的架构页数；
现有候选测试使用 0/1/2/4/8。只有非零配置才创建每缓存实例的独立预读 worker。
OFD 保存成功读取的末端和取消 cookie；read/pread/
sendfile 在首次可能睡眠的 accessed、后端或 usercopy 之前执行 read_begin，立即取消
不连续预测，read_progress 只登记成功前缀。两个连续片段在页边界完成后可申请下一窗口。
seek、非顺序读、最后关闭、截断/失效、低水位和停止取消未接纳工作。

队列固定八个 job，只持 inode pin；一实例同时处理一个配置窗口内的批次。worker 取得
inode read gate，复用 demand/full-overwrite 的 loading 页准备；已存在的页或加载由
原 owner 处理，预读不交叉等待它。需求读可订阅已发布 loading，只有成功页面对外
可见；错误页按用户引用收口，无关页面独立成功，也不记作 inode 写回错误。
准备过程禁止分配回收 I/O，取消时已经有需求读者的页仍保留加载 owner。

接纳点是进入一次 VFS batch：此前可以撤销，之后完整排空已形成的批次，
其设备区间按实际文件映射组成，不逐 DMA 取消。stop 先撤未接纳 job，再 join
已接纳批次，最后释放实例；截断/失效的 inode write gate 与加载互斥。每个批次完成后
重新开放中断并让出，队列满或内存不足只放弃推测，不制造需求读错误。

`make test-readahead-riscv` 覆盖全部窗口及 1/4 KiB ext4，验证实际缓存内容、窗口上限、
seek/非顺序/关闭、冷不连续读、单页错误隔离、低水位、截断、unlink 和回收。
`python3 -B tests/readahead-riscv.py --pages 1 8 --block-size 4096 --held --transport legacy`
及 modern 模式使用 NBD 暂扣所有窗口 READ，再保留最后一个；需求读取者和 stop/join
调用者均须保持等待，响应后才归还 owner。用户态组合、恢复与发布性能另行验收。


SQLite NBD runner 的 marker 停机使用显式受控 cut：后端先冻结磁盘并确认退出零，
再终止 guest，避免主动 kill 被误记成后端协议错误。默认及 RA8/WB8 的阶段七后
完整 DELETE/WAL 恢复结果、未到达故障序号和输入身份见[最终恢复](../learning/record-lock-sqlite-recovery.md#数据路径最终恢复与宿主收口2026-10-06)。

此前存储 20 组候选及 26 项扩展/同步负载使用当时的复制和提交结构比较，实验选择为 RA0/WB1；当前稳定版本向量与 SG 合批的机制见本文“当前成本边界”，旧结果不能直接代表新路径。RA8 改善所测顺序冷读、WB8 改善显式同步，但 WB8 在 64 MiB 缓存追加有回退；冷热、tmpfs、缓存完成与 durable 同步分列于[报告及每文件完成时间](../learning/data-path-budget-experiments.md#正式匹配结果2026-10-06)。

## 无metadata checksum的空索引目录（2026-10-07）

`ext4_dir_check_empty`按HTree根的层级/块引用确定root、inner node与leaf；不按
首个零inode整块记录猜测。关闭metadata_csum后，合法空leaf与inner node都可能
使用这种记录，错误分类曾使普通mkdir/rmdir返回EUCLEAN。root/node仍检查
count/limit/块范围/校验，leaf仍检查记录边界/名字长度/校验，损坏不静默降级。

`make test-lwext4-dir-empty-host`使用真实1/4KiB ext4、dir_index和metadata_csum
分别开/关，运行空目录覆盖及长名称目录增长/rename，再由e2fsck验证。
ASan/UBSan保护实际lwext4/JBD/块模型；宿主证明不替代DMA。LA真实两种RAM
的普通mkdir/rmdir与tmpfs挂载/卸载后rmdir、Linux同ELF、退出和根owner已通过，
程序路径由扩大的network sendfile场景保护。全恢复矩阵仍在本轮最终回归范围内。

## 双架构 SQLite DELETE/WAL 原程序

LA 同一原 SQLite3.53.4 archive 编译 LP64D 静态 workload、原 shell.c 静态/动态
CLI（16KiB ELF对齐），共享原 musl1.2.5解释器。上游 amalgamation/shell 不修改。
`tests/sqlite-rollback.py --arch loongarch` 与 `tests/sqlite-wal-riscv.py --arch loongarch`
使用同一ELF先固定Linux后BoarOS，各在512MiB和1GiB运行。两内核冻结、archive和
runtime身份检查，不能以不同二进制或不同SQL来接受架构差异。

DELETE保护提交/回滚、fork/exec多进程锁竞争、进程异常离开产生的热日志恢复。
随后独立启动原静态/动态CLI，逐字核对版本/线程编译选项/integrity_check、已提交的
`committed,parent,child` 和 spill24行且无未提交修改，再重复独立启动核对持久内容。
WAL保护多进程共享映射/锁、提交及独立重启。以上LA双侧两种RAM实际退出42、Linux
卸载与BoarOS根owner释放均已通过；RV shared DELETE/CLI 六次启动也通过。
RV固定Linux不启用initrd，直接根盘supervisor执行wait42与syncfs；LA initrd
supervisor先卸载根盘。两种profile都检查真实子程序状态，不能只依赖一条串口标记。

原RV `test-sqlite-rollback-riscv`、WAL/NBD及完整DELETE/WAL恢复矩阵保留。共用TTY
输出CRLF后，NBD shell parser只把CRLF还原为LF，保留原字节与全部NBD请求；不删
任意CR，不改变SQLite程序或放宽退出/WRITE/FLUSH/真实数据库门禁。
