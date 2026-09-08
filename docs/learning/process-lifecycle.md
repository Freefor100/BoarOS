# 进程生命周期学习总结

进程支持不是“复制一个任务结构”这么简单。一个 Linux 进程同时涉及执行现场、虚拟地址空间、文件描述符、文件系统上下文、身份、父子关系和退出事件；fork、exec、exit、wait 分别改变其中不同部分。理解这些对象的共享与所有权，才能避免父子互相污染、资源泄漏、提前释放和伪造 syscall 成功。

## 进程、线程和可调度任务

可调度任务是 scheduler 能选择执行的实体。进程是用户可观察的资源与身份容器，线程是共享大部分进程资源、但拥有独立执行现场的任务。Linux 内部用统一 task 对象表达进程和线程，再由 clone flags 决定资源共享关系；用户看到的 PID 通常是线程组 ID（TGID），TID 则标识具体任务。

BoarOS 当前每个用户进程只有一个任务，所以 TID 等于 TGID；字段仍然分开，避免以后加入线程时修改 `getpid/gettid` ABI。内核任务和 idle 没有 Linux 用户身份。

一个用户任务至少要保存两类现场：

- Trap Frame 是从 U-mode 进入内核时保存的完整用户整数寄存器和返回 CSR，用来精确恢复任意用户指令点；
- switch context 是 scheduler 的内核 C 调用链现场，只需按 psABI 保存被调用者保存寄存器、内核 SP 和 current task 指针。

Fork 要复制前者，让父子从同一个 syscall 返回点继续；context switch 使用后者，在两个内核调用链之间切换。

## fork 与 clone 的关系

传统 `fork()` 表示创建一个独立进程。Linux 在内核 ABI 上常由 `clone` 或 `clone3` 承担更通用的创建功能：flags 决定 MM、文件表、fs context、信号处理和线程组是复制还是共享。普通 fork 形态的关键可观察语义是：

- 父进程得到子 PID，子进程得到 0；
- 父子初始用户内存内容相同，之后普通写入彼此隔离；
- 文件描述符编号和 descriptor flags 被复制；
- 继承的 fd 指向同一个 open file description，所以文件 offset 和 file status flags 共享；
- 当前工作目录语义被继承；
- 子进程拥有新的 PID，并记录创建它的父进程。

BoarOS 当前只接受 RISC-V `clone(SIGCHLD, 0, 0, 0, 0)`。这形成真实的普通进程闭环，同时拒绝尚未实现的 `CLONE_VM`、`CLONE_FILES`、`CLONE_THREAD`、替代用户栈和用户态 TID 指针，避免把共享资源或线程语义伪装成 fork。

## 地址空间复制：eager copy 与 COW

普通 fork 必须表现为父子写隔离，但不规定内核一定在 fork 时复制每个页面。

Eager copy 在 fork 时为子进程分配新页、复制内容并重建页表。优点是状态直接、写入不触发额外 fault，错误与回滚边界容易验证；代价是 fork 时间和新增内存与已提交页数线性相关，即使子进程马上 exec 也要先复制。

Copy-on-write（COW）让父子暂时共享物理页，把可写映射改成只读并增加页引用。任一方写入时触发 page fault，内核才分配新页、复制并恢复写权限。COW 显著降低常见 fork+exec 的复制成本，但需要：

- 物理用户页引用计数；
- 能区分真正只读页和 COW 只读页的 PTE/软件状态；
- 写 fault 中的分配、复制、PTE 更新和失败处理；
- TLB 失效，SMP 下还要远端 shootdown；
- fork 途中修改父 PTE 后的可靠回滚或提交协议。

BoarOS 当前已经使用 COW。物理分配器只允许 order-0 页增加共享引用；Sv39 用 RSW 软件位区分真正只读页与 COW 只读页。Fork 先构造完整子页表并取得页引用，最后才无分配地把父可写 PTE 提交为 COW，因此失败不会留下“子进程没创建、父页面却突然只读”的半状态。写 fault 在引用数大于 1 时复制，等于 1 时原地恢复；uaccess 写入也必须走同一路径。

COW 降低的是数据页复制和 fork 内存峰值，不消除页表/VMA/OFD metadata 的遍历与分配；这些工作当前仍发生在关中断临界区。SMP 后需要原子页引用、MM 锁和远端 TLB shootdown，开发板上还要分别测量 fork 构造延迟、首次写 fault 延迟和实际复制页比例，才能判断下一步优化。

## fd 表与 open file description

文件描述符只是进程表中的小整数槽。槽通常保存 descriptor flags（例如 close-on-exec）并指向 open file description；后者拥有底层打开文件、当前 offset 和 file status flags。

这一区分解释了几个容易混淆的行为：

- 两次独立 `open` 同一路径得到不同 open file description，offset 独立；
- `dup` 复制 fd 槽，但两个 fd 指向同一 description，offset 共享；
- 普通 fork 复制整张 fd 表，但继承槽仍指向相同 description，父子 offset 共享；
- `CLONE_FILES` 不复制表，而是让任务共享同一张 fd 表，因此一方 close 会直接改变另一方可见的 fd 集合；
- `FD_CLOEXEC` 属于 fd 槽，exec 关闭相应槽；普通 fd 和其 description 继续存在。

BoarOS 普通 clone 已复制 fd 槽数组并给每个继承的 open file description 增加引用。最后一个引用消失时才关闭底层 VFS file。fs context 则独立复制 cwd 字符串并继续借用同一个 root mount。

## 退出为什么需要 zombie

进程执行 `exit` 后，父进程仍需要取得退出原因和状态。如果内核立刻释放 PID 和全部任务信息，wait 就没有可查询的事件；如果保留整个地址空间和所有文件，又会让未及时 wait 的父进程长期占用大量资源。

常见做法是把退出分成两段：

```text
运行进程
  -> 停止执行并释放 MM、文件等重资源
  -> zombie：只保留身份、亲缘、退出状态和少量调度对象
  -> 父进程 wait
  -> 释放 PID 和最后的任务对象
```

BoarOS 也采用这个边界。退出路径先切回稳定内核地址空间，再在任务自己的内核栈上释放 exec/files/fs/MM；清理完整且存在父任务时成为 zombie。任务页不能在当前仍使用它的栈上释放，所以它必须保留到父进程 wait。若重资源清理暂时失败，任务进入 exited 清理队列，由 idle 在可信栈上重试，完成后才发布 zombie 事件。

普通退出码在 wait status 中放在 bit 8..15；信号终止使用低 7 位，core 默认动作另置 bit 7。同步异常则转换为信号终止形态，例如非法指令对应 SIGILL、地址访问故障通常对应 SIGSEGV。标准信号现在可以在用户返回尾进入 handler，`rt_sigreturn` 恢复整数/FP frame；实时排队和备用信号栈仍未实现。

## wait、阻塞与唤醒

`wait4` 先按 pid 参数选择子进程集合：

| pid | 选择范围 |
|---:|---|
| `> 0` | 指定 PID 的子进程 |
| `0` | 与调用者同进程组的子进程 |
| `-1` | 任意子进程 |
| `< -1` | 进程组 ID 等于 `-pid` 的子进程 |

没有匹配子进程时返回 `ECHILD`。存在匹配子进程但没有可报告事件时，`WNOHANG` 返回 0；否则调用者必须进入 BLOCKED，让出 CPU，直到子进程 stop/continue/exit 路径把它放回 READY。阻塞不是循环执行 `wfi` 或忙等，而是 scheduler 状态转换；wait4 和 pipe/nanosleep 等 interruptible waiter 还可由未阻塞信号返回 `SIGNALLED`，被唤醒后必须重新扫描条件，因为未来 SMP、信号和多个子事件都会使“一次唤醒必然对应目标事件”的假设失效。

Linux 的 wait 回收与用户复制之间有一个重要顺序：选中的 zombie 先被内核逻辑回收，再把 status 写到用户地址。因此 status 指针错误返回 `EFAULT` 时，同一个子进程也不能再次 wait。测试要验证这个副作用，不能只检查 errno。

## 父进程退出与 reparent

父进程可以在子进程之前退出。内核不能让 child 的 parent 指针悬空，也不能丢掉将来的退出事件。Linux 会把孤儿重新挂到合适的 reaper，通常最终由 PID 1 或 subreaper 承担。

BoarOS 当前把仍有父关系的后代重新挂到存活的 PID 1。若 PID 1 本身正在退出或不存在，任务成为 parentless，由 idle 在退出后静默回收；它不会伪造一个 PID 1 completion。父子树使用双向兄弟链，并在 clone、wait、退出和 reparent 边界检查首尾、前后链接、反向 parent、身份与状态，避免只读一两个指针就把结构损坏当成合法事件。

## 信号 ABI 与恢复现场

信号处理函数是插入用户执行流的一次调用。内核必须保存被中断的 PC、整数/浮点寄存器和 mask，再安排 handler 入口；sigreturn 恢复的是这份用户现场，不是内核 C 调用栈。RISC-V 即使只实现 D 扩展，也要遵守 libc 为 Q 扩展保留的对齐和容量：ucontext 的 mcontext 偏移 176 字节、总大小 960 字节。仅让内核用同一套错误偏移保存和恢复，可能看似能返回，却会让 SA_SIGINFO handler 读取错误寄存器；应通过 libc 类型和真实上下文访问交叉验证。

sigsuspend 的临时 mask 用于选择本次 handler，原 mask 放进恢复现场，不能在选择信号前恢复。SA_RESTART 也不是所有 syscall 的统一开关：可重启的阻塞 I/O 可以重执行，nanosleep 遇到用户 handler 仍应 EINTR；没有 handler 的停止/继续则可借 restart block 保持原绝对 deadline。

SIGCHLD 的默认忽略与显式 SIG_IGN 具有不同回收语义：前者仍允许 wait 获取 zombie，后者及 SA_NOCLDWAIT 不保留待 wait 的子进程。通知、调度唤醒和资源释放应分别建模，不能用“是否安装 handler”同时决定三者。

## 所有权与失败处理

内核资源释放可能只完成一部分。例如文件槽已经不可见，但底层 close 或堆对象释放失败；页表的部分叶子已经回收，但记录页仍未释放。这样的对象不能退回 LIVE，也不能丢失唯一指针。

BoarOS 的处理原则是：对象状态表达剩余 owner 和准确清理阶段，错误码只描述本次尝试。进程退出依依赖顺序释放 exec transaction、files/OFD、fs context、MM、PID、任务页；任一步失败都把仍拥有资源的对象留在可重试队列。克隆构造在发布父子关系和 ready 状态前完成，失败子对象不产生用户可见 completion。

检查完整性时可以把每个任务按“调度状态 × 是否有父任务 × 重资源是否清空 × PID 是否拥有”分类：

- READY/RUNNING/BLOCKED 用户任务必须拥有 LIVE MM 和有效 PID；
- EXITED 可能保留待清理重资源，也可能只等待 PID/任务页释放；
- ZOMBIE 必须有父任务、有效 PID，且 exec/files/fs/MM 已清空；
- 已由 wait 摘除的对象不再有父关系或 PID，只能在任务页释放失败时短暂位于静默 EXITED 队列；
- boot completion 只属于启动路径直接创建的顶层任务，普通子进程由 wait 消费。

这种状态树比按 `if/else` 数量判断覆盖更可靠；测试还要注入每个提交点之后的释放失败，并核对最终 PID、heap 和物理页基线，才能发现静默泄漏或双重释放。

## 性能观察点

当前 context switch 本身不复制内存、不遍历进程树，也不修改资源引用；ASID 0 导致的全局 TLB 刷新是主要固定成本。进程相关成本集中在：

- COW fork 的页表/VMA 遍历，以及后续首次写入的单页复制；
- fd 表和 cwd 的堆分配与复制；
- wait 按子进程数线性扫描；
- 退出时关闭文件和销毁页表；
- 单 hart 关中断临界区造成的中断延迟。

QEMU 可以验证语义和结构成本，但不能替代 VisionFive 2 上的 cycle、cache miss、TLB miss 和最坏中断延迟测量。COW 已解决 fork 的结构性整页复制问题；更细的 walker、ASID、哈希 PID 查找或复杂 wait 队列仍应由真实进程规模和可重复基准驱动，同时保持现有 ABI 与生命周期边界。

## vfork 与 CPU 记账（当前结论）

- vfork 的本质是"共享地址空间 + 挂起父进程"：子进程经 MM 句柄引用共享（`kernel_mm_acquire`）运行在父进程页表上，父进程在 clone syscall 内阻塞到子进程 exec 或退出。完成条件绑定具体 child：共享 MM 引用释放后，先清除 child 的完成关联，再唤醒父进程；父进程循环检查一次性完成条件，普通信号不能提前解除等待。两个代码位置都调用 wake 并不天然幂等，旧 child 的后续 exit 可能误唤醒下一次 vfork。VFORK 不带 CLONE_VM 在 Linux 里是另一种语义（只挂起不共享），没有真实消费者前保持 ENOTSUP。
- 共享地址空间窗口内父子不同时运行（单 hart + 父挂起），因此没有 TLB/并发问题；这依赖"父进程必须挂起"的 vfork 契约，线程化后该论证不再成立。
- 每 task CPU 记账在 tick 边界记给被中断的任务：固定 tick 记账的量化误差最多一个 tick，且 CLK_TCK=100 时 `tms` 的用户可见值本来就是整数 tick，切换边界采样的精度收益不可观察。子进程记账在 wait 回收点回卷（自身 + 已回卷的孙辈），孤儿由 PID 1 回收时同路径归并，无父回收时消亡——与 Linux 的 rusage 回卷一致。
- 可观察的共享证明：子进程在共享窗口内修改 brk，父进程恢复后 `brk(0)` 直接读到修改值；这比检查私有/共享页内容更能确定地证明语义。

## 资料依据

执行 `make references` 后可核对：

- `references/linux/kernel/fork.c`：Linux task 创建、clone 资源复制/共享入口；
- `references/linux/kernel/exit.c`：退出、reparent、zombie 与 wait 事件处理；
- `references/linux/fs/file.c`、`references/linux/include/linux/fdtable.h`：fd table 与 open file description 生命周期；
- `references/riscv/riscv-privileged-20260120.pdf`：RISC-V Trap 返回、页表与 TLB 同步机制。

当前实现接口、不变量与测试入口见[内核调度与进程生命周期模块](../modules/kernel-scheduler.md)、[内核 MM 模块](../modules/kernel-mm.md)和[进程文件资源模块](../modules/kernel-files.md)。
