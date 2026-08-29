# 系统调用解码模块

本文描述与架构 Trap Frame 解耦的系统调用语义接口。模块只解码已经从用户寄存器复制出的请求；RISC-V Trap 层负责 `a7/a0..a5` 转换、`sepc` 推进和 `a0` 回写，scheduler 负责退出与资源回收。

## 接口

入口位于 `include/kernel/syscall.h`，实现在 `kernel/syscall.c`：

```c
enum kernel_syscall_status kernel_syscall_dispatch(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *result);
```

调用者是 scheduler 当前不透明 task，供依赖任务身份或资源的系统调用取得明确上下文；通用解码层不从 RISC-V `tp` 隐式寻找 current。请求包含系统调用号和六个 64 位参数。结果是以下两种动作之一：

- `KERNEL_SYSCALL_ACTION_RETURN`：把 `value` 写回用户返回值寄存器后继续执行。
- `KERNEL_SYSCALL_ACTION_EXIT`：以 `value` 作为退出状态终止当前用户任务。

空调用者、空请求或空输出返回 `KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT`；依赖任务资源的调用无法取得有效身份、LIVE MM 或一致的文件资源，以及 uaccess 报告内核状态损坏时，也返回该状态并由 Trap 边界视为 fatal。失败时不修改输出。有效请求均返回 `KERNEL_SYSCALL_STATUS_OK`，包括负 Linux errno；具体语义由结果动作表达。

## 当前 ABI 与验证

当前采用 Linux RISC-V 系统调用编号和错误值：

- `openat` 编号为 56，通过调用任务的 fs context 解析用户路径并在文件表分配最低可用 fd；当前只支持根 mount 上的只读普通文件。准确 flags、路径和 errno 边界见[进程文件资源模块](kernel-files.md)。
- `close` 编号为 57，从调用任务的文件表移除 fd；无效或已关闭 fd 返回 `-EBADF`。
- `read` 编号为 63，使用 open file description 的当前 offset 把数据复制到用户缓冲区；返回实际字节数、0 表示 EOF，用户 fault 与部分复制按 Linux read 形态提交。
- `exit` 编号为 93，产生 `EXIT`；状态保留参数 0 的低 8 位。
- `uname` 编号为 160，把六个 65 字节字段组成的 Linux `new_utsname` 写到参数 0 指向的用户缓冲区；成功返回 0，用户范围、映射或写权限错误返回 `-EFAULT`（-14）。当前固定报告 `Linux/boaros/0.1.0-boaros-dev/#1 BoarOS/riscv64/(none)`，其中 release 是 BoarOS 自身开发版本而非 Linux 能力等级，机器名由架构构建配置提供。
- `getpid` 编号为 172，返回调用任务所属线程组的 TGID。
- `gettid` 编号为 178，返回调用任务自己的 TID。
- 其他编号产生 `RETURN`，返回 `-ENOSYS`（-38）。

生产用户任务拥有文件表、fs context 和 MM。底层 U-mode 调度探针可以有 MM 而故意没有进程文件资源，此时 `openat` 返回 `-ENODEV`，`read/close` 返回 `-EBADF`，用于明确区分探针配置与内核对象损坏；这不是生产进程模型。当前用户任务都是单成员线程组，所以 `getpid()` 与 `gettid()` 数值相等；接口语义和内部字段已经分离，增加线程组成员后无需改变 syscall ABI。内核任务与 idle 没有 Linux 身份，Trap 层只会从用户任务进入该接口。

`make test-syscall-riscv` 验证空指针失败原子性、`exit(93)` 的状态截断和未知编号的统一返回。`make test-uaccess-riscv` 验证数据和路径复制的错误语义，`make test-files-riscv` 验证文件层 Linux 结果与资源生命周期。`make test-user-riscv` 从真实 U-mode 验证 `uname/getpid/gettid`、无文件资源探针、未知 ecall 和退出；`make test-root-init-riscv` 则让生产 PID 1 对真实 ext4 文件执行 `openat/read/close`。
