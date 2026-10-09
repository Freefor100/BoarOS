# Linux 线程组与 futex

## 进程身份与执行线程

Linux 的调度对象是线程。TID 标识一个执行线程，TGID 标识线程组；`gettid` 返回前者，`getpid` 返回后者。每个线程拥有寄存器、内核栈、用户 TLS 指针、信号屏蔽字和线程定向 pending；地址空间、fd 表、cwd/root 和信号 disposition 则可以共享。

共享 fd 表与共享 open file description（OFD）不是同一件事。普通 fork 复制 fd 表，父子可以独立关闭同一个编号，但所引用 OFD 的 offset 仍共享。`CLONE_FILES` 共享整张表，任一线程的 open/close/dup 都会改变其他线程看到的编号。阻塞 I/O 必须在睡眠前取得 OFD 引用，否则其他线程关闭并复用该编号时，恢复的 I/O 会访问已经释放或完全不同的对象。引用必须沿原调用栈释放，不能通过直接丢弃阻塞任务的栈完成组退出。

BoarOS 使用组长任务作为进程身份容器和父子树节点，不另建 process 对象。组长线程先退出时，只回收它的执行资源，保留身份容器；最后一个成员退出且资源完成清理后，才形成一次可供父进程 wait 的 zombie。非组长 exec 必须接管原 TGID 和父子关系，不能让成功 exec 看起来变成另一个进程。

## clone、TLS 与退出通知

`clone` 的 flag 决定资源共享，不是一个简单的“是否线程”开关。`CLONE_THREAD` 要求 `CLONE_SIGHAND`，后者要求 `CLONE_VM`。资源组合只有在退出、fork、exec、错误回滚都正确时才算支持；无法提供完整语义的合法组合应明确拒绝。

RISC-V 的 `tp` 保存用户线程指针。动态链接器或 libc 为各线程分配 TLS，然后由 `CLONE_SETTLS` 把新线程的 `tp` 设置为对应值；内核无需执行 ELF 重定位或理解 libc 的 TLS 分配算法。整数寄存器和 FP 状态也必须完整继承。

`exit` 只结束调用线程，`exit_group` 结束线程组。`CLONE_CHILD_CLEARTID` 的完成通知由内核承担：退出线程在释放 MM 前，仅当仍有其他任务使用同一 MM 时向指定用户字写零并唤醒 waiter，pthread join 才能安全确认它不再使用用户栈。不能先释放地址空间再访问该用户字，也不能把无效用户地址当成内核对象损坏。

## futex 的原子等待

futex 是“用户态原子变量 + 内核等待队列”，不是每次加锁都进入内核的锁对象。无竞争 mutex 通常只在用户态用原子指令完成；竞争时才请求内核睡眠或唤醒。

固定 Linux `kernel/futex/waitwake.c` 中，无超时 WAIT 的 signal 结果是 `-ERESTARTSYS`；带超时 WAIT 则把用户地址、expected、flags 和首次换算出的 absolute time 写入 `restart_block`，返回 `-ERESTART_RESTARTBLOCK`。RISC-V signal 返回路径只让前者受 handler 的 `SA_RESTART` 控制；后者一旦实际执行用户 handler 就改为 EINTR，只有没有 handler 的路径切换到 `restart_syscall`。因此 timed WAIT 不能简单套用 generic restart，也不能在重启时重新解析原 relative timeout。

glibc 2.44 的 `pthread_join` 在 fixed source `nptl/pthread_join_common.c` 调用 `__futex_abstimed_wait_cancelable64`；`nptl/futex-internal.c` 以 `FUTEX_WAIT_BITSET | FUTEX_CLOCK_REALTIME` 发出 raw syscall，即使未传 timeout 也使用 bitset 命令。BoarOS 曾让 `pthread_create` 成功，却在 `pthread_join` 返 ENOSYS 后由 glibc 报 futex fatal。只让 `FUTEX_BITSET_MATCH_ANY` 伪装成普通 WAIT 会错误处理其他非零掩码、绝对截止时刻和 WAKE_BITSET；在既有 per-task futex key 模型内，等待者另存掩码、REQUEUE 保持掩码，WAKE_BITSET 仅按相交位唤醒。固定 Linux `kernel/futex/syscalls.c` 与 `waitwake.c` 是错误顺序、掩码和绝对时钟依据。当前 BoarOS 没有修改 realtime 的 syscall，启动偏移不变；未来支持调时时，已经阻塞的 realtime wait 不能继续依赖这一固定偏移假设。

WAIT 必须原子地完成“比较用户字与 expected → 登记 waiter → 阻塞”。若比较与登记之间允许另一个线程修改用户字并执行 WAKE，唤醒可能落在空队列上，随后登记的线程就会错过通知。单 hart 的 BoarOS 通过关闭中断覆盖该区间；未来 SMP 必须在同一哈希桶锁保护下重新完成比较与登记，关本地中断并不能阻止其他 hart。

private waiter 使用单调分配且不复用的 MM 身份号和四字节对齐用户地址；共享匿名 waiter 使用后备对象身份和对象内连续字节偏移。哈希仅用于定位桶，命中后仍须比较完整 key。不同 MM 的相同虚拟地址不会串扰，fork 后不同 MM 的同一共享对象可以互相唤醒。等待者持有共享对象引用到等待调用恢复；requeue 为迁移者取得目标引用并释放源引用，避免最后一个映射消失后旧对象地址重用。单 hart 关中断串行化解析、比较与登记，SMP 仍须独立锁协议。

选择单调 MM 身份号，是因为 shared→private requeue 可以把等待者迁移到发起方的私有 key；发起方 MM 若随后释放，单用 MM record 页地址会在物理页复用后让旧等待者撞上新 MM。让等待者长期持有该 MM 会延长整个地址空间生命周期。64 位单调号在耗尽时拒绝创建新 MM，不回卷复用；共享匿名对象则由现有引用保护其地址身份。此取舍只针对单 hart 生命周期，并未建立跨核同步。

真实 U-mode 探针先用 fork 的独立 MM 验证共享唤醒，再覆盖共享→私有 requeue、同一对象内不同偏移迁移及不同对象同 VA 的隔离。最后由辅助线程撤销等待方的映射，发起方也撤销最后映射；等待者直到超时才释放对象引用。固定 Linux 同一 ELF 的差分入口同时覆盖基础共享 futex 与 bitset 参数、时钟和掩码交错。现有用户映射接口不能把一个共享匿名对象另映射到不同 VA，所以那一类别名仍需后续 mremap 或共享文件映射验收。

REQUEUE 先唤醒指定数量，再将剩余指定数量移动到另一个 key；迁移不等于唤醒。条件变量可以借此避免广播时所有线程同时抢同一 mutex。源 key 与目标 key 可能哈希到同一桶，遍历必须以原队尾为边界，不能反复处理刚追加的节点。

## 验证与成本

robust-list 的注册只保存用户地址，不能视作链内容可信或永久可访问。固定 Linux `kernel/futex/syscalls.c` 接受长度为 24 字节的 RV64 链头并允许注销；`kernel/futex/core.c` 在退出时限制遍历 2048 项，先读下一链接，再对 owner TID 匹配的 32 位字原子设置 `OWNER_DIED`，保留 `WAITERS` 并唤醒。`list_op_pending` 还覆盖解锁与唤醒之间死亡的窗口。`kernel/fork.c` 在普通退出及成功 exec 的 MM release 前调用 futex 清理。BoarOS 非组长 exec 会采用原组长 TID，因此清理时必须使用换号前保存的 TID。上述路径均依据本页末尾固定 Linux commit。

musl 1.2.5 的普通 `pthread_exit` 自己遍历 robust mutex 并处理 owner 死亡，detached 线程还会在释放用户栈前注销链头；仅让 libc-test 的 `pthread_robust_detach` 通过不能证明内核清理。真实 U-mode 测试另用原始 `SYS_exit` 绕开 libc 退出清理，检验 owner-died、pending、唤醒、坏链、PI 标记和 fork COW。跨 MM 共享匿名 futex 的 key 已接入普通 WAIT/WAKE/REQUEUE；PI 协议与共享文件后备仍须另行设计。

pthread 成功创建并不证明线程组完整：还应检查 join/TLS、竞争等待、超时和取消，以及组长先退、非组长 exec、阻塞成员终止和最终资源回收。模块验证负责 key 匹配和状态边界，真实 libc 消费者负责组合 ABI；两者不是互相替代关系。

退出路径也属于唤醒链路。`clear_child_tid` 已把 joiner 放入 ready，并不保证它马上
获得运行机会：如果退出强制切回 idle，恢复点恰在空闲循环的 yield 之后，下一条
`wfi` 会把就绪工作拖到下次中断。原先的 idle 回收要求在引入独立 cleanup worker
后已经不再适用于生产路径。复用现有 ready 选择、由 worker 保持回收 owner，可以
消除这个中转，不需要更换调度策略或提高 tick 频率。无 timer 的退出／join 反例负责
证明连续进展，原 pthread 程序负责检查实际成本；二者都要检查已退出任务的栈和 MM
只能由可信上下文释放。

BoarOS 使用 256 个桶和每队列 FIFO 成员链。普通唤醒不扫描全局 blocked 链，futex 唤醒仍需检查目标桶中的碰撞 waiter。deadline 到期由按 (deadline, tid) 排序的索引处理，只弹出已到期者。线程 clone 共享 MM/files/fs，避免复制页表和 fd 槽；首次建立共享 disposition 时，原本尚未分配的信号表仍需要分配，不能宣称所有 clone 都只分配一个页。QEMU 运行时间不构成真实硬件性能结论。

一次可复用的调试经验：线程控制块变大后，原本通过的 ext4 目录枚举可能耗尽剩余内核栈，而错误要到后续 syscall 的 canary 检查才被发现。用硬件 watchpoint 监视 canary 的首次写入，能把“nanosleep 失败”的表象还原到真正的 getdents 调用链。此处直接复用待返回 dirent 中的文件名空间消除了冗余 256 字节副本，不需要为每次枚举增加堆分配。

## 固定资料

- `references/linux/kernel/fork.c`、`kernel/exit.c`、`fs/exec.c`、`kernel/signal.c`、`kernel/futex/`：Linux commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e`。
- `references/musl/musl-1.2.5.tar.gz` 内 `src/thread/`、`src/ldso/` 与 `ldso/dynlink.c`：musl 1.2.5，SHA-256 `a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4`。
- `references/glibc/glibc-2.44.tar.xz` 内 `nptl/pthread_join_common.c`、`nptl/futex-internal.c`：glibc 2.44，SHA-256 `37f600f2bef3c5e8300147059568b2a2e40a7ad6ccc65ce942556d49429cc667`。固定 RV64 loader/libc 二进制身份见 `tests/userland/glibc/inputs.json`。

## child-TID 生命周期差分（2026-09-28）

通用基线 `fa38845` 只放开 process clone 的 CHILD 标志，尚未证明生命周期。
`tests/diff-abi/child_tid.c` 对照 `references/linux/kernel/fork.c` 的
`mm_release`（commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e`），旧实现实际出现三处差异：
父上下文写子 COW MM 导致 CHILD_SETTID 失败；独立 MM 退出错误清零共享匿名页；
vfork 成功 exec 提前丢掉 clear 指针，没有清零旧共享 MM。

修复把 CHILD_SETTID 延到子任务首次用户返回；MM 另计活跃任务，避免清理器的延迟引用
被误算为 mm_users；成功 exec 在旧 MM 激活期间完成 robust、清 TID，再切页表，
失败 exec 保留注册。共享物理页不等于共享 MM，坏用户地址不撤销 clone。

重建：`make test-mm-riscv test-exec-riscv test-user-riscv test-diff-abi-riscv`。
本次聚焦测试通过，完整差分 556/556（新增 8 条 tid.*）；Harness 同时检查 PID 1
完成、堆和任务资源回收。此阶段交给评测分支后应重跑原始 clone/pthread 测例；
比赛启动环境尚未完成，不能据此宣称评分已改善。

整合审查补充了清 TID 缺页期间的并发退出：A 尚在复制时 B 退出，两者都应按当时 MM 使用者数清零。MM 聚焦测试在真实页/MM 上从复制入口注入 B 退出，旧注销顺序得到 0x603（B 未清零）；将注销移至复制/唤醒之后通过。该注入是确定性模块交错，并不冒称真实磁盘时序；真实 U-mode 证据仍由 tid.* 提供。

## 统一进程身份对象（2026-09-29）

固定依据为 `references/linux` commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e`
的 PID、exec 和退出实现。TID/TGID/PGID/SID 现在通过同一编号/代次对象的角色成员链
维持所有权；编号在最后成员和临时引用释放前不复用。只剩 PGID/SID 引用的对象不能
被 task lookup 当成活任务。proc 原代次契约保留，非组长 exec 转移进程角色，退出的
组长容器保留到真正的进程结束。`__WNOTHREAD` 创建者关系也持身份引用，避免 TID 复用混淆。

对象在身份修改区外准备；修改区只发布/摘链，退役对象在区外释放。当前 syscall 的
SIE=0 本身不表示不可睡眠，禁止的是身份原子修改过程中分配、yield 或清理资源。
编号和可编码代次空间耗尽均返回资源耗尽，非法引用/重复释放仍视为内核不变量错误。

验证：`make test-pid-object-host` 覆盖角色存续、临时 pin、代次复用、角色转移、容量
和代次耗尽回滚；独立 ASan/UBSan 运行通过。`make test-scheduler-cases-riscv test-scheduler-riscv test-userland-riscv` 通过，包含 fork/vfork、pthread 与非组长 exec。
迁移后加 coarse clock 的完整 ABI 差分为 797 条匹配。此记录只证明身份迁移，
会话 syscall 与孤儿组事件另外验收。

## 会话、进程组与孤儿事件（2026-09-29）

固定 `references/linux` commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e` 的
`kernel/sys.c` 为 setpgid/getsid/setsid 的 errno 与状态依据，`kernel/exit.c` 的
will_become_orphaned_pgrp、reparent_leader、exit_notify 为孤儿组判定与通知依据。
setpgid/setsid 自身只改角色；固定 Linux 不在这两个 syscall 里主动发孤儿 HUP/CONT。
父子 exec 检查使用独立 fork_no_exec，会话组长使用独立 session_leader，避免把
整数等于 TGID 错当成 setsid 成功记录。成员引用让 PGID/SID 在原组长被收割后继续存在；
此时按旧编号查 task 必须失败，而存活成员的组/会话查询仍返回原身份。

测试的一个易混淆点是裸 Linux PID 1 初始 PGID/SID 为 0，而 BoarOS 的初始身份为 1。
把这个 0 再传 setpgid 会解释为目标自身，不能用于“重新加入父组”的验证。真实 ABI
用例先由辅助进程 setsid，再 fork 待测进程，在双方都有非零组/会话的上下文中比较。
父子 exec、停止和退出顺序由 pipe、wait4 和 proc zombie 状态握手，不用延时猜竞态。

孤儿 HUP/CONT 的 siginfo 来源是 SI_KERNEL=128，原 int8 来源字段会截断；现用 int16
保存来源，并贯通 sigtimedwait 与 SA_SIGINFO 信号帧。测试先暴露用户帧仍为 0 的差异，
再由统一 delivery.code 修复，不能只修等待 syscall。组信号查询也必须区分“进程仍有
身份”和“还有可运行线程”：zombie 收割前 kill 成功，发出的信号不向死线程排队。

`tests/diff-abi/session.c` 的 105 条同 ELF 窄差分在固定 Linux 与 BoarOS 上匹配，
覆盖四个 syscall、错误优先级、非组长目标/exec/setsid、PGID 成员阻止 setsid、zombie、
proc 字段、组信号/wait 选择，以及退出/reparent/handler 三类孤儿组观察。PGID/SID
在原组长收割后保留，SID-only 对象不作为 task 命中；临时引用和强制编号复用由阶段 A
的 PID 对象 host 契约测试覆盖。没有因此宣称控制终端、完整凭据权限或 SMP 已支持。

会话提交的独立源码树导出到 `build/session-stage-src/`，执行 `make -C build/session-stage-src -j4 all` 后，以 `python3 -B tests/diff-abi/harness.py --kernel build/session-stage-src/kernel-rv --program build/diff-abi/cases-rv` 跑完整差分，936 条全部匹配。内核 SHA-256 `96e4f211220c78a6d9d67a2c39a7505b9c1d2f5bf5fedf303a0b958d9aa3f554`，同 ELF SHA-256 `9c31cdff503d182da4a54707a181fcd54c1ed62724f85950abb3169e2a2a21e6`。该验证不包含尚在实现的 FIFO/RR 与预算机制。新增 session exec 用例必须安排在 proc 已打开执行文件 unlink 压力之前，避免测试自身先删除 `/init` 后等待一个无法 exec 的探针。

## 用户态重启测试的握手边界（2026-10-05）

网络服务回归暴露一次旧 `raw futex:11` 失败。独立提取原信号重启函数后，
原生 Linux 7.2.7-zen1-1-zen 在首次尝试返回 EAGAIN，原 BoarOS `5687377`
在第44次出现同样结果。父线程看见handler写入标志，不等于子线程已重新进入
WAIT；此时父线程先改字，重启重新比较后返回EAGAIN合法。固定依据为
`references/linux/kernel/futex/waitwake.c`，commit
`f4cdf7ca9a1fdcca413157df19753f388a5a224e` 的值检查。

原fixture还混用private WAIT与shared WAKE；固定Linux的原循环在首次等待
挂住，不能将BoarOS上的偶然通过当作正确跨平台握手。测试现统一使用private
WAKE，重启成功场景保持用户字不变，直到WAKE明确返回选中一个等待者。
仍要求最终WAIT返回0，不通过放宽为任意错误来隐藏失败；其他改变用户字和
非重启的场景继续检查各自错误。

修正后的原生Linux重复2000次通过；同一RV64 ELF（SHA-256
`5a994a989c1866b0bbc84a944bc42810efaf2b01387009d5b803d2b562cc3898`）
在原BoarOS和固定Linux各重复200次通过，完整 `make test-userland-riscv`
亦通过。重建的常规入口仍是该目标；内核futex实现没有随此测试修正改变。

## CPU 本地与 raw 锁的边界

内核 tp 继续指向任务，任务关联 CPU 记录；它与用户 TLS 的保存完全独立。
CPU 记录持有 current、待调度标志和短锁/抢占深度，I/O guard 与回收深度仍由任务持有。
禁止抢占只保证本地执行上下文稳定；共享元数据还需要带 acquire/release 内存序的互斥。
raw guard 保存原 IRQ 状态并登记到 CPU 持有链，禁止递归、逆序和持锁阻塞。
timer 可以记账并留下待调度请求，解除禁止抢占不在任意调用栈直接切换。
延期状态还须保留OTHER轮转原因：只有need_resched时，同级peer既不higher、OTHER也不expired，
安全返回会清掉请求并继续当前任务。idle对照会被higher条件掩盖，因此另用两个普通OTHER
任务保护延期tick的轮转和退出回收。显式preempt计数可嵌套在raw内，但不能抵消raw自身的深度。

固定依据为 `references/linux/Documentation/locking/locktypes.rst`，commit
`f4cdf7ca9a1fdcca413157df19753f388a5a224e`；LA CPU ID 来自
`references/linux/arch/loongarch/include/asm/loongarch.h` 的 CPUID CSR 0x20。
宿主四线程互斥与客户机单核抢占是不同证据，二者都不能代替客户机多核等待/回收验收。

## PID 1 关机与存活后台 owner（2026-10-06）

原版五项脚本全结束后出现 root CLEANUP（0xb）。最小复现为 PID 1 fork 出
setsid 后的后台进程，后者保留文件、cwd、MM 和监听 socket，PID 1 随后退出。
同步输出实际得到 `root finish failure stage=0x4 error=0xfffffff0`：卸载根盘
返回 EBUSY。旧 5687377 生产 ELF 同样失败；控制流程可追溯到 8dfffb7，不能
归因于本轮页缓存优化或协议池调整。原 iperf 脚本启动 daemon 而不在末尾停止它，
暴露了这个边界；之前的受控网络执行器主动停止 server，未覆盖该关机路径。

责任在内核关机顺序：cleanup worker 只确认 PID 1 已回收就调用 root finish，
其余用户进程仍是真实 owner。VFS 拒绝卸载是正确行为，不能忽略 EBUSY 或强制
释放它们的对象。修复保存 PID 1 completion，通过既有组退出/取消路径结束所有
用户任务，等待其回收，再停止内核 I/O 服务及卸载。正在关机时新发布的 fork 也由
后续收口处理；只保存原 PID 1 状态，避免被其他 completion 或复用编号覆盖。
普通父进程退出仍按原来的 reparent 语义处理。

失败 owner 原本已经有输出，但排在异步 UART 队列中，紧接着切 emergency 并关机，
现场行可能来不及发送。错误分支现在先切同步输出，再打印 stage/error，无需新增
日志队列或诊断构建。

独立实际 U-mode 回归使用存活 daemon、STOPPED 子进程、join 后仍存活的线程组，
以及关机期间 fork。旧基线 daemon 场景先失败；修复后四项均保留 PID 1 的退出码
37、heap-live=0 和物理页基线。原有未 wait 的 zombie 收口作为另一个窄回归。

```sh
make all INIT_CONFIG=config/init.json
python3 -B tests/root-shutdown-riscv.py
make test-root-orphan-riscv
```

这是进程/关机生命周期修复，没有修改日志 durable、checkpoint 或设备 reset 语义；
不为此重跑存储恢复、参数或完整 ABI 矩阵。

## 代次等待与切换尾部

登记与park分离后，wake可能发生在任务仍执行旧栈时。token代次解决旧通知误中下一次等待，
on_cpu与新栈完成点解决同一任务重新入队或旧栈提前回收；两者解决不同问题。任务提交阻塞后
即使已经收到通知，也只能在switch_finish后重新发布runnable。回调借用同样不能由摘队代替：
退休节点保留引用后继，context释放前同步等借用结束。调度raw域只做这些短元数据操作，
callback在锁外同步运行并拒绝阻塞，业务对象的锁与存活仍由消费者负责。

2026-10-09聚焦验证使用真实`kernel/sched/park.c`的2/4宿主通知线程及RV/LA单CPU512MiB/1GiB
真实切换；重建命令为`make test-wait-host test-wait-riscv test-wait-loongarch`。固定同步资料仍为
`references/linux/kernel/sched/core.c`及`include/linux/wait.h`，commit
`f4cdf7ca9a1fdcca413157df19753f388a5a224e`；实现保留BoarOS的任务资格与owner契约。

可睡眠锁的内部资格与业务对象的数据保护分开：前者用每实例raw30及调度raw40，后者继续由
mutex/RW guard的任务owner承担。读者批次释放内部raw时，handoff owner保留原批次边界；
后来到达者不能搭上尚未完成的旧批次。宿主握手在第16个读者后强制插入后来读者；删除批次
边界的候选被反例拒绝，生产实现保持后来者等待旧批次退出。重建为`make test-sleep-lock-host`。

futex过滤wake不能沿用跨解锁的裸next。游标借用node，requeue额外借task再释放当前游标，
旧node借用收完后才移动登记；后备引用取得/归还留在raw域外。全局不回退的等待代次还覆盖
任务页复用：同地址的新任务不能消费旧token。当前资料与客体配置不变，这些同步原语的宿主
并发结果仍不等于共享MM、FD/OFD、缓存或协议业务数据已完成SMP同步。

可睡眠锁必须区分通知与资格。额外通知时，等待者结束旧token、在对象raw内复查资格，
尚未授予才重新登记，原pending位置不变。结束登记只清局部token副本；pending中的token
只在对象raw内更新，释放者可在该间隙授资格并发送已经过期的通知。等待者随后复查即可前进。
`test-sleep-lock-host`以握手覆盖这两个间隙，保护继续等待及资格唯一性。
