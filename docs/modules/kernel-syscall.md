# 系统调用解码模块

本文描述与架构 Trap Frame 解耦的系统调用语义接口。模块只解码已经从用户寄存器复制出的请求；RISC-V Trap 层负责 `a7/a0..a5` 转换、普通返回时的 `sepc` 推进和 `a0` 回写，scheduler 负责退出、exec 提交与资源回收。

## 接口

入口位于 `include/kernel/syscall.h`，实现在 `kernel/syscall.c`：

```c
enum kernel_syscall_status kernel_syscall_dispatch(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *result);
```

调用者是 scheduler 当前不透明 task，供依赖任务身份或资源的系统调用取得明确上下文；通用解码层不从 RISC-V `tp` 隐式寻找 current。请求包含系统调用号和六个 64 位参数。结果是以下五种动作之一：

- `KERNEL_SYSCALL_ACTION_RETURN`：把 `value` 写回用户返回值寄存器后继续执行。
- `KERNEL_SYSCALL_ACTION_EXIT`：以 `value` 作为退出状态终止当前用户任务。
- `KERNEL_SYSCALL_ACTION_EXEC`：新映像已经准备完成；不推进旧 `sepc`，由 scheduler 切换 MM 并重建 Trap Frame。
- `KERNEL_SYSCALL_ACTION_CLONE`：参数已经符合当前普通进程 clone 子集；架构 Trap 层把完整寄存器快照交给进程层构造子进程。
- `KERNEL_SYSCALL_ACTION_WAIT4`：参数保持 Linux ABI 形态，由 scheduler 完成选择、阻塞、唤醒与 zombie 回收。

空调用者、空请求或空输出返回 `KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT`；依赖任务资源的调用无法取得有效身份、LIVE MM 或一致的文件资源，以及 uaccess 报告内核状态损坏时，也返回该状态并由 Trap 边界视为 fatal。失败时不修改输出。有效请求均返回 `KERNEL_SYSCALL_STATUS_OK`，包括负 Linux errno；具体语义由结果动作表达。

## 当前 ABI 与验证

当前采用 Linux RISC-V 系统调用编号和错误值：

- `openat` 编号为 56，通过调用任务的 fs context 解析用户路径并在文件表分配最低可用 fd；当前只支持根 mount 上的只读普通文件。准确 flags、路径和 errno 边界见[进程文件资源模块](kernel-files.md)。
- `close` 编号为 57，从调用任务的文件表移除 fd；无效或已关闭 fd 返回 `-EBADF`。
- `read` 编号为 63，使用 open file description 的当前 offset 把数据复制到用户缓冲区；返回实际字节数、0 表示 EOF，用户 fault 与部分复制按 Linux read 形态提交。
- `exit` 编号为 93，产生 `EXIT`；状态保留参数 0 的低 8 位。
- `uname` 编号为 160，把六个 65 字节字段组成的 Linux `new_utsname` 写到参数 0 指向的用户缓冲区；成功返回 0，用户范围、映射或写权限错误返回 `-EFAULT`（-14）。当前固定报告 `Linux/boaros/0.1.0-boaros-dev/#1 BoarOS/riscv64/(none)`，其中 release 是 BoarOS 自身开发版本而非 Linux 能力等级，机器名由架构构建配置提供。
- `getpid` 编号为 172，返回调用任务所属线程组的 TGID。
- `getppid` 编号为 173，返回当前父任务的 TGID；PID 1 或 parentless 进程返回 0，reparent 后观察到 PID 1。
- `gettid` 编号为 178，返回调用任务自己的 TID。
- `brk` 编号为 214，通过调用任务的 mutable MM borrow 调整精确 program break。raw syscall 成功返回请求值；参数 0 查询当前值；越过 ELF heap 起点/栈 guard、VMA 冲突、metadata OOM 或待回收页暂时无法释放时返回原 break，不使用负 errno。跨页增长登记 demand-zero heap，缩小撤销越界页；libc 把 raw 返回再包装成自己的 0/-1 接口，不属于内核 ABI。
- `munmap` 编号为 215，要求页对齐起点和非零长度，长度向上按 4 KiB 对齐；范围包含未映射洞仍成功。越界或未对齐返回 `-EINVAL`。
- `clone` 编号为 220。当前只接受 `flags=SIGCHLD` 且 `child_stack/parent_tid/tls/child_tid` 全为零的普通进程形态；成功时父进程返回子 PID，子进程返回 0。已识别但涉及共享资源、线程、tid 指针或另一个栈的组合返回 `-ENOTSUP`，未知 flag 或非 `SIGCHLD` 退出信号返回 `-EINVAL`。
- `execve` 编号为 221，准备并提交新的静态 RISC-V `ET_EXEC` 映像；成功不返回，失败返回负 Linux errno。路径、参数、提交点和资源保持规则见[进程映像替换模块](kernel-exec.md)。
- `mmap` 编号为 222，当前接受 `MAP_PRIVATE|MAP_ANONYMOUS` 和任意 `PROT_NONE/R/W/X` 组合。普通 hint、`MAP_FIXED`、`MAP_FIXED_NOREPLACE`、`MAP_STACK` 和 `MAP_NORESERVE` 已实现；fixed-noreplace 冲突返回 `-EEXIST`，地址空间/metadata 不足返回 `-ENOMEM`。shared、file-backed 和 `MAP_POPULATE` 返回 `-ENOTSUP`，未知 flag、非零 offset 或同时指定两种 fixed 模式返回 `-EINVAL`；anonymous fd 参数按 Linux 语义忽略。
- `mprotect` 编号为 226，要求页对齐起点，整个非空范围必须已有 VMA；洞返回 `-ENOMEM`。长度 0 成功。`PROT_NONE` 保留 resident 内容，恢复权限后内容仍在；RISC-V 仅写请求被规范化为 RW。
- `wait4` 编号为 260，支持 Linux pid selector、`WNOHANG` 和 wait flag 校验；普通退出与同步故障产生 Linux 形态 status。无匹配子进程返回 `-ECHILD`，非法 option 返回 `-EINVAL`，status 用户指针错误返回 `-EFAULT`。当前不提供 rusage，非空指针返回 `-ENOTSUP`。
- 其他编号产生 `RETURN`，返回 `-ENOSYS`（-38）。

当前 `brk`/mmap 还没有 `RLIMIT_DATA`、VMA 数量上限、内存承诺或 overcommit accounting；`MAP_NORESERVE` 因而与普通匿名映射等价。成功增长只承诺虚拟 VMA，实际物理页耗尽发生在后续 demand fault。这是明确的兼容性限制，不由伪造的 syscall 成功或预分配全部页来掩盖。

生产用户任务拥有文件表、fs context 和 MM。底层 U-mode 调度探针可以有 MM 而故意没有进程文件资源，此时 `openat` 返回 `-ENODEV`，`read/close` 返回 `-EBADF`，用于明确区分探针配置与内核对象损坏；这不是生产进程模型。当前每个进程仍是单成员线程组，所以 `getpid()` 与 `gettid()` 数值相等，但普通 clone 已创建独立父子进程。接口语义和内部字段已经分离，增加线程组成员后无需改变 syscall ABI。内核任务与 idle 没有 Linux 身份，Trap 层只会从用户任务进入该接口。

`make test-syscall-riscv` 验证空指针失败原子性、`exit(93)`、raw `brk`、mmap flag/errno、munmap/mprotect 转发、内部 MM 状态升级、clone 参数分类和未知编号。`make test-uaccess-riscv` 与 `make test-files-riscv` 验证用户复制和文件生命周期。`make test-mmap-riscv` 让真实 ext4 `/init` ELF 从 U-mode 完成 demand-zero、PROT_NONE 恢复、fixed replace/noreplace、打洞/重填和释放；`make test-brk-riscv` 继续覆盖 heap/fork/exec 生命周期。
