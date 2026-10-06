# VFS 与 ext4 模块

本文描述当前根文件系统的稳定接口和 lwext4 私有适配。第三方版本与许可证见[第三方代码](../third-party.md)，存储知识见[存储与文件系统学习总结](../learning/storage-filesystems.md)。

## 通用边界

`include/kernel/block.h` 定义同步块设备（支持读与可选写），`include/kernel/vfs.h` 定义 mount/file 对象以及根挂载、open/create、pread/pwrite、ftruncate、mkdir、unlink、rmdir、close、unmount、`kernel_vfs_fstat()` 与 `kernel_vfs_mount_is_readonly()` 查询。VFS 对外返回负 Linux errno；lwext4 的结构、全局设备名和正值 errno 不泄漏到调用者。当前有独立的 ext4、tmpfs 和 proc 实例；lwext4 共用 heap binding 按实例引用计数维护，每实例携带自己的锁上下文；进程 fd/open-file-description 位于独立的[文件资源层](kernel-files.md)，所有任务共享一棵挂载树，没有 mount namespace 隔离；命名空间修改使用可睡眠 mutex，inode、OFD 与后端各自同步。

`fs/vfs_objects.h` 定义通用实例、inode 节点、路径与后端操作表；
`fs/vfs.c` 管理路径身份、引用、执行/写租约、记录锁、缓存及映射登记。
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

通用路径已有内部挂载树：挂载持有新实例根路径与被遮蔽路径的引用，父挂载记录
子挂载数；进入、同点覆盖、`..` 和 `getcwd` 都沿对象身份遍历。普通卸载须先
排除子挂载、其他打开对象及根路径引用，再摘挂载边；失败保留原挂载树。
跨挂载路径解析逐分量取得对应实例的命名空间锁，修改操作只在目标实例锁下
完成后端事务；记录锁、OFD 和页缓存仍按各自 owner 同步。ext4 inode 仍是
32 位，但通用 inode 标识已扩展至 64 位；ext4 适配层拒绝越界标识。
RISC-V VFS 测试的内存后端覆盖内部边界；用户态 `mount(2)`/`umount2(2)`
已接入 proc、tmpfs 和块设备节点识别的 ext4。普通挂载、`MS_RDONLY`、`MS_SILENT`、同点覆盖、cwd 忙引用与
卸载恢复均经真实 U-mode 差分；bind/remount/传播和 lazy detach 返回不支持。
卸载先检查忙引用并标记 quiescing，阻止新操作，再停 worker、完成在途 I/O 和持久化交接。失败时挂载树、设备 claim 和错误 owner 保持可达；可重试卸载。成功后才摘树、释放最后 root handle 和实例，最终释放不再产生 I/O。

`kernel_vfs_mount_root()` 根据传入块设备是否提供 `write` 回调决定只读还是读写挂载。读写 journal 挂载先 replay、校验 orphan 记录、启动日志并回收遗留 orphan，完成后才发布根路径。只读介质不能完成恢复时明确拒绝；未知必需特性、损坏日志或元数据也不能作为干净镜像继续访问。

`kernel_vfs_file_read_source()` 把保持打开的文件导出为带 `size/context/read_at` 的精确随机读源。回调只有填满整个范围才返回零；EOF 以内的短读转成 `-EIO`。ELF parser 因而能复用内存和 VFS 来源，而不依赖文件系统类型。

`kernel_vfs_open_executable()` 在普通 open 之上统一要求 regular file 和至少一个执行位；目录、非普通文件或无执行位返回 `-EACCES`。若目标 node 存在活动的写打开者（`node->write_openers > 0`），返回 `-ETXTBSY`；检查通过后累加 `node->exec_users` 并持有 `file->exec_lease`。对应地，以写权限打开普通文件需调用 `kernel_vfs_file_acquire_write()`，当 `node->exec_users > 0` 时准确返回 `-ETXTBSY`，否则累加 `node->write_openers` 并持有 `file->write_lease`。该租约在 `kernel_vfs_close()` 时释放，严格保障运行中二进制与写入者之间的互斥。

挂载为 lwext4 注册可选 realtime 时钟；尚未初始化时钟的纯模块环境保留原时间。`kernel_vfs_file_accessed()` 按活 inode 执行 relatime（包含页缓存命中的 read/pread），`kernel_vfs_file_modified()` 在非零写请求复制前更新时间。操作预留或已知 mount 错误当场传播；异步组登记后发生的持久化错误由 journal/mount 保留并由同步接口观察。create、目录链接/删除和 truncate 在拥有 inode 引用的后端更新相应时间；同尺寸 truncate 也走后端，unlink 后仍打开的文件不依赖路径。时间字段编码、操作时机与失败 owner 的依据见[时间戳学习记录](../learning/file-timestamps.md)。

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

VFS 以文件系统实例与后端 inode 标识为活节点身份，普通文件、目录和字符节点都持有引用计数 node；路径对象另持有父目录项身份和一份活 inode 引用。独立 open file description 各自保存 offset，但同一 inode 指向共享 node。文件大小通过 `kernel_vfs_file_size()` 实时查询所属 node 的实时大小，确保写入或截断后各共享描述符观察到一致的文件长度。

活 node 还嵌入记录锁区间树与等待队列，因此独立 open 必须在同一 inode 上冲突；unlink 后仍打开的旧 inode 保持原锁身份，同名重建使用新 inode 和新锁状态。末节点释放要求树与等待队列都为空；锁 owner、close/退出释放与阻塞 pin 契约见[文件资源模块](kernel-files.md)。该状态属于单 hart 临界区，不代表已经具备跨核并发锁。

每个 ext4 实例建立独立的 4 KiB 页缓存，键为 `(node, page_index)`：开放寻址哈希提供平均常数时间查找，双向 LRU 维护回收次序。缓存项持有 node 引用和一份物理页引用；命中时再给调用者一份临时引用，因此 `read`、不同 fd 与文件私有/共享映射可以安全引用同一页。共享 PTE 另持物理引用；MM 拥有的别名记录由缓存项反向索引借用，含别名的缓存项不能失效或回收。

普通写把已复制的字节写入同一缓存页并更新 node 的逻辑大小；不同 fd、VFS pread、ELF 读源与映射缺页立即看到该内容。跨 EOF 的普通写先清零旧尾页中将要暴露的空隙；显式扩展亦清零新暴露字节，并重新保护共享别名，防止页仍可写时漏记脏。缓存项按 inode 另建全页链供截断/失效使用，并维护独立脏页链与计数。范围写回比较目标页数和脏页数，选择页号哈希或脏链中较小的集合；按范围 `msync` 只提交目标文件页。每项保留脏范围、修改代次与 writeback 状态；写回前重新保护本页共享别名，再固定页面，只有完整提交且代次未变才清脏。失败保留缓存页/node owner，并记录 inode 的错误序列。lwext4 非 journal 写入/截断用操作范围固定本次修改的缓冲，建立完整的分配所有权后才提交该集合；不会全量 drain 历史 dirty list。底层最后引用的 flush 错误显式返回，失败缓冲仍在 mount dirty list。最后一个 fd 关闭不释放仍有脏页的 inode。

截断只写回新 EOF 之前的脏范围，再调用后端；成功后按实际结果更新逻辑长度、通知 MM、丢弃越界页并清零尾页。后端失败且磁盘 size 未变时保留原逻辑长度与脏 owner，截零不需要保存待丢弃数据。删除仍打开的文件保留缓存；只有 orphan 最后一个打开/执行 owner 退出时，才丢弃已不再可观察的缓存并回收 inode。

缓存索引失效本身不撤销用户 PTE。向下截断另通过稳定 node–MM 登记通知相关地址空间，撤销越过新 EOF 的整页（含 private COW 与 PROT_NONE），并按驻留来源处理非对齐尾页；普通写直接修改共享缓存，不撤销 private COW 页面。VMA 保留，后续访问重新缺页并检查 live inode size。实现与单 hart 同步边界见本文末尾及[MM 模块](kernel-mm.md)。

miss 路径先分配并清零页，再通过 node 的无 offset 副作用 `pread` 填充，有效字节数按 node 的逻辑大小计算，尚未写回的稀疏扩展区从清零页读取。读取整个越过 EOF 的页返回 `OUT_OF_RANGE`，尾页剩余字节保持为零。物理页分配器的压力入口由多缓存共同 owner 注册，按实例轮转；分配首次耗尽时从 LRU 尾部仅回收引用数为 1 的未固定干净页，安全的普通任务可等待后台一轮后重试一次。映射/请求固定页不驱逐；lwext4、堆与 IRQ 不等待后台写回，防止重入共享 handle、后端锁和 bcache 分配。阈值 worker 的生命周期见下文。

缓存销毁和 mount 卸载有严格顺序：先释放所有进程 MM 和临时读者，停止并 join 页缓存 worker，再写回并 purge 该 mount 的缓存项、关闭最后的 node；日志 worker 继续服务这些交接，排空到 checkpoint 后才停止并 join，之后允许 lwext4 unmount 与设备 flush；缓存最后注销 reclaimer 并释放哈希表。物理页和堆对象的合法释放完成即返回，分配器不变量错误进入 fatal；只有真实 ext4/block I/O 清理错误保留 mount owner。

`kernel_vfs_sync()` 先交接目标 inode 脏页，再捕获 full/data 目标序号并等待提交完成；journal 路径已包含持久化屏障，不额外发无意义的设备 flush。无 journal 路径仍显式 flush。独立 open 各持错误观察位置，dup/fork 共用 OFD 的位置。`fsync/fdatasync` 支持普通文件与目录；共享 inode 的后端 handle 记录完整和数据序号。full 同步为涵盖经其他 handle 的 namespace 修改，保守地捕获当前 mount 序号一次；data 同步等待数据/检索依赖序号，纯时间修改不推进它。同组时间字段可以顺带持久化；新修改不会无限延长已有等待目标。`O_SYNC/O_DSYNC` 分别对已接受的写入前缀执行 full/data 同步，失败返回 errno，但已接受字节与 offset 保留。journal 的关键写入、checkpoint 或屏障失败使 mount 持续拒绝修改和同步；OFD 错误游标不能清除该错误。

## 挂载范围与全局同步

`kernel_vfs_sync_mount()`先取得根路径owner，再在短发布保护区捕获并引用全部活节点；
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

内核编译 lwext4 journal/replay、orphan 和分批截断路径，启用内部xattr操作供chown删除capability；关闭debug/assert和mkfs，并把 malloc/calloc/realloc/free 绑定到当前内核堆。尚未接入用户xattr/ACL syscall或capability执行权限。根设备是 raw whole-disk ext4，物理块大小固定为 512 字节；当前不解析分区表。

事务接口 `ext4_transaction_begin/end/abort` 支持同一 mount 的嵌套修改；外层提交前保留 metadata 和数据缓冲的 before-image 与引用。明确发生在日志提交前的 OOM、空间不足或关联数据 I/O 失败可回滚内存并重试；已可能影响日志持久状态的错误由 mount 保留，不能清除后继续。外层 abort 后，调用者须重新打开在内层修改过的 lwext4 handle；VFS 的普通操作各自完成事务，不持有跨 syscall 的开放事务。

`ext4_journal_group_enable/service/drain` 由可写 journal mount 的独立 joinable 线程驱动；根启动与动态磁盘挂载启动该线程，宿主 fixture 显式推进同一引擎。操作仍各自持有 before-image，成功后合入挂载点 running transaction；同块修改合并，后一次失败只回滚自身。封口把 metadata 和 ordered data 复制到预留的不可变版本，提交准备使用预留日志缓冲和挂载期固定映射；设备提交和 checkpoint 不再读取可变 bcache。未提交 owner 通过 `journal_pending` 禁止隐式 home writeback，块与 inode 的释放范围保留到 checkpoint 屏障和日志起点更新完成，分配器跳过这些范围。首脏 100 ms 或 256 KiB 镜像是软封口条件，同步目标与强制请求另触发封口，块载荷按恰好一个文件系统块单独分配，控制记录按实际尺寸档容量分配；同运行块复用 after/log 预留，本次 undo 仍独立。每个 home buffer 在第一个版本 pin 时计费，最后一个版本释放时撤销计费。空闲镜像和控制记录池最多保留 min(256KiB, 预算/4)，也计入硬预算；不足时先释放空闲资源。固定日志映射也纳入挂载点预算，运行时上限为 min(4MiB, RAM/32)。运行组之外最多两组冻结 FIFO 和一组提交中，commit 屏障后独立发布 durable；已提交 FIFO 留存 metadata 版本、日志空间与 quarantine 到 checkpoint 完成。worker 优先就绪提交；空队列、回收压力或累计四组 checkpoint 时执行最多八组连续 checkpoint 批次，完成后重新选择。内部等待区分 sealed/durable/checkpoint；软阈值只封口，封口 FIFO 满才等 sealed，实际预算、日志 credit 或复用不足才等 checkpoint。页交接写入在操作前预留 data undo/version/home 与有界 metadata 路径的容量；私有原子操作内不释放修改锁等待。同步返回具有 commit 持久化保证。S6–S8 已通过宿主版本/环绕/断电、真实 IRQ 组提交及最终 lwext4/SQLite DELETE/WAL 恢复矩阵、双盘隔离；S9 测量完成但收益和 musl 普通读进展未达当时预期，见[本轮验收](../learning/cost-baseline.md#s9-存储流水线验收2026-10-01)。旧 S5 的 lwext4、SQLite DELETE/WAL 恢复、双盘与消费者结论见[验收分析](../learning/cost-baseline.md#异步日志与组提交验收2026-10-01)。

`make test-lwext4-group-host` 使用实际引擎与独立设备计数，覆盖 32 次时间修改合成一批、嵌套 abort、后操作各预留点 OOM、提交期间同块新修改、冻结后禁止新分配、1/4 KiB 文件系统的 WRITE/FLUSH 失败与重启恢复。命名空间回归暂扣提交进度，核对正常容量下的 truncate、空文件/数据文件删除、仍打开的无链接文件及 inode 复用隔离。`sh tests/lwext4-reclaim-host.sh` 在同组回收与意图先 durable、checkpoint 暂未执行的跨组回收上，逐实际 I/O 边界检查丢失/重排、两次恢复和文件系统一致性。默认同步路径的 metadata/几何/错误原子性回归继续由 `make test-lwext4-metadata-host` 保护。

同阶段的冻结 ordered-data、日志和 checkpoint 使用 `ext4_blocks_set_batch()`，最多八项，经可选 `bwrite_batch` 回调交给块层；没有批量回调时按序回退。每项完整预检后才发布，同批不包含重叠 LBA；跨 checkpoint 组的同 LBA 先完成旧版本，再发布新版本。阶段仍排空并保留原屏障，镜像 owner 持有到所有 DMA 完成或 reset 确认停止。热读 relatime 先持 inode/backend 共享资格查询；无更新直接释放，需更新时先释放共享资格，再取得独占资格重查当前时间，不做锁升级。读结果仍不受 atime I/O 失败覆盖。

每次提交先预留全部日志空间、映射与缓冲，再依次完成关联文件数据及 flush、日志内容及 flush、commit 记录及 flush。预留失败不写当前事务的数据；预留缓冲由事务持有至提交或回滚，内存成本随本次 metadata 日志大小增长。checkpoint 把已提交内容写回原位置并 flush，再持久化日志起点，最后释放日志空间和缓冲 owner。数据不写入 metadata 日志。主 superblock 的分配计数、恢复位和校验和也属于事务；挂载/卸载不在日志之外直接覆盖它。512 字节原子扇区模型下若 superblock 校验失败，只允许根据合法几何信息进入受限恢复，必须重放有效 superblock 日志后才能访问文件。

`ext4_orphan.c` 支持传统 `last_orphan/i_dtime` 链和 `orphan_file`，校验范围、重复记录、循环、分配状态和 checksum。unlink 将最后一个链接摘除与持久 orphan 记录放在同一事务；缩小文件先在操作事务中登记最终 size 与 orphan，再由 `ext4_truncate.c` 每事务最多释放 32 个尾部数据块及已空的索引路径。意图与回收可以合入同一运行组，也可以按 journal FIFO 跨组提交；普通 truncate/unlink 不为每次回收等待挂载点 checkpoint。释放范围仍由 quarantine 保留，实际预算、日志空间或复用压力才等待 checkpoint，显式同步及卸载保持各自的 durable／排空边界。无 orphan 的新建、同尺寸截断和扩展不启动回收事务。恢复根据实际映射找到尾部，支持稀疏 extent 和三级间接块，不依赖已缩小的 size 推测待回收块。最后删除 orphan 记录与释放无链接 inode 同事务完成；删除复用完整校验的去重计数，在集合为空时同事务清除 ORPHAN_PRESENT，不重复扫描来判断是否为空。后续失败仍回滚本操作的 superblock 和记录修改，重复恢复可继续前次进度。仍被打开的无链接 inode 在正常运行期间保留，重启才回收。

支持有界的 JBD2 checksum v2/v3、32/64-bit revoke；异步 commit、未知必需特性和旧 CRC32 journal 格式明确拒绝。已提交事务的损坏不能当作未提交尾部丢弃。只读脏日志、恢复 I/O 错误或损坏均拒绝开放用户访问。失败的日志/挂载 owner 保留到重启；普通合法内存释放不建立重试链。挂载准备阶段的 ENOMEM/ENOSPC 与关键 I/O 错误分开：资源不足保留可恢复的准备状态，后续 cleanup 可继续恢复并卸载，不能永久锁住 heap binding。
已有路径先逐分量取得目录项和 inode 身份，再用 `ext4_fopen_inode` 按 inode 打开普通文件、目录或字符节点；新建仍由 `ext4_fopen2` 提交。目录 handle 不在内核任务栈上分配完整 `ext4_dir` 结构。
普通文件随机写入与追加先进入页缓存；append 以共享 node 的逻辑 EOF 为起点并推进实际接收字节。定向写回通过 `ext4_fpwrite` 的独立局部位置提交脏范围，等待事务预算时不会被其他任务改变共享 handle 的 offset。lwext4 的 `SEEK_SET` 允许定位到 EOF 后，磁盘逻辑块只在写回时按数据范围分配；新分配的部分块先清零，旧 EOF 块尾清零，完整中间 hole 保持未映射。底层部分写失败不缩小缓存已经接收的逻辑长度，整段脏范围由页缓存保留供重试。`st_blocks` 报实际已分配存储，不由逻辑大小猜测；检查磁盘块生命周期的测试需先同步。

截断统一通过 sparse-capable `ext4_ftruncate` 执行：缩小仍释放尾部块，扩大只清零已分配的旧 EOF 块尾并发布新 inode size，不为完整逻辑 gap 分配块，且不改变调用 OFD offset。文件打开时按实际 inode mapping 缓存可寻址 size 上限：extent inode 使用 `EXT_MAX_BLOCKS * block_size`；legacy block-map inode 取指针树容量、`EXT_MAX_BLOCKS` 个可安全计数的逻辑块和 inode `i_blocks` 容量（包含间接块开销）的最小值，再乘以 `block_size`。因此 legacy 最后可用逻辑块也是 `0xfffffffe`：即使 8 KiB 三级间接树容量超过 2^32，也不会让 `ext4_lblk_t` 在 2^45 字节处回绕到块 0。truncate/write 超界返回 `EFBIG`，seek 超界返回 `EINVAL`，在任何尾部清零、64-bit offset 缩窄为 `ext4_lblk_t` 或 inode size 更新前拒绝。lwext4 在写入或截断所有可能改变状态的返回路径上，让 handle 的 `fsize` 保持为当前 inode 中可知的最新 size。

新分配的数据块在 caller data 写入前整块初始化；初始化失败只撤销该 exact logical block 的 mapping，不回滚此前已经完成的块。对预置 unwritten extent 的初始化和 caller 部分写使用同一个 block-cache buffer，避免延迟的 zero buffer 在 direct I/O 之后覆盖用户数据；读取 mapped block 前先排空该物理块的 dirty cache，flush 失败则返回错误而不从旧磁盘内容读取。lwext4 在写入或截断所有可能改变状态的返回路径上，让 handle 的 `fsize` 保持为当前 inode 中可知的最新 size；VFS 在 truncate 后据此同步 node/file size 并失效缓存；writeback 期间逻辑长度仍由缓存接收进度决定。部分写返回依据固定 Linux `references/linux` commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e` 的 `mm/filemap.c::generic_perform_write()` 与 `fs/read_write.c::new_sync_write()`：正进度覆盖后续错误并只推进相同字节数的文件位置。

`kernel_vfs_fstat()` 把当前 open handle 指向的 raw ext4 inode 转换为统一 `kernel_vfs_stat`，不按路径重新查找。普通文件 size 来自共享 node 的逻辑大小，其余字段来自实际 inode。它读取 inode 的 ino/mode/nlink、Linux low/high uid/gid、64-bit size、以 512 字节为单位的 allocated block count，以及 filesystem block size；atime/mtime/ctime 按磁盘 inode size 与 `extra_isize` 共同判断 extra 字段是否存在，再解码 2-bit epoch 与纳秒。该编码依据固定 Linux commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e` 的 `references/linux/fs/ext4/ext4.h`。根 mount 保存稳定的内部 ID 1 作为 `dev`，不表达物理设备 major/minor。lwext4 的 `ext4_fraw_inode_fill()` 是最小 handle adapter，使最后一个 dentry 删除后仍能查询活着的 inode。
目录操作中，`kernel_vfs_mkdir` 调用 `ext4_dir_mk`；`kernel_vfs_unlink` 实现了真正的 Linux `unlink`-but-open 语义：
- `kernel_vfs_unlink` 在需要时先从挂载的 orphan 记录池预留一个回收记录，再调用 `ext4_funlink_dentry` 从父目录中立即移除目标目录项；后续对原路径的 `open` 立即返回 `-ENOENT`，并在同名路径重新创建时分配独立全新 inode。没有现存 node 的文件也使用这份预留记录，因而 orphan 回收失败时由 mount 单独持有。
- 若目标文件当前仍处于打开状态（`node->open_files > 0`），VFS 标记 `node->unlinked = 1`，旧 open 描述符（包括只读/读写 OFD 以及正在运行的源映射 ELF 可执行文件）保留底层 inode 数据与有效物理块，继续正常执行 `read/write/fstat` 与缺页加载（demand fault）；
- 只有当最后一个打开描述符与执行租约释放（`node->open_files == 0 && node->exec_users == 0`）时，VFS 才通过专有的 `kernel_vfs_try_release_orphan` 驱动物理存储释放。该过程先持有临时节点引用并安全使页缓存失效，再在扁平调用栈上调用 `ext4_orphan_free` 截断释放底层 inode，最后标记 `node->retired = 1`。通用的 `kernel_vfs_node_release` 仅负责内存节点对象的生命周期，绝不隐式或嵌套调用 `ext4_orphan_free`，避免双重释放与页缓存回收深度嵌套额外消耗任务栈；若底层释放失败，节点转移至 `adapter->cleanup_nodes`，由 mount 保留唯一重试 owner，绝不重新指向 file。若文件在 unlink 时没有现存 node，则使用预留记录直接调用 orphan free；失败后记录留在 mount 链，路径仍保持已删除。
由于 lwext4 的 `ext4_dir_rm` 会递归删除非空目录，`kernel_vfs_rmdir` 通过 `ext4_fdir_unlink_dentry` 只摘目标目录项；后端 `ext4_dir_check_empty` 校验目录记录和 checksum、跳过 `.`/`..` 并拒绝非空目录。被持有的目录 inode 延至最后引用释放才回收；同名重建得到不同 inode，旧路径对象仍可查询零链接状态。目录项 checksum 在设置 inode 字段后计算，卸载后的 `e2fsck -fn` 验证磁盘结构。
只读挂载下，所有上述修改操作直接返回 `-EROFS`。
unmount 在仍有 open file 或路径引用时返回 `-EBUSY`。末节点 `ext4_fclose` 失败把 node 转移到 mount cleanup 链，卸载重试同一个 handle；测试注入路径末引用和重复 inode 合并两种 close 失败并确认都被实际重试。非法引用或释放顺序触发 fatal，合法 heap/page 释放不返回可重试状态。

当前 VFS 同时服务 ELF 随机读、进程文件表和文件私有缺页，但仍不是完整 Linux VFS：已支持硬链接、tmpfs 和多 ext4 挂载；仍没有负目录项缓存、逐分量权限检查、周期 writeback 或 read-ahead。`kernel_vfs_path` 持有 mount/inode 与父目录项引用；`ext4_lookup_child` 按父目录 inode 查找。统一逐分量解析处理 `.`、`..`、相对/绝对符号链接、尾斜线和最多 40 次展开；open/stat 的尾斜线按目录查找，mkdir/unlink/rmdir/symlink 保留不跟随的最终目录项语义。创建允许缺失的最终分量，并把已解析父对象及最终名称转换为 lwext4 修改接口所需的临时路径。适配缓冲按真实祖先长度分配，用户输入/符号链接展开仍限制为 4096 字节；已存在的长父链不挤占短相对输入额度，255 字节组件可用于修改。路径对象释放不依赖原始绝对路径仍存在。该规则依据固定 Linux commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e` 的 [`fs/namei.c`](../../references/linux/fs/namei.c)。fs context 和目录 fd 直接持有解析起点；绝对路径忽略 dirfd，删除或改名不会把旧引用重定向到同名新 inode。目录支持打开与按后端 cookie 查询（`kernel_vfs_dir_entry`），会返回真实的 `.`/`..` 条目；每次查询从传入的 ext4 字节位置开始，而不是从目录起点重走，顺序枚举的条目访问为 O(N)。适配层把 lwext4 的正 errno 与 EOF 分开，再向文件资源层返回负 Linux errno。页缓存只保存普通文件内容；进程层只持有 VFS mount/file 抽象，lwext4 handle 没有泄露到 task 或 syscall ABI。

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

## 当前成本边界

增长与缩小分别进入 `kernel_page_cache_extend` 和 `kernel_page_cache_truncate`。
增长只查询旧 EOF 的非对齐尾页，清零新暴露字节并重新保护别名，再发布长度；
页对齐或同长度不访问 inode 页链，也不排空历史 cleanup。真正缩小仍在撤映射后
处理越界页、尾页脏范围和 owner。该优化不改变私有 COW 副本，哈希探测与尾页
实际别名工作单独计量。`make test-cache-growth-riscv` 在 COST 构建检查
1/4/16/64 MiB × 1/4/64 KiB 写入的增长访问界及全文件内容，见[规模成本](../learning/single-hart-scale.md)。

正确性验收不等于吞吐已优化。`fs/files/io.c` 先将用户数据复制到请求缓冲，
再写页缓存；页级 staging 已减少分块和用户页解析次数，并非零复制。
`kernel_page_cache_writeback_range()` 按 min(范围页数, inode 脏页数) 选择哈希或脏链，
在首次 I/O 睡眠前分配并 pin 精确集合，再按页偏移排序。捕获区间关中断并禁止分配器
进入回收 I/O，内存不足返回 ENOMEM；等待期间其他读者插入新页不会改变已选数组。
首次标脏、映射变脏、成功写回、再次修改、截断和回收统一维护脏组织；旧代次完成不能
摘掉新修改。选择成本不包含哈希冲突、实际别名数量及选中集合排序 O(K log K)。
快照仍由独立页面持有，但只复制 dirty_begin 到 dirty_end 的实际区间；不能把省复制
解释为取消稳定版本或减少已分配快照页。连续写回候选由编译配置
`BOAROS_PAGE_CACHE_WRITEBACK_PAGES` 选择 1/2/4/8，生产默认仍为 1。
范围写回与阈值 worker 均只合并同 inode、相邻且脏字节相接的页面，最多 32 KiB；
首尾可为部分脏段，稀疏脏页与干净间隙不扩写。一次稳定快照调用现有
`ext4_fpwrite`，由它建立整请求预留和一次私有 undo 操作；数据继续交给既有
running group、封口版本与块批量写，ordered/log/commit/checkpoint 屏障不变。
页缓存清脏表示后端已接受内容，durable 仍由同步接口等待。

每个成员保留自己的 generation、writeback 状态和等待队列；旧快照完成不能清掉
等待期间的新修改。前台连续快照分配失败时在提交前回退单页；后台每实例预留
候选大小的连续快照，分配不足时逐级降到一页，启动后的批次受实际预留容量限制。
停止并 join worker 后按原 allocation order 释放预留；设备 I/O 失败不触发拆批重试。
worker 的成功/失败仍按页计，扫描轮次不作为后端批次数。

`python3 -B tests/writeback-batch-riscv.py` 使用独立构建目录验证
1/2/4/8 页与 1/4 KiB ext4，检查实际后端入口、部分首尾、范围裁剪、相邻脏段间隙、
前台 OOM 回退、后台预留降级、真实线程重脏和快照稳定、短写/EIO 脏页保留、
完整内容读回及 e2fsck。`make test-scale-riscv` 的独立 wrapper 另保护连续八页
的有界后端交接。上述是局部正确性和成本门禁；改变私有操作大小后的完整恢复矩阵
及真实程序性能测量仍需集成验收，不能用交接次数下降推导吞吐收益。

完整覆盖冷缓存页仅接受已经稳定的内核输入；页面完成初始化、标脏和长度更新前保持
loading，不先读取将被完全替换的旧页。部分页和用户 fault 留下的不足一页前缀保留旧路径。
这省去页缓存到 VFS 的旧内容预读，不承诺 ext4 的块内合并、undo 或元数据读取也为零。
小范围同步门禁用 64 MiB 文件的 16384 个实际常驻页：单页范围 1 次候选访问，
3 个脏页全范围同步 3 次访问，16 个脏页中选一页仍 1 次；1 字节脏快照只复制 1 字节。
规模准备采用稀疏零页，另独立验证第 0 页已存在且已同步的真实旧内容被完整覆盖。
重建 `make test-scale-riscv test-cache-growth-riscv`；这是成本界验证，吞吐结果另行测量。

`kernel_vfs_sync_range()` 依次交接数据、等待 full 同步序号；
`kernel_vfs_sync()` 按 datasync 选择 full/data，journal 提交线程负责屏障。
元数据随修改提交，频繁小写与逐次同步的事务/flush 放大尚需单独计量。

| 串行边界 | 当前必要契约与限制 |
|---|---|
| OFD offset | dup/fork 共享位置的操作互斥；独立 open 不共享同一 OFD 锁 |
| 命名空间 | 路径解析/修改按相应挂载锁保护；部分修改适配仍需遍历祖先构造完整路径，无负目录项缓存 |
| inode | 内容与 truncate/失效/孤儿回收相互保护；同页 miss 合并，不同页可各自等待 |
| ext4 实例 | 纯定位读共享进入，写事务及维护操作独占；不同盘有独立锁与错误 owner |
| 块队列 | 每设备最多八个在途槽；一次同步调用等待自己的请求，不能由八槽推断单次写回会填满队列；flush 排空此前请求并挡住后继 |

每盘后台 worker 使用专用快照页，按阈值和压力触发；不是周期刷盘，也不代替
fsync 的持久化与错误观察。全局内存快照 O(总缓存项数)、压力通知 O(实例数)。
已验证等待期间 CPU、无关缓存及另一磁盘有进展；尚无证据把 iozone 慢写归因于
某一把锁、flush 或扫描。成本测量与优化的候选、门禁统一见[路线](../goals.md)。

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

`kernel_vfs_file_set_times/path_set_times` 以活 inode 为目标，`ext4_file_set_times` 在单个 journal 事务内修改选定字段并记录同步依赖。两项 OMIT 不产生 I/O；其他修改要求可用 realtime，时间未初始化明确返回 `EIO`。128 字节 inode 仅秒精度；扩展 inode 保存纳秒，超出或等于时间范围端点时按 Linux `timestamp_truncate` 将纳秒置零。事务准备 ENOMEM 可回滚，关键 journal/write/flush 失败由 mount 保留并停止修改。

`kernel_vfs_mount_statfs` 使用 `ext4_mount_point_stats` 的 superblock 分配计数、UUID 和真实 overhead。首次成功统计校验块组及 journal inode，计算首数据块、super/GDT/预留 GDT、两种 bitmap、inode table 和 journal 长度，缓存静态开销，成本 O(块组数)；后续查询 O(1) 读取动态计数。当前无在线 resize，故无几何缓存失效协议；BIGALLOC 与外部 journal 不在支持范围。META_BG 主描述符取所在 meta group 的首组，sparse_super2 按备份组字段处理，组数扣除 first_data_block。

`make test-lwext4-metadata-host` 覆盖 1/4 KiB 块、128/256 字节 inode、60 个 OOM 点、写与 flush 失败的 sticky owner、可重试读失败、损坏拒绝、无关脏数据不写回、稀疏/截断/删除计数、META_BG/sparse_super2/GDT_CSUM/无 journal 和精确组边界，并运行 `e2fsck -fn`。用户层契约与固定资料见[文件模块](kernel-files.md)和[时间学习记录](../learning/file-timestamps.md)。

元数据收口历史验证：当时 `build/recoverable-fs-host-final.log` 的全部 host/断电矩阵通过；`build/recoverable-fs-regression-final.log` 的 RISC-V 全套、真实 musl/pthread、320 条固定 Linux 差分、1000 个函数栈界及工具自测通过。日志已清理；最大单函数当时为 kernel_main 的 1952 字节，assembly trap 288、保留 1024。这不是实板性能或 SMP 验证。

## 单 hart 可睡眠存储契约

锁序为 OFD offset（10）→ inode 写操作门闩（15）→ 命名空间（20）→ 按稳定 node 身份排序的 inode → 后端读写锁。VFS 缓存命中不需取得后端锁；纯定位读 `ext4_fpread` 使用独立游标和共享后端锁，写入、事务提交/回滚、孤儿回收、恢复、journal start/stop 和卸载持独占锁。事务只允许同一任务 owner 嵌套；写者排队后不再放入新读者。RELATIME 先在读阶段判断，确需更新时间才在释放读锁后进入独占阶段，不允许读锁升级。

同页 miss 先发布 loading 项，等待者合并到该项；不同页可各自等待设备。失败保留所属 errno，ENOMEM 不映射成磁盘错误。缓存索引/LRU/引用变更不跨睡眠；块缓存的 loading 与 wait/wake 同样合并同块读取。此同步依赖单 hart 内核不可抢占、显式等待才调度，不是 SMP 协议。

写回固定本次页快照、脏范围与 generation；等待期间映射的新修改属于下一代，旧完成不清除它。范围写回在首次等待前固定并 pin 精确页集合，不能重新遍历变化中的链表配对释放。truncate/失效/最后 orphan close 取得 inode 写锁并复查 owner，排除 loading/writeback；最后 close 不在活动写回上强行失效。

每 inode 的写操作门闩覆盖整次 write/writev/pwrite、append、O_SYNC/O_DSYNC 收尾以及 truncate；独立 OFD 也不能在页/iovec 边界交错同次写。内层 pwrite/append 借用外层门闩，不递归加锁。用户复制期间只保留 OFD offset/操作门闩，不持 inode 数据锁；缺页、读和写回不取得操作门闩，允许用户源缓冲映射同一 inode。文件写仍先复制到有界请求页再进入存储，保留短写及 EFAULT/EIO；这里的操作互斥不承诺断电原子性。持锁分配、堆和 MM 元数据操作只允许非阻塞干净页回收；脏页回收在外层取得 inode try-read 后执行，完成后复查页引用与别名。干净页回收立即释放正常 node 元数据：固定版本 `ext4_fclose` 只清理私有 handle，不取后端锁或执行 I/O。只有未完成 orphan 或真实关闭错误交给 mount 清理链，不让健康节点滞留至卸载。

验证入口：`make test-io-sleep-riscv test-files-riscv test-files-partial-write-riscv test-lwext4-recovery-host`。设备握手、同页合并、快照、并发插入缓存页、最后 close/写回及完整恢复的证据见[可睡眠存储](../learning/sleepable-storage.md)。

`kernel_vfs_mknod_at` 在 namespace 锁内定位未存在的末级名字，普通文件、字符/块设备及FIFO
创建与 mode 设置放在同一个 lwext4 写事务内。失败回滚由原 mount/日志 owner 处理；
该入口不实现 devfs，不增加新的设备后端。只读根返回 EROFS，创建不占用进程 fd。

## 后台写回与水位回收

每个当前页缓存实例拥有一个 joinable worker；worker 借用 cache/record，调用者必须保持它们有效直到 stop/join 返回。根盘运行期启动、用户磁盘挂载时创建，所属实例卸载前停止并回收。空闲低/高水位为受管页 2%/4%，脏页启动/停止为 10%/5%，小配置至少一页且阈值严格有序。没有周期清脏。分配器和持锁路径只发布请求，低水位先回收无别名/额外引用的干净页，再写回合格脏页；仅脏阈值触发则保留清洁缓存。

每批最多扫描 64 个哈希槽并让出 CPU；写回跨睡眠固定 entry 和 inode，保存槽游标而不保存可能失效的 LRU 指针。每轮代次防止扩容或重新变脏导致重复处理，无进展即等待新事件。启动持有专用 4 KiB 写回快照，后端 metadata 仍可能 ENOMEM，不是完整应急池。映射重新设保护与 generation 协议保持不变，旧完成不能清除新修改。写回错误留在 inode/mount，当前失败页在成功前不计入 MemAvailable；同一轮继续处理其他对象。后台完成不替代 fsync/fdatasync 的错误观察与 flush。

统计入口同时记录 worker 扫描、写回、失败、批次和实际释放量；内存快照单次 O(驻留页数)，分配通知为 O(1)。`test-io-sleep-riscv` 在 4 MiB 测试池上触发默认比例，覆盖单次 EIO 后继续进展、错误观察、再次修改仍排除失败页、2% 到 4% 回收、固定页不被驱逐、启动 OOM、暂扣在途写回时 stop/join 等待且不继续提交、退出资源基线；共享映射睡眠期间修改、孤儿关闭和设备延迟使用同一写回协议的既有聚焦测试。测试边界为单 hart，不推出 SMP 或实板持久性。

## 硬链接与第二磁盘

RV64 linkat 由 files 层固定源 inode/OFD，VFS 在目标父目录锁下提交后端操作；
flags 0、AT_SYMLINK_FOLLOW、AT_EMPTY_PATH 不重新解析 fd 的旧名字。目标已存在、
跨挂载、目录源、只读和零链接边界按固定 Linux 差分覆盖。不同名字共享 inode、
页与记录锁，但保留独立路径身份及独立打开的 OFD。unlink/rename 替换只在最后
链接消失后进入 orphan 生命周期。ext4 在同一日志事务更新目录项、nlink 和时间；
非 journal 修改仍明确不支持，不缩减原恢复契约。

DTB 枚举的每块设备独立维护队列、IRQ、DMA 和超时。设备登记、文件系统实例、
挂载树节点分别拥有生命周期；mknodat 块节点仅用于 stat、命名和 mount 的 st_rdev
识别，裸盘 open 返回不支持。同一设备重复独占挂载为 EBUSY。lwext4 注册名及内部
路径按实例生成，生成配置容纳八实例；root 单盘入口仍可使用。

`fs/disk_mount.c` 拥有动态 mount/cache/source 名称。构造中的实例不进入可清理
状态；真正 I/O 清理失败才移交待清理注册表。关机重试逐个认领 owner，不跨睡眠
借用链表 next，不在同轮反复重试同一失败。没有 I/O owner 的分配失败立即回滚。
proc mounts 保留用户给出的磁盘来源名，并正确标识 tmpfs。

全局缓存快照 O(总项数)，压力通知和脏阈值判断 O(实例数)。水位仍以全局受管页
计算，只扣一次低水位预算。安全分配失败至多等待共同一轮的首次实际释放，或所有
参与 worker 完成，不逐盘串行等待。测试入口包括 test-io-sleep-riscv 的两个非空
缓存、test-root-multi-block-riscv 的真实双盘与重启，以及 test-lwext4-instances-host。

成本观测分别记录缓存范围遍历、完整页快照和逻辑后端写回；真实扇区仍列 unknown，不由后端入口推断 data/metadata/journal，见[成本观测](kernel-cost.md)。

成本隔离探针：`make test-lwext4-cost-host` 使用实际 lwext4 的公开 touch/fwrite 接口、
独立 block_fault 设备计数和三个新生成的 journal 镜像，对比128次纯时间更新、
4KiB热覆盖及组合操作；核对时间戳、读回内容及fsck。它测量后端请求数量，
不经过VFS页缓存，不包含QEMU设备延迟，也不证明原iozone的CPU或总耗时占比。
当前成本及可重建配置见[成本基线](../learning/cost-baseline.md)。

`make test-journal-group-riscv` 在实际 IRQ 块 I/O 下验证 128 页内容、预算、100ms 期限、full/data 同步、truncate、unlink-but-open、最终卸载；观测构建亦检查后台来源明确的数据请求。`make test-journal-idle-negative-riscv` 禁用公共 idle 返回钩子，验证无 timer 的同一设备 IRQ 无法在 enable-to-wfi 返回边界运行等待者；正常构建该边界通过。`orphan_file` 的 slot 更新必须通过 `ext4_trans_block_get` 在修改前保留 undo/version，不能直接 get 后补记 dirty。

异步准备事务会改变故障执行器握手后的 I/O 前缀，SQLite 恢复 fixture 在 `mutation armed` 前同步根目录，排空 loader/control/SQLite 准备元数据，再开始本次数据库变更的故障计数。页缓存短写/重脏注入拦截实际 `ext4_fpwrite` 入口，避免链接器无法拦截同 translation unit 的内部调用。

接近满盘时，若释放位图中的空闲块/inode 仍归运行或提交事务的 checkpoint 所有，下一操作先释放后端锁等待已接受序号，而不是返回暂时性的 ENOSPC。近满盘宿主 fixture 在回收 checkpoint I/O 中尝试另一文件扩展，旧实现未走等待边界，修正后明确等待、checkpoint 后重试成功，随后 e2fsck 正确；正常 RV64 组提交/回收亦通过。

## lwext4 块缓存命中与回收

`ext4_block_get_noread()` 在设备/LBA检查后先 `ext4_bcache_find_get()`。命中只取得引用，
不为不存在的新分配提前回收；未命中才 shake，再由 allocator 再查一次，覆盖回收睡眠
期间其他任务先加载同块的交错。有效内容、loading等待、加载错误、dirty及journal_pending
沿原owner处理。共享后端读取不刷脏回收。生产目标保持8块（4KiB时32KiB），是回收目标，
不是引用块驻留上限。`make test-lwext4-cache-host`保护循环热块、固定引用、加载/回收/OOM
失败与最终释放；真实I/O等待和恢复仍由io-sleep、journal及SQLite回归保护。

根挂载从已登记块设备的device_number生成`/dev/block/<major>:<minor>`来源（boot首盘252:0）。
字符串在mount adapter内由mount持有，不借用启动栈；卸载清空来源指针后释放adapter。
动态挂载仍由自己的source owner管理。公共程序镜像创建相应块节点，proc mount快照
沿现有转义规则输出，原df根盘行与statfs容量/空闲/类型另做内容核对。

COST构建在成功freeze处记录封口条件，并在挂载点wait记录sealed/durable/checkpoint
经过时间；失败freeze不增加成功原因。普通构建不增加这些观测，原因可重叠且等待
不能跨任务相加。旧策略定点对照保留64次操作条件；当前已删除该条件及运行组计数字段，
相同块的重复修改不单凭调用数封口。100ms是首次脏化起算的软触发，不是持久化
完成期限。关闭观测三个启动的自动程序加durable中位由11.541/12.108降到5.129/5.359秒；
最终整启动根卸载余量约0.04秒，未把checkpoint成本隐藏为应用收尾。组数510→149，
FLUSH1547→677，但准备读取和前台固定工作仍在；完整分布、计费峰值、风险和重建见
[本轮结果](../learning/cost-baseline.md#版本量封口与socket纠错对照2026-10-02)。诊断契约见[成本模块](kernel-cost.md)。

本轮宿主回归以固定时钟下128次同块修改证明不会额外封口或重新分配组载荷；
年龄在100ms触发，48/96页独立内容在固定时钟下触发版本量边界并校验内容。
真正sealed FIFO满、预算与近满盘复用等待继续保护；后操作OOM、旧冻结版本、
环绕、commit后checkpoint前恢复和WRITE/FLUSH失败通过`make test-lwext4-group-host`。

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


## 缓存版本内的批量读

内部 `kernel_vfs_node_pread_batch` 接受最多八个独立输出区段。caller 在整个调用期间
持有 inode/read gate 和所有输出；全部参数先检查，逐项返回连续成功前缀与负 errno，
无批量后端时执行独立标量回退，不隐含完整批次成功或原子性。
ext4 的 `ext4_fpread_batch` 持 mount read lock，按 extent/传统块映射处理洞、EOF 和
块内片段；真实缺失块才进入最多八项的 `bread_batch`。`ext4_block_get_batch` 引用
当前 bcache，dirty/journal-pending 数据优先；相同块的多个片段共用一次加载。
为避免交叉等待尚未发布的 loading，本批自有加载全部发布、完成并唤醒后，才等待
其他 owner 的既有加载。失败块释放本批引用，其余项可成功；读失败不记录为 inode
写回错误。设备发布后的输出由块层持有至全部完成或确认 reset。

`make test-lwext4-cache-host test-lwext4-batch-read-host` 覆盖共享加载、旧缓存版本保护、
OOM、独立错误与重试；后者运行 1/4/8 KiB ext4、extent/传统映射的洞、EOF 和片段。
`make test-io-sleep-riscv` 另用真实八页 VFS batch，要求在释放任何响应前发布八项，
并验证保留最后一个 DMA 时取消不能提前返回。普通页缓存的顺序预读通过下述候选接入；该内部接口门禁不代表用户 read 的吞吐收益。


深度至少 2 的 extent 树截断会修改非根内部索引；每次路径上移归还该引用前，必须
通过 `ext4_ext_drop_refs` 重算已修改块的校验和。直接归还并清空块号会绕过最终
checksum 更新，使下一轮合法查找返回 EUCLEAN。`make test-lwext4-deep-truncate-host`
构造深层稀疏树，验证 1/4 KiB 文件系统部分截断、跨进程重启读回、截零和 e2fsck。


## 有界顺序预读候选

`BOAROS_PAGE_CACHE_READAHEAD_PAGES` 支持 0/1/2/4/8，默认 0；只有非零候选才创建
每缓存实例的独立预读 worker。OFD 保存成功读取的末端和取消 cookie；read/pread/
sendfile 在首次可能睡眠的 accessed、后端或 usercopy 之前执行 read_begin，立即取消
不连续预测，read_progress 只登记成功前缀。两个连续片段在页边界完成后可申请下一窗口。
seek、非顺序读、最后关闭、截断/失效、低水位和停止取消未接纳工作。

队列固定八个 job，只持 inode pin；一实例同时处理一个最多八页批次。worker 取得
inode read gate，复用 demand/full-overwrite 的 loading 页准备；已存在的页或加载由
原 owner 处理，预读不交叉等待它。需求读可订阅已发布 loading，只有成功页面对外
可见；错误页按用户引用收口，无关页面独立成功，也不记作 inode 写回错误。
准备过程禁止分配回收 I/O，取消时已经有需求读者的页仍保留加载 owner。

接纳点是进入一次 VFS batch：此前可以撤销，之后完整排空最多 32 KiB 的批次，
包括 1 KiB ext4 内部的多轮块读，不声称逐 DMA 取消。stop 先撤未接纳 job，再 join
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

存储 20 组候选及 26 项扩展/同步负载已用发布构建三次独立启动比较，默认仍为 RA0/WB1。RA8 改善所测顺序冷读、WB8 改善显式同步，但 WB8 在 64 MiB 缓存追加有回退；冷热、tmpfs、缓存完成与 durable 同步分列于[报告及每文件完成时间](../learning/data-path-budget-experiments.md#正式匹配结果2026-10-06)。

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
