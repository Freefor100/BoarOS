# Linux 线程组与 futex

## 进程身份与执行线程

Linux 的调度对象是线程。TID 标识一个执行线程，TGID 标识线程组；`gettid` 返回前者，`getpid` 返回后者。每个线程拥有寄存器、内核栈、用户 TLS 指针、信号屏蔽字和线程定向 pending；地址空间、fd 表、cwd/root 和信号 disposition 则可以共享。

共享 fd 表与共享 open file description（OFD）不是同一件事。普通 fork 复制 fd 表，父子可以独立关闭同一个编号，但所引用 OFD 的 offset 仍共享。`CLONE_FILES` 共享整张表，任一线程的 open/close/dup 都会改变其他线程看到的编号。阻塞 I/O 必须在睡眠前取得 OFD 引用，否则其他线程关闭并复用该编号时，恢复的 I/O 会访问已经释放或完全不同的对象。引用必须沿原调用栈释放，不能通过直接丢弃阻塞任务的栈完成组退出。

BoarOS 使用组长任务作为进程身份容器和父子树节点，不另建 process 对象。组长线程先退出时，只回收它的执行资源，保留身份容器；最后一个成员退出且资源完成清理后，才形成一次可供父进程 wait 的 zombie。非组长 exec 必须接管原 TGID 和父子关系，不能让成功 exec 看起来变成另一个进程。

## clone、TLS 与退出通知

`clone` 的 flag 决定资源共享，不是一个简单的“是否线程”开关。`CLONE_THREAD` 要求 `CLONE_SIGHAND`，后者要求 `CLONE_VM`。资源组合只有在退出、fork、exec、错误回滚都正确时才算支持；无法提供完整语义的合法组合应明确拒绝。

RISC-V 的 `tp` 保存用户线程指针。动态链接器或 libc 为各线程分配 TLS，然后由 `CLONE_SETTLS` 把新线程的 `tp` 设置为对应值；内核无需执行 ELF 重定位或理解 libc 的 TLS 分配算法。整数寄存器和 FP 状态也必须完整继承。

`exit` 只结束调用线程，`exit_group` 结束线程组。`CLONE_CHILD_CLEARTID` 的完成通知由内核承担：退出线程在释放 MM 前向指定用户字写零并唤醒 waiter，pthread join 才能安全确认它不再使用用户栈。不能先释放地址空间再访问该用户字，也不能把无效用户地址当成内核对象损坏。

## futex 的原子等待

futex 是“用户态原子变量 + 内核等待队列”，不是每次加锁都进入内核的锁对象。无竞争 mutex 通常只在用户态用原子指令完成；竞争时才请求内核睡眠或唤醒。

固定 Linux `kernel/futex/waitwake.c` 中，无超时 WAIT 的 signal 结果是 `-ERESTARTSYS`；带超时 WAIT 则把用户地址、expected、flags 和首次换算出的 absolute time 写入 `restart_block`，返回 `-ERESTART_RESTARTBLOCK`。RISC-V signal 返回路径只让前者受 handler 的 `SA_RESTART` 控制；后者一旦实际执行用户 handler 就改为 EINTR，只有没有 handler 的路径切换到 `restart_syscall`。因此 timed WAIT 不能简单套用 generic restart，也不能在重启时重新解析原 relative timeout。

WAIT 必须原子地完成“比较用户字与 expected → 登记 waiter → 阻塞”。若比较与登记之间允许另一个线程修改用户字并执行 WAKE，唤醒可能落在空队列上，随后登记的线程就会错过通知。单 hart 的 BoarOS 通过关闭中断覆盖该区间；未来 SMP 必须在同一哈希桶锁保护下重新完成比较与登记，关本地中断并不能阻止其他 hart。

每个 waiter 的身份由 MM 和四字节对齐用户地址组成，哈希仅用于定位桶，命中后仍须比较完整 key。不同 MM 的相同虚拟地址不是同一个私有 futex。当前没有共享映射，所以非 private 操作也只保证同一 MM 内语义；跨 MM 共享 futex 需要共享 backing 的身份，不能简单删掉 key 中的 MM。

REQUEUE 先唤醒指定数量，再将剩余指定数量移动到另一个 key；迁移不等于唤醒。条件变量可以借此避免广播时所有线程同时抢同一 mutex。源 key 与目标 key 可能哈希到同一桶，遍历必须以原队尾为边界，不能反复处理刚追加的节点。

## 验证与成本

robust-list 的注册只保存用户地址，不能视作链内容可信或永久可访问。固定 Linux `kernel/futex/syscalls.c` 接受长度为 24 字节的 RV64 链头并允许注销；`kernel/futex/core.c` 在退出时限制遍历 2048 项，先读下一链接，再对 owner TID 匹配的 32 位字原子设置 `OWNER_DIED`，保留 `WAITERS` 并唤醒。`list_op_pending` 还覆盖解锁与唤醒之间死亡的窗口。`kernel/fork.c` 在普通退出及成功 exec 的 MM release 前调用 futex 清理。BoarOS 非组长 exec 会采用原组长 TID，因此清理时必须使用换号前保存的 TID。上述路径均依据本页末尾固定 Linux commit。

musl 1.2.5 的普通 `pthread_exit` 自己遍历 robust mutex 并处理 owner 死亡，detached 线程还会在释放用户栈前注销链头；仅让 libc-test 的 `pthread_robust_detach` 通过不能证明内核清理。真实 U-mode 测试另用原始 `SYS_exit` 绕开 libc 退出清理，检验 owner-died、pending、唤醒、坏链、PI 标记和 fork COW。该阶段的 key 仍只识别同一 MM；跨 MM 共享后备对象与 PI 协议须另行设计。

pthread 成功创建并不证明线程组完整：还应检查 join/TLS、竞争等待、超时和取消，以及组长先退、非组长 exec、阻塞成员终止和最终资源回收。模块验证负责 key 匹配和状态边界，真实 libc 消费者负责组合 ABI；两者不是互相替代关系。

BoarOS 使用 256 个桶和每队列 FIFO 成员链。普通唤醒不扫描全局 blocked 链，futex 唤醒仍需检查目标桶中的碰撞 waiter。deadline 到期目前仍扫描全局 blocked 链。线程 clone 共享 MM/files/fs，避免复制页表和 fd 槽；首次建立共享 disposition 时，原本尚未分配的信号表仍需要分配，不能宣称所有 clone 都只分配一个页。QEMU 运行时间不构成真实硬件性能结论。

一次可复用的调试经验：线程控制块变大后，原本通过的 ext4 目录枚举可能耗尽剩余内核栈，而错误要到后续 syscall 的 canary 检查才被发现。用硬件 watchpoint 监视 canary 的首次写入，能把“nanosleep 失败”的表象还原到真正的 getdents 调用链。此处直接复用待返回 dirent 中的文件名空间消除了冗余 256 字节副本，不需要为每次枚举增加堆分配。

## 固定资料

- `references/linux/kernel/fork.c`、`kernel/exit.c`、`fs/exec.c`、`kernel/signal.c`、`kernel/futex/`：Linux commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e`。
- `references/musl/musl-1.2.5.tar.gz` 内 `src/thread/`、`src/ldso/` 与 `ldso/dynlink.c`：musl 1.2.5，SHA-256 `a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4`。
