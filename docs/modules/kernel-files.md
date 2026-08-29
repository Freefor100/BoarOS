# 进程文件资源模块

本文描述当前用户任务持有的文件描述符表与文件系统上下文。底层文件对象见[VFS 与只读 ext4 模块](vfs-ext4.md)，用户指针复制见[用户内存访问模块](kernel-uaccess.md)，syscall 编号与分派见[系统调用解码模块](kernel-syscall.md)。

## 对象与所有权

`include/kernel/files.h` 和 `fs/files.c` 管理文件描述符表，`include/kernel/fs_context.h` 和 `fs/fs_context.c` 管理根挂载与当前工作目录。两者都从同一内核堆分配，并随用户 task 一起被 scheduler 接管：

- `kernel_files` 是进程可见的 fd 槽数组；槽保存 descriptor flags 和指向 open file description 的指针。
- `kernel_open_file_description` 拥有一个 VFS file、当前 offset 和清理状态。分别打开同一路径会得到独立 description，因此 offset 互不影响。
- `kernel_fs_context` 借用生产根 mount，拥有当前工作目录字符串；当前 cwd 固定从 `/` 开始。

描述符表初始有 32 个槽，按 2 倍增长，硬上限为 1024。分配总是从 `next_fd` 指示的最低可能空位向后搜索；关闭较小 fd 后会回退该提示，因此当前没有预装 stdin/stdout/stderr 时第一次成功打开返回 0。`O_CLOEXEC` 作为 descriptor flag 保存在槽上；exec 提交后批量摘除这些槽，其他 fd 和 open-file offset 保持不变。

普通 clone 通过 `kernel_files_fork()` 新建并复制 fd 槽数组和 descriptor flags，同时增加每个 open file description 的引用。因此父子可以分别 close 或修改各自的 `FD_CLOEXEC` 槽，但同一个已打开文件的 offset 与底层 VFS file 生命周期共享。fs context 通过 `kernel_fs_context_fork()` 独立复制 cwd 字符串并借用同一个 root mount。当前尚无 `dup`；未来 `dup` 应复用同一 OFD，`CLONE_FILES` 则应共享整张表，而不是调用当前的 fork-copy 接口。

## `openat` 与路径边界

`kernel_files_openat()` 接收用户路径、dirfd、flags 和 mode，普通 Linux 结果通过 `linux_result` 返回，内核对象损坏或清理所有权异常则使用 `kernel_files_status` 报告。路径先复制到一张 4096 字节临时堆缓冲区：找不到 NUL 返回 `-ENAMETOOLONG`，不可读用户页返回 `-EFAULT`，空路径返回 `-ENOENT`。

绝对路径忽略 dirfd，直接使用根 mount；相对路径只支持 `AT_FDCWD`，与当前 cwd 拼接，其他 dirfd 返回 `-EBADF`。当前没有目录 fd、`chdir`、mount namespace、symlink 策略或逐分量权限检查。

只支持打开只读普通文件。`O_LARGEFILE` 和 `O_CLOEXEC` 可用；写访问、`O_CREAT/O_TRUNC/O_APPEND` 返回 `-EROFS`，已识别但未支持的打开行为返回 `-ENOTSUP`，未知位或非法 access mode 返回 `-EINVAL`。目录返回 `-EISDIR`。文件不存在等路径错误由 VFS 保留为负 Linux errno；表满返回 `-EMFILE`，堆耗尽返回 `-ENOMEM`。

## `read`、offset 与部分复制

`kernel_files_read()` 最多传送 Linux `MAX_RW_COUNT` 形态的 `INT32_MAX` 向下页对齐值。无效 fd 返回 `-EBADF`；零长度在 fd 有效且用户地址无需解引用时返回 0。与当前参照的 Linux `vfs_read()` 顺序一致，非零读取先按调用者给出的原始 count 验证整个用户范围，再把实际请求截断到上限，分配一块至多 4 KiB 的 scratch buffer，循环执行 VFS `pread` 和 `copy_to_user`。

open file description 的 offset 只增加实际复制到用户空间的字节数。首块即发生用户 fault 时返回 `-EFAULT` 且 offset 不变；已经复制前缀后再 fault 或遇到底层读错误时返回前缀长度，并只提交该前缀。短读和 EOF 返回实际长度。这样磁盘预读到 scratch 但未交付用户的数据不会被错误计入文件位置。

当前一次非零 `read` 分配一块 scratch buffer，每个至多 4 KiB 的 chunk 做一次 VFS 随机读和按用户基页的软件页表查询。模块统计调用/失败次数、字节、chunk、当前/峰值 fd、表容量与 close-on-exec 数量，测试用它固定当前分配和分块成本；尚无页缓存、read-ahead、直接用户页 I/O 或异步阻塞。优化这些路径时必须保持部分读取、offset 和错误返回语义。

## 关闭与退出回收

`close` 先从表中摘除 fd，再关闭 VFS file 和释放 description；因此即使底层清理失败，该 fd 也已不可再次使用，当前调用返回 `-EIO`。待清理 description 和堆对象保存在文件表私有链中，后续 close 或最终 release 会重试，不会重复暴露 fd 或丢失 owner。

`kernel_files_close_on_exec()` 扫描 `FD_CLOEXEC` 槽并使用同一“先逻辑摘除、再物理清理”规则。它只在新映像已经提交后调用；普通 exec 失败不会触碰文件表。VFS close 或堆释放失败返回 `CLEANUP_REQUIRED`，但被摘除的 fd 对新程序立即为 `EBADF`，待清理 owner 仍由文件表保存。

用户 task 退出时先在任务仍位于自身内核栈、但硬件已经切回内核地址空间的边界处理重资源：

```text
all fds/OFD references -> files table -> fs context -> MM -> zombie
```

最后一个 OFD 引用才关闭底层 VFS file；父进程关闭 fd 不会使仍由子进程引用的 OFD 失效。文件或 fs context 清理失败时 task 进入 exited 队列，idle 从记录状态重试；成功后有父任务的进程转成只保留轻量状态的 zombie。根 mount 必须活到 PID 1 及其子进程的文件资源全部回收，之后生产根启动路径才能 unmount 并检查 heap/物理页基线。

## 验证与限制

```sh
make test-uaccess-riscv
make test-files-riscv
make test-syscall-riscv
make test-exec-riscv
make test-root-init-riscv
make test-riscv
```

聚焦测试在真实 modern VirtIO/ext4 上覆盖绝对/相对路径、错误 flags、目录与缺失文件、4096 字节路径上限、最低 fd 复用、表扩容、`O_CLOEXEC`、跨页读取、EOF、部分 fault、fork 后 fd 表独立与 OFD offset 共享，以及可重试清理。生产 exec/clone 链验证普通 fd 与 offset 跨映像和父子保持、CLOEXEC fd 不可见，并由最终资源基线证明退出清理生效。

当前只有进程私有文件表、根 fs context 和只读普通文件；普通 clone 已实现“复制表、共享 OFD”，但没有 `CLONE_FILES`。也没有 `write/writev/lseek/fstat/getdents`、目录 fd、`dup/fcntl`、并发锁、页缓存或可写文件系统。
