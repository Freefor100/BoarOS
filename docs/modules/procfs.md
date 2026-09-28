# procfs 模块

`fs/procfs.c` 是可动态创建的 VFS 实例。当前提供真实物理页分配器读数的 `meminfo`、单 hart 运行/idle 计时的 `uptime`、进程数字目录、`/proc/self` 与进程的 `exe/cwd/root/fd` 对象链接、`/proc/self/mounts` 与 `/proc/mounts`，以及进程 `stat/status` 的首批真实字段。Linux 全量进程字段、`fdinfo` 与初始无路径 console 的链接尚未接入；不能把这批入口当作完整 procfs。

`mount(2)` 接受类型 `proc`、普通挂载、`MS_RDONLY` 与 `MS_SILENT`；源字符串只作用户地址验证，不选择设备。用户态负责创建挂载点。其他挂载模式显式拒绝。`umount2(2)` 只接受 flags 0，要求名称解析到 proc 实例根且无 cwd、文件、子挂载或其他路径引用。挂载实例持有根及覆盖路径，失败创建或发布不会改变原树；成功卸载先摘树、释放根，最后销毁无 I/O 的 proc 实例。PID 1 及后代全部退出后，根盘清理由最深子挂载开始迭代拆除 proc，不依赖用户态逐个卸载。当前所有任务共享一棵挂载树。

生成式普通文件不进入 ext4 页缓存。后端 `snapshot` 返回由 OFD 持有的堆缓冲；第一次非空读取形成快照，短读、readv 和 pread 使用它，seek 到零后释放并在下一次读取重新取值，`SEEK_END` 按固定 Linux 的 seq_file 语义返回 `EINVAL`。用户复制时不持有后端或 inode 锁；只推进已经复制的字节。OFD 末引用释放快照，mount 忙判断覆盖文件及目录引用。`fstat` 中的 size 为零是生成文件的元数据语义，不能由它推断 read EOF。

`MemTotal` 和 `MemFree` 单位为 KiB，来源分别是 `physical_page_total()` 和 `physical_page_available()`；它们描述 BoarOS 管理的物理页，并不冒充尚未统计的缓存、交换或可回收页分类。固定 Linux 的 root 可以用写模式打开 `meminfo`，但写入返回 `EIO`；该行为与内容不可修改一致。未知目录项返回 `ENOENT`。首批 U-mode 对照见 `tests/diff-abi/proc.c`；内部挂载树测试见 `tests/riscv/vfs_main.c`。

进程树从 PID 1 的父子关系遍历，包括未回收的 zombie；线程组用仍存活的成员作为查询代表，PID 代次防止旧目录指向新任务。`stat` 当前输出前 24 个标准字段，足供固定 BusyBox `libbb/procps.c` 的基本解析；不追加尚未统计的 Linux 尾字段。`status` 当前输出 Name、State、Tgid、Pid、PPid、Threads、VmSize、VmRSS、Uid、Gid，未实现的键不伪造。进程名在 exec 提交时取主程序末尾组件，长度上限 15 字节。初始 session ID 为 PID 1，子进程继承；TTY 缺失使 tty_nr=0、tpgid=-1；尚无 nice/策略接口，默认 priority=20、nice=0。Uid/Gid 均为当前单用户 root 身份。

stat 的 CPU/starttime 单位是 100 Hz scheduler ticks；utime/stime 汇总存活线程及已退出成员，cutime/cstime 汇总 wait 已回收子进程。minflt/majflt 按成功的用户缺页解析记账：如果该解析在当前任务上发布了 VirtIO 读请求，计为 major，否则计为 minor；已回收子进程同样卷入 cminflt/cmajflt。并发等待另一任务发起的缓存装载尚不能准确归为 major，这是当前的分类限制。VmSize 是 VMA 覆盖字节之和，编辑提交时维护；VmRSS 是 MM 中有效和 PROT_NONE 已拥有 PTE 的页数，单位页。Zombie 保留统计与 `Z` 状态，MM 释放后的 VmSize/VmRSS 为零；wait 回收后，已打开的旧进程目录继续查找返回固定 Linux 的 `ESRCH`。数字目录新查找则返回 `ENOENT`。

PID 数字目录的 inode 编入单调分配代次；进程退出后旧目录对象不能通过复用的 PID 转向新任务。`/proc/self` 的链接文本在每次调用时读取当前任务 TGID，不把首个访问者缓存为全局目标。`exe` 来自主程序 MM 单独持有的 OFD 引用，动态解释器不取代它；fork 的子 MM 取得独立引用，共享 MM 则共享原 owner。`cwd/root` 来自目标任务当前 fs context。VFS 跟随这三个链接时直接取得活路径引用，避免对展示字符串重新解析；readlink 仅生成展示文本，已删除对象标注 `(deleted)`。任务查询在单 hart 短关中断区内取得快照或路径引用，不钉住任务栈。生成文本和对象链接的聚焦对照为 `make test-diff-abi-riscv`；未实现链接不返回伪造内容。

`fd/` 按活文件表枚举 0–1023，查找和链接读取重新核对 PID 代次及当前槽；关闭后返回 `ENOENT`，复用同一编号时按新槽解析。动态 inode 为 PID 保留完整 16 位（当前 PID 上限 32768）、fd 保留 10 位，另编入进程代次和节点种类。目录项回调以正值报告成功、零报告 EOF，`getdents64` 才会真正递进；曾经错误地以零报告成功，导致 `ps` 只打印表头。普通文件链接钉住目标路径对象，重新打开创建独立 OFD 与 offset。pipe/socket 的展示编号来自对象创建时的单调身份，不泄露内核地址；pipe 链接可按只读、只写或读写重新打开，建立新的 endpoint owner，旧 fd 关闭不拆除新端点。`O_RDWR` 端点分别计入 reader/writer，并用独立聚合等待队列支持 poll。socket 和 epoll 链接可 `readlink`，重新打开返回固定 Linux 的 `ENXIO`。`lstat` 的链接权限位按当前 OFD 读写能力重新计算；伪对象链接的跟随式 `stat` 与无路径 console 展示仍待扩展，不能据此宣称完整的 Linux magic-link 行为。

`/proc/mounts` 的链接文本为 `self/mounts`。进程 `mounts` 的第一次非空读取在短关中断区捕获整棵共享挂载树，并分别钉住挂载根与被覆盖路径；随后无锁格式化并由 OFD 持有本次文本。根 ext4 行的来源名为内部 `rootfs`，只表达本内核的根盘身份，不伪称 `/dev` 节点；proc 行的来源和类型均为 `proc`。输出包含实际挂载点、后端类型、实例的 `ro/rw` 状态和 `0 0` 字段，路径中的空白、反斜杠及 `#` 用八进制转义。卸载期间的引用会使普通卸载返回 `EBUSY`，快照交付后立即解除临时引用。`make test-diff-abi-riscv` 在固定 Linux 对照链接文本、打开能力和真实 proc 行；嵌套挂载与并发卸载还需要专门的真实用户态用例。
