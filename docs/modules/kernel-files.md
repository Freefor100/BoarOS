# 进程文件资源模块

本文描述当前用户任务持有的文件描述符表与文件系统上下文。底层文件对象见[VFS 与只读 ext4 模块](vfs-ext4.md)，用户指针复制见[用户内存访问模块](kernel-uaccess.md)，syscall 编号与分派见[系统调用解码模块](kernel-syscall.md)。

## 对象与所有权

`include/kernel/files.h` 是公共接口；`fs/files/table.c` 管槽位、引用及回收，`io.c` 管读写/定位/枚举，`path.c` 管打开与 stat，`console.c` 管输入等待和暂存，`fs/pipe.c` 管 pipe endpoint 与 ring，`include/kernel/fs_context.h` 和 `fs/fs_context.c` 管理根挂载与当前工作目录。两者都从同一内核堆分配，并随用户 task 一起被 scheduler 接管：

- `kernel_files` 是进程可见的 fd 槽数组；槽保存 descriptor flags 和指向 open file description 的指针。
- `kernel_open_file_description` 拥有一个 VFS file、当前 offset 和清理状态。分别打开同一路径会得到独立 description，因此 offset 互不影响。
- pipe description 不拥有 VFS file，而是各自持有同一个 `struct kernel_pipe` 的读/写 endpoint；pipe 对象拥有连续 64 KiB ring buffer、读写端引用和等待队列。
- `kernel_fs_context` 借用生产根 mount，拥有当前工作目录字符串；当前 cwd 固定从 `/` 开始。

`kernel_files_pin()` 为 fd 指向的 open file description 增加一个独立引用。file-private mmap
用它把文件生命周期从 fd 槽中分离：映射成功后 MM 消耗该引用，之后即使所有 fd 都关闭，
缺页仍能读取原文件；映射失败则调用者释放 pin。一个 MM 对同一 OFD 只保存一个来源节点，
由该节点持有一份成功 mmap 转入的引用；重复映射不增加历史引用，
多个 VMA 通过 backing 指针借用它，最后一个对应 VMA 消失后在冷清理路径释放；底层 close 或堆释放失败时来源节点本身保留为可重试 owner，不重复累积引用。

描述符表初始有 32 个槽，按 2 倍增长，硬上限为 1024。分配总是从 `next_fd` 指示的最低可能空位向后搜索；关闭较小 fd 后会回退该提示，因此当前没有预装 stdin/stdout/stderr 时第一次成功打开返回 0。`O_CLOEXEC` 作为 descriptor flag 保存在槽上；exec 提交后批量摘除这些槽，其他 fd 和 open-file offset 保持不变。

普通 clone 通过 `kernel_files_fork()` 新建并复制 fd 槽数组和 descriptor flags，同时增加每个 open file description 的引用。因此父子可以分别 close 或修改各自的 `FD_CLOEXEC` 槽，但同一个已打开文件的 offset 与底层 VFS file 生命周期共享。fs context 通过 `kernel_fs_context_fork()` 独立复制 cwd 字符串并借用同一个 root mount。`dup/dup2/dup3/fcntl(F_DUPFD*)` 复用同一 OFD：`dup` 不携带 `FD_CLOEXEC`，`dup3` 只接受 `O_CLOEXEC` 且 `oldfd == newfd` 返回 `-EINVAL`；内部 `dup2` 对相同且有效的 fd 成功，不改变槽位。`dup2/dup3` 都对越界目标返回 `-EBADF`，目标槽被替换时按 close 语义摘除；`F_DUPFD/F_DUPFD_CLOEXEC` 从下界向上找第一个空槽，下界越界则返回 `-EINVAL`。固定 Linux 快照中的 [`fs/file.c`](../../references/linux/fs/file.c)、[`fs/fcntl.c`](../../references/linux/fs/fcntl.c) 与 [Linux dup 接口说明](https://man7.org/linux/man-pages/man2/dup.2.html)可用于核对这些边界。

`kernel_files_acquire()` 让另一个 handle 共享整张 fd 表及其 cleanup owner，`kernel_fs_context_acquire()` 同样共享 cwd/root context；两者只增加 record 引用，不复制槽、cwd 或 OFD。任一非末 handle release 只清空自身 handle，既不关闭 fd，也不释放 cwd；最后一个 handle 才沿用原有可重试清理链。普通 fork 接口仍保持“独立表/独立 cwd、共享 OFD”。固定 Linux `f4cdf7ca9a1fdcca413157df19753f388a5a224e` 的 [`kernel/fork.c`](../../references/linux/kernel/fork.c)、[`fs/file.c`](../../references/linux/fs/file.c) 与 [`fs/fs_struct.c`](../../references/linux/fs/fs_struct.c)分别提供 `CLONE_FILES`/`CLONE_FS` record 引用和末引用清理基线。

## `openat` 与路径边界

`kernel_files_openat()` 接收用户路径、dirfd、flags 和 mode，普通 Linux 结果通过 `linux_result` 返回，内核对象损坏或清理所有权异常则使用 `kernel_files_status` 报告。路径先复制到一张 4096 字节临时堆缓冲区：找不到 NUL 返回 `-ENAMETOOLONG`，不可读用户页返回 `-EFAULT`，空路径返回 `-ENOENT`。

绝对路径忽略 dirfd，直接使用根 mount；相对路径只支持 `AT_FDCWD`，与当前 cwd 拼接，其他 dirfd 返回 `-EBADF`。当前没有目录 fd、`chdir`、mount namespace、symlink 策略或逐分量权限检查。

支持普通文件与目录的打开，以及新建文件，严格遵循 Linux 解析与权限控制流：
- 标志支持：`O_RDONLY`、`O_WRONLY`、`O_RDWR`、`O_CREAT`、`O_EXCL`、`O_TRUNC`、`O_APPEND`、`O_LARGEFILE`、`O_CLOEXEC` 和 `O_DIRECTORY`。
- 解析顺序与 RO 保护：先尝试解析打开已有文件；若文件已存在，`O_CREAT | O_EXCL` 返回 `-EEXIST`，写访问模式（`O_WRONLY/O_RDWR`）或带有 `O_TRUNC` 在只读挂载下返回 `-EROFS`，只读打开（`O_RDONLY | O_CREAT`）在只读挂载下允许成功；若文件不存在，未指定 `O_CREAT` 一律返回 `-ENOENT`，指定 `O_CREAT` 时若挂载为只读才返回 `-EROFS`。
- 可执行互斥保护（`ETXTBSY`）：若目标普通文件作为运行中进程的可执行映像处于活跃状态（`exec_users > 0`），请求写访问（`O_WRONLY/O_RDWR`）或 `O_TRUNC` 立即返回 `-ETXTBSY`；反之，已被写打开的文件在执行 `execve` 时亦返回 `-ETXTBSY`。
- 创建语义：当指定 `O_CREAT` 且文件不存在时，调用 `kernel_open_file_create_mode()` 新建普通文件。
- 目录检查：目录以写模式（`O_WRONLY/O_RDWR`）或带 `O_TRUNC` 打开时返回 `-EISDIR`；普通文件配 `O_DIRECTORY` 返回 `-ENOTDIR`。
- 截断语义：对已存在的普通文件指定 `O_TRUNC` 时，在打开成功且取得写租约后调用 `kernel_vfs_ftruncate(&description->file, 0U)` 将大小截断为 0。
- 打开成功后按 VFS mode 把描述符分类为 regular、directory 或 console；directory 描述符支持 `getdents64`、`lseek` 与 `fstat`，`read` 返回 `-EISDIR`。文件不存在等路径错误由 VFS 保留为负 Linux errno；表满返回 `-EMFILE`，堆耗尽返回 `-ENOMEM`。

## `pipe2` 与 FIFO endpoint

`kernel_files_pipe2()` 创建两个新的 open file description 和两个 fd 槽；它们共享一个 order-4、64 KiB 连续 ring buffer，但读端只增加 read-side 引用，写端只增加 write-side 引用。`O_CLOEXEC` 保存在两个 descriptor flag 中，`O_NONBLOCK` 保存在两个 OFD status 中；`F_SETFL` 可切换 `O_NONBLOCK`，并接受 64 位目标上 musl 每次带来的 `O_LARGEFILE` 兼容位而不改变该位。

读端有数据时按 ring 顺序返回，空且仍有 writer 时：阻塞 fd 进入 interruptible wait，非阻塞 fd 返回 `-EAGAIN`。所有 writer 关闭后空读返回 EOF；写端没有 reader 时返回 `-EPIPE`，同时向当前 task 发送 SIGPIPE，因此有 handler 时先观察信号、无 handler 时按默认动作终止。写端空间不足时阻塞或返回 `-EAGAIN`；不超过 4096 字节的单次写在当前单 hart 实现中保持原子，较大写按可用空间推进。EOF/EPIPE 唤醒全部受影响 waiter；数据/空间变化也唤醒对端全部 waiter，被唤醒后重查条件，并可被未阻塞信号唤醒后返回 `-EINTR`/按 `SA_RESTART` 重启。

pipe 的 `fstat` 以 `S_IFIFO` 形态报告，`lseek` 返回 `-ESPIPE`；它不进入 ext4 页缓存，也不暴露普通 VFS node。两个 endpoint 的最后一个 OFD 关闭后，ring buffer、等待队列和 pipe owner 一起释放。创建或双 fd 安装的任一步失败都会先回收已创建 description/fd，再由文件表私有 cleanup 链保留无法立即释放的 pipe owner，避免丢失物理页或重复关闭。

## `read`、offset 与部分复制

`kernel_files_read()` 最多传送 Linux `MAX_RW_COUNT` 形态的 `INT32_MAX` 向下页对齐值。无效 fd 返回 `-EBADF`；零长度在 fd 有效且用户地址无需解引用时返回 0。与当前参照的 Linux `vfs_read()` 顺序一致，非零读取先按调用者给出的原始 count 验证整个用户范围，再把实际请求截断到上限，逐页从挂载共享缓存取得文件页并执行 `copy_to_user`。

open file description 的 offset 只增加实际复制到用户空间的字节数。首块即发生用户 fault 时返回 `-EFAULT` 且 offset 不变；已经复制前缀后再 fault 或遇到底层读错误时返回前缀长度，并只提交该前缀。短读和 EOF 返回实际长度。这样已经进入缓存但未交付用户的数据不会被错误计入文件位置。

每个 chunk 最多覆盖当前 4 KiB 文件页剩余部分：cache miss 承担一次底层随机读，hit 只增加页引用并复制；用户方向仍按基页做软件页表查询。模块统计调用/失败次数、字节、chunk、当前/峰值 fd、表容量与 close-on-exec 数量，页缓存另统计 hit/miss/insert/eviction/reclaim。当前仍有 cache-to-user 一次复制，尚无 read-ahead、直接用户页 I/O 或异步阻塞。优化这些路径时必须保持部分读取、offset 和错误返回语义。

## 描述符 `write`/`writev` 与文件修改

`write` 与 `writev` 支持 console、pipe 以及具备写权限（`O_WRONLY/O_RDWR`）的常规文件：
- console 经 `kernel_console_putc` 逐字节输出并返回完整计数；用户 fault 与部分复制按前缀保持返回。console 的 `read` 阻塞等待真实 UART 输入。
- pipe 的 `write/writev` 汇总后沿用 pipe 单次写空间、原子性、阻塞、EPIPE/SIGPIPE 和部分复制规则。
- regular 文件写入通过 `kernel_vfs_pwrite()` 执行底层介质写入，并调用节点页缓存失效确保缓存一致性。若描述符设置了 `O_APPEND`，写入前通过 `kernel_vfs_append()` 原子解析当前 EOF 并写入，成功后将 OFD offset 更新至新文件末尾。未以写权限打开的描述符或目录描述符调用 write 返回 `-EBADF`。
- `writev` 先快照完整用户 iovec 数组，校验长度和范围，再与 write 共用写入核心；`iovcnt` 上限 1024。

## 目录与文件系统操作

- `kernel_files_mkdirat()`：通过 fs context 解析路径后调用 `kernel_vfs_mkdir()`；只读挂载返回 `-EROFS`。
- `kernel_files_unlinkat()`：支持文件删除与目录删除（`AT_REMOVEDIR` 标志）。普通文件调用 `kernel_vfs_unlink()` 并使挂载存活节点页缓存失效；目录删除调用 `kernel_vfs_rmdir()`，非空目录返回 `-ENOTEMPTY`。
- `kernel_files_ftruncate()`：校验 fd 具备可写权限且为常规文件，调用 `kernel_vfs_ftruncate()` 调整文件大小（向下截断或向上连续补零）并精确失效该节点页缓存；只读描述符返回 `-EBADF`。

`read`、`write` 和 `writev` 在 fd lookup 后立即取得独立 OFD 引用，并在本次操作的全部复制、等待和唤醒处理结束后释放。共享表中的另一个线程即使在操作睡眠期间 close 并复用同一 fd 号，本次操作仍使用 lookup 时的 OFD；对于 pipe，这份引用也让原读/写 endpoint 在 in-flight I/O 结束前保持逻辑存活，避免提前产生 EOF/EPIPE 或释放等待队列。末次操作引用触发的底层 cleanup 失败会转交给共享文件表的原有 cleanup 链。该语义基线对应固定 Linux `f4cdf7ca9a1f` 中 [`fs/file.c`](../../references/linux/fs/file.c) 的 `fdget()`/`fdput()` 生命周期。

## I/O 多路复用与 `poll/select`

`kernel_files_ppoll()` 与 `kernel_files_pselect6()` 提供 Linux I/O 多路就绪通知：
- **OFD 就绪检查与等待契约**：每个 open file description 通过 `kernel_open_file_poll()` 报告当前就绪位（`KERNEL_POLLIN/OUT/PRI/ERR/HUP`）并导出所属事件等待队列。管道为空且无写者（`POLLHUP`）、读端有数据（`POLLIN`）、写端有空间（`POLLOUT`）、控制台输出可用或常规文件读取就绪等状态在第一遍扫描时直接确定；若已有就绪事件或指定超时为零，则完全跳过睡眠。
- **多队列等待节点（Wait Node）泛化**：调度器等待队列泛化为由 `struct kernel_wait_node` 串联的双向链表。当 poll 需要睡眠等待时，在被关心的所有有效 OFD 等待队列上分别挂载独立的等待节点（均关联当前 `task`），任一队列事件触发唤醒即退出阻塞，并在返回前可靠从全部队列脱链。
- **OFD 钉住引用（Pinned References）生命周期**：进入阻塞前，poll 引擎通过 `kernel_files_pin()` 对所有被监听的有效 OFD 各获取一份独立引用。这确保了在多线程共享文件表环境中，即使另一线程在睡眠期间 `close()` 并复用相应 fd，正在被 poll 的 OFD 及其内部等待队列依然受保护，不会发生 Use-After-Free；唤醒与清理时统一通过 `kernel_open_file_release()` 释放。
- **原子信号掩码替换**：`ppoll` 和 `pselect6` 支持以原子方式应用调用者指定的临时 `sigmask`，使阻塞等待能够被特定信号打断并返回 `-EINTR`，返回或打断时自动恢复原有信号掩码。
- **紧凑栈预算与堆回退**：受限于内核任务单页（4 KiB）控制块与内核栈共享布局，`poll` 与 `pselect6` 严格控制栈帧大小（快速路径最多 4 个描述符节点、select 最多 64 个 fd 的单字 bitset）；超出快速路径容量时统一从进程私有 `files->heap` 动态分配并在返回前完全回收，避免击穿内核栈金丝雀（Canary）。

## epoll 事件通知子系统

`kernel_files_epoll_create1()`、`kernel_files_epoll_ctl()` 与 `kernel_files_epoll_pwait()` 实现了 Linux 现代事件驱动 I/O 子系统：
- **epoll OFD 与长效事件绑定**：`epoll_create1` 创建 `KERNEL_OPEN_FILE_KIND_EPOLL` 类型的 open file description。不同于 `poll/select` 每次调用时的全量队列挂载，`epoll_ctl(EPOLL_CTL_ADD)` 将监听项（`struct kernel_epoll_item`）常驻注册在目标 OFD 的等待队列上，实现控制面与数据面解耦。
- **`(target_fd, description)` 键化与目标校验**：epoll 监听项以 `(target_fd, description)` 元组为唯一主键索引。`epoll_ctl(ADD)` 严格检验目标描述符能力：仅支持 pipe、console 和 epoll 等具备真实等待队列通知的对象，常规文件和目录明确拒绝并返回 `-EPERM`。同一进程中由 `dup` 派生的多个指向同一 OFD 的不同 fd 允许独立注册。
- **嵌套与环路防御（Iterative DFS）**：epoll 描述符自身可作为 target 被其他 epoll 实例监控。自监控（`epfd == target_fd` 或 `epoll_file == target_file`）返回 `-EINVAL`；嵌套图的环路检测与深度限制采用固定大小栈在栈内执行迭代 DFS，发现成环或嵌套深度超过 `EP_MAX_NESTS = 4` 时返回 `-ELOOP`，杜绝无界递归保护 1.8 KiB 内核栈。
- **Push 模型就绪列表（Ready List）与回调**：当目标 OFD 状态变化并执行 `kernel_wait_queue_wake_all()` 时，安装在 `kernel_wait_node` 上的 `kernel_epoll_wait_callback()` 自动将所属 `epitem` 追加至 epoll 实例的就绪列表（`ready_head/tail`），并级联唤醒阻塞在 `epoll_pwait` 上的进程。
- **水平触发（LT）、边缘触发（ET）与单次触发（ONESHOT）**：
  - 水平触发（LT，默认）：`epoll_wait` 在返回就绪事件后，若底层数据仍可读写（`kernel_open_file_poll` 仍报告匹配事件），将该 item 重新排入就绪队尾，确保未读完的数据持续通知；
  - 边缘触发（ET，`EPOLLET`）：事件一旦交付用户态，立即从就绪列表移出；仅当目标底层产生新的写入/唤醒边缘时才会重新入列；
  - 单次触发（`EPOLLONESHOT`）：事件交付后将 item 标记为 disarmed，直至用户显式通过 `EPOLL_CTL_MOD` 重新激活。
- **OFD 双向解绑与安全性**：目标 OFD 中维护指向所有监视它的 `epitem` 双向链表（`file->ep_items`）。当目标 OFD 的所有文件描述符被全部关闭、底层 OFD 最终释放（`kernel_open_file_release`）时，自动触发 `kernel_epoll_notify_file_release()` 从所属 epoll 实例中解绑并清理对应等待节点与内存，防止悬垂指针。同时，`close(epfd)` 销毁 epoll 实例时也会遍历所有项安全脱钩。
- **休眠唤醒竞态防护**：`epoll_pwait` 进入休眠前关闭中断，在将当前任务挂入 `epoll->wait_queue` 后再次复核就绪列表；若在挂入瞬间发生唤醒，可立即捕获事件避免漏唤醒死锁。
- **放宽 maxevents 与栈预算**：`epoll_pwait` 接受任意 `maxevents > 0`；快速路径在栈上维护至多 8 个事件缓冲（128 字节），超出时从堆分配并在返回前严格释放，守住内核栈 Canary。

## `lseek`、`fstat`/`newfstatat` 与 `getdents64`

`kernel_files_lseek()` 支持 `SEEK_SET/CUR/END` 的有符号运算与溢出检查，负结果返回 `-EINVAL` 且不移动 offset，越过 EOF 的定位成功；console 返回 `-ESPIPE`。目录也支持 `lseek`，其 offset 兼作 `getdents64` 的条目 cookie。

`kernel_files_fstat()/newfstatat()` 按 riscv64 asm-generic 128 字节 `struct stat` 填充：regular 文件的 mode/ino/size 来自 VFS 与 ext4，console 呈现 5:1 字符设备；`nlink` 固定为 1，时间戳当前填 0，需在适配层缓存 inode 时间后补全。`newfstatat` 支持 `AT_FDCWD`/绝对路径与 `AT_EMPTY_PATH`（直接按 fd 取描述符），真实 dirfd 的相对路径返回 `-EBADF`；目录路径在目录打开落地后可统计，`AT_SYMLINK_NOFOLLOW` 因无 symlink 而无条件接受。

`kernel_files_getdents64()` 只作用于目录描述符，其他类型返回 `-ENOTDIR`。每条记录按 linux_dirent64 编码（`d_reclen` 8 字节对齐，`d_type` 来自 ext4 filetype），`d_off` 是下一条记录的后端 cookie，不是条目计数；缓冲区连第一条记录都放不下返回 `-EINVAL`，用户 fault 在已完整发出的记录上返回前缀计数。

## 关闭与退出回收

`close` 先从表中摘除 fd，再关闭 VFS file 和释放 description；因此即使底层清理失败，该 fd 也已不可再次使用，当前调用返回 `-EIO`。待清理 description 和堆对象保存在文件表私有链中，后续 close 或最终 release 会重试，不会重复暴露 fd 或丢失 owner。

`kernel_files_close_on_exec()` 扫描 `FD_CLOEXEC` 槽并使用同一“先逻辑摘除、再物理清理”规则。它只在新映像已经提交后调用；普通 exec 失败不会触碰文件表。VFS close 或堆释放失败返回 `CLEANUP_REQUIRED`，但被摘除的 fd 对新程序立即为 `EBADF`，待清理 owner 仍由文件表保存。

用户 task 退出时先在任务仍位于自身内核栈、但硬件已经切回内核地址空间的边界处理重资源：

```text
fd-slot OFD references -> files table -> fs context
-> mapped OFD references/MM -> zombie
```

最后一个普通 OFD 引用才关闭底层 VFS file；pipe OFD 的最后一个读/写端引用在 detach（包括 dup 替换）时立即更新 endpoint 计数并在两端归零时释放 ring。父进程关闭 fd 不会使仍由子进程或任一 MM 文件映射引用的 OFD 失效。文件、pipe、fs context 或 MM 清理失败时 task 进入 exited 队列，idle 从记录状态重试；成功后根据父进程 SIGCHLD disposition 转成 zombie 或自动回收。根 mount 必须活到 PID 1 及其子进程的 fd 与映射来源全部回收，之后生产根启动路径才能 purge cache、unmount 并检查 heap/物理页基线。

## 验证与限制

`write` 和不超过 8 个 iovec 的 `writev` 不分配导入缓冲；更长数组为完整输入快照分配至多 16 KiB 元数据，释放失败时由文件表持有 owner。数据仍直接从用户空间复制到 pipe ring，console 使用 64 字节暂存；目录条目在固定内核缓冲区中编码一次，再复制到用户空间，目录拆分没有引入转发层或额外数据复制。等待唤醒扫描当前 blocked 链，单次 wake-all 为 O(阻塞任务数)，不是已完成的可扩展并发队列。

目录枚举现在由 OFD offset 保存可继续的后端 cookie，VFS 适配层隐藏 lwext4 类型；可变游标不放入按 inode 共享的 VFS node。独立 open 各自推进，dup 和 fork 共享同一 OFD 的目录位置。当前线性 ext4 适配器的 `ext4_dir_entry_next_status()` 区分真实 EOF 与正 errno，使用记录结束字节位置作为 cookie；未来的 htree 或其他文件系统可以替换编码而不改变文件资源接口。

实现保持以下语义：

- 待输出位置与已提交位置分离；只有整条 dirent 成功复制后才提交 cookie。缓冲区不足或 usercopy 失败时不跳过未交付记录，已交付前缀仍返回字节数。
- `d_off` 是可用于恢复枚举的位置 cookie，不要求调用者对它做序号算术。`lseek` 接受保存的 cookie；任意未对齐值由适配器向前规范化到下一个记录。
- dot 项和目录尾记录按 ext4 inode 号处理；inode 为零的尾记录只用于推进，不会被当作 EOF。lwext4 块读取或格式错误向上转成 `-EIO` 等 errno。
- 顺序完整枚举按每个物理记录一次推进，结构性条目访问为 O(N)。随机 seek 仍可能加载和扫描一个块，且尚未有开发板吞吐基线；不能把这一结构性结论写成未经测量的“更快”。

该能力沿文件主线收口，不依赖动态链接或可写文件系统；未来在启用 SMP 或共享 OFD 并发访问前，仍需为位置更新建立同步协议。

## 成本与验证入口

在当前 RV64 `-O2` 构建的编译器栈使用检查中，getdents 帧为 672 字节，writev 输入快照使用至多 8 个栈上 iovec；这些是静态成本，不是板级吞吐基准。QEMU 验证资源回收，开发板缓存行为和竞争负载性能尚缺实测。

```sh
make test-uaccess-riscv
make test-files-riscv
make test-syscall-riscv
make test-exec-riscv
make test-userland-riscv
make test-root-init-riscv
make test-riscv
```

聚焦测试在真实 QEMU legacy 与 modern VirtIO/ext4 上覆盖绝对/相对路径、错误 flags、目录与缺失文件、4096 字节路径上限、最低 fd 复用、表扩容、`O_CLOEXEC`、缓存命中后的跨页读取、EOF、部分 fault、fork 后 fd 表独立与 OFD offset 共享，以及可重试清理。它还在关闭 fd 后通过 MM backing 继续缺页，反复固定地址映射同一 OFD 并检查来源释放只发生一次，验证父子各自持有一份来源引用。生产 exec/clone 链验证普通 fd 与 offset 跨映像和父子保持、CLOEXEC fd 不可见，PID 1 的 stdio 与跨 exec 的 console 描述符由串口标记验证，并由最终资源基线证明退出清理生效。`make test-userland-riscv` 用静态 musl 程序作为 PID 1 运行 stdio、readdir、read/lseek/fstat、dup、signal 和 pipe，是真实 U-mode 外部测例的入口。

当前提供可共享的文件表与根 fs context handle，普通 clone 仍实现“复制表/复制 cwd、共享 OFD”；系统调用层是否选择共享由 clone flags 决定。当前已支持常规文件的读写（`write/writev/append`）、新建、删除（`unlinkat`）、截断（`ftruncate`）与目录修改（`mkdirat/rmdir`）；但仍无目录 fd（`dirfd` 相对路径）、`chdir`、异步脏页写回（writeback）、read-ahead、symlink、并发读写锁或多挂载。当前单 hart 下 fd lookup 与 OFD acquire 之间不可调度；启用 SMP 前必须为共享 record 引用、槽查找/替换、统计和 OFD 引用补齐同步，不能直接复用这些无锁字段。pipe 同样是单 hart 对象。console 接收现为 tick 轮询（唤醒延迟上界一个 tick），外部中断（PLIC/SEIE）落地后替换为中断驱动。
