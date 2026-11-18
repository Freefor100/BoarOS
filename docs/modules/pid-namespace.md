# PID 命名空间

`clone` 和 `clone3` 使用 `CLONE_NEWPID` 创建子命名空间，其首个进程在内部为 PID 1。
返回给父进程的是父视图中的编号。`unshare(CLONE_NEWPID)` 只为未来子进程建立目标，
调用线程仍在原 PID 命名空间中运行。创建需要现有 user namespace 的
`CAP_SYS_ADMIN`。已有不同的子进程目标时，不能再次创建 PID 命名空间或创建共享
线程组的线程。显式 `CLONE_THREAD` 与 `CLONE_NEWPID` 互斥。

`setns` 可通过 namespace FD 或 pidfd 选择调用线程自身或后代的子进程视图。
与 mount、UTS 或 IPC 的组合变更全部验证通过后一起发布。procfs 的视图在挂载时固定，
重新挂载可查看子命名空间。原挂载继续报告从其视图可见的任务，路径不会因读取者变化而
指向另一个任务。`/proc/<pid>/ns/pid` 给出任务的 active PID namespace；
`pid_for_children` 给出未来子进程的视图，目标尚无 PID 1 时无法通过该路径打开。

## 身份与创建

一个 PID 身份持有所在层的编号，另存各祖先层的编号。每层独立维护占用 bitmap 和
哈希索引。身份还持有 TID、TGID、PGID、SID 对应角色，祖先视图共享同一个身份。
向视图转换时按深度定位编号并比较命名空间指针。不可见时返回 0，不能用初始层编号
替代。task 缓存的 `tid` 是初始层内部编号，不能直接返回给用户。

`clone3` 的 `set_tid` 一次复制到内核，按照从当前新生层到祖先层的顺序使用。
未指定的层自动分配。身份与命名空间存储在调度器身份锁外准备，编号在锁内逐层预留，
与进程及亲子关系共同发布。发生冲突、用户输出失败或目标命名空间停止接纳时，预留
会归还。身份脱离全部层的索引后，旧 pidfd、信号和其他引用仍持有旧身份，不会因
同号分配给新任务而更换目标。子命名空间持有父引用，引用归零的对象在身份锁外释放。

## PID 1 退出

命名空间首个线程组关闭时停止该层及其后代新建任务。最后存活线程退出时，清理者
杀死仍在命名空间内的进程并处理已退出的子进程。外部 tracer 持有最终 wait 事件或
尚有预留编号时，清理者推迟发布 PID 1 的最终退出并继续处理其他任务；等最后的引用
和编号归还后，再完成外层 wait 与 pidfd 通知。非组长 exec 转移身份和线程组，保持
同一个命名空间 init。命名空间 init 只接受其已经安装处理动作的普通外部信号，
祖先的 SIGKILL 和 SIGSTOP 按 Linux 规则强制处理。

子 PID 命名空间内的 reboot 只终止该命名空间。它在 init 的最终退出中向外层
报告 SIGHUP（重启）或 SIGINT（关机、停机）。初始层的 reboot 继续走机器控制入口。

## ABI 视图

| 接口 | 视图 |
| --- | --- |
| getpid、gettid、getppid、按 PID 查询的 syscall | 当前调用者的活动视图 |
| clone 返回、PARENT_SETTID、CHILD_SETTID | 父、父、子各自的视图 |
| signal、wait、ptrace | 输入按调用者解析。内核生成的 PID 字段按接收者、等待者或 tracer 输出 |
| /proc/<pid>、status、fdinfo | proc 挂载视图，pidfd 继续持同一个身份 |
| cgroup.procs、cgroup.threads | 当前读取者视图，不可见成员输出 0 |
| SysV IPC 控制调用、/proc/sysvipc | 控制调用使用当前视图。表文件保留打开者的 IPC 和 PID 视图 |
| socket 发送者和 peer credential | 接收者视图。netlink 自动绑定使用发送者本地 TGID 作为初始端口候选 |
| core NT_PRSTATUS | 转储者活动视图中的线程、实际父线程、进程组与会话编号 |

进程组和会话身份仍按真实对象比较。进入新 PID 视图不改继承的组和会话，
即使该视图不能报告它们的数字。futex PI 和 robust futex 把用户可见 TID 与内核
持有的任务身份分别处理。nsfs 支持获取可见的父命名空间以及在 FD 和调用者之间
转换 TID 或 TGID。namespace FD 可以跨进程生命周期继续持有视图。

未提供独立 user namespace、完整 nsfs ioctl、cgroup namespace 或其他新增
namespace 类型。原有 clone 资源标志的支持范围见[调度模块](kernel-scheduler.md)。

## 验证

`make test-pid-namespace-host test-pid-namespace-concurrency-host
test-pid-namespace-lifecycle-host test-clone3-import-host` 检查逐层冲突与回收、角色归属、
禁止销毁中的命名空间继续发布、子进程目标与一次性用户参数导入。
`make test-sched-concurrency-host` 在生产清理和信号路径上检查外层 tracer 延迟
归还与关闭后的编号回收。

`tests/userland/api8.py --only boaros --program pid_namespace_creation` 在实际客户机
检查三层编号、三种 proc 挂载、cgroup 错误后的精确 PID 复用、权限顺序和 PID 1 退出。
还可选 `pid_namespace`、`nsfs_pid`、`pid_namespace_reboot`、
`pid_namespace_cgroup`、`pid_namespace_ipc` 和 `pid_namespace_socket`。

[Linux PID namespace 实现](https://github.com/torvalds/linux/blob/f4cdf7ca9a1fdcca413157df19753f388a5a224e/kernel/pid_namespace.c)。
