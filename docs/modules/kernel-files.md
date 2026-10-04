# 进程文件资源模块

本文描述当前用户任务持有的文件描述符表与文件系统上下文。底层文件对象见[VFS 与 ext4 模块](vfs-ext4.md)，用户指针复制见[用户内存访问模块](kernel-uaccess.md)，syscall 编号与分派见[系统调用解码模块](kernel-syscall.md)。

## 对象与所有权

`include/kernel/files.h` 是公共接口；`fs/files/table.c` 管槽位、引用及回收，`io.c` 管读写/定位/枚举，`path.c` 管打开与 stat，`metadata.c` 管显式时间和文件系统统计，`console.c` 管 UART 输入等待，`fs/char_device.c` 按设备号登记字符设备操作，`fs/pipe.c` 管 pipe endpoint 与 ring，`include/kernel/fs_context.h` 和 `fs/fs_context.c` 管理根挂载与当前工作目录。文件表与 fs context 都从同一内核堆分配，并随用户 task 一起被 scheduler 接管：

- `kernel_files` 是进程可见的 fd 槽数组；槽保存 descriptor flags 和指向 open file description 的指针。
- `kernel_open_file_description` 拥有一个 VFS file、当前 offset 和清理状态。分别打开同一路径会得到独立 description，因此 offset 互不影响。
- pipe description 不拥有 VFS file，而是各自持有同一个 `struct kernel_pipe` 的读/写 endpoint；pipe 对象拥有连续 64 KiB 数据区、16 个固定页片段的有效范围、读写端引用和等待队列。
- socket description 同样不拥有 VFS file，而是独占 `struct kernel_socket`；fd 关闭、dup 覆盖、exec CLOEXEC 或退出导致最后一个真实 OFD 引用消失时，销毁 socket 并解绑 lwIP 回调。请求期间的 OFD pin 使 fd 复用不改变当前请求的 endpoint。普通 read/write 与向量路径使用 socket 数据队列，定位 I/O 返回 `ESPIPE`；`fstat` 报 `S_IFSOCK`。协议与等待契约见[网络模块](kernel-network.md)。
- `kernel_fs_context` 持有 root 和 cwd 的独立路径引用；普通 fork 复制两份引用，`CLONE_FS` 共享 context record，exec 保留。`chdir/fchdir/getcwd` 操作共享活目录项；exec 从当前目录对象解析相对路径。

`kernel_files_pin()` 为 fd 指向的 open file description 增加一个独立引用。file-private mmap
用它把文件生命周期从 fd 槽中分离：映射成功后 MM 消耗该引用，之后即使所有 fd 都关闭，
缺页仍能读取原文件；映射失败则调用者释放 pin。一个 MM 对同一 OFD 只保存一个来源节点，
由该节点持有一份成功 mmap 转入的引用；重复映射不增加历史引用，
多个 VMA 通过 backing 指针借用它，最后一个对应 VMA 消失后在冷清理路径释放。底层 VFS/I/O
清理若暂时不能完成，来源节点由 MM registry 保留为唯一 owner，不重复累积引用；物理页和堆释放
遵循 fail-stop 不变量。

描述符表初始有 32 个槽，按 2 倍增长，硬上限为 1024。`RLIMIT_NOFILE` 的组级软限制约束新 fd 的编号；open、pipe、dup 和 `F_DUPFD*` 均通过该上限，降低限制不关闭已有 fd，也不阻止继续使用它们。限额保存在组长而非共享 fd 表中：即使不同组通过 `CLONE_FILES` 共用槽位，也各按自己的限额分配；每次 syscall 借用文件表时更新任务句柄的限额。分配总是从 `next_fd` 指示的最低可能空位向后搜索；关闭较小 fd 后会回退该提示，因此当前没有预装 stdin/stdout/stderr 时第一次成功打开返回 0。`O_CLOEXEC` 作为 descriptor flag 保存在槽上；exec 提交后批量摘除这些槽，其他 fd 和 open-file offset 保持不变。

normal open、dup/F_DUPFD、console、pipe2 和 epoll_create1 最终都经过 `table.c` 的统一 fd 安装原语。新建 OFD 与调用者已 acquire 的共享 OFD 使用两个显式入口；两者都只消费传入 ownership，不暗中改变引用计数，并统一提交槽位、open/CLOEXEC 统计和 `next_fd`。表满或安装前失败不提交这些状态。

普通 clone 通过 `kernel_files_fork()` 新建并复制 fd 槽数组和 descriptor flags，同时增加每个 open file description 的引用。因此父子可以分别 close 或修改各自的 `FD_CLOEXEC` 槽，但同一个已打开文件的 offset 与底层 VFS file 生命周期共享。fs context 通过 `kernel_fs_context_fork()` 独立持有 root/cwd 路径引用和同一个 root mount。`dup/dup2/dup3/fcntl(F_DUPFD*)` 复用同一 OFD：`dup` 不携带 `FD_CLOEXEC`，`dup3` 只接受 `O_CLOEXEC` 且 `oldfd == newfd` 返回 `-EINVAL`；内部 `dup2` 对相同且有效的 fd 成功，不改变槽位。`dup2/dup3` 都对越界目标返回 `-EBADF`，目标槽被替换时按 close 语义摘除；`F_DUPFD/F_DUPFD_CLOEXEC` 从下界向上找第一个空槽，下界越界则返回 `-EINVAL`。固定 Linux 快照中的 [`fs/file.c`](../../references/linux/fs/file.c)、[`fs/fcntl.c`](../../references/linux/fs/fcntl.c) 与 [Linux dup 接口说明](https://man7.org/linux/man-pages/man2/dup.2.html)可用于核对这些边界。

`kernel_files_acquire()` 让另一个 handle 共享整张 fd 表及其 cleanup owner，`kernel_fs_context_acquire()` 同样共享 cwd/root context；两者只增加 record 引用，不复制槽、cwd 或 OFD。任一非末 handle release 只清空自身 handle，既不关闭 fd，也不释放 cwd；最后一个 handle 才处理仍由 VFS/I/O 持有的清理 owner。普通 fork 接口仍保持“独立表/独立 cwd、共享 OFD”。固定 Linux `f4cdf7ca9a1fdcca413157df19753f388a5a224e` 的 [`kernel/fork.c`](../../references/linux/kernel/fork.c)、[`fs/file.c`](../../references/linux/fs/file.c) 与 [`fs/fs_struct.c`](../../references/linux/fs/fs_struct.c)分别提供 `CLONE_FILES`/`CLONE_FS` record 引用和末引用清理基线。

生成式 VFS 普通文件使用独立 OFD kind，不进入普通文件页缓存或 mmap；第一次非空读取从后端取得有 owner 的数据快照。read/readv 共享 OFD offset，pread 保留 offset，短拷贝 fault 只推进已复制字节，seek 到零重新生成，末引用释放快照。当前消费者是 [procfs](procfs.md) 的 meminfo、stat/status 与 mounts；文件元数据 size 为零不决定生成式读取 EOF。

`/proc/<pid>/fd` 通过短关中断区借用目标文件表，普通文件直接取得路径引用；伪对象只复制 kind/身份，不把 OFD pin 留给可能已关闭的目标任务。重新打开 pipe 时，先在该短区预留一个 endpoint 引用，解锁后分配新 OFD，最后无论成功失败都撤销临时引用。只读、只写和读写的新 OFD 分别拥有 reader、writer 或两者；`O_RDWR` 的 poll 等待使用同时由读写状态变化唤醒的聚合队列。socket/epoll 不复制底层句柄来冒充新打开。

## 传统与 OFD 记录锁

`fs/record_lock.c` 为每个活 VFS inode 维护按有符号闭区间排序、带子树最大终点的 AVL 树和等待队列；每个锁节点同时挂在 owner 的侵入式索引上。长度为零延伸到 `INT64_MAX`，负长度向文件前方锁定，锁可以超过 EOF。`fs/files/locks.c` 导入 RV64 `struct flock` 并分派 `F_GETLK/F_SETLK/F_SETLKW` 与 `F_OFD_GETLK/F_OFD_SETLK/F_OFD_SETLKW`；标量 `fcntl` 命令仍走原入口。区间转换、读写权限、`l_pid`、坏指针和溢出错误按固定 Linux `references/linux/fs/locks.c`，commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e` 核对。

错误检查先 pin fd，再导入 `flock`，最后检查对象是否支持记录锁。坏 fd 返回
`EBADF`；有效 TTY 或 pipe fd 的坏指针返回 `EFAULT`，不被对象类型错误遮蔽。
OFD pin 覆盖可能缺页睡眠的复制。`make test-record-lock-riscv` 用同一 ELF
对照固定 Linux，覆盖六种记录锁命令及两种错误的优先级。

传统锁 owner 是共享的 `kernel_files_record`，所以普通 fork 的新表不继承它，同表线程共享；该 owner 关闭同 inode 的任意 fd 就定向释放它在该 inode 的全部传统锁。OFD 锁 owner 是打开文件对象，dup/fork 共用且在最后真实引用消失时释放。两类 owner 身份不同，但相同 inode、范围和读写类型之间真实检查冲突。锁节点不反向引用 owner；VFS inode 在树或等待队列非空时不得释放。close、dup 覆盖、CLOEXEC、退出都在摘除 fd 的既有生命周期中释放对应锁，不把 OFD 锁留给历史 I/O cleanup。

写入前预留分拆和替换所需节点；`ENOLCK` 不改动已有锁集合。解锁、关闭释放和唤醒不分配。阻塞请求 pin OFD，持有转换后的稳定范围，登记与冲突检查在单 hart 关中断临界区内完成；唤醒后重新检查 fd/冲突，信号使用既有 `ERESTARTSYS` 协议。传统锁沿固定 Linux 的十步边界进行有限等待链死锁检查，OFD 锁不承诺该检测。该模块仍只按单 hart 验证，SMP 前需要为 inode 树、owner 索引、等待链与文件引用建立跨核同步。

聚焦入口为 `make test-record-lock-host`（随机独立区间模型、AVL 不变量和 OOM 原子性）、`make test-files-riscv` 及 `make test-diff-abi-riscv`。后者在固定 Linux 启用 `CONFIG_FILE_LOCKING` 后比较真实阻塞、信号重启、死锁、dup/exec/CLOEXEC 与 unlink。`make test-record-lock-riscv` 用同一个静态 musl ELF 在固定 Linux 与 BoarOS 核对 pthread 共享 owner、fork、不相关 fd close、等待期间 fd 复用和阻塞线程所在组强制退出；SQLite 原生 Unix VFS 的多进程争用另由 `make test-sqlite-rollback-riscv` 验证。

## `openat` 与路径边界

`kernel_files_openat()` 接收用户路径、dirfd、flags 和 mode，普通 Linux 结果通过 `linux_result` 返回，内核对象损坏或清理所有权异常则使用 `kernel_files_status` 报告。路径先复制到一张 4096 字节临时堆缓冲区：找不到 NUL 返回 `-ENAMETOOLONG`，不可读用户页返回 `-EFAULT`，空路径返回 `-ENOENT`。

绝对路径忽略 dirfd，从 root 对象开始；相对路径从 cwd 或目录 fd 持有的目录项开始。无效 fd 返回 `-EBADF`，非目录 fd 返回 `-ENOTDIR`。VFS 在 mount 内逐分量处理 `.`、`..`、相对/绝对符号链接目标、尾斜杠和最多 40 次链接展开；open/exec/stat 跟随最终链接，`O_NOFOLLOW` 只在最终分量为链接时返回 `-ELOOP`，`O_CREAT|O_EXCL` 对它返回 `-EEXIST`。`symlinkat/readlinkat` 和 `AT_SYMLINK_NOFOLLOW` 访问链接本身；创建和删除仅解析父路径。当前仍没有 mount namespace 或逐分量权限检查。

支持普通文件与目录的打开，以及新建文件，严格遵循 Linux 解析与权限控制流：
- 标志支持：`O_RDONLY`、`O_WRONLY`、`O_RDWR`、`O_CREAT`、`O_EXCL`、`O_TRUNC`、`O_APPEND`、`O_NONBLOCK`、`O_LARGEFILE`、`O_CLOEXEC`、`O_DIRECTORY` 和 `O_NOFOLLOW`。regular file 与 directory 接受并在 OFD status flags 中保留 `O_NONBLOCK`，`F_GETFL/F_SETFL` 可观察和切换该位；普通文件 I/O 不因此伪造 pipe 风格的 `EAGAIN`。
- 解析顺序与 RO 保护：先尝试解析打开已有文件；若文件已存在，`O_CREAT | O_EXCL` 返回 `-EEXIST`，写访问模式（`O_WRONLY/O_RDWR`）或带有 `O_TRUNC` 在只读挂载下返回 `-EROFS`，只读打开（`O_RDONLY | O_CREAT`）在只读挂载下允许成功；若文件不存在，未指定 `O_CREAT` 一律返回 `-ENOENT`，指定 `O_CREAT` 时若挂载为只读才返回 `-EROFS`。
- 可执行互斥保护（`ETXTBSY`）：若目标普通文件作为运行中进程的可执行映像处于活跃状态（`exec_users > 0`），请求写访问（`O_WRONLY/O_RDWR`）或 `O_TRUNC` 立即返回 `-ETXTBSY`；反之，已被写打开的文件在执行 `execve` 时亦返回 `-ETXTBSY`。
- 创建语义：当指定 `O_CREAT` 且文件不存在时，调用 `kernel_open_file_create_mode()` 新建普通文件。
- 目录检查：目录以写模式（`O_WRONLY/O_RDWR`）或带 `O_TRUNC` 打开时返回 `-EISDIR`；普通文件配 `O_DIRECTORY` 返回 `-ENOTDIR`。
- 截断语义：对已存在的普通文件指定 `O_TRUNC` 时，在打开成功且取得写租约后调用 `kernel_vfs_ftruncate(&description->file, 0U)` 将大小截断为 0。
- 打开成功后按 VFS mode 把描述符分类为 regular、directory 或 console；directory 描述符支持 `getdents64`、`lseek` 与 `fstat`，`read` 返回 `-EISDIR`。文件不存在等路径错误由 VFS 保留为负 Linux errno；表满返回 `-EMFILE`，堆耗尽返回 `-ENOMEM`。

该标志行为依据固定 Linux commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e` 的 `references/linux/include/linux/fcntl.h`、`references/linux/fs/open.c` 与 `references/linux/fs/fcntl.c`。固定 musl 1.2.5（`references/musl/musl-1.2.5.tar.gz`，SHA-256 `a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4`）的 `src/dirent/opendir.c` 使用 `O_RDONLY|O_DIRECTORY|O_CLOEXEC`，不依赖 `O_NONBLOCK`；因此本改动不把 musl `opendir` 作为新增能力。

`getcwd` 返回包含 NUL 的字节数；用户缓冲不足为 `ERANGE`，已断开的 cwd 为 `ENOENT`。删除目录后，其持有者仍可对 `.` 打开/统计、经 `..` 访问父目录、通过 `fchdir` 切回；不能在已删除目录中新建名字。改名会更新所有持有同一目录项的 cwd/OFD 所观察到的父链。fork 独立复制 fs record，进程形式和线程形式的 `CLONE_FS` 都共享它；切换 cwd 先取得新引用再替换，失败不改变原目录。

`renameat2` 支持 flags 0 和 `RENAME_NOREPLACE`；未知或互斥组合返回 `EINVAL`，EXCHANGE/WHITEOUT 返回 `ENOTSUP`。保留 `renameat(38)` 兼容入口；固定 RV64 Linux 与 musl 使用 `renameat2(276)` 完成普通 rename，因此差分也使用该原生入口。后端操作事务成功接受后发布内存目录项变化，异步 journal mount 的持久化由组提交及目录同步保证；覆盖目标的 OFD 保留原 inode 和内容。目录 `..`、两侧链接数和持久 orphan 由同一事务处理。

## `pipe2` 与 FIFO endpoint

`kernel_files_pipe2()` 创建两个新的 open file description 和两个 fd 槽；它们共享一个 order-4、64 KiB 连续 ring buffer，但读端只增加 read-side 引用，写端只增加 write-side 引用。`O_CLOEXEC` 保存在两个 descriptor flag 中，`O_NONBLOCK` 保存在两个 OFD status 中；`F_SETFL` 可切换 `O_NONBLOCK`，并接受 64 位目标上 musl 每次带来的 `O_LARGEFILE` 兼容位而不改变该位。

读端有数据时按 ring 顺序返回，空且仍有 writer 时：阻塞 fd 进入 interruptible wait，非阻塞 fd 返回 `-EAGAIN`。所有 writer 关闭后空读返回 EOF；写端没有 reader 时返回 `-EPIPE`，同时向当前 task 发送 SIGPIPE，因此有 handler 时先观察信号、无 handler 时按默认动作终止。写端空间不足时阻塞或返回 `-EAGAIN`；不超过 4096 字节的单次写在当前单 hart 实现中保持原子，较大写按可用空间推进。EOF/EPIPE 唤醒全部受影响 waiter；数据/空间变化也唤醒对端全部 waiter，被唤醒后重查条件，并可被未阻塞信号唤醒后返回 `-EINTR`/按 `SA_RESTART` 重启。

每个已发布的 pipe 片段对应数据区中的一页。`write/writev` 跨 iovec 复制完整片段后才增加有效长度；复制中途 fault 时丢弃尚未发布的片段，已发布的前缀仍返回。后续写入只按请求长度的页内余数尝试并入尾片段，其余使用新页片段。`read/readv` 对一个片段的本轮请求必须完整复制到用户空间才消费它；fault 可使用户缓冲区出现前缀，但该未完成片段仍可被再次读取。已消费的先前片段决定返回进度，之后不会为了下一 iovec 再等待新数据。16 个槽已占满时 `poll` 不报告可写，即使尾页尚有可合并空间；小写入仍可尝试尾页合并。这些边界按固定 Linux `f4cdf7ca9a1fdcca413157df19753f388a5a224e` 的 `references/linux/fs/pipe.c` 对照。

用户复制可能因文件映射缺页而睡眠，关中断不能单独保护复制期间的片段。共享 pipe 持有 rank 15 的可睡眠复制锁，从选择片段到复制和发布／消费完成都保留资格；等待数据或空间前释放，唤醒后重新取得并检查状态。锁顺序为 OFD offset（10）→ pipe 复制（15）→ 缺页所需 inode／backend（20／40），请求的 OFD pin 保证锁等待和复制期间 pipe 仍有 owner。`make test-io-sleep-riscv` 在真实调度中暂扣一次用户复制，核对两个写者的完整内容和最终回收；管道的容量、原子写及 fault 前缀规则不变。

匿名pipe的mode与创建时间由共享pipe对象持有，fchmod更新权限和ctime并保留FIFO类型；fstat与proc跟随查询报告稳定inode身份及同一元数据，两端、dup/fork和proc重开一致。数据读写不改变匿名pipe时间。`make test-fifo-riscv`的同ELF反例在旧内核以ENOTSUP失败，在固定Linux和修复后通过；raw ABI另保护改权及坏输出指针。

pipe 的 `fstat` 以 `S_IFIFO` 形态报告，`lseek` 返回 `-ESPIPE`；匿名pipe不进入ext4页缓存；命名FIFO另持真实VFS node。两个 endpoint 的最后一个 OFD 关闭后，ring buffer、等待队列和 pipe owner 一起释放。创建或双 fd 安装的任一步失败都会先回收已创建 description/fd；只有真实 VFS/I/O 清理错误才由文件表保留 pipe 或 OFD owner，物理页和堆释放不建立重试状态。

## `read/readv`、offset 与部分复制

`kernel_files_read()` 最多传送 Linux `MAX_RW_COUNT` 形态的 `INT32_MAX` 向下页对齐值。无效 fd 或普通文件 OFD 的访问模式为 `O_WRONLY` 时返回 `-EBADF`；该访问模式属于 OFD，因此 `dup`/fork 后仍保持。访问模式检查先于零长度和用户地址检查；通过后，零长度无需解引用用户地址并返回 0。`pread64` 对普通文件使用同一可读性契约，但按调用者给出的非负 offset 读取且不推进 OFD offset。它在 MAX_RW_COUNT 截断和 EOF 判断前校验原始用户范围，非法高地址即使 count 为零也返回 `-EFAULT`。与固定 Linux `f4cdf7ca9a1fdcca413157df19753f388a5a224e` 的 [`fs/read_write.c`](../../references/linux/fs/read_write.c) 中 `vfs_read()` 顺序一致，非零读取先按调用者给出的原始 count 验证整个用户范围，再把实际请求截断到上限，逐页从挂载共享缓存取得文件页并执行 `copy_to_user`。

open file description 的 offset 只增加实际复制到用户空间的字节数。首块即发生用户 fault 时返回 `-EFAULT` 且 offset 不变；已经复制前缀后再 fault 或遇到底层读错误时返回前缀长度，并只提交该前缀。短读和 EOF 返回实际长度。这样已经进入缓存但未交付用户的数据不会被错误计入文件位置。

`kernel_files_readv()` 使用相同的普通文件读取核心，整个调用只钉住一次 OFD，并把用户 iovec 快照到最多 8 项的栈数组或最多 1024 项的受限堆数组。先检查 fd 的读权限，再导入向量；每项长度与地址按固定 Linux 的 `references/linux/lib/iov_iter.c` 校验，单项向量先按 `MAX_RW_COUNT` 截断，多项向量先校验原始范围再截断累计长度。零项向量返回 0，零长度段跳过；普通文件按实际交付量推进共享 OFD，EOF、短读、fault 与后端错误均结束本次请求。已发布 serial TTY 的 console/ttyS0/tty 完整 readv 由 [TTY 行规程](kernel-tty.md) 保持一次请求、64B continuation 与消费前缀；未建立 transport 的裸 UART fixture 仍只做一次输入暂存。pipe 按上述片段提交规则消费数据。epoll 描述符没有 read 操作，在向量导入前返回 `-EINVAL`。

每个 chunk 最多覆盖当前 4 KiB 文件页剩余部分：cache miss 承担一次底层随机读，hit 只增加页引用并复制；用户方向仍按基页做软件页表查询。模块统计调用/失败次数、字节、chunk、当前/峰值 fd、表容量与 close-on-exec 数量，页缓存另统计 hit/miss/insert/eviction/reclaim。当前仍有 cache-to-user 一次复制，尚无 read-ahead、直接用户页 I/O 或用户态异步 I/O 接口。优化这些路径时必须保持部分读取、offset 和错误返回语义。

## 描述符 `write`/`writev` 与文件修改

`write` 与 `writev` 支持 console、pipe 以及具备写权限（`O_WRONLY/O_RDWR`）的常规文件：
- 生产 console 经 [TTY](kernel-tty.md) 接受有界 TX、OPOST 和真实输入；不自动建立 ctty。未发布 serial transport 的裸 console fixture 经 kernel_console_putc/原输入暂存。用户 fault、短计数和阻塞 pin 保持所属请求的真实 owner。
- pipe 的 `write/writev` 汇总后沿用 pipe 单次写空间、原子性、阻塞、EPIPE/SIGPIPE 和片段提交规则。
- regular 文件的 `kernel_vfs_pwrite/append()` 将已复制字节接收到共享 inode 页缓存；部分 usercopy 只发布成功复制的前缀，并推进对应 offset/逻辑大小。writeback 错误由 inode 保留，不能事后撤销已经接收的字节。
- `kernel_files_sync()` pin 选定 OFD，同步目标 inode 的数据/元数据与设备缓存；独立 open 各持错误序列观察位置，dup/fork 共享同一 OFD 的位置。普通文件和目录支持 fsync/fdatasync；pipe、字符设备、epoll 返回 EINVAL，无效 fd 返回 EBADF。
- `O_SYNC/O_DSYNC` 在普通写的成功前缀之后执行同步。同步失败返回 errno，OFD offset 与已接受内容保持；这与 Linux `generic_write_sync()` 的顺序一致。full/data 模式分别捕获完整/数据依赖序号，fdatasync 可以顺带持久化同组时间元数据；journal commit 屏障成功后发布 durable，checkpoint 独立回收旧版本。后台清脏和组提交不替代同步错误观察与屏障。
- `writev` 先快照完整用户 iovec 数组，校验长度和范围，再与 write 共用写入核心；`iovcnt` 上限 1024。

## `sendfile` 与来源片段

RV64 syscall 71 用内核有界复制支持 regular/tmpfs 输入，输出可为 regular、pipe、
stream 或 datagram socket。输入、输出各有 OFD pin，正常或信号取消均沿保存的
syscall 栈展开。stream/file/pipe 使用请求暂存页；datagram 源暂存有效容量最多
64 KiB，再接既有整包 request owner 和 charge 等待，不以普通 POLLOUT 猜整包
能否容纳。此接口尚未采用文件页到设备的零拷贝。

NULL offset 推进共享输入位置；显式 offset 只更新用户给定位置。两个 regular
OFD 的位置锁按 rank/key 排序，同一 OFD 或 dup alias 只取一个锁、只推进一次。
读取来源后释放 inode 数据锁，再经目标每批 rank 15 门闩写入；整次复制允许批次
间交错，不跨另一个 inode 的读取持目标门闩。用户 offset 的初始读取先于 fd
检查，最终复制在锁外；只读 offset 可在内容已经输出后返回 EFAULT，不能回滚
真实副作用。

每个同步输出批次都等待 O_SYNC/O_DSYNC。首批同步失败不发布位置，但保留真实
修改的内容；后批失败仅返回此前成功同步批次的正前缀。这个位置规则与普通 write
不同，依据固定 Linux v7.2 的 `fs/read_write.c::do_sendfile`、`fs/splice.c`
和 `generic_write_sync`。不改变 journal、必要 FLUSH 或 durable 条件。

pipe 的内核来源片段不允许后继普通写并尾，普通 write/writev 的来源仍可合并。
片段完全消费时清理 can_merge，复用时按新来源发布；数据仍在独立 pipe 缓冲中，
不会借用可被普通写修改的文件页。这保护容量和等待语义，不能只检查内容复制成功。

```sh
python3 -B tests/network-riscv.py --workload sendfile
make test-files-partial-write-riscv
```

同一真实用户态探针检查位置、alias、错误顺序、传输后 offset fault、EOF/count
上限、stream 短输出、datagram 边界和整包预算、pipe EINTR/EPIPE/SIGKILL，另有
动态填满 pipe 后的来源组合回归。模块复用真实 FLUSH 错误，检查八种同步批次
失败副作用；宿主、固定 RV64 Linux 与 BoarOS 证据见网络学习记录。输入特殊
设备/proc 的 splice 能力、splice/copy_file_range 与 F_GETPIPE_SZ 尚未交付。

## 目录与文件系统操作

- `kernel_files_mkdirat()`：取得 root/cwd/dirfd 起点后调用 `kernel_vfs_mkdir_at()`；只读挂载返回 `-EROFS`。
- `kernel_files_unlinkat()`：支持文件删除与目录删除（`AT_REMOVEDIR` 标志）。普通文件调用 `kernel_vfs_unlink()`，仍打开的对象保留页缓存；目录删除调用 `kernel_vfs_rmdir()`，非空目录返回 `-ENOTEMPTY`。
- `kernel_files_ftruncate()`：校验 fd 具备可写权限且为常规文件，调用 `kernel_vfs_ftruncate()` 调整文件大小（向下截断或向上 sparse 扩展）并精确失效该节点页缓存；backend 已改变 inode 后才返回错误时，仍先同步 node/file size 并失效缓存，再把 errno 返回用户态。只读描述符返回 `-EINVAL`，目录返回 `-EISDIR`。向下截断根据实际新大小通知稳定 node–MM 登记，撤销所有相关 MM 中越过新 EOF 的整页 PTE（包含私有 COW 与 PROT_NONE），保留 VMA 以便随后 fault/SIGBUS。非对齐尾页的文件来源后缀清零，已私有化内容保留；来源记录不依赖缓存索引或 PTE COW 位。

`read/readv` 与 `write/writev` 在 fd lookup 后立即取得独立 OFD 引用，并在本次操作的全部复制、等待和唤醒处理结束后释放。共享表中的另一个线程即使在操作睡眠期间 close 并复用同一 fd 号，本次操作仍使用 lookup 时的 OFD；对于 pipe，这份引用也让原读/写 endpoint 在 in-flight I/O 结束前保持逻辑存活，避免提前产生 EOF/EPIPE 或释放等待队列。末次操作引用触发的底层 cleanup 失败会转交给共享文件表的原有 cleanup 链。该语义基线对应固定 Linux `f4cdf7ca9a1f` 中 [`fs/file.c`](../../references/linux/fs/file.c) 的 `fdget()`/`fdput()` 生命周期。

## I/O 多路复用与 `poll/select`

`kernel_files_ppoll()` 与 `kernel_files_pselect6()` 提供 Linux I/O 多路就绪通知：
- **OFD 就绪检查与等待契约**：每个 open file description 通过 `kernel_open_file_poll()` 报告当前就绪位（`KERNEL_POLLIN/OUT/PRI/ERR/HUP`）并导出所属事件等待队列。管道为空且无写者（`POLLHUP`）、读端有数据（`POLLIN`）、写端有空间（`POLLOUT`）、控制台输出可用或常规文件读取就绪等状态在第一遍扫描时直接确定；若已有就绪事件或指定超时为零，则完全跳过睡眠。
- **多队列等待节点（Wait Node）泛化**：调度器等待队列泛化为由 `struct kernel_wait_node` 串联的双向链表。当 poll 需要睡眠等待时，在被关心的所有有效 OFD 等待队列上分别挂载独立的等待节点（均关联当前 `task`），任一队列事件触发唤醒即退出阻塞，并在返回前可靠从全部队列脱链。
- **OFD 钉住引用（Pinned References）生命周期**：进入阻塞前，poll 引擎通过 `kernel_files_pin()` 对所有被监听的有效 OFD 各获取一份独立引用。这确保了在多线程共享文件表环境中，即使另一线程在睡眠期间 `close()` 并复用相应 fd，正在被 poll 的 OFD 及其内部等待队列依然受保护，不会发生 Use-After-Free；唤醒与清理时统一通过 `kernel_open_file_release()` 释放。
- **原子信号掩码替换**：`ppoll` 和 `pselect6` 支持以原子方式应用调用者指定的临时 `sigmask`，使阻塞等待能够被特定信号打断并返回 `-EINTR`，返回或打断时自动恢复原有信号掩码。
- **紧凑栈预算与堆回退**：任务元数据虽已与 8 KiB 执行栈分离，仍需为 Trap 和深调用链保留余量；`poll` 与 `pselect6` 严格控制栈帧大小（快速路径最多 4 个描述符节点、select 最多 64 个 fd 的单字 bitset）；超出快速路径容量时统一从进程私有 `files->heap` 动态分配并在返回前完全回收，避免击穿内核栈金丝雀（Canary）。

## epoll 事件通知子系统

`kernel_files_epoll_create1()`、`kernel_files_epoll_ctl()` 与 `kernel_files_epoll_pwait()` 实现了 Linux 现代事件驱动 I/O 子系统：
- **epoll OFD 与长效事件绑定**：`epoll_create1` 创建 `KERNEL_OPEN_FILE_KIND_EPOLL` 类型的 open file description。不同于 `poll/select` 每次调用时的全量队列挂载，`epoll_ctl(EPOLL_CTL_ADD)` 将监听项（`struct kernel_epoll_item`）常驻注册在目标 OFD 的等待队列上，实现控制面与数据面解耦。
- **fd 安装事务**：`epoll_create1` 与普通创建路径共用 fd 安装原语；成功同步更新 open/CLOEXEC 统计与最低空闲提示，表满返回 `-EMFILE` 并释放尚未安装的 epoll/OFD owner，不覆盖已有槽。
- **`(target_fd, description)` 键化与目标校验**：epoll 监听项以 `(target_fd, description)` 元组为唯一主键索引。`epoll_ctl(ADD)` 严格检验目标描述符能力：仅支持 pipe、console 和 epoll 等具备真实等待队列通知的对象，常规文件和目录明确拒绝并返回 `-EPERM`。同一进程中由 `dup` 派生的多个指向同一 OFD 的不同 fd 允许独立注册。
- **嵌套与环路防御（Iterative DFS）**：epoll 描述符自身可作为 target 被其他 epoll 实例监控。自监控（`epfd == target_fd` 或 `epoll_file == target_file`）返回 `-EINVAL`；嵌套图的环路检测与深度限制采用固定大小栈在栈内执行迭代 DFS，发现成环或嵌套深度超过 `EP_MAX_NESTS = 4` 时返回 `-ELOOP`，杜绝无界递归保护 1.8 KiB 内核栈。
- **Push 模型就绪列表（Ready List）与回调**：当目标 OFD 状态变化并执行 `kernel_wait_queue_wake_all()` 时，安装在 `kernel_wait_node` 上的 `kernel_epoll_wait_callback()` 自动将所属 `epitem` 追加至 epoll 实例的就绪列表（`ready_head/tail`），并级联唤醒阻塞在 `epoll_pwait` 上的进程。
- **水平触发（LT）、边缘触发（ET）与单次触发（ONESHOT）**：
  - 水平触发（LT，默认）：`epoll_wait` 在返回就绪事件后，若底层数据仍可读写（`kernel_open_file_poll` 仍报告匹配事件），将该 item 重新排入就绪队尾，确保未读完的数据持续通知；
  - 边缘触发（ET，`EPOLLET`）：事件一旦交付用户态，立即从就绪列表移出；仅当目标底层产生新的写入/唤醒边缘时才会重新入列；
  - 单次触发（`EPOLLONESHOT`）：事件交付后将 item 标记为 disarmed，直至用户显式通过 `EPOLL_CTL_MOD` 重新激活。
- **OFD 双向解绑与所有权**：目标 OFD 中维护指向所有监视它的 `epitem` 双向链表（`file->ep_items`）。当目标 OFD 的底层释放时，自动触发 `kernel_epoll_notify_file_release()` 从所属 epoll 实例中解绑。销毁 epoll 实例时，逻辑解绑只执行一次，item 和 epoll 私有堆对象随后按正常 owner 顺序释放；物理页或堆释放若违反分配器不变量直接 fatal。若关联的 VFS/OFD 清理报告真实 I/O 错误，owner 留在文件表供后续回收，fd 槽已经摘除且再次 close 返回 `-EBADF`。
- **休眠唤醒竞态防护**：`epoll_pwait` 进入休眠前关闭中断，在将当前任务挂入 `epoll->wait_queue` 后再次复核就绪列表；若在挂入瞬间发生唤醒，可立即捕获事件避免漏唤醒死锁。
- **放宽 maxevents 与栈预算**：`epoll_pwait` 接受任意 `maxevents > 0`；快速路径在栈上维护至多 4 个事件缓冲（64 字节，`KERNEL_EPOLL_STACK_CAPACITY = 4`），超出时从堆分配并在返回前严格释放，守住内核栈 Canary。

## `lseek`、`fstat`/`newfstatat` 与 `getdents64`

`kernel_files_lseek()` 支持 `SEEK_SET/CUR/END` 的有符号运算与溢出检查，负结果返回 `-EINVAL` 且不移动 offset，越过 EOF 的定位成功；console 返回 `-ESPIPE`。目录也支持 `lseek`，其 offset 兼作 `getdents64` 的条目 cookie。

`kernel_files_fstat()/newfstatat()` 按 riscv64 asm-generic 128 字节 `struct stat` 填充。regular file 与 directory 都先由 `kernel_vfs_fstat()` 取得同一份 filesystem-independent metadata，再转换为 Linux ABI；dev/ino/mode/nlink/uid/gid/size、512-byte `blocks`、filesystem `blksize` 和 atime/mtime/ctime 除 size 来自共享 node 的逻辑大小外，均来自当前 ext4 inode。打开后 unlink 的 file handle 仍指向活着的 inode，因此 `fstat` 可继续读取内容与 metadata，并观察到 `nlink == 0`。当前根 mount 的 `st_dev` 是稳定的 VFS 内部 mount ID 1，只用于同一挂载内的身份比较，不冒充硬件 major/minor。

console、匿名pipe和epoll是不属于filesystem inode的合成对象，继续走各自的显式 stat 形态；console 呈现 5:1 字符设备，pipe 呈现 FIFO。`newfstatat` 支持 cwd/dirfd/绝对路径与 `AT_EMPTY_PATH`（按 fd 取对象，`AT_FDCWD` 取 cwd）；目录路径可统计，`AT_SYMLINK_NOFOLLOW` 通过路径 inode 查询返回链接自身的 mode、大小和时间戳。proc fd 的伪对象跟随式统计复用同一元数据快照。常规文件 create/read/pread/write/writev/truncate/unlink 已更新 realtime 时间戳：读取按 relatime（含缓存命中、非零 EOF 和 user fault），写入先校验 inode maxbytes，再在 usercopy 前修改 mtime/ctime，同长度 truncate 也更新；零长度或访问模式拒绝不更新。创建/移除更新父目录 mtime/ctime，unlink 后仍打开的 inode 继续通过 live handle 更新。扩展 inode 保留纳秒与 signed epoch，旧 128-byte inode 按秒截断；只读挂载不写 atime，未初始化时钟不覆盖 fixture metadata。触发、I/O 错误 owner 和固定 Linux 依据见[文件时间戳](../learning/file-timestamps.md)。

`faccessat` syscall 48 复用 cwd/dirfd/绝对路径查找并跟随符号链接；非法 mode 先返回 `EINVAL`，用户路径 fault 为 `EFAULT`，不存在路径沿用查找 errno。当前不可变 root 身份仅需对普通文件执行请求保留“至少一个执行位”约束，对只读挂载的普通文件或目录写请求返回 `EROFS`；多用户凭据、ACL 与 mount `noexec` 尚未实现。边界由 `tests/diff-abi/access.c` 在同一 RV64 ELF 的固定 Linux 和 BoarOS 上核对；依据是本地 `references/linux/fs/open.c::do_faccessat`，commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e`。

`kernel_files_getdents64()` 只作用于目录描述符，其他类型返回 `-ENOTDIR`。每条记录按 linux_dirent64 编码（`d_reclen` 8 字节对齐，`d_type` 来自 ext4 filetype），`d_off` 是下一条记录的后端 cookie，不是条目计数；缓冲区连第一条记录都放不下返回 `-EINVAL`，用户 fault 在已完整发出的记录上返回前缀计数。

## 关闭与退出回收

`close` 先从表中摘除选定 fd，并更新 open-fd/CLOEXEC/close 统计，再只释放该槽的 OFD；有效 close 返回 0，重复或无效 fd 返回 `-EBADF`。选定 OFD 的 VFS/I/O 清理需要重试时，文件表只将该 owner 入队一次。`close` 不会排空既有的 `cleanup_files`，因而不会释放或返回无关历史清理的结果；批量的 `close_on_exec` 和最终 `kernel_files_release()` 各自承担自己的清理回收。

`kernel_files_close_on_exec()` 扫描 `FD_CLOEXEC` 槽并使用同一“先逻辑摘除、再物理清理”规则。它只在新映像已经提交后调用；普通 exec 失败不会触碰文件表。VFS close 的真实 I/O 清理错误返回 `CLEANUP_REQUIRED`，但被摘除的 fd 对新程序立即为 `EBADF`，待清理 owner 仍由文件表保存；堆释放不返回可重试状态。

用户 task 退出时先在任务仍位于自身内核栈、但硬件已经切回内核地址空间的边界处理重资源：

```text
fd-slot OFD references -> files table -> fs context
-> mapped OFD references/MM -> zombie
```

最后一个普通 OFD 引用才关闭底层 VFS file；pipe OFD 的最后一个读/写端引用在 detach（包括 dup 替换）时立即更新 endpoint 计数并在两端归零时释放 ring。父进程关闭 fd 不会使仍由子进程或任一 MM 文件映射引用的 OFD 失效。只有文件系统或块 I/O 清理错误需要把 task 留在 exited 队列，idle 才从记录状态重试；合法堆/页释放已经完成，分配器不变量错误直接 fatal。根 mount 必须活到 PID 1 及其子进程的 fd 与映射来源全部回收，之后生产根启动路径才能 purge cache、unmount 并检查 heap/物理页基线。

## 显式时间与文件系统统计

`umask` 属于 fs context：初值 `0022`，`CLONE_FS` 共享，普通 fork 复制；
`openat(O_CREAT)` 与 `mkdirat` 在创建时用当前掩码削去权限位，打开已有文件
不改权限。`fchmodat` 解析路径并跟随末端符号链接，`fchmod` 用 fd 持有的
活 inode，故 unlink 后仍可修改；两者保留文件类型位、更新 ctime，
只读挂载返回 `EROFS`。匿名pipe的mode、UID/GID和ctime属于共享pipe对象，
两端、dup/fork与proc重开观察同一元数据；匿名epoll和socket的改权仍返回`ENOTSUP`。
`O_PATH` 使用明确的路径OFD，拥有稳定path引用；它不打开字符设备、会合FIFO、
初始化数据缓存或取得写/执行租约。open/openat先归一化为PATH、DIRECTORY、NOFOLLOW，
CLOEXEC归fd槽；其他访问/创建/截断位不取得数据资格。dup/fork共享OFD，最后关闭释放path。
fstat/fstatfs、目录相对操作、fchdir、FD标志和F_GETFL可用；空路径stat/chown/utimens、
linkat和readlinkat使用原身份，NOFOLLOW可持有链接自身。数据、seek、mmap、ioctl、记录锁、
直接fd改权/所有权/时间与同步入口先返回EBADF；poll显示POLLNVAL。
unlink/rename及同名重建不会替换句柄身份。execveat、openat2仍未交付。
`make build/diff-abi/path-rv`配`tests/diff-abi/path-cases.txt`可聚焦差分；完整ABI仍包含同一记录。
`O_NOCTTY` 是合法open flag，其控制终端语义见[TTY模块](kernel-tty.md)。
固定 Linux
`references/linux/fs/open.c`（commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e`）
和 raw `tests/diff-abi/access.c`、`mode.c` 核对 errno、权限与 fork/共享边界。

`fchown(55)`与`fchownat(54)`接受32位UID/GID；`UINT32_MAX`保留对应字段，
其他值不会被截断到16位。路径形式支持cwd、dirfd、符号链接跟随及
`AT_SYMLINK_NOFOLLOW`；空字符串配`AT_EMPTY_PATH`选取fd或cwd，NULL路径返回
`EFAULT`。未知flags先返回`EINVAL`；空字符串无该flag返回`ENOENT`；有效fd
不要求写访问模式，只读挂载返回`EROFS`。用户路径只复制一次且在存储锁外进行；
fd形式从查表到后端修改结束持有选定OFD，unlink后仍修改原inode。

所有权修改更新ctime、保留atime/mtime；非目录清除S_ISUID，带组执行位时
清除S_ISGID，目录保留set-ID位。ext4还在同一操作事务内删除`security.capability`，
失败回滚UID/GID、mode、ctime与属性。setgid目录创建的节点继承父GID，子目录
另继承setgid位。命名FIFO走VFS inode，匿名pipe走共享pipe元数据；没有真实后端
的其他合成对象不返回空成功。当前进程仍是不可变root，本接口不提供setuid、账户、
完整权限检查或文件capability执行语义。

`utimensat` 的 times 先完整复制，两项 `UTIME_OMIT` 随即成功，不解析路径、fd 或 flags。其他请求先检查 flags 并取得目标，再检查纳秒，最后检查只读挂载；不存在路径与坏 fd 优先于非法纳秒。NULL pathname 且 dirfd 不是 `AT_FDCWD` 是 musl `futimens` 使用的 fd 形式，只接受 flags=0；空字符串配 `AT_EMPTY_PATH` 支持 fd 与 cwd。`AT_SYMLINK_NOFOLLOW` 修改链接自身，默认跟随链接。NULL times 或 `UTIME_NOW` 使用本次同一个 realtime 值，`UTIME_OMIT` 保留字段，实际修改同时更新 ctime，不修改父目录时间。fd 无须写访问模式；权限仍限于当前不可变 root 模型。

`statfs/fstatfs` 输出 RV64 asm-generic 的 120 字节布局，输出前取得真实 mount 统计；路径/fd 错误优先于输出指针 fault。fd 直接使用其持有的 mount，unlink 后仍可查询；没有文件系统 mount 的 pipe/匿名 epoll/初始 console 返回 `ENOTSUP`，不伪装成根盘。统计不触发文件数据写回，空闲块只计实际磁盘分配；容量扣除真实 ext4 元数据及内部 journal 开销，bavail 扣除 superblock 保留块。BoarOS 没有 Linux 的紧急 extent 保留池，因此不额外扣除不存在的池。只读和 relatime 标志来自实际挂载契约。

固定依据为 `references/linux` commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e` 的 `fs/utimes.c`、`fs/statfs.c`、`fs/ext4/super.c`、`fs/inode.c` 与 `include/uapi/asm-generic/statfs.h`。测试入口为 `make test-files-riscv test-lwext4-metadata-host test-userland-riscv test-diff-abi-riscv`。
所有权窄宿主回归用`./tests/lwext4-metadata-host.sh ownership`，覆盖大ID、
内联/外部/共享属性块、嵌套abort、逐分配点OOM、WRITE/FLUSH错误、重开与独立e2fsck；
`tests/diff-abi/ownership.c`用同ELF比较ext4/tmpfs、pipe、符号链接、只读与错误顺序。

## 请求暂存与成本

普通文件与 TCP 写入按需分配一个 4 KiB 请求页，通过当前任务登记覆盖等待与退出；pipe 保持自己的 ring 协议，console 保持小块暂存。缓冲分配失败返回 ENOMEM；有效用户复制前缀仍只按后端已接受字节推进 offset，O_APPEND、定位写与同步错误观察规则不变。AF_UNIX DGRAM 使用有界整包缓冲而非页分块提交，任务登记临时发送 owner；socket 请求 owner 见网络模块。

`kernel_files_statistics.write_chunks` 记录普通文件/TCP 的用户复制分块，`read_chunks` 包括 socket 的暂存复制；页解析与 TCP 协议提交有独立计数。`make test-scale-riscv` 用真实 VFS/MM/uaccess 验证对齐 1 MiB 文件写入不超过 512 分块和 512 次用户页解析，并检查内容与暂存页 OOM。当前值均为 256；它是结构成本，不是吞吐倍数。

## 验证与限制

`readv/writev` 的 8 项以内向量不分配导入缓冲；更长数组为完整输入快照分配至多 16 KiB 元数据，释放遵循堆不变量。数据仍直接从用户空间复制到 pipe 数据区，console 使用 64 字节暂存；目录条目在固定内核缓冲区中编码一次，再复制到用户空间，目录拆分没有引入转发层或额外数据复制。等待唤醒扫描当前 blocked 链，单次 wake-all 为 O(阻塞任务数)，不是已完成的可扩展并发队列。

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
make test-files-partial-write-riscv
make test-syscall-riscv
make test-exec-riscv
make test-userland-riscv
make test-diff-abi-riscv
make test-root-init-riscv
make test-riscv
```

`make test-files-partial-write-riscv` 覆盖跨入未映射页前可读 0、1、32、63、64、65 字节的普通写、append 和 writev（含已有进度及后续可读 iovec），核对返回值、offset、文件长度、内容和字节统计；真实 ext4 后端注入带前缀的写回错误，验证缓存内容与逻辑大小保留、重试完整写回，以及独立 open/dup 的错误观察。另注入设备 flush 错误，验证 O_SYNC/O_DSYNC 返回错误后 offset 与内容保留。`make test-userland-riscv` 以 `mmap/mprotect(PROT_NONE)` 对同组边界执行真实 musl 系统调用并核对内容和 metadata。

`make test-diff-abi-riscv` 用相同 raw-syscall ELF 比较固定 RISC-V Linux 和 BoarOS 的 readv 参数顺序、0/1/32/63/64/65 字节可写前缀、1024/1025 项、跨文件页/共享 OFD、pipe 片段故障后重读、尾片段合并、环回以及 poll 可写边界；保留原始串口与规范化差异。`make test-files-riscv` 注入 UART 三字节批次并核对 console readv 跨两个向量分散，同时注入长向量分配失败，核对 `ENOMEM`、OFD 释放和调用统计；`make test-userland-riscv` 以真实 musl 验证两个 pipe 读者及 readv 的 EINTR/SA_RESTART。固定 libc-test 的静态/动态 `ungetc` 和完整 BusyBox 的 `od/hexdump` 另以真实程序套件逐例比较完整输出。

聚焦测试在真实 QEMU legacy 与 modern VirtIO/ext4 上覆盖绝对/相对路径、错误 flags、目录与缺失文件、4096 字节路径上限、最低 fd 复用、表扩容、统一 fd 安装统计、epoll 满表原子性、`O_CLOEXEC/O_NONBLOCK`、缓存命中后的跨页读取、EOF、部分 fault、fork 后 fd 表独立与 OFD offset 共享，以及 VFS orphan/I/O owner。stat 回归核对 regular/directory 的真实 inode metadata、allocated blocks、fstat/newfstatat 共同字段和 unlink-but-open 的零链接计数。它还在关闭 fd 后通过 MM backing 继续缺页，反复固定地址映射同一 OFD 并检查来源释放只发生一次，验证父子各自持有一份来源引用。生产 exec/clone 链验证普通 fd 与 offset 跨映像和父子保持、CLOEXEC fd 不可见，PID 1 的 stdio 与跨 exec 的 console 描述符由串口标记验证，并由最终资源基线证明退出清理生效。`make test-userland-riscv` 用静态和动态 musl 程序作为 PID 1 运行 stdio、readdir、read/lseek/fstat、dup、signal、pipe、pthread、TLS 和 dlopen；其中写打开普通文件的真实 `read/pread` 及其 dup 均验证 `EBADF`，是真实 U-mode 外部测例的入口。

当前提供可共享的文件表与根 fs context handle，普通 clone 仍实现“复制表/复制 cwd、共享 OFD”；系统调用层是否选择共享由 clone flags 决定。当前已支持常规文件的读写（`write/writev/pwrite64/append`）、新建、删除（`unlinkat`）、截断（`ftruncate`）与目录修改（`mkdirat/rmdir`）及符号链接（`symlinkat/readlinkat`）；并支持 cwd/dirfd、普通/NOREPLACE rename；已有阈值驱动后台写回，仍无周期清脏或 read-ahead；linkat 硬链接、tmpfs 和独立第二 ext4 盘已接入，块节点用于挂载识别，裸设备 OFD 明确不支持。当前单 hart 下 fd lookup 与 OFD acquire 之间不可调度；启用 SMP 前必须为共享 record 引用、槽查找/替换、统计和 OFD 引用补齐同步，不能直接复用这些无锁字段。pipe 同样是单 hart 对象。生产 console 接收由 DTB UART IRQ 收割、worker 推进行规程；raw/tick 轮询只保留在未建立 transport 的独立模块 fixture。IRQ、worker 和任务运行延迟没有硬实时上界，具体 continuation、termios 与 owner 见[TTY 模块](kernel-tty.md)。

字符设备节点由 ext4 提供名称和 `st_rdev`；`openat` 只按设备号查找 `fs/char_device.c` 的内建操作表，未知设备号返回 `ENXIO`。OFD 持有选定的静态操作及 open 创建的实例，read/write/poll/ioctl 从它分派；初始标准 fd 也取得同一 console 后端。启动时优先打开根盘已有的 5:1 `/dev/console`，使标准 fd 持有真实路径；只有该节点缺失才使用无路径 UART OFD。其他查找/I/O 错误明确中止启动，不伪装为节点缺失。该表在当前执行地址域中初始化回调，兼容分页前模块测试与生产高半区。console 的 UART 输入等待支持非阻塞 `EAGAIN` 和信号打断；null 读 EOF、写消费请求长度，zero 读按实际用户复制进度填零；两者不经过普通文件页缓存和 ext4 数据 I/O。设备 OFD 由 fd 表安装和引用，dup/fork 共享，关闭 fd 不撤销已 pin 的 I/O。`readv/writev/pread64/pwrite64/lseek/fstat/ppoll` 及 console 非阻塞读取经固定 Linux 差分验证；未知 ioctl 对有效 fd 返回 `ENOTTY`。另登记 1:8 random 和 1:9 urandom：random 读取等待可信源初始化，遵循固定 Linux 的 random_read_iter，O_NONBLOCK 在未就绪时返回 EAGAIN；urandom 允许未初始化的不安全流。random poll 未就绪报告可写，就绪报告可读；urandom 始终可读写。用户写入经 BLAKE2s 混种但不计可信熵。随机 ioctl 的 RNDGETENTCNT 返回已计入可信字节数乘 8（最多 256），坏输出地址返回 EFAULT；未知请求返回 EINVAL，尚未实现的特权注熵、清池与重播种请求明确返回 ENOTSUP，不声称增加熵成功。random 支持 epoll，urandom 遵循固定 Linux 无 poll 回调的 EPERM 边界。节点仍由用户态 mknodat 建立。serial transport 发布后另按 rdev 登记 ttyS0/console/tty，并提供 [TTY 会话与行规程](kernel-tty.md)。没有动态设备注册、设备 mmap 或 devfs。

设备号与操作依据固定 Linux commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e` 的 [`fs/char_dev.c`](../../references/linux/fs/char_dev.c)、[`drivers/char/mem.c`](../../references/linux/drivers/char/mem.c)、[`fs/eventpoll.c`](../../references/linux/fs/eventpoll.c) 与 [`fs/read_write.c`](../../references/linux/fs/read_write.c)。

## 可睡眠文件调用

共享 OFD 的普通读写、seek 和目录游标受 offset mutex 保护；定位 I/O 不取得 offset 锁。调用在可能等待前 pin OFD，元数据与目录操作同样适用；路径起点获得独立 path 引用，不借用可能被 close/chdir 替换的对象。open 先预留 fd，再执行可睡眠解析/创建；失败取消预留，fork 不复制未安装槽，dup3 对正在预留的目标返回 EBUSY。dup 替换在安装新 OFD 前不执行会睡眠的最后释放。延后清理链先摘下本轮集合，再执行可能等待的关闭并重新挂入仍有真实 owner 的失败项，避免并发 drain 覆盖新项。

文件写先完成请求缓冲的用户复制，再进入 inode/后端锁；读持引用取得数据，解除存储锁后复制到用户。与用户缺页重入、共享 offset、close 后 fd 复用相关的门禁为 files、partial-write、真实 userland 和差分测试。

## 节点创建

`mknodat(33)` 复用 dirfd/cwd/root 路径快照，应用进程 umask 后调用 VFS。
当前交付普通文件（类型 0 或 S_IFREG）、字符设备（S_IFCHR）与块设备节点（S_IFBLK）；目录返回 EPERM，
非法类型EINVAL；FIFO节点与传输已接入，socket节点仍返回ENOTSUP。块节点仅用于 stat、命名和 mount 的 st_rdev 识别，裸盘 open 不支持。设备号取 Linux
32 位编码；任意字符设备号可存储，打开时既有 null/zero/console/random/urandom 及RTC后端可用，
未知号返回 ENXIO。不以路径名识别设备。

用户路径复制在存储锁外完成；路径引用和临时堆缓冲在所有结果分支回收。已有节点
返回 EEXIST，不覆盖类型或设备号；空路径、坏地址、坏 dirfd 与父目录错误保留原 errno。
`make test-diff-abi-riscv` 的 `mknod.*` 保护创建、umask/stat、真实设备读写和错误。

`kernel_files_linkat()` 固定源 path 或 fd 引用后再操作目标父目录，支持 flags 0、
AT_SYMLINK_FOLLOW、AT_EMPTY_PATH。独立打开的硬链接有独立 OFD，记录锁按 inode
共享；同 OFD 的 dup/fork 共享规则不变。tmpfs read/readv/pread 用无分配空洞读路径，
只有 mmap 缺页或实际写入才消耗后备页配额。

普通文件整次 write/writev/pwrite 在有界 staging 循环外取得 inode 操作门闩（rank 15），直到同步写收尾返回才释放；非定位写先取得共享 OFD offset 锁（rank 10）。truncate 与直接 VFS pwrite/append 走同一门闩，fault/read/writeback 不取得它。`make test-io-sleep-riscv` 以独立 OFD 在块间复制等待时安排追加、重叠定位写与截断，检查整次结果和最终清理；`make test-userland-riscv` 补实际同 inode 未驻留映射缓冲和多页向量追加。

成本诊断版本记录真实请求/接受和 staging/usercopy，保持短写及同步尾部错误的原行为；接口与单位见[成本观测](kernel-cost.md)。

## 仅读RTC与OFD设备资格

Goldfish RTC按st_rdev=10:135选择，/dev/rtc0、/dev/rtc和/dev/misc/rtc是同一设备身份，
不是路径特判。`RTC_RD_TIME(0x80247009)`复制Linux九整数布局的UTC日历，秒值直接来自
`riscv_virt_rtc_read_ns`；无设备/不合理读数返回ENODEV。`ioctl`命令在syscall边界截为
unsigned32位，与Linux相同，兼容musl传来的符号扩展。未知命令ENOTTY，设置时间、
告警/事件等已知但未交付操作ENOTSUP，普通事件read/write同样明确不支持。

字符设备回调携带实例上下文、调用者及open flags；open在OFD发布前构造实例，
失败不留下实例owner。最后OFD清理才release；dup/fork/请求pin不重新open。
可选整次readv/writev入口用于有请求状态的后端，简单设备继续使用缓冲回调。
RTC独占打开，dup/fork/请求pin增加同一OFD
引用，不重新打开；最后真实引用脱离即释放资格，VFS后续清理错误不会继续霸占RTC。
失败打开不发布资格；CLOEXEC、退出和最终关闭沿统一文件引用路径处理。
验证：`make test-rtc-host test-environment-riscv`与固定Linux环境ABI记录，含闰日、
2100世纪例外、dup/fork/exec、坏指针、符号扩展请求与失败后重开。依据固定Linux
`include/uapi/linux/rtc.h`、`drivers/rtc/dev.c`及`Documentation/admin-guide/devices.txt`，
commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e`；平台设备协议保持Goldfish原契约。

## socket终止事件与接收独占（2026-10-02）

阻塞接收的内部等待谓词由socket的实际数据/错误/EOF和reservation决定，不能把
poll的HUP/ERR直接当作取得队首资格。等待期间释放CPU并保留当前请求/OFD owner，
完成或fault释放reservation后唤醒后继；SO_RCVTIMEO与信号仍由同一次等待处理。
`make test-io-sleep-riscv`的定向交错复用现有MM/uaccess包装器，保护完成、fault、
超时和信号的结果以及页基线；对外poll和非阻塞语义另由network与固定ABI验证。

## 命名 FIFO

ext4/tmpfs 的 FIFO inode 保存持久身份、权限与时间，传输仍使用现有64KiB pipe。
节点只保存弱关联；每个打开中或已打开的OFD同时钉住节点与pipe，最后owner先摘关联、
丢弃内容，再关闭VFS引用。dup/fork共享OFD资格，hardlink/rename/unlink不改变实例；
同名重建是新inode。创建节点不分配传输缓冲。

阻塞只读/只写与对端会合，非阻塞只写无读者返回ENXIO，O_RDWR立即成功。
到达代次保护对端出现后立即关闭的交错；初始非阻塞只读尚未见过写者时抑制HUP，
后续写者离开才报告HUP。等待前没有namespace/inode锁；信号唤醒后先检查会合代次，只有对端未到达才按重启协议归还资格。
pipe端点先关闭一次，真实VFS关闭错误继续归mount清理，不重新归还端点。

正数读写更新命名节点时间，匿名pipe不更新；只读挂载允许FIFO传输而不修改时间。
FIFO不可seek或fsync，节点由文件系统同步持久化，传输内容永不恢复。
`make test-fifo-riscv`以同ELF核对Linux的身份、打开、poll/select/epoll、时间、信号、
fd满、只读挂载与重启；`make test-files-riscv`另注入对象/缓冲OOM并验证重试与回收。
