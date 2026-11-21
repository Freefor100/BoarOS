# procfs 模块

`fs/procfs.c` 是可动态创建的 VFS 实例。当前提供真实物理页分配器读数的 `meminfo`、单 hart 运行/idle 计时的 `uptime`、进程及线程数字目录、`/proc/self`、`/proc/thread-self` 与进程的 `exe/cwd/root/fd` 对象链接、`/proc/self/mounts` 与 `/proc/mounts`，以及进程 `maps`、`fdinfo`、`comm`、`stat/status` 的真实字段、`/proc/sysvipc/{shm,sem,msg}` 和 `/proc/sys/kernel/sched_rt_period_us`、`sched_rt_runtime_us`、`sched_rr_timeslice_ms`、`hostname`、`domainname`、只读的`pid_max`。线程目录提供 `comm`、`stat/status`、`fd/fdinfo`、`exe/cwd/root`、`maps`、`mounts` 和 `ns/{mnt,uts,ipc,pid,pid_for_children}`；Linux 全量进程字段与无路径 console 的链接仍待接入。

`mount(2)` 的 proc 实例不使用源字符串选择设备；用户态负责创建挂载点。bind、卸载和挂载属性的共同规则见[VFS 挂载说明](vfs-ext4.md)。挂载实例持有根及覆盖路径，最后释放时销毁无 I/O 的 proc 实例。PID 1 及后代退出后的根盘清理从最深子挂载开始拆除；其他命名空间的私有树随进程和命名空间 FD 的最后引用退休。

## PID 挂载视图

每个 proc 实例固定创建者的 active PID namespace，而不是读取者的当前视图。`fsopen("proc")` 在创建配置 FD 时保留该视图，之后的创建、fsmount、bind 及 FD 继承不会替换它；legacy mount 在创建实例时捕获。动态 registry 的引用直到配置 FD 或挂载实例末引用才归还，双退休队列在 identity 锁外回收，初始静态 registry 不增加堆 owner。

路径解析把本地 PID/TID 转为 canonical root 编号及全局 allocation generation，inode 保存后两者。后续 maps、fd、ns、comm 等资源查询继续检查这个 generation、任务资源状态和当前权限，不把 inode 中的编号解释成当前读取者的本地号。根目录和 task 目录按挂载视图的 PID bitmap 枚举，名字与 continuation cookie 使用本地号；不扫描全局任务或逐个遍历祖先。self/thread-self 也按同一挂载转换，读取者不可见时返回 ENOENT。

stat/status 在 identity 锁内捕获 PID/TGID/PPID/PGID/SID 与 tracer 的 TID，外层身份在内层不可见时显示 0。NSpid/NStgid/NSpgid/NSsid 从挂载层列到任务 active 层。pidfd fdinfo 持有原 FD 的真实身份，在挂载视图中显示 Pid 与 NSpid；跨兄弟视图显示单个 0，任务已不存在显示 -1，不能用历史整数重新查找。快照的可睡眠阶段只使用已捕获的 PID 标量；资源引用与权限仍按原 proc 规则复核。

生产查询的 `test-proc-index-host` 覆盖三层与兄弟同号、读取者更换、线程成员过滤、外层/非 leader tracer、旧 role 转移、pidfd 可见/不可见/死亡和挂载最后引用回收。`test-proc-maps-host` 另检查本地目录名/cookie 与 canonical inode 的适配。RV/LA 的 pid_namespace、ptrace、mount_context 程序验证真实初始视图 status/继承 FD/pidfd fdinfo/TracerPid 和挂载路径；受控 host 测试不等同完整 nested guest。嵌套创建与 IPC/cgroup 等独立 PID 输出见 [PID namespace](pid-namespace.md)。

生成式普通文件不进入 ext4 页缓存。后端 `snapshot` 返回由 OFD 持有的堆缓冲；第一次非空顺序读取或非零seek形成快照，连续短读、readv和pread复用已生成的内容，seek 到零后释放并在下一次读取重新取值；定位读取偏移与上次读取进度不连续时也重新生成，`SEEK_END` 按固定 Linux 的 seq_file 语义返回 `EINVAL`。用户复制时不持有后端或 inode 锁；只推进已经复制的字节。OFD 末引用释放快照，mount 忙判断覆盖文件及目录引用。`fstat` 中的 size 为零是生成文件的元数据语义，不能由它推断 read EOF。

`/proc/self/io` 通过当前进程的数字目录展示输入输出统计，进程目录汇总同一线程组，
包括已退出线程和已经回收的子进程。线程目录的 `io` 只报告指定线程。文本包含
`rchar`、`wchar`、`syscr`、`syscw`、`read_bytes`、`write_bytes` 和
`cancelled_write_bytes`。普通、定位、向量读写及 `sendfile`、`copy_file_range`
按系统调用返回的已完成字节记字符数。向量操作完成参数预检查后，无论是否传输数据都计一次，
其中无效文件描述符也计入。当前标量调用按非负返回计次数。数据读写量沿任务资源记账，
区分已经装入的缓存读与实际提交的块读。截断撤下还未交给写回的整脏页时，由发起者记录
取消写入量。普通文件和块设备的直接写入按已完成前缀计入 `write_bytes`，退回缓冲写入的
部分仍按缓存页首次变脏记账。

文件为所有者只读，生成快照时还检查读取者的文件系统凭据与目标的可转储状态。
任务借用覆盖采样，结束前复核进程编号代次、凭据和执行映像。快照文本按打开文件描述
（Open File Description，OFD）保留，顺序短读可以拼接，回到偏移零后再次采样。
线程组历史复用现有的资源限制对象，普通 fork 初始化新组历史，非组长 exec 保留组历史。

`/proc/<pid>/comm` 和 `/proc/<pid>/task/<tid>/comm` 展示对应线程的名称，
`PR_SET_NAME/PR_GET_NAME` 与这两个文件共用任务中已有的16字节 `comm`。
最多保存15字节并补NUL；fork/clone复制名称，成功exec取本次执行请求路径的末级名称；空路径execveat取脚本展开后的执行inode名称，PT_INTERP装载器不替换这一名称，
不使用调用者提供的argv[0]。名称发布和stat/status快照使用同一短保护。

comm读取采用上述OFD快照规则，输出名称后附一个换行。写入仅复制每次写请求的前15字节，
超长部分不访问但计入成功返回长度；writev分别处理各段，后段故障保留前段已完成的名称修改。
零长度write设置空名称，零总长度writev不修改；定位写入返回ESPIPE。
写入者必须与目标属于同一线程组；线程comm的同组访问不受dumpable变化影响。
节点保留查找时的PID代次，目标退出或编号复用后不会修改新线程的名称。

`/proc/sysvipc` 下的 `shm`、`sem`、`msg` 为 0444 的生成式文件，列名、列顺序及数值格式沿固定 Linux 的 `ipc/{shm,sem,msg}.c`：key 使用有符号十进制，权限使用八进制，ID、归属、时间和计数来自真实对象。元数据列表对普通用户可读，与 `IPC_STAT` 的对象访问权限分别处理。共享内存还报告实际后备对象的 resident 字节；当前没有 swap 后备，swap 列为零。已标记 `IPC_RMID` 但仍有附加的共享段继续出现，权限列带 `SHM_DEST`，最后附加和临时 owner 归还后才消失。

这三类文件在普通 open 创建 OFD 时持有打开者当时的 IPC namespace 引用，
第一次读取、短读和 rewind 重新生成都从该 namespace 的实际对象表取行。
调用者后来 unshare/setns 不改变旧 FD 的观察范围；dup、fork 与 SCM_RIGHTS
共享同一 OFD，最后关闭才归还 namespace。O_PATH 不建立这份对象表观察引用。

sem/msg 按 slot 索引逐行遍历，在索引和对应对象的短锁内复制描述符；删除操作使用相同锁序，因此取行时对象及 PID 元数据有效。SHM 管理继续由 CPU0 执行，沿其已有本地中断保护复制段状态与 resident 计数。格式化、快照缓冲扩容和用户复制在这些保护外完成，文本没有 IPC 对象借用。并发创建或删除期间，各行分别取一致状态，整张表不占用全程全局锁。BoarOS 的完整文本沿上述共同生成文件规则保存到 rewind；Linux 的 seq_file 可以分批生成，跨 read 的并发删除不保证与该整份文本相同。`tests/host/proc_sysvipc.c` 检查目录 cookie、格式、空表及分配失败归还；`tests/workloads/ipc/proc_sysvipc.c` 用三类真实对象检查字段、数字用户读取、稳定对象的短读拼接、延迟删除及删除后 rewind。

`/proc/sys/kernel/pid_max`只读提供实际PID分配器的排他上界（最大可分配PID加1）；该值随分配器配置取得，查询此数值对应的TID返回ESRCH。它与已有实时预算控制同处`/proc/sys/kernel`，沿生成式文件快照读取，不提供运行时改写接口。`/proc/<pid>/status`的VmSwap在当前无swap机制下为0。

`/proc/<pid>/maps`（包括 `self/maps`）按目标真实 VMA 地址顺序输出地址区间、当前读写执行许可、private/shared、文件偏移、设备号、inode 和路径；匿名行使用零设备号与 inode，初始堆、栈和 vDSO 分别标为 `[heap]`、`[stack]` 和 `[vdso]`。文件及 ELF 映射从实际后备 OFD 取得身份，ELF 全零区按匿名映射显示；路径中的换行转义为 `\012`，删除后仍保有后备的文件标为 `(deleted)`。VMA 元数据与后备 OFD 引用在 MM gate 内同次捕获，路径渲染、文件 stat、堆分配及用户复制在 gate 外。目标 PID 代次、凭据、dumpable 和 MM 身份在捕获前后检查，exec、退出和 PID 复用不会把旧许可用于新 MM。它沿用第一次读取生成文本、同 OFD 短读复用、rewind 后重新捕获的规则，首次生成时取得目标的当前 MM；不额外保存跨 rewind 的 MM 引用。

RT 控制文件标记为 `generated_control`，通过单独后端 `control` 回调动态读写，不形成 OFD 快照。offset 为零时每次读取 scheduler 的原子 period/runtime 对；非零读取直接 EOF，因而短读不会继续输出剩余数字。write/writev 先完整复制全部用户数据，再按固定 Linux 的单整数 sysctl 规则解析和调用原子 setter：base 0、不接受 `+`，只消耗首个数字及其后空白，32 位溢出和不合法配置返回 `EINVAL`。非零偏移写完整复制后忽略配置并推进偏移；复制失败不发布配置、不推进偏移，读复制失败也返回 `EFAULT` 且偏移不变。OFD offset 锁串行化同描述符操作，setter 自身保护全局配置；无 inode 锁跨用户复制或 scheduler 操作。只读 proc mount 写打开返回 `EROFS`。

`hostname`、`domainname` 为 0644 字符串控制文件，与 `sethostname/setdomainname` 及 `uname` 共用 `kernel/uts.c` 中的状态。每次操作查询调用者当前 UTS namespace，已打开 FD 不固定原 namespace；读取取得当前名称，按偏移继续输出字符串和末尾换行，seek 到零可重新读取。写入先完整复制用户数据，再从当前偏移替换到首个 NUL、换行或 64 字节上界，并加终止 NUL；返回完整输入长度，位置按输入长度推进。若写偏移超过当前字符串末尾，仍返回输入长度，但状态和位置均不变。并发写的字段替换在 UTS 短锁内完成；复制用户数据不持锁。按固定 Linux sysctl 表权限，以当前有效 UID 判断，只有 euid 0 可写，不由 DAC capability 绕过；已打开 FD 降权后再写返回 `EPERM`，写打开被拒绝为 `EACCES`。系统调用自身的 `CAP_SYS_ADMIN` 规则见 [系统调用模块](kernel-syscall.md)。

控制文件使用 Linux `default_llseek` 语义：`SEEK_END` 以元数据 size 0 为基准，非负 `SEEK_DATA/HOLE` 返回 `ENXIO`；其他生成文件保留原 seq_file 规则。`sys/kernel` 目录使用固定 cookie；普通 proc 文件原有快照、fault 前缀和 rewind 行为不改变。实时预算的聚焦同 ELF 对照为 `tests/diff-abi/rt_controls.c`，覆盖动态刷新、数值/偏移、scatter fault、readonly mount 与 seek；解析及取舍依据见 [proc 控制文件](../learning/proc-controls.md)。

meminfo 使用与 `sysinfo` 相同的 `kernel_memory_snapshot()`，输出 KiB。`Cached` 包含文件缓存和共享匿名后备页，`Shmem` 单列后者；`Dirty`、`Writeback`、`Buffers` 分别来自缓存状态与真实块缓冲。`MemAvailable` 的资格、余量和上下界见[物理页模块](physical-pages.md)。没有 swap 和可延后收缩的 slab cache，因此 `SwapTotal/SwapFree/SReclaimable` 为零；不能把普通堆空闲等同于 slab reclaimable。原 LTP 已越过缺少 Cached 的失败点，后续 chown ENOSYS 单列。固定 Linux 的 root 可以用写模式打开 `meminfo`，但写入返回 `EIO`；该行为与内容不可修改一致。未知目录项返回 `ENOENT`。首批 U-mode 对照见 `tests/diff-abi/proc.c`；内部挂载树测试见 `tests/riscv/vfs_main.c`。

数字进程目录按 PID 分配位图跳过空字，并经已有哈希表取得下一项已发布身份；只筛选带 TGID 角色的身份，检查其是否属于 PID 1 的后代，包括未回收的 zombie。未发布的 fork/clone 预留及仅残留 PGID/SID 引用的数字不形成目录项。已知 PID 的路径查询直接命中 TGID 哈希，不重新枚举任务；在 wait 域内取得目标和 PID 1 的存储借用，在域外分段检查祖先关系，最后重新核对角色 owner 和身份代次。单成员子进程从退出到完成清理、转为 zombie 期间仍占有 PID，也保持数字目录可见，直到 wait 回收。其他已退出成员不充当线程组可见代表；数字 PID 对象的内容与 exe/cwd/root/fd 资源始终查询组长，PID 代次防止旧目录指向新任务。退出者在 clear_child_tid 唤醒前发布 `proc_exiting`，使其状态先显示为 Z；Threads 随组成员释放而递减；组长退出但成员仍存活时，组长状态为 Z，其资源链接返回 `ENOENT`。这个标志只影响观察，不转移 MM、文件表或任务栈的清理 owner。`stat` 当前输出前 41 个标准字段，包括真实 priority、rt_priority 和 policy。`status` 当前输出 Name、State、Tgid、Pid、PPid、Threads、VmSize、VmRSS、VmSwap、SigQ、SigPnd、ShdPnd、SigBlk、SigIgn、SigCgt、Uid、Gid、Groups、CapInh/Prm/Eff/Bnd/Amb 和 NoNewPrivs。进程名在 exec 提交时取主程序末尾组件，长度上限 15 字节。初始 session ID 为 PID 1，子进程继承；tty_nr/tpgid 来自线程组真实ctty与稳定前台PGID；无ctty时为0/-1，不能由普通console fd猜测；OTHER 的 priority=20，FIFO/RR 的 priority=-(rt_priority+1)，nice 固定为 0；尚未实现 nice 调整。Uid/Gid 的 real/effective/saved/fs 四项、补充组和 capability 来自同一不可变凭据快照；文本生成完成后归还该引用。SigQ 使用目标真实 UID 的账户使用量。

stat 的 rsslim 为当前线程组 `RLIMIT_RSS` 软值（该限额记录不驱逐驻留页）；start_code/end_code 是主 ELF 的可执行 PT_LOAD 中最小虚拟地址和最大 `vaddr+filesz` 加 load bias，start_stack 是初始用户 SP。这些值在新 MM 构建时保存，fork 复制，失败 exec 保留原 MM；动态解释器不替代主 ELF，后续映射编辑不改初始边界。进程 stat 的 signal/blocked/ignored/caught 是组长的低 31 位信号状态，线程 stat 则取指定 TID 的状态，CPU 为 0，进程 exit_signal 为当前支持的 SIGCHLD。kstkesp/kstkeip 不暴露寄存器，wchan 按单线程任务是否不在运行/就绪状态（包括 zombie）输出 0/1，不暴露内核地址；nswap/cnswap 为 Linux 废弃的 0。字段依据固定 Linux `references/linux/fs/proc/array.c` 与 `fs/binfmt_elf.c`（`f4cdf7ca9a1fdcca413157df19753f388a5a224e`）。`tests/riscv/sched-bandwidth.sh` 检查策略改变后的 stat、代码/初始栈、fork 继承和失败 exec 保持；`tests/diff-abi/sched_stat.c` 对照真实FIFO/RR字段及STOPPED/zombie wchan。

stat 的 CPU/starttime 单位是 100 Hz scheduler ticks；utime/stime 汇总存活线程及已退出成员，cutime/cstime 汇总 wait 已回收子进程。minflt/majflt 在共用 fault 完成出口按当前任务自身用户 MM 的成功解析记账（含 tmpfs 共享首次缺页和私有 COW）；重试、失败、内核受控任务及其他 MM 的服务复制不重复或代记：如果该解析在当前任务上发布了 VirtIO 读请求，计为 major，否则计为 minor；已回收子进程同样卷入 cminflt/cmajflt。并发等待另一任务发起的缓存装载尚不能准确归为 major，这是当前的分类限制。VmSize 是 VMA 覆盖字节之和，编辑提交时维护；stat/status 在捕获任务元数据的同一 wait 域取得目标 MM 的独立引用，随后统计使用这份引用，避免 exec 或退出替换任务句柄时混读另一 MM。VmRSS 是 MM 中有效和 PROT_NONE 已拥有 PTE 的页数，单位页。退出线程在已发布 TID 真正回收前仍可查询，`EXITED`/`GROUP_DEAD` 不提前隐藏数字目录；NPROC 归还与线程移出组目录或撤身份在同一调度域边界完成。Zombie 保留统计与 `Z` 状态，MM 释放后的 VmSize/VmRSS 为零；wait 回收后，已打开的旧进程目录继续查找返回固定 Linux 的 `ESRCH`。数字目录新查找则返回 `ENOENT`。

PID 数字目录的 inode 编入单调分配代次；进程退出后旧目录对象不能通过复用的 PID 转向新任务。`/proc/self` 的链接文本在每次调用时读取当前任务 TGID，`/proc/thread-self` 返回当前调用者的 `TGID/task/TID`，不缓存首个访问者的目标。`exe` 来自主程序 MM 单独持有的 OFD 引用，动态解释器不取代它；fork 的子 MM 取得独立引用，共享 MM 则共享原 owner。`cwd/root` 来自目标任务当前 fs context。VFS 跟随这三个链接时直接取得活路径引用，避免对展示字符串重新解析；readlink 仅生成展示文本，已删除对象标注 `(deleted)`。任务查询在短 wait 域内取得元数据快照或路径引用；任务存储借用跨越域外检查，不钉住任务栈。生成文本和对象链接的聚焦对照为 `make test-diff-abi-riscv`；未实现链接不返回伪造内容。

`/proc/<tgid>/task` 枚举属于该线程组的可见 TID，目录继续遍历以 TID 为 cookie；每项沿同一 PID 位图及哈希取得 TID 角色，在 wait 域内检查所属线程组及其代次。组长先退出时，可见代表只沿该组的成员游标寻找，不扫描其他任务；数字 cookie、inode 代次和用户复制提交后才推进目录位置的规则保持一致。`task/<tid>/stat` 与 `status` 的 inode 绑定 TID 自身的身份代次。查找、打开和快照重新检查身份，退出或 TID 复用后，旧节点不能读取新线程；已生成的 OFD 文本仍遵循普通快照及 rewind 规则。失效的线程 status/stat 首次读取返回 `ESRCH`，旧线程目录下继续查找返回 `ENOENT`；进程目录的旧身份查找仍返回 `ESRCH`。组长先退出时，组员目录仍可查询。非组长 exec 释放原 TID 并承接组长身份，所以旧成员节点失效，已打开的 TGID 目录及组长身份节点继续指向 exec 后的进程。

线程目录中的 `fd/fdinfo`、`exe/cwd/root` 和 `maps` 也按 TID 查找其真实 files、fs 和 MM，权限按该线程的凭据及执行安全状态检查。取得路径、OFD 或 MM 后再次核对目标状态；线程退出或身份代次变化后，旧节点不会转向新任务。FD 目录 cookie 与进程目录一致。当前线程 clone 入口要求共享 files 和 fs，但 proc 查询不依赖这一限制。

进程与线程使用同一快照字段和格式化路径。线程的 state、comm、starttime、调度策略、优先级、信号 mask 及 utime/stime/minflt/majflt 来自指定 TID，usage 只统计该线程；Threads、子进程统计、会话及终端仍取线程组，MM 字段取目标线程当前共享 MM。线程 stat 的 wchan 不应用进程的多线程强制零规则，exit_signal 为 -1。status 的 Pid 为 TID、Tgid 为组身份；SigPnd 为线程私有 pending，ShdPnd 为组共享 pending，五个信号位图均输出完整 64 位。SigIgn/SigCgt 在捕获其他信号字段的同一 wait 域内从动作表计算，与远端修改信号动作共用保护；文本格式化在域外执行。所有 proc 后端调用仍在 CPU0 内核服务上下文中执行，任务存储借用覆盖查询，MM 句柄只在这次调用中使用，不保存裸任务或 MM 指针。

同一 RV ELF 还覆盖已打开旧进程目录在 wait 回收及后续 fork 后仍不指向新进程、执行文件 unlink 后 readlink 标注删除但链接仍可打开原 ELF，以及 meminfo 用户缓冲跨页 fault 后只推进已复制前缀并可继续读取。新增线程组用例在成员 clear-TID 后读取线程数，组长退出但成员存活时读取 status 和 exe/cwd/fd 链接，并验证退出前已打开的 status 在退出后首次读取；非组长 exec 后重新查询 PID、线程数和主执行文件。聚焦线程查询使用 `tests/diff-abi/proc-cases.txt` 清单及 `build/diff-abi/proc-rv`（LoongArch 为 `build/loongarch/proc-la`）。pipe 读取与状态 S 确认阻塞，检查独立 RR 优先级、线程及共享 pending、组长退出、旧节点与替换线程以及非组长 exec 后的身份变化。`proc.fd-reuse-stress` 以同步握手重复 256 轮普通文件/pipe fd 关闭复用、目录枚举、readlink 与重新打开，验证保留引用仍读取旧对象、重开形成独立 OFD；线程 clear-TID 后再回收。它不要求并发枚举具有额外快照一致性。根目录与 task 目录还检查数字项 cookie 的 seek/继续遍历，以及 EFAULT 后再次读取仍取得同一项。`make test-proc-index-host` 直接调用生产 proc/PID 查询，检查有洞、预留和残留身份、组长退出、祖先检查后的身份转移及借用归还，并通过双线程握手检查远端重置信号动作前捕获的快照。

敏感 `maps` 读取及 `exe/cwd/root/fd` 链接使用与 get_robust_list 共用的跨任务访问判断：同组允许；其他调用者按 fs IDs（get_robust_list 使用 real IDs）、目标三组身份、dumpable 和 capability 集合检查。当前调用者使用实际临时凭据，因此 access 按 real IDs 构造的视图也覆盖 proc 链接跟随；目标始终读取自己的已发布凭据。取得路径或 OFD 前保存目标凭据、MM 身份和执行安全对象引用，取得实际引用后重新核对，避免把 exec 前的权限用于新映像资源。proc 节点的属主从目标 effective IDs 动态取得；不可转储映像的非 0555 目录及文件使用 root 属主。FD 目录的共同 DAC 失败后，同线程组仍可访问，保留 Linux 的自查规则；这一许可不改变 stat 展示的属主。

`fd/` 按活文件表枚举 0–1023，查找和链接读取重新核对 PID 代次及当前槽；关闭后返回 `ENOENT`，复用同一编号时按新槽解析。动态 inode 为 PID 保留完整 16 位（当前 PID 上限 32768）、fd 保留 10 位，另编入进程代次和节点种类。目录项回调以正值报告成功、零报告 EOF，`getdents64` 才会真正递进；曾经错误地以零报告成功，导致 `ps` 只打印表头。普通文件链接钉住目标路径对象，重新打开创建独立 OFD 与 offset。pipe/socket 的展示编号来自对象创建时的单调身份，不泄露内核地址；pipe 链接可按只读、只写或读写重新打开，建立新的 endpoint owner，旧 fd 关闭不拆除新端点。`O_RDWR` 端点分别计入 reader/writer，并用独立聚合等待队列支持 poll。socket 和 epoll 链接可 `readlink`，重新打开返回固定 Linux 的 `ENXIO`。`lstat` 的链接权限位按当前 OFD 读写能力重新计算；跟随式 `newfstatat` 对伪对象在短关中断区内取得与目标 `fstat` 相同的元数据快照，不通过展示文本查找。PID 1 启动时若根盘已有真实 5:1 `/dev/console`，标准 fd 持有该 VFS 路径并可展示；缺失节点时仍使用无路径 UART 兜底，后者没有路径链接。

`/proc/mounts` 的链接文本为 `self/mounts`。进程或线程 `mounts` 的第一次非空读取按目标 PID/TID 的当前挂载命名空间和根目录捕获挂载树，并分别钉住挂载根与被覆盖路径；随后无锁格式化并由 OFD 持有本次文本。根 ext4 行的来源名为内部 `rootfs`，只表达本内核的根盘身份，不伪称 `/dev` 节点；proc 行的来源和类型均为 `proc`。输出包含实际挂载点、后端类型、实例的 `ro/rw`、实际 `nosuid/nodev/noexec` 状态和 `0 0` 字段，路径中的空白、反斜杠及 `#` 用八进制转义。卸载期间的引用会使普通卸载返回 `EBUSY`，快照交付后立即解除临时引用。`make test-diff-abi-riscv` 在固定 Linux 对照链接文本、打开能力和真实 proc 行；嵌套挂载与并发卸载还需要专门的真实用户态用例。

默认关闭的成本诊断构建另外提供完整命令控制与冻结快照，具体 owner、窗口和失败契约见
[成本观测模块](kernel-cost.md)。普通构建的目录 cookie、节点集合和生成文件语义保持一致。


COST 构建的 `/proc/boaros_net_stats` 保留原 16 个字段，并追加 segment、pbuf、
pbuf pool 与 PCB 的实际高水位；包计数改用 32 位，release 仍保持 lwIP 默认宽度。
缺失旧字段的消费者必须保留未知值，不把旧 16 位计数差当作无界窗口内的准确增量。
同一诊断构建另有 `/proc/boaros_mem_stats`，输出 managed/heap 的 current/peak 字节数。
managed 峰值由分配器在 bootstrap、finalize 与 buddy 成功分配时维护，释放不降低，
从本分配器初始化持续到快照；包含受管内核运行页、缓存和用户页，排除不在受管池中的
内核映像、固件和 MMIO。heap 是其中的子集，两种峰值不可相加，也不是 workload
窗口专属峰值。读取在分配自身快照缓冲之前捕获计数，字段仅在 COST 构建出现；
关闭 COST 不增加该峰值字段或更新代码。`meminfo` 继续只表示当前时点状态。

上述 heap 指打开该 proc OFD 所属的 heap，本轮为 root heap；网卡 final 行稍后记录的 root-heap-peak 还可能包含 COST 文本格式化和结果文件持久化，不能把不同采样时点当成同一峰值。静态 lwIP 协议堆/pool 数组在内核映像中，另由 ELF 符号预算记录，不能因 managed 峰值相近就宣称候选内存成本相同。完整 scope 与诊断扰动见[数据路径报告](../learning/data-path-budget-experiments.md)。

进程 fdinfo 目录按同一 FD 表枚举并输出 position、flags、mount ID 和 inode。目录及条目同时检查 FSCREDS 进程访问权限和文件 DAC；读取期间持有目标实际 OFD，再复核目标身份与执行安全状态。pidfd 增加 Pid/NSpid，单命名空间中两者相同，目标 TID 已撤下后均为 -1；固定 Linux 的 NSpid 输出还取决于 CONFIG_PID_NS。pidfd 的 FD 链接展示 anon_inode:[pidfd]，getfd 传递实际 OFD，proc 路径重新打开仍返回 ENXIO。文本沿共同生成文件规则，rewind 后重新取值。

`/proc/<pid>/ns/{mnt,uts,ipc,pid,pid_for_children}` 与线程目录的对应链接使用 FSCREDS 进程对象权限
检查，`readlink` 分别显示稳定的 `mnt:[id]`、`uts:[id]`、`ipc:[id]`。普通打开与
`/proc/self/fd` 重新打开使用共同 namespace FD 实现：inode 对应 namespace ID，
`fstatfs` 报告 NSFS，`NS_GET_NSTYPE` 返回实际 CLONE_NEWNS、CLONE_NEWUTS 或
CLONE_NEWIPC。捕获和复核覆盖 PID 代次、目标资源存续、exec/MM、凭据与 dumpable；
线程路径取得指定 TID 的对象。持有 FD 会保留真实 namespace，目标退出不会撤销它。
pidfd setns 的多类型查询在同一目标资源保护中取得 MNT/UTS/IPC 引用，按 REALCREDS
检查权限；proc 链接继续按 FSCREDS 检查。挂载路径和引用规则见
[VFS 挂载说明](vfs-ext4.md)。

`NS_GET_OWNER_UID` 查询用户命名空间的所有者用户标识。对当前导出的非用户命名空间描述符，内核在访问用户输出地址前返回 `EINVAL`。以 `O_PATH` 打开的描述符仍返回 `EBADF`。

`/proc/<pid>/mountinfo`、线程目录的对应文件及self/thread-self链接展示目标的实际挂载命名空间。每行包含挂载ID、父ID、与fstat一致的文件系统设备号、文件系统内的bind根、目标进程根下的挂载点、视图属性及后端属性；可选字段shared/master/propagate_from/unbindable来自真实传播关系。chroot所在挂载使用其根路径生成挂载点，因此可显示为`/`；根外的挂载按Linux规则不输出。

mounts与mountinfo共用一次拓扑快照，捕获时分别持有进程根和命名空间top的真实路径引用；pivot_root更新top后，已有快照仍保留自己的旧引用。挂载点及文件系统根名称在同一拓扑下捕获，转义、结果格式化和用户复制在保护外完成。设备身份与挂载ID分别维护，多个bind视图保持相同st_dev。读取沿已有生成文件规则，首次读取及rewind后重新捕获；关闭快照归还全部引用。当前根路径随快照取得，尚未按Linux在FD打开时固定根路径，因此跨pivot/chroot后查询新根应使用新打开的FD；这一打开时根捕获语义仍是内核缺口。直接查询及生命周期由挂载传播消费者检查。

## BPF 状态

`/proc/filesystems` 包含实际可挂载的 bpf、tracefs 和 sysfs。BPF/perf FD 的链接名与 fdinfo 来自对象快照，程序、map、BTF 和 link ID 对应仍存活的对象；共享 map 的 maps 行使用其 OFD 来源。`/proc/sys/kernel/unprivileged_bpf_disabled` 提供实际权限策略，取值与写入规则见 [BPF 模块](kernel-bpf.md#对象接口与权限)。

`fsopen`与`fspick`的配置FD在fd目录显示为`anon_inode:[fscontext]`；fstat采用共享匿名inode的模式，fstatfs报告匿名inode文件系统。它不提供VFS路径，不能通过该FD绕过路径解析取得挂载树。挂载视图FD与命名空间FD仍沿各自真实对象展示，生命周期见[VFS挂载上下文](vfs-ext4.md#挂载树-fd-与根切换)。
