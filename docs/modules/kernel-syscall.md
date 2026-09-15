# 系统调用解码模块

本文描述与架构 Trap Frame 解耦的系统调用语义接口。模块处理已经从用户寄存器复制出的请求、用户访问和所属子系统调用；RISC-V Trap 层负责 `a7/a0..a5` 转换、普通返回时的 `sepc` 推进、signal return 和 syscall restart，scheduler 负责退出、exec 提交与资源回收。

## 接口

公共入口位于 `include/kernel/syscall.h`；`kernel/syscall/dispatch.c` 负责分派和简单系统信息，`file.c`、`memory.c`、`process.c`、`signal.c`、`time.c` 处理对应 ABI。实际执行命名为 `syscall_handle_*`，纯参数转换才使用 decode；共享声明限于 `private.h`，不引用 scheduler 私有任务布局：

```c
enum kernel_syscall_status kernel_syscall_dispatch(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *result);
```

调用者是 scheduler 当前不透明 task，供依赖任务身份或资源的系统调用取得明确上下文；通用解码层不从 RISC-V `tp` 隐式寻找 current。请求包含系统调用号和六个 64 位参数。结果是以下六种动作之一：

- `KERNEL_SYSCALL_ACTION_RETURN`：把 `value` 写回用户返回值寄存器后继续执行。
- `KERNEL_SYSCALL_ACTION_EXIT`：以 `value` 作为退出状态终止当前用户任务。
- `KERNEL_SYSCALL_ACTION_EXEC`：新映像已经准备完成；不推进旧 `sepc`，由 scheduler 切换 MM 并重建 Trap Frame。
- `KERNEL_SYSCALL_ACTION_CLONE`：参数已经符合当前普通进程 clone 子集；架构 Trap 层把完整寄存器快照交给进程层构造子进程。
- `KERNEL_SYSCALL_ACTION_WAIT4`：参数保持 Linux ABI 形态，由 scheduler 完成选择、阻塞、唤醒与 zombie 回收。
- `KERNEL_SYSCALL_ACTION_YIELD`：参数已经为空；由 scheduler 把当前任务重新排到 ready 队尾并在存在竞争者时切换。

空调用者、空请求或空输出返回 `KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT`；依赖任务资源的调用无法取得有效身份、LIVE MM 或一致的文件资源，以及 uaccess 报告内核状态损坏时，也返回该状态并由 Trap 边界视为 fatal。失败时不修改输出。有效请求均返回 `KERNEL_SYSCALL_STATUS_OK`，包括负 Linux errno；具体语义由结果动作表达。

## 当前 ABI 与验证

当前采用 Linux RISC-V 系统调用编号和错误值：

- `mkdirat` 编号为 34，创建目录；只读根挂载返回 `-EROFS`。
- `unlinkat` 编号为 35，删除普通文件或目录（`AT_REMOVEDIR` 标志）；非空目录返回 `-ENOTEMPTY`，只读挂载返回 `-EROFS`。
- `ftruncate` 编号为 46，调整可写常规文件的大小；只读 descriptor 返回 `-EINVAL`，目录返回 `-EISDIR`，只读挂载返回 `-EROFS`。
- `openat` 编号为 56，通过调用任务的 fs context 解析用户路径并在文件表分配最低可用 fd；支持常规文件读写打开与新建（`O_CREAT/O_EXCL/O_TRUNC/O_APPEND`），只读挂载拒绝写访问与修改性标志（返回 `-EROFS`）。准确 flags、路径和 errno 边界见[进程文件资源模块](kernel-files.md)。
- `close` 编号为 57，从调用任务的文件表移除 fd；无效或已关闭 fd 返回 `-EBADF`。
- `pipe2` 编号为 59，创建一对共享 64 KiB 环形缓冲的 read/write OFD；支持 `O_CLOEXEC` 与 `O_NONBLOCK`，成功返回两个最低可用 fd，表满或资源不足返回准确错误。读写、EOF、`EPIPE`/SIGPIPE 和 FIFO stat 形态见[进程文件资源模块](kernel-files.md)。
- `dup` 编号 23、`dup3` 编号 24 与 `fcntl` 编号 25 复制或检查描述符；flag 边界、目标替换与 `F_DUPFD*` 搜索规则见[进程文件资源模块](kernel-files.md)。
- `read` 编号为 63，使用 open file description 的当前 offset 把数据复制到用户缓冲区；返回实际字节数、0 表示 EOF，用户 fault 与部分复制按 Linux read 形态提交。console 描述符的 read 阻塞等待 UART 输入，经 tick 轮询唤醒后整批交付，行为见[进程文件资源模块](kernel-files.md)。
- `pread64` 编号为 67，按调用者给出的非负 offset 读取 regular file，保留 OFD 当前 offset；`O_WRONLY` OFD 返回 `-EBADF`，用户缓冲区部分 fault 返回已复制前缀。pipe、console 和目录按 Linux 形态返回不可定位错误。
- `pselect6` 编号为 72，支持 `fd_set`（可读/可写/异常）、相对超时与可选的 16 字节 `sigset_argpack` 临时信号屏蔽字；空集合或任何未打开 fd 按 Linux 语义返回 `-EBADF`，就绪总数通过返回值输出，行为与生命周期见[进程文件资源模块](kernel-files.md)。
- `ppoll` 编号为 73，支持 `struct pollfd` 数组（`POLLIN/POLLOUT/POLLPRI/POLLERR/POLLHUP/POLLNVAL`）、相对超时与可选的临时信号屏蔽字；负 fd 忽略不报错，未分配 fd 产生 `POLLNVAL` 并计入就绪数，信号打断返回 `-EINTR` 且自动恢复原信号掩码，行为见[进程文件资源模块](kernel-files.md)。
- `epoll_create1` 编号 20、`epoll_ctl` 编号 21、`epoll_pwait` 编号 22 构成 epoll 事件通知子系统：`epoll_create1` 支持 `EPOLL_CLOEXEC`（0x80000），创建专用的 epoll OFD；`epoll_ctl` 支持 `EPOLL_CTL_ADD/DEL/MOD` 并复制 16 字节 `struct linux_epoll_event`（支持 `EPOLLIN/EPOLLOUT/EPOLLPRI/EPOLLERR/EPOLLHUP/EPOLLET/EPOLLONESHOT`，禁止对 epfd 自身监听形成环路）；`epoll_pwait` 支持就绪队列提取、LT 重新入队校验、ET 边沿触发、ONESHOT 自动去使能、相对超时阻塞与 8 字节临时信号屏蔽字原子切换，行为见[进程文件资源模块](kernel-files.md)。
- `write` 编号 64 与 `writev` 编号 66 作用于 console、pipe 与具备写权限的可写常规文件：console 经架构串口输出，pipe 在 `PIPE_BUF=4096` 内保持单次写原子并按可用空间阻塞或返回 `-EAGAIN`；可写 regular fd 写入介质并失效页缓存，支持 `O_APPEND` 自动定位文件尾，无写权限返回 `-EBADF`。用户 fault 按前缀保持。console read、pipe read/write 的阻塞语义、`lseek` 编号 62 的 SEEK 形态与目录 cookie、`fstat` 编号 80 与 `newfstatat` 编号 79 的 128 字节 stat 填充、`getdents64` 编号 61 的 linux_dirent64 编码与条目 cookie，均见[进程文件资源模块](kernel-files.md)。
- `clock_gettime` 编号 113、`clock_getres` 编号 114、`gettimeofday` 编号 169、`clock_nanosleep` 编号 115 与 `nanosleep` 编号 101 构成时间族，语义见[内核时间模块](kernel-time.md)。
- `sched_yield` 编号 124 在存在 READY 竞争者时把当前任务排到 ready 队尾并切换；无竞争者时立即返回 0。调度失败属于内核不变量破坏，由 Trap 边界 fatal。
- `exit` 编号 93 产生线程 `EXIT`，`exit_group` 编号 94 产生全组 `EXIT_GROUP`；状态保留参数 0 的低 8 位。组退出等待成员沿原内核调用栈释放在用资源，最后产生一次进程退出通知。
- `set_tid_address` 编号 96 记录 clear_tid 用户指针并返回调用 TID；线程退出在释放 MM 前清零并唤醒同 key 的一个 futex waiter。坏用户指针不破坏内核状态。
- `futex` 编号 98 支持 WAIT、WAKE、REQUEUE 和 PRIVATE 标志。key、FIFO、错误码、同 MM 语义边界见[调度模块](kernel-scheduler.md)；无超时 WAIT 按 SA_RESTART 选择 EINTR 或重新等待，带超时 WAIT 的用户 handler 总是看到 EINTR、无 handler restart 保持原 absolute deadline。不支持的 PI、bitset、wake-op 等命令返回 ENOSYS，不计作能力完成。
- `uname` 编号为 160，把六个 65 字节字段组成的 Linux `new_utsname` 写到参数 0 指向的用户缓冲区；成功返回 0，用户范围、映射或写权限错误返回 `-EFAULT`（-14）。当前固定报告 `Linux/boaros/0.1.0-boaros-dev/#1 BoarOS/riscv64/(none)`，其中 release 是 BoarOS 自身开发版本而非 Linux 能力等级，机器名由架构构建配置提供。
- `getpid` 编号为 172，返回调用任务所属线程组的 TGID。
- `getppid` 编号为 173，返回当前父任务的 TGID；PID 1 或 parentless 进程返回 0，reparent 后观察到 PID 1。
- `gettid` 编号为 178，返回调用任务自己的 TID。
- `brk` 编号为 214，通过调用任务的 mutable MM borrow 调整精确 program break。raw syscall 成功返回请求值；参数 0 查询当前值；越过 ELF heap 起点/栈 guard、VMA 冲突或 metadata OOM 时返回原 break，不使用负 errno。跨页增长登记 demand-zero heap，缩小撤销越界页；libc 把 raw 返回再包装成自己的 0/-1 接口，不属于内核 ABI。
- `munmap` 编号为 215，要求页对齐起点和非零长度，长度向上按 4 KiB 对齐；范围包含未映射洞仍成功。越界或未对齐返回 `-EINVAL`。
- `clone` 编号为 220。支持 SIGCHLD fork/vfork 和 VM/FS/FILES/SIGHAND/THREAD 共享线程形态；线程形态接受 SETTLS、PARENT_SETTID、CHILD_SETTID、CHILD_CLEARTID、SYSVSEM、DETACHED 位。RISC-V 参数顺序为 flags/stack/parent_tid/tls/child_tid。非法 flag 依赖返回 EINVAL，未支持的合法资源组合返回 ENOTSUP；资源、vfork 致命取消、发布和回滚契约见[调度模块](kernel-scheduler.md)。
- `execve` 编号为 221，准备并提交 RISC-V `ET_EXEC`/`ET_DYN` 映像及非递归 `PT_INTERP` source；成功不返回，失败返回负 Linux errno。路径、解释器、提交点和资源保持规则见[进程映像替换模块](kernel-exec.md)。
- `mmap` 编号为 222，当前接受 anonymous 或可读普通文件的 `MAP_PRIVATE`，以及任意 `PROT_NONE/R/W/X` 组合。普通 hint、`MAP_FIXED`、`MAP_FIXED_NOREPLACE`、`MAP_STACK` 和 `MAP_NORESERVE` 已实现；fixed-noreplace 冲突返回 `-EEXIST`，地址空间/metadata 不足返回 `-ENOMEM`。文件映射要求有效且非 `O_WRONLY` 的 fd 与页对齐 offset；零长度、非法 fixed 地址、offset 溢出与 fixed-noreplace 冲突在可读性检查前返回各自错误，不可读 OFD 才返回 `-EACCES`。两类拒绝都会释放 syscall 临时 pin 且不提交 VMA。成功后由 MM 独立持有 OFD，所以 close fd 不撤销映射；shared 和 `MAP_POPULATE` 返回 `-ENOTSUP`，未知 flag、未对齐 offset 或同时指定两种 fixed 模式返回 `-EINVAL`；anonymous fd 参数按 Linux 语义忽略。
- `mprotect` 编号为 226，要求页对齐起点，整个非空范围必须已有 VMA；洞返回 `-ENOMEM`。长度 0 成功。`PROT_NONE` 保留 resident 内容，恢复权限后内容仍在；RISC-V 仅写请求被规范化为 RW。
- `wait4` 编号为 260，支持 Linux pid selector、`WNOHANG`、wait flag 校验与 rusage 输出；普通退出与同步故障产生 Linux 形态 status。无匹配子进程返回 `-ECHILD`，非法 option 返回 `-EINVAL`，status/rusage 用户指针错误返回 `-EFAULT`（回收先行，子进程不可再次 wait）。
- `kill`/`tkill`/`tgkill` 编号为 129/130/131，按进程、线程或 TGID+TID 发送标准信号；`rt_sigsuspend`/`rt_sigaction`/`rt_sigprocmask`/`rt_sigpending` 编号为 133/134/135/136，`rt_sigreturn` 为 139，均采用 8 字节有效 signal set。公共用户返回尾负责默认动作、handler frame、stop/continue 和已登记 syscall 类别的 `SA_RESTART`；只有接入该框架的阻塞 syscall 才会在 sigreturn 后重执行，其他调用返回自身规定的 `-EINTR`，细节见[内核信号模块](kernel-signal.md)。
- `restart_syscall` 编号为 128，当前用于 nanosleep 和带超时 FUTEX_WAIT 的绝对 deadline 重启；它不是可由用户任意伪造的通用成功存根。
- RISC-V 使用 asm-generic syscall 编号，没有独立 dup2；musl 经 dup3 实现相应调用。编号 33 是尚未实现的 mknodat，返回 ENOSYS，不能分派为 fd 替换。
- `times` 编号为 153，填写可选的 32 字节 `tms`（`utime/stime/cutime/cstime`，单位为 scheduler tick，`CLK_TCK`=100）并返回自启动的 uptime tick 数；tms 为 NULL 时只返回 uptime。记账在 tick 边界记到被中断任务，idle 不记账；子进程记账在 wait 回收时回卷给父进程，孙辈随回收归并。
- 其他编号产生 `RETURN`，返回 `-ENOSYS`（-38）。

当前 `brk`/mmap 还没有 `RLIMIT_DATA`、VMA 数量上限、内存承诺或 overcommit accounting；`MAP_NORESERVE` 因而与普通匿名映射等价。成功增长只承诺虚拟 VMA，实际物理页耗尽发生在后续 demand fault。这是明确的兼容性限制，不由伪造的 syscall 成功或预分配全部页来掩盖。

生产用户任务拥有文件表、fs context 和 MM。底层 U-mode 调度探针可以有 MM 而故意没有进程文件资源，此时 `openat` 返回 `-ENODEV`，`read/close` 返回 `-EBADF`，用于明确区分探针配置与内核对象损坏；这不是生产进程模型。普通 clone 已覆盖独立父子进程，线程 clone 子集共享 MM、files、fs context 和信号 disposition；完整线程组与共享资源矩阵仍有限。接口语义和内部字段已经分离，内核任务与 idle 没有 Linux 身份，Trap 层只会从用户任务进入该接口。

`make test-syscall-riscv` 验证空指针失败原子性、`exit(93)`、`exit_group(94)`、`set_tid_address(96)`、raw `brk`、匿名/文件 mmap 参数、不可读文件映射的 `EACCES`、fd pin/失败释放、errno、munmap/mprotect 转发、内部 MM 状态升级、clone 参数分类和未知编号。`make test-signal-riscv` 验证信号 syscall、handler frame/sigreturn、默认动作和 syscall restart；`make test-uaccess-riscv` 与 `make test-files-riscv` 验证用户复制、页缓存、pipe 和映射所需的文件生命周期。`make test-mmap-riscv` 让真实 ext4 `/init` ELF 从 U-mode 完成 demand-zero、file-private COW、EOF/SIGBUS、PROT_NONE、fixed replace/noreplace、打洞/重填和释放；`make test-userland-riscv` 继续覆盖真实 musl 的 heap/fork/exec、文件访问模式、signal 和 pipe 生命周期。
