# 内核线程与抢占调度学习总结

本文整理实现最小可抢占内核/用户任务需要掌握的执行上下文、ABI、状态转换、栈与地址空间所有权和 timer 调度知识，并记录 BoarOS 当前已经确定的选择。当前接口和限制以[内核线程调度模块](../modules/kernel-scheduler.md)为准。

## 内核线程需要保存什么

线程不只是一个入口函数。它至少包含可恢复的 CPU 执行状态、独立内核栈、调度状态和拥有的资源。切换出去时必须保留足够状态，使它以后看起来像一次普通函数调用返回；被异步中断时则必须保留被打断指令可能仍依赖的完整现场。这两种要求产生两类不同上下文。

Trap Frame 面向任意指令边界。中断发生前没有调用者按 ABI 准备现场，所以入口必须保存全部可写整数寄存器以及 `sstatus/sepc/scause/stval`。它位于被中断线程自己的内核栈上，最终由 `sret` 恢复。

Switch context 面向 `riscv_context_switch(previous, next)` 这一普通函数调用。编译器已经按 psABI 允许 caller-saved 寄存器被调用破坏；调用链只要求 callee-saved 寄存器恢复。RV64 整数 psABI 中，`s0..s11` 和 `sp` 是 callee-saved，`ra` 决定 `ret` 的恢复点。BoarOS 还保存 `tp`，因为它被确定为当前任务指针。于是基础 switch context 是 `ra/sp/tp/s0..s11`，不重复保存 `a*`、`t*`、Trap CSR 或完整 Trap Frame。

这种分工也适用于会阻塞的系统调用：线程可以在任意内核调用深度通过 switch context 暂停，恢复后继续原 C 调用链；用户或中断现场仍由该线程栈上的 Trap Frame 管理。用户 F/D 状态再由 per-task FP image 按 FS 状态单独保存，不能把它误当成普通 C callee-saved 寄存器。若只交换最外层 Trap Frame，普通内核调用链的阻塞点就无法自然保存。

## `tp` 为什么适合表示 current

RISC-V psABI 把 `tp` 作为固定用途寄存器，普通函数不能把它当临时寄存器。内核可以约定它始终指向当前可调度任务，从而无需全局查找或用 `sp & page_mask` 推导对象。

`sp` 掩码方案把线程对象布局、栈大小和对齐永久耦合起来；一旦改成多页栈、guard page 或独立控制块，所有调用点都要变化。`tp=current` 让元数据与执行栈的布局保持 scheduler 私有。代价是内核不能同时把 `tp` 用作 C TLS 基址。用户态可以拥有自己的 `tp`；trap 入口通过 `sscratch <-> tp` 暂存它并恢复内核 current，返回用户态时再反向交换。

## 从 Timer Trap 到抢占再返回

一次已经运行过的线程 A 被 timer 抢占并切到线程 B，执行顺序是：

```text
A 普通代码
-> timer trap 在 A 栈建立完整 Trap Frame
-> dispatcher 重设 deadline、累计 elapsed tick
-> scheduler 把 A 放回 ready queue，选择 B
-> switch context 保存 A 的 scheduler 调用链，恢复 B 的调用链
-> B 从自己先前的 scheduler 调用继续
-> B 的 dispatcher 返回
-> B 栈上的 Trap Frame 恢复并 sret
-> B 回到自己被中断的位置
```

若 B 从未运行过，它的初始 `ra` 指向 trampoline，初始 `sp` 是自己的栈顶，`s0/s1` 临时携带入口和参数。trampoline 开启 SIE 后调用入口；入口返回则关闭 SIE 并进入线程退出路径。

首次运行用户任务不是调用一个 U-mode C 函数。Scheduler 预先在任务内核栈上构造 Trap Frame，令 switch context 的 `ra` 指向公共 trap return、`sp` 指向该 Frame。第一次被选中时，context switch 直接进入返回汇编，验证 U-mode 来源后由 `sret` 恢复用户 PC、SP、TP 和整数寄存器。这样首次进入和以后从 trap 恢复共用一条架构路径。

这个流程说明“timer handler 返回”不一定立刻回到触发本次中断的线程。context switch 先换了 C 调用链和栈，dispatcher 最终返回的是被恢复线程先前留下的 trap 调用。每个线程始终使用自己的 Trap Frame 和内核栈。

## FIFO、时间片与 elapsed

最小 round-robin 可以用 FIFO ready queue 表示：时间片结束时，把仍可运行的普通线程放到队尾，从队首取下一个。队首/队尾指针使入队和出队都是 O(1)。idle 是没有普通线程可运行时的特殊上下文，不应进入普通队列，否则会与真实工作竞争时间片。

BoarOS 的 timer backend 保留 deadline 相位，并可能一次报告多个迟到 tick。Scheduler 接收真实 `elapsed_ticks`，但当前一次 timer trap 最多切换一次。连续补做多次切换既不会让期间未执行的线程获得实际 CPU 时间，还会增加无意义的上下文开销。当前一个非零 elapsed 事件就耗尽一 tick 时间片；以后增加多 tick 时间片时可以在 scheduler 内部扣减，而不改变 timer 后端。

一次 context switch 只保存十几个寄存器，ready 操作是 O(1)。100 Hz 表示正常情况下每个 hart 最多每 10 ms 做一次调度检查。无 READY 竞争者时只验证少量状态并原线程返回；因此当前单 hart 内核不需要为了性能提前引入复杂策略或 tickless 状态。真正的优化应基于调度延迟、切换次数、栈高水位和空闲唤醒测量。

## 状态与所有权必须一起变化

当前任务生命周期为：

```text
metadata + stack -> READY -> RUNNING -> READY
                                  |
                                  +-> BLOCKED --event/deadline/signal--> READY
                                  |
                                  +-> EXITED -> idle releases stack; retains metadata if needed
```

队列操作不只是移动指针，还转移“谁拥有元数据与栈、谁可能仍在使用这张栈”的事实。创建只有在页访问、元数据、canary 和初始 context 全部成功后才能提交 READY；此前失败必须回滚页。RUNNING 线程返回时，当前 SP 仍在自己的页中，因此不能边退出边释放。它先进入 EXITED 并永不恢复，等 idle 已运行在静态 boot stack 上再释放执行栈。元数据页可以作为真实 I/O 清理 owner、GROUP_DEAD 或 zombie 继续存在，两种生命周期不再绑定。

用户任务持有 MM 句柄，而不是直接拥有页表树。首次创建采用移动所有权：入口、用户栈权限、初始 Frame 和 TID 建立成功后，MM 才从调用者转交任务；失败时调用者仍拥有它。普通 clone 通过 `kernel_mm_fork()` 建立独立逻辑地址空间，父子物理页先以 COW 共享；`acquire` 则表达多个 owner 共享同一 MM，未来 `CLONE_VM` 可以复用这个引用边界而不伪造页表所有权。调度切换在修改队列/current 前使用任务创建时缓存的 `satp`，不会在 tick 热路径解析 MM；ASID 0 的根切换仍会全局刷新 TLB。父子/zombie/wait 的生命周期知识见[进程生命周期学习总结](process-lifecycle.md)。

退出切换把旧寄存器写入一份永不入队的 discard context。这样退出线程没有可再次选择的 switch context，idle 回收页也不会留下悬空恢复点。若退出路径发现 `tp`、状态、边界或 canary 损坏，它不能像普通函数那样返回错误；安全做法是记录错误、切到可信 idle 栈，再由仍能返回状态的 timer 调用链执行 fatal 诊断。

## Task、线程组和 Linux 身份

Linux 的 task 表示一条可独立调度的执行流。每个 task 有自己的 TID；同一线程组共享一个 TGID，组首 task 满足 `TID == TGID`。用户通常把 TGID 称为进程 PID，因此 `getpid()` 返回 TGID，`gettid()` 返回当前 task 的 TID。线程组身份与 MM 是否共享是相关但不同的选择：clone flags 可以分别控制加入线程组和共享地址空间，内核不应把“同一 MM”硬编码为“同一 PID”。

BoarOS 用不透明 `kernel_task` 保存调度状态、TID、组首关系和 MM 引用，syscall dispatcher 显式接收 caller task。当前每个用户任务都是自己的组首，所以 `getpid/gettid` 数值相等；字段和 ABI 已经按最终语义分离。内核任务与 idle 没有用户可见身份，ID 0 留作内部“无 PID/TID”，用户 ID 从 1 开始。

有界 ID 可用位图管理：一位表示一个数值是否占用，分配搜索和释放不会额外分配内存，32768 个 ID 只需 4 KiB。线性扫描最坏为 O(limit)，但循环游标使连续创建通常很快；只有进程创建/退出触碰它，不进入 timer/context switch。未来若真实并发创建使扫描或全局锁成为瓶颈，可换成分层位图或 per-CPU 缓存，而不改变 TID/TGID ABI。

回收顺序必须与可观察生命周期一致：先释放 task 的 MM 引用，再归还 TID，最后释放元数据页；执行栈在可信 idle 上先行归还，不等父进程 wait。真实 VFS/block I/O 清理失败才保留 exited 节点并从准确阶段重试；合法页/堆释放完成即返回，分配器不变量错误进入 fatal。只有全部完成后才发布 completion。以后实现 `wait` 时，退出后的 zombie 元数据和父进程观察点会延长“退出”和“最终释放 PID”之间的生命周期，不能直接沿用当前立即完成记录作为完整 Linux wait 语义。

## 内核栈大小、对齐和保护

RISC-V psABI 要求函数入口栈保持 16 字节对齐。线程初始栈顶必须满足这一点，context switch 也必须恢复原 ABI 对齐。中断会在当前栈额外压入 BoarOS 的 288 字节 Trap Frame，再调用 C dispatcher 和 scheduler，因此评估栈大小不能只看线程入口的局部变量。

原先控制块与栈共享 4 KiB 页，控制块增至 1552 字节时，按 16 字节对齐后只剩 2528 字节，压入 288 字节用户 Trap Frame 后只剩 2240 字节。当前将元数据页与连续物理栈分别分配，8 KiB 栈除去底部 16 字节后有 8176 字节，元数据增长不再侵占它。栈以 buddy order 1 分配和释放，调度器初始化要求页分配器已 finalize；构造失败也按原 order 归还整个连续块。

栈先填充 `0xa5`，canary 单独放在底部。由于填充不再为零，首次用户入口必须显式清零完整 Trap Frame，避免把填充值作为用户整数寄存器。clone 复制父 Frame，exec 清零 Frame，两条路径保留既有语义。退出切换后从栈底扫描连续填充值，累计最小未触及空间；扫描只在非运行栈回收时进行，不增加 tick 热路径工作。

`-fstack-usage` 记录单帧与动态分配类别；汇编额外占用的 288 字节必须显式预算。单帧通过不能排除深调用链、间接调用和递归，因此还要求 root-init、静态 musl、动态 pthread 真实组合负载的最小实测余量至少 1 KiB。如果不足，按倍数扩大栈并重跑；测量只说明已执行路径。初始 4 KiB 配置虽通过普通真实负载，但下述冷文件映射加内存回收的代表链只留下 784 字节，未达到已确定的 1 KiB 余量，因而扩大到 8 KiB。2026-09-16 最终 8 KiB 生产构建实测如下：

| 真实负载 | 已释放栈数 | 最小未触及空间 | 峰值使用 |
|---|---:|---:|---:|
| root-init / exec 链 | 10 | 6208 B | 1968 B |
| 静态 musl | 40 | 5904 B | 2272 B |
| 动态 pthread | 34 | 6208 B | 1968 B |

这些数值来自 `make test-root-init-riscv test-userland-riscv` 的退出报告。`make test-stack-usage` 强制重建的生产报告覆盖 804 个函数，最大单帧为 boot/idle 上的 `kernel_main` 1920 字节，没有无界动态栈。该门禁使用实际头文件导出的 288 字节汇编 Frame 与 1024 字节余量预算；共享映射、更多驱动或更深调用链接入后应重新测量。

Canary 只能发现越过栈底后的部分破坏，不能像未映射 guard page 那样在第一次越界访问时立即 fault；它也不是任意内存破坏的恢复机制。

未来是否扩大栈或增加 guard page，应结合真实调用深度和高水位。把页布局留在 scheduler 内部、通过 `tp` 获取 current，正是为了让这种变化不扩散成公共 ABI。

### 代表调用链与中断叠加预算

以下核对使用 `build/stack-usage/` 的生产 `-O2/-fstack-usage` 报告及同次 ELF 反汇编，ELF SHA-256 为 `2a29d7fb4238c7886631561540c6e9bf59ebf3422168542ef0d97df5cf2d90aa`。这是最终 8 KiB 构建的静态快照；修改优化选项、时间戳/文件/MM 路径或栈策略后须重建再核对。对应 `.su` 入口是 `arch/riscv/{trap,signal,uaccess,mm}.su`、`kernel/{sched/process,syscall/dispatch,syscall/file,physical_page}.su`、`fs/{files/io,vfs,open_file,page_cache,lwext4_port}.su`、`mm/heap.su` 和 `third_party/lwext4/src/{ext4,ext4_fs,ext4_extent,ext4_blockdev,ext4_bcache}.su`。

下列数字单位均为字节，来自编译后的帧大小，不另给已内联的 `map_private_file_page`、`map_cached_file_page`、`ext4_buf_alloc` 加一份栈。`ext4_find_extent` 与 `read_extent_tree_block` 使用对应 `.constprop` 记录；`ext4_fs_get_inode_dblk_idx_internal` 使用 `.isra` 记录。反汇编确认的零帧/尾调用不重复计数，普通兄弟调用取各自链路，不把返回后的帧相加。

为展示共享的深路径，先列出三个后缀：

- **文件 extent 读路径 F，672**：`kernel_vfs_node_pread(80) → ext4_fread.part.0(160) → ext4_fs_get_inode_dblk_idx_internal(96) → ext4_extent_get_blocks(144) → ext4_find_extent(144) → read_extent_tree_block(48)`。零帧的 `kernel_open_file_pread`、`kernel_vfs_pread`、`ext4_fread` 包装不改变和。这里选择已有多层 extent 的冷元数据读取分支。
- **新块缓存对象分配 A，576**：`ext4_block_get(48) → ext4_block_get_noread(48) → ext4_bcache_alloc(48) → ext4_user_calloc(32) → kernel_heap_allocate_zeroed(48) → kernel_heap_allocate(64) → physical_page_allocate_order(32) → physical_page_allocate_order_once(96) → free_list_insert(64) → free_block_valid(96)`。适用于 slab 需要新页且 buddy 需要分裂的分支。
- **分配触发回收 A'，912**：上述 A 在 `physical_page_allocate_order` 以下，不再处于已经返回的 `allocate_order_once`，而走 `reclaim_callback(0) → kernel_page_cache_reclaim(112) → cleanup_entry(32) → physical_page_release_order(288) → free_list_insert(64) → free_block_valid(96)`。`physical_page_release` 的 finalized 分支恢复自身栈后尾调 order release，因此不再加其 bootstrap 分支的 48 字节；`remove_entry` 返回后才执行 cleanup，也不叠加。`allocator->reclaiming` 防止再入同一 reclaimer，但这里没有由此推断所有其他 I/O/清理分支的栈上界。

| 代表触发与仍在栈上的前缀 | 加 A | 加 A' | 8 KiB 栈在 A' 后的余量 |
|---|---:|---:|---:|
| writev 分配/扩展 extent：`Trap(288) → dispatch(112) → syscall_dispatch(64) → writev_handler(64) → kernel_files_writev(272) → write_request(240) → kernel_vfs_pwrite(80) → ext4_fwrite(208) → ext4_fs_get_or_alloc_inode_dblk_idx(80) → ext4_extent_get_blocks(144) → ext4_ext_insert_extent(240) → ext4_find_extent(144) → read_extent_tree_block(48)`，前缀 1984 | 2560 | 2896 | 5280 |
| writev 从冷 file-private 源缓冲复制：上述前缀到 `write_request` 后，改走 `kernel_copy_from_user(96) → resolve_user_page(80) → kernel_mm_resolve_user_fault(224) → F(672)`，前缀 2112 | 2688 | 3024 | 5152 |
| 硬件 file-private 读缺页：`Trap(288) → dispatch(112) → kernel_scheduler_resolve_current_user_fault(32) → kernel_mm_resolve_user_fault(224) → kernel_open_file_get_page(48) → kernel_page_cache_get(160) → F(672)`，前缀 1536 | 2112 | 2448 | 5728 |
| clone 的 `PARENT_SETTID` 写入冷 file-private 地址：`Trap(288) → dispatch(112) → riscv_process_clone_current(160) → kernel_copy_to_user(96) → resolve_user_page(80) → kernel_mm_resolve_user_fault(224) → F(672)`，前缀 1632 | 2208 | 2544 | 5632 |
| 向冷 file-private 用户栈投递 signal：`Trap(288) → riscv_signal_prepare_user_return(128) → riscv_signal_build_frame(896) → kernel_copy_to_user(96) → resolve_user_page(80) → kernel_mm_resolve_user_fault(224) → F(672)`，前缀 2384 | 2960 | 3296 | 4880 |

另核对 clone 的 MM 分叉分支：`Trap(288) → dispatch(112) → clone_current(160) → kernel_mm_fork(112) → riscv_sv39_user_space_fork(160) → physical_page_allocate(尾调，0) → physical_page_allocate_order(32) → allocate_order_once(96) → free_list_insert(64) → free_block_valid(96)` 合计 1120；分配处改走上述回收分支则为 1456。它浅于表中的用户 TID 存储缺页分支。`kernel_syscall_dispatch` 在 dispatcher 执行 clone action 前已经返回，不能再次叠加其 64 字节。writev 的 append 后端比表中 pwrite 后端多 16 字节；正常底层读 I/O 后缀 `ext4_block_get(48) → ext4_bdif_bread(64) → block_read(16) → kernel_block_read_at(0) → virtio_block_read(96) → submit_request(32)` 共 256，浅于这里选取的缓存对象分配/回收后缀。这些间接调用目标来自当前 VFS/block adapter 注册和 VirtIO block 操作表，并以该 ELF 核对。

**288 字节与嵌套的计入方式：**`trap_entry.S` 在进入 U/S trap 时只压入一次 288 字节整数 Frame，随后调用 C dispatcher。uaccess 经软件调用 `kernel_mm_resolve_user_fault` 补页，不再次制造硬件 Trap Frame。signal 投递位于汇编 `riscv_trap_return`：此时 dispatcher 已返回，所以表中 signal 链不加 dispatcher 的 112 字节；`riscv_trap_return_prepare` 自身为零帧尾调。`riscv_fpu_switch` 和 `riscv_fpu_reset_current` 各自额外使用 16 字节，`riscv_fpu_state_save` 本身不压栈；signal 的 FP 保存与后续用户复制是兄弟调用，不同时压在上述文件缺页链底。

S-mode trap 入口令 SIE 清零，当前 syscall/用户缺页/signal-return 链没有重新打开它，因此不为这些正常链凭空再叠一层 timer。该事实依据固定 `references/riscv/riscv-privileged-20260120.pdf` §12.1、文档页 113（SHA-256 `d0f818af6fa519d39e68f822aa795bff9f38032a2f352afdf43e91c0d480e408`），以及 BoarOS 的 trap/context 汇编和中断恢复调用点。普通内核线程的 trampoline 会打开 SIE；若在这样的执行位置发生 timer，应在被中断的 C 链之外再预算 `288 + 实际 timer C 链`。context switch 把保存区写入任务元数据并切换 SP，不再在旧栈压一份 switch context。SIE=0 仍不能阻止同步 S-mode fault：那会再压 288 并进入 fatal 诊断，属于已损坏内核执行的路径，不能假设它具有可恢复的正常栈预算。

这次代表链检查直接发现了 4 KiB 的不足：signal + A' 为 3296，原来可用 4080 只剩 784，小于 1024；8 KiB 可用 8176 为同一分支留下 4880。聚焦测试另外保留一个真实 3 KiB 活跃 C 帧并调用 scheduler，再检查退出扫描的 1 KiB 余量；它在旧 4 KiB 上失败，在 8 KiB 上通过。上述检查覆盖明确的合法分支和调用叠加，不是全调用图证明；不同 extent/回收/错误清理分支、间接调用目标变化及新驱动仍需另行审查，单帧门禁和有限高水位负载也不能替代这种审查。

## 单 hart 为什么可以只关本地中断

ready/exited 队列会同时被普通线程创建路径、timer handler 和 idle 回收访问。单 hart 上，保存并关闭 SIE 可以阻止当前 hart 在修改一半时被 timer 抢占，因此足以形成临界区。恢复时只恢复旧 SIE 位，避免一个原本在关中断环境中的调用意外开中断。

第二个 hart 不受本 hart 的 SIE 控制，仍可同时修改共享队列；因此“关中断”不是 SMP 锁。接入 SMP 时应在当前私有队列边界增加锁，或改成 per-hart runqueue，再定义跨 hart 唤醒和内存序。当前没有第二个可验证使用点，不预建这些接口。

## BoarOS 当前选择

- RISC-V 异步现场继续使用完整 Trap Frame，普通调度使用独立的 psABI switch context。
- 内核 `tp` 固定为 current；用户 `tp` 独立保存，`sscratch` 只在 U-mode 保存 current，内核态保持为零。
- 单 hart FIFO round-robin，一个 tick 时间片，一次 trap 最多切换一次。
- 普通内核/用户任务分别拥有私有 4 KiB 元数据页与 8 KiB 连续物理内核栈；用户任务持有可共享 MM 引用和独立 TID/线程组身份；boot context 成为永久 idle，继续使用静态 boot stack。
- 内核线程入口返回即退出；用户任务通过 syscall 或同步故障退出。idle 在可信栈上先释放已停止执行的任务栈，继续完成 MM/文件等清理；TID 与元数据按组关系和 wait 生命周期释放。
- 队列临界区保存并关闭 SIE；interruptible wait 使用同一 blocked 链并由未阻塞 pending signal 返回 `SIGNALLED`，vfork 等资源生命周期等待保持不可中断。F/D 状态由 scheduler switch 的 FS Dirty 检查按需保存/恢复；不为尚未实现的 SMP、优先级或 V 状态建立占位层，MM/身份/信号字段则是 `clone/fork/exec/wait` 已确定路径的必要永久机制。

这些选择形成完整、可测的内核线程闭环，同时把以后可能变化的策略、栈布局和 per-hart 组织留在模块内部。RISC-V context 机制可在 QEMU `virt` 与 VisionFive 2 复用；平台 timebase 仍由 DTB 决定。LoongArch 需要自己的 switch context、CSR/中断和 16 KiB 栈页实现，不能复用 RISC-V 汇编。

## 验证和调试经验

- 初始化测试要证明失败尝试不会把 singleton 置为半初始化，输出参数在失败时保持不变。
- 页访问失败必须同时检查空闲页计数恢复，才能发现“返回了错误但泄漏页”的实现。
- 只让线程打印一次不能证明抢占；worker 必须不调用 yield 并持续忙等，由真实 timer 形成 A -> B -> A。
- 给 `s0..s11` 设置独立哨兵并跨多次抢占比较，可发现错误偏移、漏保存和 32/64 位宽度错误；同时检查最终对象反汇编，验证实际链接指令而非源码文本。
- 记录每个 worker 的 `sp/tp`、最终 idle `sp/tp` 和分配器空闲计数，能同时验证独立栈、current ABI、退出切换和页回收。
- 用户任务测试还应跨真实 timer 抢占检查用户 `gp/sp/tp/s0..s11`；让两个任务以不同栈和身份共享正常 MM，并用另一独立根隔离故障任务，可以同时验证共享末引用、`satp` 切换和故障隔离。完成后还要核对全部 MM、叶子页、页表页、TID 和任务页归还。
- 正常生产内核会永久 idle，有限关机逻辑应放在测试 ELF 的链接包装中；生产映像需用符号表确认不含测试 worker。
- 静态分析适合发现 C 状态路径中的空指针、未初始化、双重释放和释放后使用；汇编寄存器集合、Trap Frame/context 配合及真实抢占顺序仍需要反汇编和 QEMU 端到端测试。

## 阻塞与唤醒（当前结论）

- BoarOS 的阻塞机制沿用 wait4 确立的模式：关中断内检查条件、置 BLOCKED、经 `riscv_context_switch` 切走；唤醒方把任务置 READY 并入 ready 队尾，被唤醒者从原调用点返回后必须重查条件。所有 BLOCKED 任务都在全局 blocked 链上，事件通道（wait_queue 令牌）与超时（deadline 刻度）是任务的字段而不是独立节点，唤醒按 FIFO 走链。
- 单 hart 下丢失唤醒的唯一来源是"检查条件与阻塞之间开中断"；因此内核线程（trampoline 运行在 SIE=1）调用阻塞接口前必须用 `riscv_interrupt_save/restore` 收敛临界区，而 syscall/trap 上下文天然关中断。Linux 的 `schedule()` 把这一职责收进调度器本身并保存/恢复中断状态，等 BoarOS 引入线程和 SMP 时需要对齐这一语义。
- 超时唤醒与事件唤醒共用一条 blocked 链：tick 处理器在抢占检查之前先扫描到期 deadline，使刚到期的任务能在同一次切换中被选中；唤醒延迟上界是一个 tick 周期。Linux 用红黑树/timer wheel 组织到期任务，等待队列按需唤醒；BoarOS 的 O(阻塞数) 走链是有意的阶段性简化，扩展路径已在模块文档声明。

## FP 状态为什么不塞进基础 Trap Frame

RISC-V `sstatus.FS` 给出了 Initial/Clean/Dirty 状态，适合把少见的 F/D 使用从每次整数 trap 和普通内核线程切换中分离。BoarOS 的基础 Frame 保持固定 288 字节；用户 task 另有 272 字节的 32 个 D 寄存器、fcsr 和 saved 标志。context switch 只在前一个 task 为 Dirty 时保存，再在下一个 task 有 saved image 时恢复，恢复后把 FS 留在 Clean。

这要求 FP 保存代码是唯一触碰 F/D 的汇编边界，并且在 scheduler 关闭 SIE 的窗口内与 current/task owner 一起切换。exec 必须清空硬件和 image，signal frame 必须把 image 纳入 ucontext；否则旧映像或 handler 会观察到不属于自己的浮点寄存器。当前只覆盖单 hart F/D，V 扩展和 SMP owner 协议留在后续独立阶段。

## 资料依据

- [RISC-V ELF psABI](https://riscv-non-isa.github.io/riscv-elf-psabi-doc/)：RV64 调用约定、callee-saved 寄存器和栈对齐。
- `references/riscv/riscv-privileged-20260120.pdf`：SIE、trap 进入/返回和特权 CSR。
- `references/linux/arch/riscv/kernel/entry.S`：Linux RISC-V `__switch_to` 保存集合、`tp=current` 和 trap/current 配合。
- `references/linux/arch/riscv/include/asm/switch_to.h`：Linux RISC-V switch 调用边界与扩展状态组织。
