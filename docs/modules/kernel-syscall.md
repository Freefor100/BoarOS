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

调用者是 scheduler 当前不透明 task，供依赖任务身份或资源的系统调用取得明确上下文；通用解码层不从 RISC-V `tp` 隐式寻找 current。请求包含系统调用号和六个 64 位参数。结果动作定义在公共头中：

- `KERNEL_SYSCALL_ACTION_RETURN`：把 `value` 写回用户返回值寄存器后继续执行。
- `KERNEL_SYSCALL_ACTION_EXIT`：以 `value` 作为退出状态终止当前用户任务。
- `KERNEL_SYSCALL_ACTION_EXEC`：新映像已经准备完成；不推进旧 `sepc`，由 scheduler 切换 MM 并重建 Trap Frame。
- `KERNEL_SYSCALL_ACTION_CLONE`：参数符合当前支持的进程/线程 clone 子集；架构 Trap 层把完整寄存器快照交给进程层构造任务。
- `KERNEL_SYSCALL_ACTION_WAIT4`：参数保持 Linux ABI 形态，由 scheduler 完成选择、阻塞、唤醒与 zombie 回收。
- `KERNEL_SYSCALL_ACTION_YIELD`：忽略无参数调用的残留寄存器，由 scheduler 把当前任务重新排到 ready 队尾并在存在竞争者时切换。
- `KERNEL_SYSCALL_ACTION_SIGNAL_RETURN`：由架构信号返回路径恢复用户现场。
- `KERNEL_SYSCALL_ACTION_EXIT_GROUP`：终止调用任务所属线程组，各成员沿原栈清理资源。

空调用者、空请求或空输出返回 `KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT`；依赖任务资源的调用无法取得有效身份、LIVE MM 或一致的文件资源，以及 uaccess 报告内核状态损坏时，也返回该状态并由 Trap 边界视为 fatal。失败时不修改输出。有效请求均返回 `KERNEL_SYSCALL_STATUS_OK`，包括负 Linux errno；具体语义由结果动作表达。

## 当前 ABI 与验证

当前采用 Linux RISC-V 系统调用编号和错误值：

| 调用族（RISC-V 编号） | 契约归属 |
|---|---|
| epoll_create1/ctl/pwait（20–22）、dup/dup3/fcntl（23–25） | [文件模块](kernel-files.md)：OFD 共享、就绪与生命周期 |
| getcwd（17）、mkdirat/unlinkat/symlinkat（34–36）、renameat/renameat2（38/276）、chdir/fchdir（49/50）、openat/close（56/57）、readlinkat（78） | [文件模块](kernel-files.md)：路径、flags、dirfd 与错误 |
| ftruncate（46）、pipe2（59）、getdents64/lseek（61/62）、read/write/readv/writev/pread64（63–67） | [文件模块](kernel-files.md)：部分成功、offset、pin、稀疏文件、pipe 与目录 cookie |
| pselect6/ppoll（72/73）、newfstatat/fstat（79/80） | [文件模块](kernel-files.md)：集合/信号屏蔽、stat 编码和元数据 |

- `clock_gettime` 编号 113、`clock_getres` 编号 114、`gettimeofday` 编号 169、`clock_nanosleep` 编号 115 与 `nanosleep` 编号 101 构成时间族，语义见[内核时间模块](kernel-time.md)。
- `sched_yield` 编号 124 在存在 READY 竞争者时把当前任务排到 ready 队尾并切换；无竞争者时立即返回 0。调度失败属于内核不变量破坏，由 Trap 边界 fatal。
- `exit` 编号 93 产生线程 `EXIT`，`exit_group` 编号 94 产生全组 `EXIT_GROUP`；状态保留参数 0 的低 8 位。组退出等待成员沿原内核调用栈释放在用资源，最后产生一次进程退出通知。
- `set_tid_address` 编号 96 记录 clear_tid 用户指针并返回调用 TID；线程退出在释放 MM 前清零并唤醒同 key 的一个 futex waiter。坏用户指针不破坏内核状态。
- `futex` 编号 98 支持 WAIT、WAKE、REQUEUE 和 PRIVATE 标志。key、FIFO、错误码、同 MM 语义边界见[调度模块](kernel-scheduler.md)；无超时 WAIT 按 SA_RESTART 选择 EINTR 或重新等待，带超时 WAIT 的用户 handler 总是看到 EINTR、无 handler restart 保持原 absolute deadline。不支持的 PI、bitset、wake-op 等命令返回 ENOSYS，不计作能力完成。
- `uname` 编号为 160，把六个 65 字节字段组成的 Linux `new_utsname` 写到参数 0 指向的用户缓冲区；成功返回 0，用户范围、映射或写权限错误返回 `-EFAULT`（-14）。当前固定报告 `Linux/boaros/0.1.0-boaros-dev/#1 BoarOS/riscv64/(none)`，其中 release 是 BoarOS 自身开发版本而非 Linux 能力等级，机器名由架构构建配置提供。
- `getpid` 编号为 172，返回调用任务所属线程组的 TGID。
- `getppid` 编号为 173，返回当前父任务的 TGID；PID 1 或 parentless 进程返回 0，reparent 后观察到 PID 1。
- `getuid/geteuid/getgid/getegid` 编号 174/175/176/177，返回当前不可变 root 身份的 0；调用无参数，忽略残留寄存器。fork/线程 clone/exec 不改变该身份。凭据变更、补充组与完整权限检查尚未实现，不能把此结果扩展成多用户支持；引入可变凭据时须统一替换查询和现有 root 权限假设。
- `gettid` 编号为 178，返回调用任务自己的 TID。
- `brk` 编号为 214，通过调用任务的 mutable MM borrow 调整精确 program break。raw syscall 成功返回请求值；参数 0 查询当前值；越过 ELF heap 起点/栈 guard、VMA 冲突或 metadata OOM 时返回原 break，不使用负 errno。跨页增长登记 demand-zero heap，缩小撤销越界页；libc 把 raw 返回再包装成自己的 0/-1 接口，不属于内核 ABI。
- `munmap` 编号为 215，要求页对齐起点和非零长度，长度向上按 4 KiB 对齐；范围包含未映射洞仍成功。越界或未对齐返回 `-EINVAL`。
- `clone` 编号为 220。支持 SIGCHLD fork/vfork 和 VM/FS/FILES/SIGHAND/THREAD 共享线程形态；线程形态接受 SETTLS、PARENT_SETTID、CHILD_SETTID、CHILD_CLEARTID、SYSVSEM、DETACHED 位。RISC-V 参数顺序为 flags/stack/parent_tid/tls/child_tid。非法 flag 依赖返回 EINVAL，未支持的合法资源组合返回 ENOTSUP；资源、vfork 致命取消、发布和回滚契约见[调度模块](kernel-scheduler.md)。
- `execve` 编号为 221，准备并提交 RISC-V `ET_EXEC`/`ET_DYN` 映像及非递归 `PT_INTERP` source；成功不返回，失败返回负 Linux errno。路径、解释器、提交点和资源保持规则见[进程映像替换模块](kernel-exec.md)。
- `mmap` 编号为 222，当前接受 anonymous 或可读普通文件的 `MAP_PRIVATE`，以及任意 `PROT_NONE/R/W/X` 组合。普通 hint、`MAP_FIXED`、`MAP_FIXED_NOREPLACE`、`MAP_STACK` 和 `MAP_NORESERVE` 已实现；fixed-noreplace 冲突返回 `-EEXIST`，地址空间/metadata 不足返回 `-ENOMEM`。文件映射要求有效且非 `O_WRONLY` 的 fd 与页对齐 offset；零长度、非法 fixed 地址、offset 溢出与 fixed-noreplace 冲突在可读性检查前返回各自错误，不可读 OFD 才返回 `-EACCES`。两类拒绝都会释放 syscall 临时 pin 且不提交 VMA。成功后由 MM 独立持有 OFD，所以 close fd 不撤销映射；shared 和 `MAP_POPULATE` 返回 `-ENOTSUP`，未知 flag、未对齐 offset 或同时指定两种 fixed 模式返回 `-EINVAL`；anonymous fd 参数按 Linux 语义忽略。
- `mprotect` 编号为 226，要求页对齐起点，整个非空范围必须已有 VMA；洞返回 `-ENOMEM`。长度 0 成功。`PROT_NONE` 保留 resident 内容，恢复权限后内容仍在；RISC-V 仅写请求被规范化为 RW。
- `wait4` 编号为 260，支持 Linux pid selector、`WNOHANG`、wait flag 校验与 rusage 输出；普通退出与同步故障产生 Linux 形态 status。无匹配子进程返回 `-ECHILD`，非法 option 返回 `-EINVAL`，status/rusage 用户指针错误返回 `-EFAULT`（回收先行，子进程不可再次 wait）。
- `prlimit64` 编号为 261，当前实现 `RLIMIT_NOFILE(7)` 与 `RLIMIT_STACK(3)` 的查询和设置；pid 0 指当前线程组，正 pid 指存活任务所属组。新值先从用户复制，设置和旧值快照完成后才向用户写回旧值，因此旧值地址故障仍保留已生效的设置。软值不得大于硬值；当前硬容量分别是 1024 个 fd 和 8 MiB 栈，超出拒绝。其他有效 resource 返回 `-ENOTSUP`，未知 resource 返回 `-EINVAL`，不返回虚假的成功。
- `kill`/`tkill`/`tgkill` 编号为 129/130/131，按进程、线程或 TGID+TID 发送标准信号；`rt_sigsuspend`/`rt_sigaction`/`rt_sigprocmask`/`rt_sigpending` 编号为 133/134/135/136，`rt_sigreturn` 为 139，均采用 8 字节有效 signal set。公共用户返回尾负责默认动作、handler frame、stop/continue 和已登记 syscall 类别的 `SA_RESTART`；只有接入该框架的阻塞 syscall 才会在 sigreturn 后重执行，其他调用返回自身规定的 `-EINTR`，细节见[内核信号模块](kernel-signal.md)。
- `restart_syscall` 编号为 128，当前用于 nanosleep 和带超时 FUTEX_WAIT 的绝对 deadline 重启；它不是可由用户任意伪造的通用成功存根。
- RISC-V 使用 asm-generic syscall 编号，没有独立 dup2；musl 经 dup3 实现相应调用。编号 33 是尚未实现的 mknodat，返回 ENOSYS，不能分派为 fd 替换。
- `times` 编号为 153，填写可选的 32 字节 `tms`（`utime/stime/cutime/cstime`，单位为 scheduler tick，`CLK_TCK`=100）并返回自启动的 uptime tick 数；tms 为 NULL 时只返回 uptime。记账在 tick 边界记到被中断任务，idle 不记账；子进程记账在 wait 回收时回卷给父进程，孙辈随回收归并。
- 其他编号产生 `RETURN`，返回 `-ENOSYS`（-38）。

当前 `brk`/mmap 还没有 `RLIMIT_DATA`、VMA 数量上限、内存承诺或 overcommit accounting；`MAP_NORESERVE` 因而与普通匿名映射等价。成功增长只承诺虚拟 VMA，实际物理页耗尽发生在后续 demand fault。这是明确的兼容性限制，不由伪造的 syscall 成功或预分配全部页来掩盖。

生产用户任务拥有文件表、fs context 和 MM。底层 U-mode 调度探针可以有 MM 而故意没有进程文件资源，此时 `openat` 返回 `-ENODEV`，`read/close` 返回 `-EBADF`，用于明确区分探针配置与内核对象损坏；这不是生产进程模型。普通 clone 已覆盖独立父子进程，线程 clone 子集共享 MM、files、fs context 和信号 disposition；完整线程组与共享资源矩阵仍有限。接口语义和内部字段已经分离，内核任务与 idle 没有 Linux 身份，Trap 层只会从用户任务进入该接口。

`make test-syscall-riscv` 验证空指针失败原子性、`exit(93)`、`exit_group(94)`、`set_tid_address(96)`、raw `brk`、匿名/文件 mmap 参数、不可读文件映射的 `EACCES`、fd pin/失败释放、errno、munmap/mprotect 转发、内部 MM 状态升级、clone 参数分类和未知编号。`make test-signal-riscv` 验证信号 syscall、handler frame/sigreturn、默认动作和 syscall restart；`make test-uaccess-riscv` 与 `make test-files-riscv` 验证用户复制、页缓存、pipe 和映射所需的文件生命周期。`make test-mmap-riscv` 让真实 ext4 `/init` ELF 从 U-mode 完成 demand-zero、file-private COW、EOF/SIGBUS、PROT_NONE、fixed replace/noreplace、打洞/重填和释放；`make test-userland-riscv` 继续覆盖真实 musl 的 heap/fork/exec、文件访问模式、signal 和 pipe 生命周期。

身份查询依据本地 `references/linux/kernel/sys.c` 的四个 `SYSCALL_DEFINE0` 和
`references/linux/include/uapi/asm-generic/unistd.h`，commit
`f4cdf7ca9a1fdcca413157df19753f388a5a224e`。BoarOS 已有 root 假设见
`kernel/syscall/process.c:syscall_handle_prlimit64`；查询没有新增对象或引用 owner，
也没有改变凭据。`test-syscall-riscv` 保护四项返回与忽略参数，
`test-diff-abi-riscv` 保护真实 U-mode 查询及 fork/exec 继承。
