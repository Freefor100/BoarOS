# 存储与文件系统学习总结

本文整理从块设备读取 ext4、把文件作为可执行映像来源并向进程提供文件描述符时需要掌握的知识、BoarOS 当前选择及验证经验。稳定接口见[RISC-V VirtIO MMIO 块设备](../modules/riscv-virtio-block.md)、[VFS 与 ext4](../modules/vfs-ext4.md)、[进程文件资源](../modules/kernel-files.md)和[RISC-V 根启动](../modules/riscv-root-boot.md)。

## 从设备到文件的层次

块设备只认识有容量边界的扇区或字节范围，不认识目录、文件名和权限。文件系统把 superblock、block group、inode、extent 和 directory entry 解释成文件；VFS 再给进程和可执行装载器提供稳定的 mount/file/read 接口。把这几层分开，才能让 ext4 复用不同的存储后端，也让同一块设备以后承载其他文件系统。

VirtIO 也分 transport 与 device type。MMIO 或 PCI transport 规定寄存器、设备发现和队列配置；virtio-blk 规定 sector、capacity 和请求类型；split virtqueue 是 descriptor/available/used 三部分共享内存协议。设备与驱动通过状态位和 feature negotiation 确认共同能力，不能看到 VirtIO magic 就假定是块设备，更不能把 legacy 与 modern 队列布局混用。

DMA 的地址是设备可见地址，不等于任意内核虚拟地址。QEMU `virt` 当前无 IOMMU、RAM 有固定 direct map，因此可把 direct-map VA 转回 PA；真实开发板还必须核对 DMA 可达位宽、cache coherency、内存屏障和 IOMMU。对齐的最终目标缓冲区可以 direct DMA；非整扇区范围需要 bounce，避免设备覆盖调用者未请求的前后字节。

## 为什么当前是同步 I/O

首个存储消费者发生在单 hart 启动期，尚无外部中断控制器、等待队列与阻塞调度。一个 outstanding request 加有界轮询能形成真实 I/O 闭环，并把过渡复杂性限制在设备后端。其缺点是等待期间 CPU 忙等且不能并行 I/O；建立 IRQ 和 sleep/wake 后，应替换完成方式而保留块设备和 VFS 语义。

当前 ext4 既可挂载为只读，也可在块设备提供写回调时挂载为读写；同步轮询只解决首个单 hart 消费者的 I/O 边界。写路径增加了介质更新、页缓存失效和真实清理错误，不能把它们简化成分配器重试。ext3/4 若带 `needs_recovery`，最近的元数据事务可能只在 journal 中；没有 JBD2 replay 的实现必须拒绝挂载。metadata checksum 还要求根据 incompat feature 在 superblock checksum seed 与 UUID 派生 seed 之间正确选择，不能因镜像“能列目录”就认定所有元数据校验正确。

BoarOS 引入固定 lwext4 源码快照，自有 block/VFS 接口保持在外层。这样避免从零实现 ext4 inode、extent、目录索引和 checksum 的高风险，同时不让第三方结构成为未来进程 ABI。代价是需要维护 freestanding libc/allocator adapter，并承担组合后的 GPL 许可证约束。

## 文件随机读与 ELF

ELF header、program header 和各个 `PT_LOAD` 位于文件不同偏移。接口若只接受完整连续 buffer，会要求启动时整文件常驻并再复制到用户页；接口若暴露文件系统 handle，又把 ELF 层绑定到 ext4。带总长度的 `read_at(context, offset, destination, length)` 是更稳定的中间语义：解析器先做范围检查，内存和 VFS 各提供 adapter，段内容直接读入目标用户页。

“read_at 返回零”必须表示精确填满请求。文件变短、EOF 内短读或底层 I/O 错误不能留下半个 program header 后继续解析。格式或 I/O 失败时，输出 ELF image 与最终用户空间保持不变；已经取得的临时页由明确 owner 回收。这个接口以后可以接页缓存或按需分页，但本身不承诺缓存、异步 I/O 或 mmap。

## fd、打开文件描述与文件系统上下文

Linux 进程看到的整数 fd 只是文件描述符表的索引。槽内的 descriptor flags（典型例子是 `FD_CLOEXEC`）属于 fd；真正的打开文件描述（open file description）保存文件位置、打开状态和底层文件引用。`dup` 产生两个 fd 指向同一打开文件描述，所以共享 offset；两次 `open` 同一路径则产生两个描述，offset 独立。`fork` 通常复制 fd 表引用而共享打开文件描述，`CLONE_FILES` 才共享整张 fd 表。Exec 不替换文件表，只关闭标记了 `FD_CLOEXEC` 的槽，因此其他 open-file offset 要继续累积。把 offset 直接放进 fd 槽虽然早期简单，却会阻碍这些既定 Linux 语义。

路径解析需要另一组进程状态：根目录、当前工作目录和用于相对路径的目录 fd。Linux `openat` 对绝对路径忽略 dirfd；相对路径的 `AT_FDCWD` 表示从 cwd 开始，其他值必须引用有效目录 fd。BoarOS 当前只有单根 mount、cwd `/` 和 `AT_FDCWD`，但把 fs context 与 fd table 分开，是为了让以后 `CLONE_FS` 与 `CLONE_FILES` 独立控制共享关系，而不是把两类资源固化为同一个对象。

Linux `read` 的返回值不仅取决于磁盘读取结果，还取决于数据实际交付用户空间的程度。若第一字节就无法写入用户 buffer，应返回 `-EFAULT` 且不推进文件位置；若已经复制一段连续前缀，之后 fault 或 I/O 出错，通常返回已复制长度并只推进这部分。用内核 staging buffer 时，不能把“已从文件系统读入”误当成“已交付用户”：open-file offset 必须按 usercopy 成功字节提交。零长度读仍先要求 fd 有效，但不应解引用用户地址。

目录位置也属于 open file description，而不是 fd 槽或 inode。固定 Linux 快照
`f4cdf7ca9a1f` 的 [`fs/readdir.c`](../../references/linux/fs/readdir.c) 由 `iterate_dir()` 在
open file 的 `f_pos` 与 `dir_context.pos` 间传递位置，并由 `filldir64()` 把可恢复位置写入
`d_off`；[`fs/ext4/dir.c`](../../references/linux/fs/ext4/dir.c) 还显示线性目录和 htree 目录
可使用不同形态的 cookie，所以调用者不应对它做序号算术。固定 musl 1.2.5 压缩包
[`musl-1.2.5.tar.gz`](../../references/musl/musl-1.2.5.tar.gz) 中的
`src/dirent/readdir.c` 把返回项的 `d_off` 保存为 `DIR.tell`，`src/dirent/telldir.c` 返回该值，
`src/dirent/seekdir.c` 用它执行 `lseek(fd, off, SEEK_SET)` 并清空用户态目录缓冲。因此内核若承诺
`telldir`/`seekdir` 可恢复枚举，必须让目录 `lseek` 接受先前返回的 cookie，并使下一次
`getdents` 从相应位置继续；cookie 的编码仍由文件系统适配层掌握。

BoarOS 的线性 ext4 适配器现在把每条记录结束的字节位置作为 cookie，OFD offset 保存下一条记录
的位置；独立 `open` 得到独立游标，`dup`/`fork` 因共享 OFD 而共享游标。`getdents64` 只有在整条
记录完成 usercopy 后才提交 offset，缓冲区不足或坏指针不会跳过该记录。lwext4 的错误返回入口
把 inode 为零的目录尾记录与真实 EOF 区分，并把块读取/格式错误转换为 `-EIO`，避免把损坏目录
误报成正常结束。该设计使顺序目录枚举从旧的反复重走 O(N²) 变为每条记录一次推进的 O(N)；
这是条目访问的结构性结论，不等同于已经测得的 QEMU 或开发板吞吐提升。

BoarOS 当前让 `read` 按文件页从共享页缓存取得内容，再复制到用户页。命中避免重复 ext4/块 I/O，但仍有 cache-to-user 复制和用户页软件遍历；read-ahead、固定用户页后的直接 I/O 或异步请求仍未实现。无论数据来自磁盘还是缓存，都必须保持 fd/open-description 分层、短读、offset 与 errno 语义：只有实际交付用户的前缀才能推进 offset。性能取舍需要在 QEMU 和开发板上用文件大小、顺序/随机模式、page fault 比例及 cache/TLB 数据说明，不能只比较函数层数。

## 页缓存、私有映射与文件尾

页缓存要按“文件对象身份 + 页号”而不是 fd 或 open offset 建键。fd 会关闭和复用，两次
open 的 offset 也应独立；底层 VFS node/inode 身份才表达“这是同一文件内容”。缓存项持有
node 和物理页引用，调用者临时 acquire 页引用，因此驱逐只能选择没有外部引用的页面。
开放寻址哈希适合 fault/read 的查找热路径，LRU 链用于内存压力下选择冷页；两者承担不同职责。

`MAP_PRIVATE` 的读 fault 可以直接把只读缓存页映入多个地址空间，写 fault 则必须保持文件
和其他映射不变。常见做法是把缓存映射标记为 COW：若页面已经缓存，首次写复制一页；若
write-first 且缓存未命中，直接把文件内容读入私有页可避免“先填缓存、马上再复制”的双分配。
这项优化不能改变后续另一个只读映射看到原文件内容的语义。

文件长度不是 VMA 长度。映射可以延伸到 EOF 之后：包含文件末字节的最后一个页，其页内剩余
字节读取为零；但故障页的起点已经在 EOF 之外时应产生 `SIGBUS`。因此缓存需要同时返回尾页
有效字节数，MM 需要在取页前按页起始 offset 分类。关闭 fd 也不能撤销映射，MM 必须独立持有
文件引用；fork 后父子各自持有一份来源引用，直到各自最后一个相关 VMA 被撤销。同一 MM 对同一 OFD 的重复映射仍只保留一个来源 owner，临时 pin 在成功提交后立即释放。

内存压力回收必须避免无界递归。BoarOS 的物理分配器只注册一个缓存回收器：第一次分配失败
时请求 LRU 释放目标页数并重试一次，回调期间抑制再次进入回收器。当前只读文件系统没有脏页，
所以未固定缓存页可以直接丢弃；加入可写映射后，clean/dirty/writeback/error 状态会成为新的
生命周期，而不是给现有驱逐函数加一个无条件写盘调用。

## 可写文件系统与介质写入演进

从只读走向可写是内核文件系统的关键跨越，涉及块驱动、VFS、文件资源和缓存一致性多个层次：

1. **VirtIO 块设备写驱动与只读协商**：
   - 块设备写请求采用 `VIRTIO_BLOCK_REQUEST_OUT` 类型，与读取不同，数据描述符**不得**带有 `VIRTQ_DESC_WRITE` 标志（对设备而言是只读输入）。
   - 非整扇区写入通过 512 字节 bounce buffer 执行读-改-写（RMW）：先读入包含目标偏移的完整扇区，将修改字节合并入缓冲，再整扇区写回介质，避免破坏相邻数据。
   - 特性协商与只读降级：QEMU 或虚拟化平台在指定 `readonly=on` 时会提供 `VIRTIO_BLK_F_RO`（bit 5）。驱动在探测阶段读取 low 32-bit 特性，若包含只读标志则回写确认该特性，并将 `block.write` 置空（0）。上层 VFS 通过 `kernel_vfs_mount_is_readonly()` 感知该状态，避免在只读介质上尝试写回超级块/日志导致挂载失败（如错误码 5/EIO）。

2. **lwext4 写路径与 POSIX 语义修正**：
   - 普通文件创建调用 `ext4_fopen2`。普通写调用 `kernel_vfs_pwrite`（`ext4_fseek` + `ext4_fwrite`）；追加写入由 `kernel_vfs_append` 在底层原子解析当前 EOF 并写入，确保多 OFD 或与 `lseek` 组合时，写入点严格原子重定位到文件尾并推进 offset。
   - 文件大小截断通过 `kernel_vfs_ftruncate` 实现：向下截断调用 `ext4_ftruncate` 释放末尾块；向上截断由于 lwext4 原生实现只做向下收缩，VFS 适配层通过连续写零填充至目标长度，严格符合 POSIX 中“文件扩展部分读取为零”的契约。
   - 可执行映像互斥（`ETXTBSY`）：VFS node 维护 `write_openers` 与 `exec_users` 计数器。打开已在运行的可执行二进制请求写权限（或带 `O_TRUNC`）返回 `-ETXTBSY`（错误码 26）；已被写打开的文件被 `execve` 装载时同样返回 `-ETXTBSY`。写租约与执行租约在 `kernel_vfs_close` 时对称释放。
   - 目录与删除语义：lwext4 的 `ext4_dir_rm` 默认递归删除，VFS 适配层在 `kernel_vfs_rmdir` 中将判空逻辑封装进独立函数 `check_directory_empty`，遍历目录项（跳过 `.` 和 `..`），存在子条目时准确返回 `-ENOTEMPTY`，并在进入递归删除前及时退出该函数，避免 304 字节的 `ext4_dir` 局部栈帧堆叠在递归删除调用链上。对于文件删除，实现真正的 Linux `unlink`-but-open 语义：`kernel_vfs_unlink` 调用 `ext4_funlink_dentry` 从目录中摘除 dentry，使路径查找立即返回 `-ENOENT`。若文件仍被打开或作为源可执行文件映射运行（`node->open_files > 0 || node->exec_users > 0`），标记 `node->unlinked = 1`，保留底层 inode 块数据供已有描述符正常 `read/write/fstat` 与缺页加载；只有当所有打开描述符和执行租约释放时，才由 `kernel_vfs_try_release_orphan` 显式临时 pin 节点、排空页缓存后在顶层栈帧执行一次 `ext4_orphan_free`。通用的 `kernel_vfs_node_release` 仅回收内核对象内存，不隐式销毁磁盘 inode，防止深度递归栈击穿与 double free。
   - 任务内核栈深约束：BoarOS 的 4 KiB 任务页被 `struct kernel_task`（约 1.5 KiB）和内核执行栈共用，留给系统调用与中断嵌套的栈空间仅约 2.5 KiB。在进行 VFS 目录操作与多层文件系统操作时，避免在栈上直接实例化 304 字节的 `ext4_dir` 或 264 字节的 `ext4_direntry`；目录文件句柄打开应采用专有的 `ext4_dir_open_file` 直连 `node->file`，目录项遍历直接复用 `directory.de`，并在 `unlink` 前消除重复的路径属性查找（`ext4_mode_get`），确保当硬件定时器中断在深层块设备写入时发生时，中断 Trap 帧（288 字节）有充足的安全栈余量，绝不击穿栈底保护金丝雀（Canary）。

3. **页缓存失效（Page Cache Invalidation）**：
   - 当文件被 `write` 或 `ftruncate` 改变时，`kernel_page_cache_invalidate_node()` 从哈希表与 LRU 链中精确摘除该 node 关联的所有物理页项并释放页引用，确保后续的 `read` 或缺页重新从磁盘介质加载最新数据。
   - 当执行文件删除（`unlink`）时，若目标文件未被打开，立即通过 `ext4_orphan_free` 截断释放并使缓存失效；若目标文件仍被打开，页缓存继续为现有描述符与缺页服务，直至最后一次 `close` 释放孤儿 inode 时同步调用 `kernel_page_cache_invalidate_node()`。unlink 前会为无现存 node 的路径预留 mount orphan 记录；`ext4_orphan_free` 的真实 I/O 错误由 mount 保留唯一 owner，路径不会复现，重试成功后才完成卸载。

## 根设备与 PID 1

无命令行解析阶段需要一个确定的根选择规则。BoarOS 当前使用 DTB 翻译后的物理 MMIO 地址排序，选择第一个成功初始化的 block device；选中后若不是可挂载 ext4 或缺少 `/init`，启动失败，不扫描磁盘内容寻找替代根。这让平台拓扑决定设备顺序，行为可复现；以后支持 Linux `root=` 时可在块设备身份层增加显式选择，而不改变 ext4/VFS。

PID 1 是用户空间生命周期的根。Linux 通常在 init 退出时 panic，因为继续运行已没有负责收养孤儿和维持用户空间的进程。BoarOS 已实现普通父子进程、reparent、zombie/wait、标准信号 handler 和同步故障终止状态；pipe endpoint 也在同一 task 资源回收边界内关闭。PID 1 及全部后代退出后，scheduler 先关闭 fd、pipe、释放 fs context、地址空间、PID 和任务页，再由根启动 purge 文件缓存、卸载根并关机。顺序不能倒置：OFD、MM 文件 backing 与缓存 node 都借用 mount，必须先释放这些引用。完成记录必须保存 TID/TGID 快照，否则任务页和 PID 被释放后就无法可靠判断退出者身份。

## 验证经验

- 块设备测试要覆盖 direct DMA、bounce、批量合并、最后一个扇区、整数溢出、timeout 后 reset 和队列页回收。
- 文件系统测试应建立真实镜像并通过工具设置 mode、checksum 与 incompat feature；只用手写 superblock fixture 很难覆盖 extent、目录和校验链。
- 成功读取文件不足以证明生命周期完整；应在 open file 时验证 unmount 为 busy，并在 close/unmount/device destroy 后比较物理页和 heap live/current pages。
- 进程文件测试还应覆盖最低 fd 复用、扩容边界、两次 open 的独立 offset、路径 NUL 上限、跨页 usercopy、部分 fault 后 offset，以及 close 已摘除 fd 后真实 VFS/I/O owner 的状态。
- pipe 测试要覆盖两端引用、≤PIPE_BUF 写原子性、读写阻塞/`O_NONBLOCK`、EOF、EPIPE/SIGPIPE、FIFO `fstat`/`ESPIPE` 和创建/关闭时的 VFS/I/O owner。
- 页缓存测试要区分 hit/miss、尾页有效长度、被映射页 pin、LRU 驱逐和分配失败触发的有界回收；file-private mmap 还要验证写后其他别名与文件内容不变、关闭 fd 后仍可 fault、fork 后来源有效，以及整页越过 EOF 的 `SIGBUS`。
- Exec 文件测试要同时保留普通 fd 和 CLOEXEC fd：新映像应从普通 fd 的原 offset 继续读取，而 CLOEXEC fd 即使底层 VFS/I/O close 需要后续处理也必须立即不可见；失败的 exec 则不能关闭任何 fd。
- 根启动 fixture 应独立链接并写入磁盘，不能把 ELF 同时嵌入 kernel，否则无法证明 VFS 是生产数据来源。
- QEMU 默认可能提供 legacy VirtIO MMIO；驱动和生产根盘必须分别验证默认 legacy 与显式 `virtio-mmio.force-legacy=false` 的 modern 路径。legacy 的 `GuestPageSize/QueueAlign/QueuePFN` 与 modern 的 64 位队列地址不能混用；开发板 transport 和 DMA 一致性必须重新验证，不能从 QEMU 行为外推。

## 资料依据

- [VirtIO 1.3](https://docs.oasis-open.org/virtio/virtio/v1.3/virtio-v1.3.html)：modern transport、设备状态、feature negotiation、split virtqueue 和 virtio-blk。
- `references/qemu/hw/virtio/virtio-mmio.c` 与 `references/qemu/hw/block/virtio-blk.c`：QEMU VirtIO MMIO 和块设备行为参照。
- `third_party/lwext4/` 与上游文档：ext4 数据结构和当前库实现。
