# 内核调度与进程生命周期模块

本文描述当前单 hart FIFO 调度、任务身份、父子关系、普通进程 clone、阻塞 wait、退出与 zombie 回收契约。执行现场原理见[内核线程与抢占调度学习总结](../learning/kernel-scheduling.md)，进程语义背景见[进程生命周期学习总结](../learning/process-lifecycle.md)，地址空间所有权见[内核 MM 模块](kernel-mm.md)。

## 范围与入口

| 文件 | 当前职责 |
|---|---|
| `include/arch/riscv/context.h`、`arch/riscv/context.c` | 初始化 RISC-V switch context，提供 current `tp` 与 SIE 临界区操作 |
| `include/arch/riscv/thread.h`、`arch/riscv/context_switch.S` | 定义 Trap 汇编可见任务前缀，保存/恢复 `ra/sp/tp/s0..s11` |
| `include/arch/riscv/process.h` | 隔离依赖 RISC-V Trap Frame 的 clone 入口 |
| `include/kernel/pid.h`、`kernel/pid.c` | 管理 1..32768 的可回收 PID/TID 位图 |
| `include/kernel/task.h` | 暴露不透明 current task、TID/TGID/PPID 与当前资源借用 |
| `kernel/sched/core.c` | idle、任务页、ready FIFO、tick 抢占和首次用户任务创建 |
| `kernel/sched/process.c` | 父子链、clone/wait、BLOCKED/wakeup、exit/zombie/reap |
| `kernel/sched/exec.c` | exec 映像提交 |
| `kernel/sched/private.h` | scheduler 私有对象布局与跨实现文件接口 |

主要进程接口为：

```c
enum kernel_scheduler_status riscv_process_clone_current(
    const struct riscv_trap_frame *parent_frame,
    uint64_t child_stack,
    int64_t *linux_result);

enum kernel_scheduler_status kernel_scheduler_wait4_current(
    int64_t pid,
    uint64_t status_address,
    uint32_t options,
    uint64_t rusage_address,
    int64_t *linux_result);

enum kernel_mm_status kernel_scheduler_resolve_current_user_fault(
    uint64_t virtual_address,
    uint32_t access);

void kernel_user_thread_exit(
    enum kernel_thread_exit_reason reason,
    uint64_t status,
    uint64_t detail) __attribute__((noreturn));
```

RISC-V clone 接口接收 syscall 入口保存的完整寄存器快照；通用 scheduler 头不暴露架构 Trap Frame。普通任务创建、tick、exec、资源借用和 idle reaper 仍由 `include/kernel/scheduler.h` 与 `include/kernel/task.h` 提供。

## 任务对象与创建所有权

每个非 idle 任务使用一张 4 KiB 物理页，页内依次是任务元数据、canary 和向下增长的内核栈。用户任务拥有独立 TID、单成员线程组、MM 句柄、files 句柄、fs context 与可选 exec 清理事务。生产创建要求入口具有 U+X，`SP-1` 具有 U+R+W，SP 按 16 字节对齐，MM 与文件资源使用 scheduler 的分配器。

创建先验证全部输入，再分配任务页、构造 Trap Frame/context 和 TID，最后移动 MM/files/fs owner 并发布到 ready 队列。成功消耗调用者传入的 owner；普通失败保留调用者资源。任务页立即归还失败时由 scheduler 的单页清理槽保留唯一 owner，idle reaper 重试后才允许下一次可能占用该槽的创建。

`struct kernel_task` 对公共层不透明。MM/fs 借用只读，files 借用允许同步 syscall 更新 fd 表和 open-file offset；借用不增引用，只在 current RUNNING 用户任务的内核调用链内有效。

## 普通 clone 的资源语义

当前只接受 Linux RISC-V `clone(SIGCHLD, 0, 0, 0, 0)`，语义等同普通 fork 进程：

- 子进程获得新 TID/TGID，线程组只有自己，进程组继承父进程；
- RISC-V Trap Frame 完整复制，子进程 `a0=0`，父进程得到子 PID，两者都从 ecall 后一条指令继续；
- MM 通过 `kernel_mm_fork()` 克隆 VMA/页表并以 COW 共享用户页，父子写入后按需获得不同 PA；
- fd 表和 descriptor flags 独立复制，open file description 引用共享，因此 offset 与底层 file 生命周期共享；
- fs context 独立复制当前 cwd，并继续借用同一个 root mount；
- exec 清理事务不继承。

构造过程在子任务进入父子树和 ready 队列前完成。失败先释放已经取得的 fs/files/MM/TID/任务页；不能立即完成的 owner 放入不发布 completion 的 exited 清理队列，父进程仍得到准确的 `-ENOMEM` 或 `-EAGAIN`，不会看到半构造子进程。

当前 COW fork 仍需遍历已提交页并建立子页表，时间与页数线性，且构造期间关闭本 hart 中断；它避免了 fork 时的数据页复制和同量内存峰值，但最坏中断延迟仍需在开发板测量。文件 VMA 的 OFD 来源也在发布子任务前克隆，失败不会留下半构造父子关系。

## 父子树、状态与 wait

父任务保存双向兄弟链的首尾，子任务保存 parent、前后兄弟。进程路径在 clone、wait、reparent 与退出回收边界检查以下不变量：首尾同时为空或同时存在；首节点无前驱、尾节点无后继；每个节点反向指向同一父任务；链长不超过 PID 上限；链上任务拥有有效用户身份且不发布 boot completion。tick 热路径不遍历父子树。

状态转换为：

```text
                      timer
READY <------------------------------ RUNNING
  ^                                      |
  |                                      +-- wait4(no event) --> BLOCKED
  |                                              |
  +---------------- child event / wake ----------+

RUNNING -- exit, cleanup complete, has parent --> ZOMBIE
RUNNING -- exit, cleanup pending/no parent -----> EXITED
EXITED  -- idle retry complete, has parent -----> ZOMBIE
ZOMBIE  -- matching parent wait4 --------------> PID/task page released
EXITED  -- parentless retry complete -----------> PID/task page released
```

`wait4` 支持 `pid>0`、`pid==0`、`pid==-1` 和 `pid<-1` 的 Linux 选择规则，并接受 `WNOHANG/WUNTRACED/WCONTINUED/__WNOTHREAD/__WALL/__WCLONE` 位。当前没有 stopped/continued 事件，因此后两类选项只影响等待集合，不会制造事件。普通 SIGCHLD 子进程被 `__WCLONE` 排除，除非同时使用 `__WALL`。无匹配子进程返回 `-ECHILD`；有匹配但无事件时 `WNOHANG` 返回 0，否则父进程进入 BLOCKED，由子进程成为 zombie 时唤醒。

退出码编码为 `(status & 0xff) << 8`。已分类的页访问错误以内部 `SIGNAL` 原因保存 `SIGSEGV(11)` 或 `SIGBUS(7)`；其他用户同步故障按 scause 转换为 SIGILL/SIGTRAP/SIGBUS/SIGSEGV 形态 wait status。用户缺页耗尽物理页时，内部 completion 保留 `RESOURCE/NO_MEMORY`，父进程看到 SIGKILL 形态的 wait status 9。当前这只是不可捕获的终止与 wait 编码，还没有信号投递/handler。非空 rusage 当前返回 `-ENOTSUP`；未知 option 返回 `-EINVAL`，无法安全取负的 `INT32_MIN` pid 返回 `-ESRCH`。与 Linux 回收顺序一致，zombie 先被逻辑回收，再向用户复制 status；因此 status 指针错误返回 `-EFAULT` 时，该子进程也已不可再次 wait。

## 退出、reparent 与失败恢复

用户任务退出时仍运行在自己的任务页栈上，不能释放该页。路径先切到稳定内核 `satp`，把现有子进程重新挂到仍存活的 PID 1；没有可用 PID 1 时，活子进程成为 parentless，已有 zombie 转入静默 exited 回收。随后按以下顺序释放重资源：

```text
pending exec transaction -> files/open descriptions
-> fs context -> MM -> zombie 或 exited
```

有父进程且重资源已清空时只保留 task 页、PID、亲缘和 wait status，进入 ZOMBIE；这保证 zombie 不长期占用用户页、页表或文件对象。清理失败时进入 EXITED，由 idle 在可信内核地址空间和 boot 栈上重试；成功后再转 zombie 并唤醒父进程。Parentless 任务由 idle 继续释放 PID 和任务页；只有启动路径直接创建且标记发布的任务会产生 boot completion，克隆失败和孤儿清理不会伪造 PID 1 完成事件。

父进程 wait zombie 时先释放 PID、摘除亲缘，再释放 task 页。若最后一步失败，节点以无 PID、不可发布状态进入 exited 队列重试，避免重复 wait 或重复释放 PID。所有部分失败都由对象状态保留唯一 owner，错误码本身不代替所有权判断。

## 临界区与性能边界

当前单 hart 通过关闭 SIE 串行化 runqueue、父子树、PID、files/fs/MM 生命周期；这不是 SMP 锁。接入多 hart 时必须为 runqueue、进程树、PID 分配、文件表/OFD 引用和 MM/COW 增加锁或原子协议，并处理远端 TLB shootdown。

tick/context switch 只检查 task/queue 常量状态、读取缓存的 `satp` 并切换 context，不获取资源引用、不遍历父子树、不复制页。当前 ASID 0 的地址空间切换执行全局 `SFENCE.VMA`，是明确的切换成本。process 路径按子进程数线性扫描 wait 集合；fork 的主要成本是用户页表/VMA 遍历以及 fd/cwd/OFD 来源复制，数据页只在后续 COW 写 fault 时复制。开发板性能验证尚未进行，不能据 QEMU 时间宣称硬件性能。

## 验证与限制

```sh
make test-scheduler-cases-riscv
make test-scheduler-riscv
make test-mm-riscv
make test-files-riscv
make test-user-riscv
make test-root-init-riscv
make test-demand-page-riscv
make test-exec-riscv
make test-riscv
```

聚焦测试覆盖调度状态、创建与清理失败；MM/files 测试分别证明地址空间 COW 与 OFD 引用共享。生产 ext4 三映像链覆盖 clone 双返回、PPID、WNOHANG/阻塞唤醒、wait selector、退出码、`SIGSEGV`/`SIGBUS` 状态、EFAULT 后已回收、fd offset 共享、MM 写隔离、孙进程向 PID 1 reparent，以及最终 heap/物理页基线。demand-page OOM 版本验证资源退出编码为 wait status 9，且仍走同一 zombie/reap 资源闭环。

当前限制是 RISC-V64 单 hart、ASID 0、FIFO/单 tick 时间片、4 KiB 单页内核栈、单成员线程组和普通 SIGCHLD clone。尚无 `CLONE_VM/CLONE_FILES/CLONE_THREAD`、信号投递/handler、futex、vfork、rusage、停止/继续事件、SMP COW 同步、内核栈 guard、F/V 上下文或 LoongArch context。
