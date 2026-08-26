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

空调用者、空请求或空输出返回 `KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT`；身份查询无法从调用者取得有效 TID/TGID 时也返回该状态。失败时不修改输出。有效请求均返回 `KERNEL_SYSCALL_STATUS_OK`，具体语义由结果动作表达。

## 当前 ABI 与验证

当前采用 Linux RISC-V 系统调用编号和错误值：

- `exit` 编号为 93，产生 `EXIT`；状态保留参数 0 的低 8 位。
- `getpid` 编号为 172，返回调用任务所属线程组的 TGID。
- `gettid` 编号为 178，返回调用任务自己的 TID。
- 其他编号产生 `RETURN`，返回 `-ENOSYS`（-38）。

当前用户任务都是单成员线程组，所以 `getpid()` 与 `gettid()` 数值相等；接口语义和内部字段已经分离，增加线程组成员后无需改变 syscall ABI。内核任务与 idle 没有 Linux 身份，Trap 层只会从用户任务进入该接口。

`make test-syscall-riscv` 验证空指针失败原子性、`exit(93)` 的状态截断和未知编号的统一返回。`make test-user-riscv` 从真实 U-mode 执行 `getpid/gettid`、未知 ecall 和 `exit(93)`：身份值必须为正且首线程二者相等，未知调用返回后继续执行，exit 形成用户完成记录且不再返回该 Frame。
