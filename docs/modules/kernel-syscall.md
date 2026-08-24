# 系统调用解码模块

本文描述与架构 Trap Frame 解耦的系统调用语义接口。模块只解码已经从用户寄存器复制出的请求；RISC-V Trap 层负责 `a7/a0..a5` 转换、`sepc` 推进和 `a0` 回写，scheduler 负责退出与资源回收。

## 接口

入口位于 `include/kernel/syscall.h`，实现在 `kernel/syscall.c`：

```c
enum kernel_syscall_status kernel_syscall_dispatch(
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *result);
```

请求包含系统调用号和六个 64 位参数。结果是以下两种动作之一：

- `KERNEL_SYSCALL_ACTION_RETURN`：把 `value` 写回用户返回值寄存器后继续执行。
- `KERNEL_SYSCALL_ACTION_EXIT`：以 `value` 作为退出状态终止当前用户任务。

空请求或空输出返回 `KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT`；失败时不修改输出。有效请求均返回 `KERNEL_SYSCALL_STATUS_OK`，具体语义由结果动作表达。

## 当前 ABI 与验证

当前采用 Linux RISC-V 系统调用编号和错误值：

- `exit` 编号为 93，产生 `EXIT`；状态保留参数 0 的低 8 位。
- 其他编号产生 `RETURN`，返回 `-ENOSYS`（-38）。

`make test-syscall-riscv` 验证空指针失败原子性、`exit(93)` 的状态截断，以及多个未知编号的统一返回。`make test-user-riscv` 从真实 U-mode 执行未知 ecall 和 `exit(93)`：未知调用返回后继续执行，exit 形成用户完成记录且不再返回该 Frame。
