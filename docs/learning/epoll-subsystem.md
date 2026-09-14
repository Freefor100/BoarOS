# Linux epoll 事件通知子系统

## 事件驱动模型：从轮询 Pull 到就绪 Push

在传统的 `pselect6` 与 `ppoll` 架构中，多路复用采用“拉模型”（Pull Model）：每次调用必须把用户关注的完整描述符集合或 `pollfd` 数组拷贝进内核，内核自头至尾轮询（poll）每一个 OFD 的当前状态并挂载等待节点。一旦事件产生或超时唤醒，内核再次遍历全部节点进行检查，并在返回前全量摘链。当并发连接数达到数千甚至数万时，绝大多数描述符在单次周期内并无事件就绪，$O(N)$ 的每次进入遍历、拷贝与挂摘链开销成为性能的主要瓶颈。

Linux `epoll` 引入“推模型”（Push Model）：
1. **状态常驻**：通过 `epoll_ctl(ADD)` 将关注的文件描述符与事件掩码一次性登记在内核 epoll 实例中；
2. **事件驱动推入**：被监听对象（如管道、套接字）在状态发生改变（写入数据、写端关闭 EOF 等）时，主动调用其等待队列的唤醒链；epoll 通过注册的回调函数直接将就绪的项（`epitem`）追加到就绪链表（`ready_list`）；
3. **常数级就绪收集**：`epoll_pwait` 无需遍历数千个监听目标，仅需从就绪链表中按需提取已就绪的事件，时间复杂度降为与就绪事件数成正比的 $O(K)$。

## 双向关联与 (target_fd, OFD) 键化模型

Linux `epoll` 监听条目的索引键并非单纯的进程私有 fd，也不是单纯的打开文件描述（OFD），而是 `(target_fd, struct file *)` 二元组（在 Linux 源码中体现为 `struct epoll_filefd`）。

这一设计的关键语义推论包括：
1. **dup 描述符的独立监听**：若一个进程通过 `dup()` 将 `fd1` 复制为 `fd2`（两者指向同一底层 OFD），用户可以将 `fd1` 与 `fd2` 均添加到同一个 epoll 实例中，分别配置不同的事件掩码或用户 `data` 标记，两者在 epoll 树中互为独立的合法条目；
2. **OFD 存活与生命周期解耦**：若 `fd1` 被 `close()`，但该 OFD 仍有 `fd2` 引用存活，`fd1` 的监听项会被摘除或停用，而 `fd2` 关联的监听依然有效；只有当底层 OFD 的最后一个引用归零、执行析构时，内核才隐式全量清理挂在该 OFD 上的所有 epoll 关联。
3. **目标类型能力约束**：与 `poll/select` 允许传入任何合法文件描述符（常规文件和目录无条件立即报告可读写）不同，Linux `epoll` 仅允许监听其文件操作集提供了真实等待队列与回调通知机制的对象（如管道、套接字、终端等）。对常规文件或目录执行 `epoll_ctl(ADD)` 时，内核严格拒绝并返回 `-EPERM`。

若只由 epoll 实例单向持有指向 target OFD 的指针，当某个进程通过 `close` 释放了 target OFD 的最后一个引用时，target OFD 将被释放，而 epoll 仍保留野指针，导致后续 `epoll_pwait` 或 `epoll_ctl` 触发 Use-After-Free (UAF)。

BoarOS 建立了 epoll 实例与 target OFD 之间的**双向链表关联**：
```text
  [struct kernel_epoll]
      │
      ├─ items ────► [struct kernel_epoll_item (target_fd, ofd)] ◄─── ep_items ─── [target OFD]
      └─ ready_list ───┘ (epitem 挂入就绪链)
```
- 每个 `struct kernel_epoll_item` 记录了 `target_fd` 与所属 target OFD 指针，拥有两条链表节点：
  1. `item_link`：挂入所属 epoll 实例的 `items` 全局链表；
  2. `file_link`：挂入目标 OFD（`struct kernel_open_file_description`）的 `ep_items` 链表。
- 当 target OFD 的引用计数降为 0（`kernel_open_file_release`）时，内核主动遍历其 `ep_items`，调用 `kernel_epoll_notify_file_release` 将所有监听该文件的 `epitem` 从其所属的 epoll 实例中彻底摘除并释放；
- 当 epoll 实例自身被关闭销毁（`kernel_epoll_destroy`）时，内核遍历其 `items`，将每个 item 从对应 target OFD 的 `ep_items` 中摘除，并注销等待节点。

这种对称设计确保了无论 target OFD 先关闭还是 epoll 描述符先关闭，系统都能在 $O(1)$ 常数时间内完成关联解除与资源回收，无需全局全表扫描。

## 等待队列回调扩展

在单任务等待模型中，等待队列节点 `struct kernel_wait_node` 仅需记录 `struct kernel_task *task`，唤醒操作 `kernel_wait_queue_wake_*` 直接将该任务设为 READY。而在 epoll 体系中，被监听对象的等待队列上挂入的不再直接是任务，而是 epoll item 的事件探针。

BoarOS 将等待队列节点泛化，扩展了可选的自定义唤醒回调函数：
```c
typedef void (*kernel_wait_callback_fn)(struct kernel_wait_node *node, uint32_t reason);

struct kernel_wait_node {
    struct kernel_task *task;
    struct kernel_wait_node *prev;
    struct kernel_wait_node *next;
    struct kernel_wait_queue *queue;
    kernel_wait_callback_fn callback;
    void *context;
};
```
当被监听对象（如管道写入、连接就绪）调用 `kernel_wait_queue_wake_*` 时：
1. 若 `node->callback` 为空，执行传统调度器任务唤醒；
2. 若 `node->callback` 非空，直接执行回调函数 `kernel_epoll_wait_callback`；
3. epoll 回调函数检查 `item->event.events` 关注掩码与事件理由，若匹配且该 item 尚未处于就绪链表中，则将其推入 `epoll->ready_list`，并紧接着唤醒正在 `epoll_pwait` 上睡眠的用户任务。

通过等待节点回调，epoll 将“被动等待”与“主动通知”无缝桥接在统一的调度器等待基础设施之上，避免为 epoll 重写一套重复的等待队列机制。

## 触发模式状态机：LT、ET 与 ONESHOT

Linux epoll 拥有三种核心触发语义，BoarOS 通过对 `ready_list` 的出队与再入队协议予以完整支持：

1. **水平触发（Level-Triggered, LT，默认）**：
   - 只要目标底层对象仍处于就绪状态（例如管道内仍有未读字节），该事件就会持续通知；
   - 在 `epoll_pwait` 提取事件时，item 首先从 `ready_list` 摘除，并将就绪事件拷贝至用户输出缓冲；
   - 提取完成后，LT 模式会立刻对目标 OFD 重新调用 `poll` 查询当前残留就绪状态。若底层依然就绪，则**重新将 item 挂回 `ready_list` 队尾**。这保证了用户若仅进行部分读取（Partial Read），下一次 `epoll_wait` 会立刻返回剩余就绪通知。
2. **边沿触发（Edge-Triggered, ET，`EPOLLET`）**：
   - 仅在状态从“未就绪”转为“就绪”（如管道从空变为有数据，或有新的数据写入）的边沿跳变时产生通知；
   - `epoll_pwait` 将 ET item 交付给用户后，直接从 `ready_list` 摘除，**不进行自动重新入队**。直到下一次被监听对象再次产生写入并触发 wait 回调时，才会重新进入就绪队列。这要求用户态必须循环非阻塞读取直到返回 `EAGAIN`。
   - *注意管道写端的唤醒语义*：传统管道实现若仅在 `pipe->bytes == copied`（即从空转为非空）时唤醒等待队列，会导致已有数据残留时的新追加写入无法触发 ET 唤醒。BoarOS 修正了管道写逻辑，只要单次写入字节数 `copied > 0`，便无条件触发读端等待队列唤醒，确保 ET 模式不漏事件。
3. **单次触发（One-Shot，`EPOLLONESHOT`）**：
   - 事件触发并被 `epoll_pwait` 交付一次后，item 自动被内核禁用（`item->disabled = true`），后续底层状态变化不再进入 `ready_list`；
   - 直到用户显式调用 `epoll_ctl(EPOLL_CTL_MOD)` 重新配置并使能事件，该项才恢复监听。

## epoll 作为 OFD：嵌套与可组合性

在 UNIX “一切皆文件”哲学下，`epoll_create1` 返回的也是一个正规的文件描述符，其底层是一个类型为 `KERNEL_OPEN_FILE_KIND_EPOLL` 的 OFD。

这意味着 epoll 文件描述符自身也可以被其他多路复用机制所监听：
- 对 epoll fd 执行 `poll` 或 `ppoll`；
- 将一个 epoll fd 通过 `epoll_ctl(ADD)` 注册到另一个 epoll 实例中（嵌套 epoll）。

BoarOS 为 epoll OFD 实现了 `kernel_epoll_poll` 操作：当且仅当 epoll 内部的 `ready_list` 非空时，报告 `POLLIN`。同时，为了防止死锁、深度递归击穿内核栈与无限循环依赖，BoarOS 实现了对齐 Linux 的严格嵌套校验与循环检测机制：
1. **直接自监听校验**：`epoll_ctl` 显式禁止将 epoll fd 自身添加到自己的监听集中（`epfd == fd` 或底层 OFD 相同），返回 `-EINVAL`。
2. **深度优先遍历与环路检测（Cycle Detection）**：
   - 在将一个 epoll 文件注册到另一个 epoll 实例时，执行 `epoll_check_nesting()`；
   - 通过向下迭代式深度优先搜索（`epoll_check_downward`）检查目标 epoll 内部是否已经直接或间接包含当前 epoll 实例；一旦出现环路，立即返回 `-ELOOP`；
   - 通过向上迭代式深度优先搜索（`epoll_check_upward`）检查父级 epoll 树；
   - 累加双向嵌套深度：当 `down_depth + 1 + up_depth > KERNEL_EPOLL_MAX_NESTS`（上限为 4）时，拒绝注册并返回 `-ELOOP`；
   - 整个 DFS 遍历采用固定长度栈帧（容量为 8），完全在紧凑内核栈内以迭代代替 C 语言函数递归，零堆分配且绝不击穿栈 Canary。
3. **确定性无级联清理**：当嵌套的 epoll OFD 被 `close` 销毁时，`kernel_epoll_destroy` 仅遍历并解绑其直接注册的 item 与等待节点，不进行跨实例级联递归清理，保证销毁路径的常数栈开销与资源回收安全。

## 竞态防护与原子阻塞等待协议

在多任务操作系统中，多路复用等待（`epoll_pwait` / `ppoll`）的核心挑战之一是**唤醒丢失（Lost Wakeup）**：如果任务在发现就绪链表为空后、但在真正将自身挂入等待队列并休眠前发生硬件中断（如定时器中断、网卡或块设备完成中断、管道写入抢占），而中断处理程序恰好执行了唤醒，则后续任务将陷入死等直到下一次事件或超时。

BoarOS 在 `kernel_files_epoll_pwait` 与调度器等待协议中构建了原子关中断保护路径：
1. **关中断原子校验与入队**：
   - 任务在提取就绪事件为空且需要休眠前，调用 `riscv_interrupt_save()` 关闭本地中断；
   - 在关中断状态下重新检查 `epoll->ready_head != 0`；若在提取间隙已有事件到达，立即开中断并重试提取，避免无效睡眠；
   - 确认无就绪事件后，调用 `kernel_scheduler_block_current(&epoll->wait_queue, deadline, 1, &wake_reason)`，将任务状态原子切换为 `INTERRUPTIBLE` 并挂入 epoll 等待队列；
   - 调度恢复后调用 `riscv_interrupt_restore()` 重新使能中断。
2. **信号临时掩码安全还原**：
   - 若传入了 `sigmask`，内核在进入等待循环前设置临时掩码；
   - 退出循环后无论正常唤醒、超时还是信号中断，均通过 `kernel_signal_restore_temporary_mask(task, saved_mask, interrupted)` 恢复原掩码，确保信号中断（`-EINTR`）与信号处理上下文一致。

## 紧凑栈预算与堆回退设计

BoarOS 在 RISC-V 架构下采用单页（4 KiB）紧凑任务布局，扣除任务控制块与 Canary 后，内核栈安全预算仅约 1.8 KiB。

每个 `struct linux_epoll_event` 占用 16 字节（包含 32 位 events 掩码与 64 位用户 data 联合体）。如果用户在 `epoll_pwait` 中传入 `maxevents = 128` 甚至更高，若在内核栈上分配该数组，将消耗超过 2 KiB 栈空间，必然击穿 Canary 导致内核崩溃。

因此，BoarOS 延续严格的栈预算分级策略：
- **微型栈缓冲（Fast-path）**：
  - 定义 `KERNEL_EPOLL_STACK_CAPACITY = 4`，占用 $4 \times 16 = 64$ 字节栈空间；
  - 常见小规模事件提取循环（$\le 4$ 个事件）完全在栈内完成，零堆内存分配开销；
- **动态堆回退（Heap Fallback）**：
  - 当 `maxevents > KERNEL_EPOLL_STACK_CAPACITY` 时，从进程私有 `files->heap` 中动态分配临时事件数组；
  - 无论正常返回还是用户空间写错误（`EFAULT`），在函数退出路径（统一 `out` 标签）无条件确定性释放堆内存，严格保持堆基线 `heap-live=0x0`。

## 关闭与所有权

epoll OFD 关闭时先把监听项从等待队列、目标 OFD 和就绪队列解绑，再按 owner 顺序释放 item 与实例；同一逻辑解绑不会执行两次。物理页和堆释放若遇到非法地址、引用或 allocator metadata，直接进入 fatal，不建立 `cleanup_items` 或人工失败注入路径。若目标 OFD 或关联 source 的真实 VFS/block I/O 清理失败，owner 由文件表或 mount 保留，fd 槽已经摘除，当前 `close()` 仍返回 0，重复 close 返回 `-EBADF`。这让 epoll 的监听图生命周期与文件系统 I/O 错误分属不同失败域。

## 固定资料

- `references/linux/fs/eventpoll.c`：Linux epoll 核心实现，commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e`。
- `references/linux/include/uapi/linux/eventpoll.h`：标准 epoll 事件标志与结构定义。
- `references/musl/musl-1.2.5.tar.gz` 内 `src/select/epoll.c`、`src/select/epoll_pwait.c`：musl 1.2.5 用户态包装实现。
