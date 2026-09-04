# 存储与文件系统学习总结

本文整理从块设备读取 ext4、把文件作为可执行映像来源并向进程提供文件描述符时需要掌握的知识、BoarOS 当前选择及验证经验。稳定接口见[RISC-V VirtIO MMIO 块设备](../modules/riscv-virtio-block.md)、[VFS 与只读 ext4](../modules/vfs-ext4.md)、[进程文件资源](../modules/kernel-files.md)和[RISC-V 根启动](../modules/riscv-root-boot.md)。

## 从设备到文件的层次

块设备只认识有容量边界的扇区或字节范围，不认识目录、文件名和权限。文件系统把 superblock、block group、inode、extent 和 directory entry 解释成文件；VFS 再给进程和可执行装载器提供稳定的 mount/file/read 接口。把这几层分开，才能让 ext4 复用不同的存储后端，也让同一块设备以后承载其他文件系统。

VirtIO 也分 transport 与 device type。MMIO 或 PCI transport 规定寄存器、设备发现和队列配置；virtio-blk 规定 sector、capacity 和请求类型；split virtqueue 是 descriptor/available/used 三部分共享内存协议。设备与驱动通过状态位和 feature negotiation 确认共同能力，不能看到 VirtIO magic 就假定是块设备，更不能把 legacy 与 modern 队列布局混用。

DMA 的地址是设备可见地址，不等于任意内核虚拟地址。QEMU `virt` 当前无 IOMMU、RAM 有固定 direct map，因此可把 direct-map VA 转回 PA；真实开发板还必须核对 DMA 可达位宽、cache coherency、内存屏障和 IOMMU。对齐的最终目标缓冲区可以 direct DMA；非整扇区范围需要 bounce，避免设备覆盖调用者未请求的前后字节。

## 为什么当前是同步只读

首个存储消费者发生在单 hart 启动期，尚无外部中断控制器、等待队列与阻塞调度。一个 outstanding request 加有界轮询能形成真实 I/O 闭环，并把过渡复杂性限制在设备后端。其缺点是等待期间 CPU 忙等且不能并行 I/O；建立 IRQ 和 sleep/wake 后，应替换完成方式而保留块设备和 VFS 语义。

只读 ext4 降低的是写回、崩溃一致性和 journal replay 范围，不代表所有磁盘都能安全读取。ext3/4 若带 `needs_recovery`，最近的元数据事务可能只在 journal 中；没有 JBD2 replay 的实现必须拒绝挂载。metadata checksum 还要求根据 incompat feature 在 superblock checksum seed 与 UUID 派生 seed 之间正确选择，不能因镜像“能列目录”就认定所有元数据校验正确。

BoarOS 引入固定 lwext4 源码快照，自有 block/VFS 接口保持在外层。这样避免从零实现 ext4 inode、extent、目录索引和 checksum 的高风险，同时不让第三方结构成为未来进程 ABI。代价是需要维护 freestanding libc/allocator adapter，并承担组合后的 GPL 许可证约束。

## 文件随机读与 ELF

ELF header、program header 和各个 `PT_LOAD` 位于文件不同偏移。接口若只接受完整连续 buffer，会要求启动时整文件常驻并再复制到用户页；接口若暴露文件系统 handle，又把 ELF 层绑定到 ext4。带总长度的 `read_at(context, offset, destination, length)` 是更稳定的中间语义：解析器先做范围检查，内存和 VFS 各提供 adapter，段内容直接读入目标用户页。

“read_at 返回零”必须表示精确填满请求。文件变短、EOF 内短读或底层 I/O 错误不能留下半个 program header 后继续解析。格式或 I/O 失败时，输出 ELF image 与最终用户空间保持不变；已经取得的临时页由明确 owner 回收。这个接口以后可以接页缓存或按需分页，但本身不承诺缓存、异步 I/O 或 mmap。

## fd、打开文件描述与文件系统上下文

Linux 进程看到的整数 fd 只是文件描述符表的索引。槽内的 descriptor flags（典型例子是 `FD_CLOEXEC`）属于 fd；真正的打开文件描述（open file description）保存文件位置、打开状态和底层文件引用。`dup` 产生两个 fd 指向同一打开文件描述，所以共享 offset；两次 `open` 同一路径则产生两个描述，offset 独立。`fork` 通常复制 fd 表引用而共享打开文件描述，`CLONE_FILES` 才共享整张 fd 表。Exec 不替换文件表，只关闭标记了 `FD_CLOEXEC` 的槽，因此其他 open-file offset 要继续累积。把 offset 直接放进 fd 槽虽然早期简单，却会阻碍这些既定 Linux 语义。

路径解析需要另一组进程状态：根目录、当前工作目录和用于相对路径的目录 fd。Linux `openat` 对绝对路径忽略 dirfd；相对路径的 `AT_FDCWD` 表示从 cwd 开始，其他值必须引用有效目录 fd。BoarOS 当前只有单根 mount、cwd `/` 和 `AT_FDCWD`，但把 fs context 与 fd table 分开，是为了让以后 `CLONE_FS` 与 `CLONE_FILES` 独立控制共享关系，而不是把两类资源固化为同一个对象。

Linux `read` 的返回值不仅取决于磁盘读取结果，还取决于数据实际交付用户空间的程度。若第一字节就无法写入用户 buffer，应返回 `-EFAULT` 且不推进文件位置；若已经复制一段连续前缀，之后 fault 或 I/O 出错，通常返回已复制长度并只推进这部分。用内核 staging buffer 时，不能把“已从文件系统读入”误当成“已交付用户”：open-file offset 必须按 usercopy 成功字节提交。零长度读仍先要求 fd 有效，但不应解引用用户地址。

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
文件引用；fork 后父子各自持有来源引用，直到各自最后一个相关 VMA 被撤销。

内存压力回收必须避免无界递归。BoarOS 的物理分配器只注册一个缓存回收器：第一次分配失败
时请求 LRU 释放目标页数并重试一次，回调期间抑制再次进入回收器。当前只读文件系统没有脏页，
所以未固定缓存页可以直接丢弃；加入可写映射后，clean/dirty/writeback/error 状态会成为新的
生命周期，而不是给现有驱逐函数加一个无条件写盘调用。

## 根设备与 PID 1

无命令行解析阶段需要一个确定的根选择规则。BoarOS 当前使用 DTB 翻译后的物理 MMIO 地址排序，选择第一个成功初始化的 block device；选中后若不是可挂载 ext4 或缺少 `/init`，启动失败，不扫描磁盘内容寻找替代根。这让平台拓扑决定设备顺序，行为可复现；以后支持 Linux `root=` 时可在块设备身份层增加显式选择，而不改变 ext4/VFS。

PID 1 是用户空间生命周期的根。Linux 通常在 init 退出时 panic，因为继续运行已没有负责收养孤儿和维持用户空间的进程。BoarOS 已实现普通父子进程、reparent、zombie/wait 和同步故障终止状态，但尚无可投递、阻塞或捕获的完整信号。PID 1 及全部后代退出后，scheduler 先关闭 fd、释放 fs context、地址空间、PID 和任务页，再由根启动 purge 文件缓存、卸载根并关机。顺序不能倒置：OFD、MM 文件 backing 与缓存 node 都借用 mount，必须先释放这些引用。完成记录必须保存 TID/TGID 快照，否则任务页和 PID 被释放后就无法可靠判断退出者身份。

## 验证经验

- 块设备测试要覆盖 direct DMA、bounce、批量合并、最后一个扇区、整数溢出、timeout 后 reset 和队列页回收。
- 文件系统测试应建立真实镜像并通过工具设置 mode、checksum 与 incompat feature；只用手写 superblock fixture 很难覆盖 extent、目录和校验链。
- 成功读取文件不足以证明生命周期完整；应在 open file 时验证 unmount 为 busy，并在 close/unmount/device destroy 后比较物理页和 heap live/current pages。
- 进程文件测试还应覆盖最低 fd 复用、扩容边界、两次 open 的独立 offset、路径 NUL 上限、跨页 usercopy、部分 fault 后 offset，以及 close 已摘除 fd 但底层释放需要重试的状态。
- 页缓存测试要区分 hit/miss、尾页有效长度、被映射页 pin、LRU 驱逐和分配失败触发的有界回收；file-private mmap 还要验证写后其他别名与文件内容不变、关闭 fd 后仍可 fault、fork 后来源有效，以及整页越过 EOF 的 `SIGBUS`。
- Exec 文件测试要同时保留普通 fd 和 CLOEXEC fd：新映像应从普通 fd 的原 offset 继续读取，而 CLOEXEC fd 即使底层 close 需要重试也必须立即不可见；失败的 exec 则不能关闭任何 fd。
- 根启动 fixture 应独立链接并写入磁盘，不能把 ELF 同时嵌入 kernel，否则无法证明 VFS 是生产数据来源。
- QEMU 默认可能提供 legacy VirtIO MMIO；现代驱动测试与生产根盘必须显式设置 `virtio-mmio.force-legacy=false`。开发板 transport 和 DMA 一致性必须重新验证，不能从 QEMU 行为外推。

## 资料依据

- [VirtIO 1.3](https://docs.oasis-open.org/virtio/virtio/v1.3/virtio-v1.3.html)：modern transport、设备状态、feature negotiation、split virtqueue 和 virtio-blk。
- `references/qemu/hw/virtio/virtio-mmio.c` 与 `references/qemu/hw/block/virtio-blk.c`：QEMU VirtIO MMIO 和块设备行为参照。
- `third_party/lwext4/` 与上游文档：ext4 数据结构和当前库实现。
