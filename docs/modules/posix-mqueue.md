# POSIX 消息队列

RV64、LoongArch64 使用 asm-generic `mq_open` 至 `mq_getsetattr`（180–185）。
`struct mq_attr` 为 64 字节；`sigevent` 为 64 字节，`sigval` 保留完整 64 位。
ABI 与行为参考仓库固定的 Linux `ipc/mqueue.c`、`include/uapi/linux/mqueue.h`
及 musl/glibc 的 `mq_*` 实现；来源版本统一见 [references](../../references/README.md)。

## 名称、描述符和账户

所有 `mqueue` 挂载与 `mq_open` 共用一个名称表。系统调用接收不带开头斜杠的
单个名称；libc 负责去掉 POSIX 名称的开头斜杠。名称最长 255 字节，不允许内嵌
斜杠、`.` 或 `..`。不同挂载中的同名项指向同一个队列身份。

名称表同时维护共享根目录的属主、权限和时间，初始为 `root:root`、`01777`。
任意挂载中的根目录 chmod/chown 会影响其他挂载及后续创建、删除。新建需要根
目录写入和搜索权限，删除还检查 sticky 所有者；普通路径访问也检查根目录搜索
权限。Linux 的 `mq_open` 对已有名称使用不检查父目录权限的查找，再检查队列
权限，因此已有名称的打开保留这个区别。创建时直接使用调用者 FSUID/FSGID，
不继承根目录 SGID；这是 `mqueue_get_inode` 的明确实现，与普通文件系统不同。

名称表、每个 OFD 和每个 VFS inode handle 分别持有队列。`mq_unlink` 或文件系统
`unlink` 只删除名称；已经打开的队列仍可收发，重新创建同名队列会取得新 inode。
最终持有者释放队列时才销毁消息、通知及创建额度。`dup`、fork 和线程共享 FD 表
共享原 OFD 的 `O_NONBLOCK`；再次打开同名队列取得独立 OFD。
`mq_open` 安装的 FD 默认设置 `FD_CLOEXEC`。

创建额度向创建调用者的真实 UID 账户收取，之后修改 UID、文件属主或执行
`unlink` 都不转移该额度。初始 `RLIMIT_MSGQUEUE` 为 819200 字节；Linux 64 位
管理开销公式为：

```
maxmsg * (msgsize + 48) + min(maxmsg, 32768) * 48
```

超过当前调用者的创建限额返回 `EMFILE`。打开已有队列不再收取创建额度。
普通创建最多 256 个存活队列，属性上限为 10 条、每条 8192 字节；具有
`CAP_SYS_RESOURCE` 的调用者可越过队列数量限制，属性硬上限为 65536 条、
每条 16 MiB。属性只声明容量，消息载荷按实际发送分配。

## 收发和等待

名称表只保护发现和名称变更；每个队列的短 raw 锁保护容量、优先级树、等待者
以及通知登记。名称表锁等级为 25，队列锁为 30，调度/PID 域为 40。
分配、用户复制、停车和 poll 回调都在名称表及队列锁外执行，收发由实际调用者
所在 CPU 执行。

发送请求先导入不可变的分段载荷，每段分配不超过 4096 字节，队列和直接接收者
接管同一份消息，不再次复制内核载荷。AVL 树按消息优先级索引，每个优先级内
使用 FIFO。接收移除最高优先级的最早消息，再在锁外复制给用户；输出地址故障
返回 `EFAULT`，已经移除的消息不会放回队列。

空队列的接收者和满队列的发送者通过请求对象登记；等待者按登记时的调度优先级
选择，同级保持先后顺序。发送遇到已经等待的接收者时直接交付；接收腾出容量时
直接接纳等待发送者持有的消息。操作结果与消息所有权在队列锁内同时更新，
随后通知对应的调度 token。超时或信号到达时再次在队列锁下检查结果，已经完成
的交付保留成功结果。绝对超时使用 `CLOCK_REALTIME`，能够立即完成的操作不因
期限已过而失败，`O_NONBLOCK` 的满/空结果保持 `EAGAIN`。

任务持有活动请求，覆盖用户复制、等待和待发布通知。退出清理先结束调度等待
借用，再调用 `kernel_mqueue_abort`，归还仍由请求持有的消息、优先级节点、OFD
和未发布 cookie。系统关机先摘除名称，实际 FD/VFS 持有者继续独立释放；最后
再次调用 `kernel_mqueue_shutdown`，成功后才可销毁堆。

## 通知及文件视图

每个队列最多登记一个 `SIGEV_NONE`、`SIGEV_SIGNAL` 或 `SIGEV_THREAD` 通知。
登记到非空队列不立即触发。没有等待接收者时，消息数由零变为非零消耗本次登记；
直接交付给等待接收者时，登记保留。登记者所在的线程组关闭任意一个指向该队列
的已安装 FD 都取消其登记，另一个进程的关闭不取消。临时 OFD 借用归还不会
触发取消。

信号登记保留实际 TGID 身份和 exec 安全对象；触发时核对执行身份，随后投递
`SI_MESGQ`，内容为发送者真实 UID、TGID 和完整 `sigval`。身份引用避免 PID
数字重用后误投递。`SIGEV_THREAD` 使用 libc 的 AF_NETLINK cookie 协议：
`sigev_signo` 指定 FD，`sigev_value` 指向 32 字节 cookie。全部用户输入复制后，
在队列锁外预留 cookie；登记失败取消私有预留，触发发送原因 1，已发布登记的
删除发送原因 2。通知传输和信号投递均在队列锁外完成。NETLINK cookie 独立持有端点存储；
等待接收额度前释放临时 OFD 引用，关闭 FD 可撤销队列并唤醒等待者。
等待 token 结束、未发布预留归还后，最后一个实际持有者销毁端点存储。

普通 `open` 队列项取得同类 MQ 描述符，并持有实际 VFS 文件、路径及挂载，直到
最后 OFD 释放；因此活动路径 FD 会使 `umount` 返回 `EBUSY`。`mq_open` 的描述符
独立于可见挂载。`read/readv/pread` 读取 Linux 的
`QSIZE/NOTIFY/SIGNO/NOTIFY_PID` 文本，不接收消息。普通读共享 OFD 位置，`pread`
不改变位置；`lseek` 允许重新读取。队列文件大小为 80 字节。`write/pwrite`
没有消息发送含义，返回 `EINVAL`；映射返回 `ENODEV`。`fstat`、权限与时间变更、
`statfs`、poll/epoll 与 `/proc/<pid>/fd` 使用真实队列状态；取消名称后 proc
链接带 ` (deleted)`。队列时间戳采用 Linux mqueue 的一秒粒度；隐藏的
`mq_open` 描述符与文件视图均支持 `futimens`。poll 可读条件为存在消息，可写条件为消息数低于容量。

## 检查

`tests/host/mqueue.sh` 将真实队列代码、raw 锁和调度停车代码链接到受控机器边界，
用明确等待握手检查直接交付、发送者接纳、通知竞争和任务弃栈后的资源归还。
同时检查优先级/FIFO、分段载荷、用户复制错误、创建额度和销毁时机。
`tests/workloads/ipc/posix_mqueue.c` 使用原始 libc 接口检查 ABI、poll/epoll、
信号值、libc `SIGEV_THREAD`、额度、文件视图和两个挂载间的共享状态。
这些检查验证行为与所有权；实现中的树索引和分段存储不构成应用吞吐提升的测量。
