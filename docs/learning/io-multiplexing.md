# Linux I/O 多路复用与等待架构

## 机制背景与接口差异

I/O 多路复用允许单个线程同时监听多个文件描述符的就绪状态（可读、可写、异常或挂断），避免为每个描述符创建专用线程或使用忙轮询消耗 CPU。

Linux 提供两种经典的就绪通知模型：基于位图的 `select`/`pselect6` 和基于描述符列表的 `poll`/`ppoll`。在现代 RISC-V 64 Linux 体系结构中，C 库（如 musl）的 `select`、`pselect`、`poll` 均直接映射到底层系统调用 `pselect6 (72)` 和 `ppoll (73)`。

两者在语义与 ABI 细节上有关键差异：
1. **参数打包（argpack）**：
   - `ppoll` 的第 4 和第 5 参数分别为信号屏蔽字指针 `const sigset_t *` 和其大小 `size_t`；
   - `pselect6` 的第 6 参数不是裸指针，而是打包指针 `const struct sigset_argpack { const sigset_t *p; size_t size; } *`。未按照该结构解包会导致内核读取错误的用户地址。
2. **无效描述符的处理规则**：
   - `select` 要求集合中所有被查询的 fd 必须是当前打开的有效描述符；任意一个未分配 fd 都会导致调用立即失败并返回 `-EBADF`；
   - `poll` 则区分对待：负数 fd（`fd < 0`）被内核静默忽略，其 `revents` 被强制清零，不计入就绪数也不报错；而未打开的非负 fd 则在对应的 `revents` 中置位 `POLLNVAL`，并作为一个就绪事件计入返回值，不导致整体系统调用返回负错误码。
3. **挂断（HUP）与 EOF 就绪**：
   - 当管道的所有写端关闭后，读端处于 EOF 状态。如果环形缓冲区中仍有残留数据，`poll` 报告 `POLLIN | POLLHUP`，确保用户态能把剩余数据完全读出；
   - 当缓冲区数据排空后，`poll` 报告 `POLLHUP`；在 `select` 中，EOF 被统一视为“可读就绪”，从而使随后的 `read()` 调用立即返回 0。
4. **零超时与非阻塞扫描**：
   - 超时参数若为 `0` 秒 `0` 纳秒，系统调用仅执行单次遍历扫描即返回，绝不进入调度器阻塞队列。

## 等待队列泛化与多节点挂载

在单事件等待（如 `wait4`、`nanosleep`、futex）中，任务通常只需要挂入一个等待队列。然而在 `poll`/`select` 中，一个调用可能同时关心数个甚至数十个不同 OFD 的读写就绪。

若等待队列直接串联 `struct kernel_task`，任务同一时刻只能存在于一个队列中，无法表达多路等待。BoarOS 将等待队列节点抽象为独立的 `struct kernel_wait_node`：
```c
struct kernel_wait_node {
    struct kernel_task *task;
    struct kernel_wait_node *prev;
    struct kernel_wait_node *next;
    struct kernel_wait_queue *queue;
};
```
- `struct kernel_task` 内部保留一个 `default_wait_node`，供单队列等待（`wait4`、futex、信号等待）零分配复用；
- `core_poll_run` 在栈或堆上分配与所监听 OFD 对应的 `struct kernel_wait_node` 数组；
- 在进入阻塞前，每个节点分别插入对应 OFD 的等待队列；任一队列被唤醒时，唤醒操作沿着 node 指针找到任务并转为 READY；
- 任务被唤醒恢复后，遍历节点数组将所有已登记的 node 可靠地从各个队列中摘除（脱链），彻底杜绝迷途唤醒（Spurious Wakeup）与悬空指针。

## 共享文件表下的 OFD 钉住生命周期

在支持 `CLONE_FILES` 的多线程环境中，一个线程正在执行 `ppoll` 阻塞等待，另一个并发线程完全可能执行 `close(fd)`。

如果 poll 仅在遍历时读取 OFD，在睡眠期间不持有引用，则并发 close 会使 OFD 及其内部的等待队列立即被析构释放，导致等待链表破坏或睡眠唤醒时发生 Use-After-Free。

BoarOS 在进入 poll 阻塞前，通过 `kernel_files_pin()` 为每一个监听的有效 OFD 显式获取一份独立引用。在睡眠期间，即使该 fd 在进程文件表中被关闭或复用，底层的 OFD 依然存活，其等待队列保持完整。当 poll 完成（超时、唤醒或信号打断）并清理完所有等待节点后，统一通过 `kernel_open_file_release()` 释放这些钉住引用。若这是最后一个引用，才安全地将 OFD 回收。

## 内核栈预算与堆回退设计

BoarOS 采用单 4 KiB 物理页承载任务控制块（`struct kernel_task`）、金丝雀（Canary）与内核运行栈的紧凑设计。控制块及内部字段占用约 2.2 KiB，留给整条内核调用链的栈空间仅约 1.8 KiB。

在多路复用实现中，若在栈上静态分配支持 1024 个 fd 的完整位图数组（$6 \times 16 \times 8 = 768$ 字节）以及 16 个描述符节点（约 650 字节），栈帧将迅速超过 1.5 KiB。一旦发生 Trap 或中断嵌套，栈指针将越界并击穿 Canary，触发 `KERNEL_SCHEDULER_STATUS_STACK_CORRUPT`。

因此，BoarOS 确立了严格的栈分配与堆回退原则：
- **快速路径（Fast-path）微型化**：
  - `KERNEL_POLL_STACK_CAPACITY` 设为 4 个描述符；
  - `pselect6` 栈到位图仅保留 1 个 64 位整型（覆盖常见 $\le 64$ 个 fd 的场景），位图栈消耗仅 48 字节；
  - 整个 `ppoll` / `pselect6` 栈帧控制在 150～200 字节以内，为 Trap 和中断留出超过 1 KiB 的充足安全裕量。
- **大集合堆回退（Heap Fallback）**：
  - 超过快速路径容量时，从进程私有 `files->heap` 动态分配节点块与多字位图缓冲；
  - 无论正常返回还是中间出错（如用户地址非法 `EFAULT`、描述符未打开 `EBADF`），均在函数退出前确定性释放堆内存，确保堆内存不发生任何泄漏（`heap-live=0x0`）。

## 固定资料

- `references/linux/fs/select.c`：Linux `pselect6` 与 `ppoll` 核心实现，commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e`。
- `references/linux/include/uapi/asm-generic/poll.h`：标准 poll 事件位定义。
- `references/musl/musl-1.2.5.tar.gz` 内 `src/select/select.c`、`src/select/poll.c`、`src/select/pselect.c`、`src/select/ppoll.c`：musl 1.2.5 实现细节。
