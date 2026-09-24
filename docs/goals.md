# 目标、TODO 与阶段依赖

目标是在 RISC-V64、LoongArch64 的已声明平台上运行未经 BoarOS 特改的 Linux 用户程序，分别验证 ABI、资源生命周期、并发、实板与性能。Linux 是用户可观察契约和机制参考，不要求复制其全部内部数据结构。当前能力摘要见 [README](../README.md)，稳定契约见[模块文档](README.md)；本文集中维护任务、依赖、验收和未决取舍。

本轮输入为 2026-09-16 独立评审、用户补充的逐模块意见，以及本地基线 `7971eedbd3ca879f6528082d9e023ac42fadd417`。2026-09-17 将任务细化如下。评审建议不自动成为已批准设计；不纳入不在本地的参赛作品信息。固定 Linux 为 `references/linux` 的 `f4cdf7ca9a1fdcca413157df19753f388a5a224e`，其他资料由 `references/sources.tsv` 唯一记录。实施前先读对应本地版本，资料不足再查官方来源并记录访问日期。

## 状态、执行顺序与证据

`[x]` 表示该条具体交付已有验证，不表示整个阶段完成；`[ ]` 表示尚未完成。下文新增文件/测试均标为“拟新增”，名称可以随已确认方案调整，不预建空目录、空接口或成功存根。源码入口用于定位，不要求一次修改整组文件。

每个可提交任务按“触发条件与契约 → 真实 owner → 最小失败输入 → 修复 → 正常/失败/回收证据”执行。架构、ABI、所有权、依赖或结构性性能取舍先比较真实候选，由维护者确认；已确认范围内的小修继续完成。本批用户已授权按块同步→逐 inode 写回→journal/replay→cwd/dirfd/rename→时间与统计的顺序逐阶段实施；push、发布与比赛提交仍由维护者决定。

### 已落地的首批工作

- [x] **P0a 事实与文档归位**：核对 228 项基线、七类直接失败；修正文档中目录 fd、全 LRU 失效成本、块层无 flush 的边界。README 保留摘要，modules 保留契约，learning 保留证据与根因，TODO 集中于本文。
- [x] **P1a root 身份查询**：`getuid/geteuid/getgid/getegid` 符合现有不可变 root 模型；四项单测先失败，同一 ELF 差分先观察 ENOSYS，修复后全部 185 条一致，含残留参数和 fork/exec。凭据变更/权限检查仍属于 P2e。
- [x] **P0b 首轮时序复跑**：三轮静态/动态 `pthread_cancel_points` 双侧通过；BusyBox 后台 sleep+kill 每轮失败，日志与固定 ash 源码确认 `/dev/null` 是后台子进程前置依赖。证据见 `build/review-timing-{1,2,3}/`，未关闭旧取消异常。

本批最终证据：`build/recoverable-fs-host-final.log`（含 journal/orphan/rename 断电与 metadata 故障）、`build/recoverable-fs-regression-final.log`（RISC-V 全套、真实 musl/pthread、320 条差分、1000 个函数栈界、工具自测）、`build/recoverable-fs-full/runs/suite.json`（228 项实际重跑，无旧通过项回退）。恢复承诺仍限于已验证块模型。

robust-list 阶段证据：`build/robust-focused-first/`（静态/动态真实 entry）、`build/diff-abi/run/`（启用 futex 的固定 Linux 与 BoarOS 同一 ELF 329 条一致）、`build/robust-riscv-stack.log`（RISC-V 全套与栈检查）、`build/robust-full-20260923/runs/suite.json`（223 pass/2 direct/3 wrapper，无旧通过项回退）。`build/cancel-repeat-20260923/` 记录静态/动态取消 entry 各 30 次双侧通过，历史异常根因仍未确定。

P4a 共享匿名阶段证据：`build/p4a-vma-final.log`（含 fork 当下的受保护页、真正 fixed 覆盖、OOM 与回收）、`build/p4a-userland-final.log`（真实 musl）、`build/p4a-diff-final.log`（固定 Linux 同一 ELF 330 条一致）、`build/p4a-riscv-final.log` 与 `build/p4a-stack-final.log`（RISC-V 全套与栈检查）、`build/p4a-full-20260925/runs/suite.json`（228 项 223/2/3，逐项状态与 robust 阶段相同）。

### 后续推进顺序

| 顺序 | 任务 | 开始条件 / 独立成果 |
|---|---|---|
| 已完成 | P1d cwd/dirfd、P1g rename 子集 | 共享活目录项、cwd/dirfd、普通/NOREPLACE rename 已接入日志与 orphan |
| 已完成 | P1e 时间、P1f 统计 | utimensat/futimens、真实 statfs、原始静态/动态 entry 与 BusyBox pwd/cd/mv/touch 已通过 |
| 已完成 | P2a 同 MM 非 PI robust-list | raw `exit`、exec、musl、差分及全量清单已验收；PI 与跨 MM key 仍后置 |
| 已完成 | P4a 共享匿名对象 | 专用稀疏对象、fork 双向可见、失败回滚和固定 Linux 差分已验证；不包括共享文件页或跨 MM futex |
| 下一阶段 | P4d 跨 MM futex 设计；P0c 取消异常调查持续 | 历史取消异常本轮 30 轮/版本未复现，不能伪称已找到根因 |
| 持续支线 | P5a glibc 试跑、L0/L1 第二架构入口 | 可现在固定输入或调查边界，不以全量清单全绿为前提 |
| 后续 | P3c/e 记录锁与 SQLite、N socket、P4 文件共享、P6 SMP | 按下述具体依赖进入，不按测试名称排接口 |

### 主要依赖

```text
P0 证据 ─┬─ P1 路径/文件/设备 ─┬─ P3 同步/锁/SQLite 回滚日志
         │                     └─ N socket/loopback/设备网络
         ├─ P2 robust/信号/线程/资源
         └─ L LoongArch 最小入口 → 双架构与实板

P4a 共享匿名对象已完成，提供稳定后备身份
P1 文件身份 + P3 写回/错误协议 → P4b/c 共享文件页与 msync
P4 后备身份 + P2 等待协议 → P4d 跨 MM futex → SQLite 多进程 WAL
P1–P4 按真实依赖 → P5 glibc/单核编译完整交付（试跑可提前）
P2/P4 单 hart 契约 + IPI/IRQ 入口 → P6 SMP 正确性
P5 + P6 → P7 多核编译与性能；P7 + N + L → P8 平台交付
```

箭头表示主要验收依赖，不禁止先做块 flush、试跑 SQLite/编译器或启动第二架构。网络不依赖多核编译；匿名共享不必等文件持久化；2 hart 启动不等于 SMP 已完成。

## P0：固定证据与时序问题

**入口**：`tests/program-inventory/{inputs.json,run.py,suites.py,reports.py}`、`tests/diff-abi/`、`.github/workflows/ci.yml`、[程序清单](learning/user-program-inventory.md)。2026-09-23 最近 228 项记录为 223 一致、2 个直接 entry 退出不符、3 个包装失败；静态/动态和脚本重叠不重复计算缺陷。

### P0c 时序根因闭环

- [ ] 固定 kernel/ELF/loader/fixture/runner/QEMU 身份与次数，分别复跑静态、动态取消 entry 和原脚本；保存双侧 stdout/stderr、真实 wait status、超时和第一个失败。先区分 reference-not-pass、setup-error、脚本顺序依赖。
- [x] 在固定构建输入上重复运行静态/动态取消直接 entry 各 30 次：Linux 与 BoarOS 均 30/30 通过，原始逐次证据和身份在 `build/cancel-repeat-20260923/`；未复现不等于旧异常已定位，原脚本复跑和最小因果序列继续保留在上条待办。
- [ ] 取消异常缩成可独立运行的 shm_open、取消登记、阻塞、join/clear_tid 序列；确认取消点前后状态，不让错误诊断中的 write 再次隐藏原错误。用同步事件安排顺序，不靠随意 sleep 假定先后。
- [x] P1c 后以同步握手确认独立后台子进程进入目标阶段，再执行 kill/回收，固定重复 20 轮均成功；原 BusyBox 包装器中的 sleep+kill 子项也为 20/20，包装器其他缺口继续单列，未据此关闭历史取消异常。
- [ ] 若定位到支持范围内的错误，先加入能证伪旧实现的最小回归，再修所属模块；记录失败前和修复后的重复次数。一次转绿不能关闭未知根因。

### P0d 持续证据与清单维护

- [x] 本轮证据保留工作树内核、ELF/loader、fixture、runner、QEMU 与固定 Linux/BusyBox/libc-test 身份；旧基线只保留历史定位用途。
- [x] 原七类直接失败按设备/身份、cwd、时间设置、文件系统统计、robust、socket 聚类；设备、cwd、显式时间、统计和 robust 已关闭；直接 entry 只剩 socket，保留原始日志与最小复现归属，不按非零退出码猜 syscall。
- [x] 已有硬回归严格通过；能力清单如实保存缺口。全量严格验收复用 `--require-pass`，不重建状态系统或跳过失败；相关静态/动态 entry、原包装器与 228 项清单均已重跑。
- [x] 限定集合的 `--require-pass` 只严格判定本次 selection，未选项目保持历史状态或 `not-run`；未知 ID、所选失败/未完成、中断、参考侧失败与全量严格模式均有 runner 回归。

**验收**：每个新增任务都有可证伪输入、有效 Linux 参考和归属机制；历史原始失败保留，未运行项目不计通过。不要求 P0 达到 228 项全绿。

## P1：路径、设备与文件元数据

**入口**：[文件模块](modules/kernel-files.md)、[VFS/ext4](modules/vfs-ext4.md)；`include/kernel/{vfs.h,open_file.h,fs_context.h}`、`fs/{vfs.c,open_file.c,fs_context.c}`、`fs/files/{path.c,metadata.c,io.c,table.c,poll.c,epoll.c}`、`kernel/syscall/{file.c,dispatch.c}`。

### P1b 最小对象与操作边界（最小 mount/路径对象路线已完成）

- [x] 文件身份使用 mount+inode，路径对象另持父目录项；OFD 持共享 flags/offset/引用，fd 槽持访问与 CLOEXEC。open→pin/dup/fork→close→末引用回收均有聚焦回归。
- [x] ext4 适配器提供按目录 inode 查找、按 inode 打开和读链接；字符节点按 `rdev` 选择后端，lwext4 类型未泄露到 task/syscall。普通文件、pipe、epoll 保留统一分派下的各自语义。
- [x] root/cwd、普通文件、目录和字符节点均持真实引用；unlink/rmdir 后旧对象存活，同名重建获得新 inode，外部引用使卸载返回忙。rename 子集已由 P1g 交付。
- [x] 发布流程按路径→后端→OFD→fd 逐级取得 owner；分配/打开/fd 满与 close/dup/fork/阻塞 pin 有失败和回收检查。ext4 close 失败转交 mount cleanup node，非法释放继续 fatal。

**交付**：一个可复用的 ext4/设备纵向入口及所有权说明；拟新增 `fs/path_walk.c`、`fs/mount.c` 等仅在职责确定时拆分，不要求一次重做 Linux dcache/RCU。

### P1c 真实设备文件

- [x] 统一路径/OFD 后端可打开 `/dev/null`、`/dev/zero`、`/dev/console`；身份来自 ext4 字符节点与 `rdev`，未知设备号返回 `ENXIO`，初始标准 fd 复用 console 实现。
- [x] null/zero/console 的读写、非阻塞、信号打断、零长度、坏指针、跨页部分 fault、向量与定位 I/O 均由真实 U-mode 或固定 Linux 差分验证。
- [x] 设备 `st_rdev`、seek、未知 ioctl、poll/epoll、访问模式与 `O_TRUNC` 已核对；OFD pin、dup/fork/close/退出及分配失败回到资源基线。
- [x] 原静态/动态 `stat`、`syscall_sign_extend` 已通过；后台 sleep+kill 同步重复 20 轮成功。daemon 的 cwd 阻塞后由 P1d 解除，P1e/f 收口时静态/动态原始 entry 均已通过；完整 session/TTY 仍归 P2d。

### P1d cwd 与目录 fd

- [x] 实现目录 fd 起点及 `chdir/fchdir/getcwd`；无效 fd 与非目录 fd 分开，绝对路径忽略坏 dirfd，空路径/尾斜线/过长路径和缓冲不足按固定契约返回。
- [x] 按分量处理 `.`/`..`、相对和绝对 symlink、循环上限与 mount 边界；保留 `symlinkat/readlinkat/O_NOFOLLOW/lstat`。不能先把整个字符串折叠再跟随 symlink。
- [x] fork 拥有独立 fs context，CLONE_FS 共享同一上下文，exec 保留；切换 cwd 时先取得新引用再提交，失败保持旧 cwd，退出释放准确 owner。
- [x] 测试不同任务/线程的 cwd 可见性、相对 symlink+`..`、打开目录后 rename 再 openat、unlink 后仍持目录句柄/getcwd 的行为。rename 完成前先建立身份契约，相应组合用例在 P1g 一起关闭。

### P1e 显式时间设置

- [x] 在活 inode 上实现 `utimensat` 与 libc `futimens` 所需入口；复用已有 realtime、relatime 和纳秒编码，不建立旁路时间戳缓存。
- [x] 对照固定 Linux 的 NOW/OMIT、空 times、非法纳秒、两项 OMIT、路径/nofollow、fd/dirfd 和权限检查顺序；保留未修改字段，ctime 的变化也必须符合契约。
- [x] 覆盖只读挂载、坏指针/跨页导入、无效 fd、不存在路径及真实写失败；明确元数据已变更后 I/O 错误的归属，不伪造回滚或错误成功。
- [x] 受控时钟测试与真实 Linux 差分共同验证；重跑原 `utime` 静态/动态及 BusyBox touch，保持 create/read/write/truncate/unlink 时间回归。

### P1f 文件系统统计

- [x] 从挂载和真实 ext4 superblock/分配状态提供 `statfs/fstatfs`；核对目标架构结构、块大小、块/inode 总量与空闲量、名称限制和只读标志，不返回固定容量数字。
- [x] 以 mount 为统计 owner，fd 查询不依赖旧路径重查；定义与正在进行的分配/释放及失败 I/O 的一致性范围。
- [x] 对比文件分配/截断/删除前后计数，覆盖 sparse hole 不等于已分配块、只读、坏 fd/用户指针和卸载；重跑原 `statvfs` 两种 entry。

### P1g 链接、rename 与权限相关文件操作

- [ ] `linkat` 覆盖同 inode 身份、nlink、打开后删除、跨 mount EXDEV、目录限制与失败后原对象；符号链接跟随 flags 单独验证。
- [x] `renameat/renameat2` 支持普通/NOREPLACE；单事务文件/空目录覆盖、跨目录移动、祖先拒绝、同 inode、活目标及失败回滚，EXCHANGE/WHITEOUT 明确不支持。单根挂载的跨 mount 拒绝存在，真实多挂载验证归 P1h。
- [ ] 按真实消费者接 umask、chmod/faccess 等；明确 mask 共享/复制和凭据依赖，与 P2e 权限模型一致，不能总返回允许。已有 open 未知 bits 拒绝策略另用差分核对，不能写成 Linux 通用要求。

### P1h 虚拟文件系统与多挂载

- [ ] 先完成 devfs 或等价最小设备后端，再以 tmpfs/真实第二挂载验证 mount/路径身份；临时内存文件具有真实页/目录生命周期、空间耗尽和卸载回收。
- [ ] 最小 procfs 的 uptime、meminfo、self/fd 和进程状态直接读取内核对象；枚举/读期间持有所需引用，任务或 fd 消失的竞态按契约处理，未实现项不伪造 Linux 文本。
- [ ] 多挂载覆盖路径跨越、根和 `..`、挂载点被引用、卸载忙、跨挂载文件操作和失败回滚；设备/内存/磁盘文件各自错误保持所属 owner。
- [ ] eventfd/timerfd 只在真实消费者提出需求后接统一 OFD，就绪、非阻塞、poll/epoll 和退出回收一起验收；signalfd 另依赖 P2c 队列。

**验证与退出**：先扩展 `tests/riscv/files_main.c` 等现有聚焦入口；当前入口为 `tests/userland/{namespace.h,metadata.h}` 与 `tests/diff-abi/{devices,namespace,metadata}.c`，均接入现有 runner。`make test-files-riscv test-vfs-riscv test-lwext4-host` → `test-userland-riscv`/`test-diff-abi-riscv`。设备、cwd、时间、统计分别解除对应真实程序阻塞，旧 ext4/pipe/epoll/COW/回收不退步；daemon 新出现的 setsid 依赖交给 P2d。

## P2：线程、futex、信号与资源

**依赖与入口**：可与 P1 独立推进；[调度模块](modules/kernel-scheduler.md)、[信号模块](modules/kernel-signal.md)、[时间模块](modules/kernel-time.md)；`kernel/sched/{process.c,futex.c,signal.c,wait.c,private.h}`、`kernel/syscall/{process.c,signal.c,time.c}`、`arch/riscv/signal.c`、`include/kernel/task.h`。不重写已有 pthread/普通 futex。

### P2a robust-list 退出协议（同步所有权路线已完成）

- [x] 增加 `set_robust_list/get_robust_list` 和每线程注册信息；RV64 长度、存活目标 TID、当前 root 凭据、输出故障顺序、坏地址注册与 fork/exec 生命周期有真实 U-mode 和固定 Linux 差分。
- [x] 同步清理发生在退出线程旧 MM 与原 TID 有效时；有界处理链、signed offset、pending、owner-died 和唤醒。普通退出、致命信号、exit_group 和 exec 清退成员共用退出调用链；存活 exec 线程使用换号前 TID。
- [x] 坏指针、循环链、非 owner、PI 标记、COW 与 OOM 的用户访问失败均有真实路径验证；2048 项上界和先读下一链接防止无限遍历及已释放页访问。robust 清理先于 clear_child_tid 和 MM 释放。
- [x] 未特改 musl 静态/动态 `pthread_robust_detach`、真实 pthread owner-died/consistent/不可恢复状态与原始 `SYS_exit` 探针均通过；PI 与跨 MM 共享语义不计入 P2a。

### P2b 等待、取消与 futex 扩展

- [ ] 写出比较、登记、睡眠、超时、signal、wake、requeue、clear_tid、组终止的状态转换与唯一队列 owner；保证 compare-and-block 不可分割，无漏唤醒/重复摘链。
- [ ] 按实际调用补 `WAIT_BITSET/WAKE_BITSET`、`CMP_REQUEUE/WAKE_OP`；每个 operation 单独核对参数宽度、bitset、比较失败、relative/absolute 和 CLOCK_REALTIME，未知/未支持操作不算完成。PI futex 后置。
- [ ] 保持无超时 WAIT 的 SA_RESTART、带超时 WAIT 的 EINTR 与无 handler restart 保持原 deadline；测试信号在登记前后到达、超时与 wake 交错、重启前用户字变化、哈希碰撞和同桶 requeue。
- [ ] 取消/exec/exit_group 让阻塞线程沿原栈释放 pin 的 OFD、等待节点及 MM 引用；不能直接删除仍执行的内核栈。跨 MM key 留给 P4d，单 hart 关中断仅是当前实现条件。

### P2c 替代栈与实时信号

- [ ] `sigaltstack` 覆盖配置/查询、边界和禁用、嵌套 handler、主栈耗尽后的执行、sigreturn、普通 fork 继承、CLONE_VM 且无 CLONE_VFORK 时禁用、exec 重置；错误用户栈产生规定 fault，不破坏内核栈。
- [ ] 实时信号用可拥有的队列项表示，与标准信号合并位分开；验证重复排队、顺序、目标线程/组、屏蔽/等待/handler 消费，以及退出线程的队列回收；成功 exec 保留存活线程/组仍有效的 pending，不把 handler 重置当成 pending 清空。
- [ ] 队列 OOM、用户复制失败和取消时明确是否已消费；在真实队列计费后实现 `RLIMIT_SIGPENDING`。signalfd 等待队列稳定且有 OFD 后端后再接。
- [ ] 保留已有 `rt_sigtimedwait`、stop/continue 和各 syscall 不同的 EINTR/SA_RESTART；对嵌套、超时、信号排队与线程退出运行固定重复回归。

替代栈与 exec 生命周期依据本页固定 Linux 的 `kernel/fork.c`、`fs/exec.c`，pending 保留同时与现有 [信号模块](modules/kernel-signal.md) 契约对齐。

### P2d 会话、进程组与 TTY

- [ ] 设计 session/pgrp 与线程组的身份及引用；实现 setsid/setpgid 和必要查询，覆盖组长限制、父子/exec 竞态、非法目标、组信号与等待状态。setsid 不得仅返回 getpid。
- [ ] 验证 daemon 的 cwd/device 解除后真实会话变化、孤儿组/退出传播；SIGHUP 等行为按固定 Linux 的具体触发条件实现，不因为进程退出就无条件广播。
- [ ] TTY 对象出现后接控制终端、前台组、作业控制与终端信号；无 TTY 阶段不能宣称交互 shell 作业控制完整。对象末引用与关闭唤醒分别验收。

### P2e clone、凭据与资源限制

- [ ] 逐项扩大合法 clone 组合，区分 flags 依赖错误与当前未支持组合；核对 MM/files/fs/disposition 各自复制/共享，TLS、TID 发布失败、组长先退、非组长 exec、收养和 wait 仍正确。
- [ ] 在 P1a 查询基础上设计显式凭据：真实/有效/保存 UID/GID、补充组及权限消费者；确认复制/共享/变更/exec 所有权后再支持 set 类接口。统一替换当前 root 假设，不出现查询身份与资源访问权限脱节。
- [ ] 保持 NOFILE/STACK：调低不关闭旧 fd、线程组共享、fork 继承、exec 保留、exec 后调高栈和非页对齐限制；用户输出 EFAULT 不回滚已生效的 prlimit 设置。
- [ ] 按需求增加 AS/DATA/FSIZE/CPU，分别说明记账 owner、生效点、越限 errno/信号和恢复；AS 是虚拟区间而非驻留页，VMA 合并/切分/mmap/brk/mremap 与 lazy allocation 保持一致。
- [ ] 先核对固定 Linux 对 RSS/LOCKS 的非执行语义、NPROC 的真实 UID 和特权例外，再决定支持范围；不机械地对每个历史枚举加限额，也不把有效但未支持资源成功空返回。

**验证与退出**：robust 已由 `tests/userland/pthread.c` 的原始退出/真实 mutex 消费者、`tests/riscv/uaccess_oom.S`、`tests/diff-abi/robust.c` 和静态/动态原 entry 验收；聚焦 `make test-scheduler-cases-riscv test-signal-riscv test-syscall-riscv`、`test-userland-riscv`、329 条差分、`test-riscv` 与栈检查均通过。拟新增 `tests/userland/signal_stress.c`、`tests/diff-abi/futex_signal.c` 仍属后续；旧取消异常继续 P0c，不以全部当前 pthread 通过冒充共享 futex 或 SMP 完成。

## P3：同步持久化、文件锁与 SQLite

**依赖与入口**：文件/目录同步依赖稳定 VFS/OFD，块层可先做；`include/kernel/block.h`、`kernel/block.c`、`arch/riscv/virtio_mmio_block.c`、`fs/{vfs.c,page_cache.c}`、`fs/files/io.c`、`kernel/syscall/file.c`、`fs/lwext4_config/generated/ext4_config.h`。涉及 lwext4 变更时维护来源、许可证与本地 patch 记录。

### P3a 块设备 flush（已完成）

- [x] 块层已有缓存模式与 flush 回调；VirtIO legacy/modern 在 writeback/writethrough 四种组合通过真实请求测试。`make test-block-host` 验证能力拒绝、错误传播和可复用的易失缓存/稳定镜像故障模型；journal 与文件系统恢复证据另见 P3d。

- [x] 对照固定设备资料定义能力描述、缓存属性和 flush 请求：协商支持、提交、完成、I/O 错误、超时和只读边界；两种 VirtIO MMIO transport 都验证，不能把内存 fence 当介质同步。
- [x] 定义请求/描述符/bounce buffer 的 owner；超时/reset 后必须确认 DMA 不再访问，才能释放或复用请求内存。不可恢复设备错误由所属设备/mount 保留明确状态。
- [x] 无 flush 能力时依据设备真实缓存契约返回结果；不能忽略能力并承诺不可满足的持久性。测试正常完成、拒绝/失败、超时与 reset 回收。

### P3b fsync、fdatasync 与目录同步（已选逐 inode 路线）

- [x] 明确调用覆盖的文件数据、必要元数据、目录项和设备缓存；从 syscall/OFD 接到 ext4 提交与块 flush，顺序和成功承诺可解释。目录 fsync 单独验收。
- [x] 已选择并实现 inode 缓存页索引、脏范围/修改代次、定向写回及 OFD 错误观察；独立 open 与 dup、关闭后脏 owner、O_SYNC/O_DSYNC、真实 musl 和 260 条 Linux 差分验证。journal 事务依赖已接入 P3d。
- [x] 定义延迟错误属于哪个 mount/文件，以及后续哪些调用观察它；用户错误、真实写/flush 错误和内核不变量分开。关闭先摘 fd，不让历史 cleanup 改变当前有效 close 结果。
- [x] 同步链建立后才接 `O_SYNC/O_DSYNC`；覆盖普通/部分写、metadata 必要性、无效 fd、只读/设备及重复同步。新进程/重启看到已承诺内容只是正常路径证据，不能单独证明崩溃一致性。

### P3c 记录锁

- [ ] 定义 fcntl 传统进程关联锁与 OFD 锁的 owner、范围、合并/拆分、查询和冲突；不能把已有 F_DUPFD/F_SETFL 支持当作记录锁。
- [ ] 按固定契约验证独立 open、dup、fork、exec、任意相关 fd close、线程组退出的保留/释放；尤其不能把进程锁简单挂成随单个 OFD 生灭。
- [ ] 冲突时真实阻塞或返回规定错误，信号打断、取消、等待者退出/OOM 都释放队列引用；两个进程争用同一文件必须观察到冲突，而非分别在私有锁表成功。

### P3d ext4 journal 与恢复（已选本批交付）

- [x] 已选择并启用 ordered journal/replay；事务 before-image、flush 顺序、checksum v2/v3 和 revoke、superblock 恢复、持久 orphan_file/传统链及分批 extent/间接块回收已接入生产。
- [x] 元数据事务提交与 checkpoint 分阶段 flush；关键日志错误 sticky，损坏或只读无法恢复时拒绝开放用户访问。已知日志提交前的资源不足可安全回滚，不能据此清除不确定 I/O 错误。
- [x] `make test-lwext4-recovery-host` 使用 512 字节原子写、易失缓存/稳定镜像，覆盖写与 flush 失败、丢失/重排、两次恢复和 e2fsck；完整 orphan 回收矩阵为 16 种组合、4,362 次断电执行。QEMU 正常退出和未验收实板不属于该恢复证据。
- [x] 真实 musl 已验收“临时文件→fsync→rename→两侧父目录 fsync”应用序列，后端 rename 另有断电/重排矩阵；robust/socket、记录锁、SQLite、共享映射留后续。

### P3e SQLite 回滚日志负载

- [ ] 固定未特改 SQLite 版本、构建选项和输入；先回滚日志模式、`mmap_size=0`，记录它实际调用的 VFS 锁/同步接口，缺口回到上述机制。
- [ ] 建表、提交/回滚、多个连接、writer 冲突、关闭重开和 integrity_check 全部验证；保留数据库/日志、退出码及可重复命令，不只比较 SELECT 输出。
- [ ] 正常重开和故障恢复分别记录；成功同步后的约定数据在新进程/新启动可见，错误确实返回。多进程 WAL 留到 P4，不通过绕过共享内存需求冒充支持。

**验证与退出**：先 `test-block-riscv`/`test-lwext4-host`/`test-vfs-riscv`，再生产 U-mode 与 Linux 差分；拟新增块 flush 故障测试、`tests/userland/file_sync.c`、`tests/diff-abi/file_locks.c`、`tests/workloads/sqlite/`。退出时同步、锁、正常重开与恢复各自有结果，回滚日志可先单独交付。

## P4：共享后备对象、文件页与跨 MM futex

**依赖与入口**：[MM](modules/kernel-mm.md)、[VMA](modules/kernel-vma.md)、[VFS](modules/vfs-ext4.md)；`include/kernel/{mm.h,vma.h,file_mapping.h,shared_anon.h}`、`mm/{vma.c,shared_anon.c}`、`arch/riscv/mm.c`、`fs/{page_cache.c,vfs.c}`、`kernel/syscall/memory.c`、`kernel/sched/futex.c`。P4a 使用专用共享匿名对象；文件页及共享 futex 复用稳定身份所需的接口按各自契约扩展，不预建通用插件框架。

### P4a 共享匿名对象（已完成）

- [x] mmap 时建立可引用身份，即使尚无驻留物理页；fork 共享后备对象，按对象+页索引发布页。已驻留页保留共享 PTE 权限，私有页继续 COW。
- [x] VMA 借用对象并保存连续 offset，MM registry 持对象引用；对象页槽与每个 PTE 分别持物理页引用。切分、合并、fixed replace、munmap 和 fork 后身份与 offset 连续，末引用回收页槽。
- [x] fork 前已驻留、fork 后子先缺页、fork 后父先缺页均用同步握手验收双向可见；固定 Linux 差分加入同一 ELF 的三页共享案例。
- [x] 覆盖 PROT_NONE/恢复、部分 unmap、替换、父/子先退出与 fault/PTE 分配 OOM；中途失败回滚新页槽，引用计数回到基线。

### P4b 共享文件页与权威数据源

- [ ] 同挂载/inode/文件 offset 的独立 open/mmap 指向同一文件页；普通 read/write 与映射读写三向可见。普通 read/write/private fault 已统一缓存；仍需建立 MAP_SHARED 可写 PTE 的脏标记和回写协议。
- [ ] 定义 clean/dirty/writeback/error、写回中再次修改、pin/映射引用和回收资格；文件页 identity 不随 fd 关闭或路径删除而改变。
- [ ] 两个独立 MM、两个独立 open、多个 VA 别名组合验收；MAP_PRIVATE 写不得污染共享页，共享页修改及普通写的可见性按固定 Linux 声明范围核对。
- [ ] 写回失败由仍存活的文件/mount owner 保存并传播；页被映射/pin 时不能按普通无引用缓存驱逐。并发 miss/锁外 I/O 的跨核发布在 P6d 验收。

### P4c 截断与 msync

- [ ] 对跨 MM 的 EOF 越界整页撤映射并产生规定 fault；非对齐尾页、私有修改、先缩后扩、O_TRUNC 分开测试，保持已有 private COW/PROT_NONE 截断回归。
- [ ] 关闭 fd 后映射仍活、unlink 后仍能访问、退出/固定替换中途失败均持有正确文件/对象引用；不能借用已经回收的 OFD/node。
- [ ] `msync(MS_SYNC)` 走真实 dirty→写回→同步与错误传播，不能只失效缓存；其他 flags/地址/区间边界按固定契约支持或明确拒绝。

### P4d 共享 futex

- [ ] private key 保留 MM+虚拟地址；shared key 从稳定后备身份+字偏移派生，不依赖可变化的物理页地址。处理合法对齐、坏页和不支持的后备类型。
- [ ] WAIT 登记、WAKE 和 REQUEUE 全过程持有所需对象引用；unmap/关闭 fd/最后映射消失与等待者退出不会悬空或重用旧 key。
- [ ] 同一对象不同 MM/VA 能正确唤醒；不同对象同 VA 不串扰；哈希冲突、同桶/跨桶 requeue、超时/信号与对象释放均验证。单 hart 先通过，跨核原子比较/登记属于 P6。

### P4e mremap、madvise 与 WAL

- [ ] 对象生命周期稳定后实现 mremap，覆盖移动/增长/收缩、旧/新地址失败原子性、共享身份和 AS/DATA 记账；不复制成意外私有对象。
- [ ] madvise 按真实消费者逐项增加；DONTNEED 区分 private/shared/file，已丢弃的私有内容不能再次读出，错误不能成功空返回。
- [ ] SQLite 普通多进程 WAL 验证共享索引、锁、并发连接、异常终止和恢复；保留 P3 回滚日志基线，失败精确分到共享页/锁/同步。

**验证与退出**：P4a 已由 `test-vma-riscv`、真实 U-mode 的 `tests/userland/shared_mapping.h`、`tests/diff-abi/shared_mapping.c` 验证，最终完整回归结果见提交记录。P4d 的共享 futex 聚焦测试待新增；共享文件页、共享 futex 分别收口，不合成一个笼统的“MAP_SHARED 已支持”。

## P5：glibc、exec 与单核真实工具链

**入口**：[ELF](modules/user-elf.md)、[exec](modules/kernel-exec.md)、[程序环境](modules/program-environment.md)；`kernel/{elf64.c,elf64_source.c,exec.c,random.c}`、`arch/riscv/elf_image.c`、`kernel/syscall/`、`tests/program-inventory/inputs.json`。试跑可现在开始，完整应用依赖按真实调用落到 P1–P4。

### P5a glibc 独立矩阵

- [ ] 固定 glibc loader/libc、相应 ELF、构建/运行环境与许可证/哈希；不修改二进制绕过内核缺失，不把 musl 结果套用为 glibc 结果。
- [ ] 静态、动态、PIE、额外 DSO、初始 TLS、dlopen TLS、pthread、信号及其组合分项验证；保留装载、main 之前、运行时和退出阶段的首个失败。
- [ ] 明确内核和用户动态链接器的职责；PT_INTERP 能装载不等于 glibc 运行时全支持，加载后失败也不一律归因于动态链接。

### P5b shebang 与 exec 组合

- [ ] 解释器路径、可选参数、argv 重组和 envp 保持，循环/递归深度及 E2BIG/ENOEXEC 等错误按固定 Linux 验收；失败保持旧映像、fd/cwd/身份和可继续执行状态。
- [ ] 保留 PT_PHDR/auxv、段重叠/对齐、文件尾页+BSS、PIE/解释器布局回归；与线程组 exec、信号、CLOEXEC 和文本写互斥组合验证，不在内核代替动态链接器重定位。

### P5c 随机数与系统环境

- [ ] getrandom 区分可信熵就绪/未就绪、flags、阻塞/信号及用户 fault；确定性 ASLR 降级不能充当安全随机数成功。熵源按 QEMU/实板实际能力记录。
- [ ] sysinfo/prctl 等只按真实调用链新增；版本和统计来自内核事实，未知能力返回规定错误，用户查询不能触发整机 fatal。

### P5d 离线编译闭环

- [ ] 固定 C 编译器完成预处理→编译→汇编→链接→运行，保留中间阶段退出码、产物哈希与最终输出；版本字符串或目标文件存在不算通过。
- [ ] 再固定 rustc/cargo、依赖锁定和离线最小项目，成功后扩大完整项目。先 `-j1` 建立正确性，不要求 SMP，也不把网络下载失败混入内核 ABI。
- [ ] 每个新失败最小化、对照固定 Linux，再修通用机制；保留可恢复的成功输入和失败样本，不能为构建脚本改写预期输出。

**验证与退出**：现有 `test-exec-riscv`、`test-elf-tail-riscv`、`test-userland-riscv`；拟新增 `tests/userland/exec_scripts.c`、`tests/userland/glibc/`、`tests/workloads/toolchain/`。未特改动态 glibc 与离线最小 C/Rust 构建分别有闭环；不能把剩余运行失败合并成一个“动态链接未支持”。

## P6：SMP、TLB 与中断/I/O 并发

**前置与入口**：共享对象和线程契约已有单 hart 回归，平台具备可验证的 IPI/timer/IRQ 入口。`kernel/sched/{core.c,wait.c,process.c,futex.c}`、`kernel/physical_page.c`、`mm/heap.c`、`fs/{page_cache.c,vfs.c}`、`arch/riscv/{context.c,trap.c,mm.c,sv39.c}` 及设备后端。锁/每核/远端 TLB 接口数量由真实消费者决定，路线仍待确认。

### P6a 每核运行状态

- [ ] current、idle、trap 栈、FPU owner 和 timer 状态按核分离；2 hart 能进入内核只算启动探针，必须继续证明任务正常进出 U-mode。
- [ ] ready 发布、取出、迁移和退出具有唯一 owner，同一线程不能在两核同时执行；增加可检测重复运行的探针，避免只靠应用输出“看似正确”。
- [ ] 保持单 hart 基线和错误诊断，逐个消费者替换当前 tp/全局状态假设；页分配/引用/堆元数据同步也必须纳入。

### P6b 锁、等待与中断规则

- [ ] 列出哪些锁可在 IRQ 中获取、哪些允许睡眠、锁顺序及关中断范围；本地 SIE 不保护另一核，不能只把 refcount 改成原子就宣称 SMP 安全。
- [ ] futex compare→登记→阻塞由同桶/等价同步协议保护；跨核 wake 与入队、timeout、signal、clear_tid 对撞不漏唤醒，不重复运行或摘队。
- [ ] 缺页/设备 I/O 可能睡眠，不能持 spinlock 调 lwext4 或等磁盘；退出/取消期间，阻塞原栈释放已 pin 对象，IRQ 路径不做复杂生命周期回收。

### P6c TLB 完成与页回收

- [ ] PTE 失效/降权→通知相关 CPU→确认旧翻译不能再用→释放物理页、页表和映射引用；完成含义、目标核集合与并发地址空间切换必须明确。
- [ ] 避免持 MM 锁等待也需要该锁的远核；确定锁外完成及延期回收 owner。未确认不能安全复用旧页，也不能把超时转换成普通用户成功。
- [ ] 一核读写、另一核 unmap/truncate/mprotect，跨核 COW 同页与页表释放反复验证；保留单 hart 本地 SFENCE 顺序和 PROT_NONE 所有权。

### P6d 缺页和页缓存并发发布

- [ ] 锁内定位 VMA 并 pin 对象/版本；锁外读取/分配；回锁重查 VMA、权限、EOF、对象和版本。另核已 fault 或 unmap/truncate 时丢弃过期候选，释放准确引用后重试/返回。
- [ ] 同文件页并发 miss 只发布一个权威页；错误状态、dirty/writeback 和被固定页不会被后到的候选覆盖。不可持 spinlock 跨后端 I/O。
- [ ] 交错 fault、COW、fixed replace、退出和 OOM，核对页/对象/缓存/任务资源回到基线；不将所有资源耗尽升级成整机 fatal，不掩盖非法释放等核心不变量。

### P6e 请求完成与外部中断

- [ ] 从同步轮询演进为请求对象+completion/等待队列，先串行实际磁盘请求也可以，但等待期间其他任务能运行；IRQ 只确认状态与唤醒。
- [ ] 明确提交、完成、取消、超时、reset、DMA 停止后内存可释放的 owner 转移；组退出时 I/O 尚未完成、另核关闭/复用 fd 不会回收在用对象。
- [ ] 在单请求正确后再考虑多请求队列，覆盖乱序完成/队列满、失败唤醒、设备停止；不要把吞吐优化和基础所有权改动混成一个难以归因的提交。

### P6f 多核验收矩阵

- [ ] 2→4→8 hart 各自重复：同页 COW、同文件页 fault、读写与 unmap/truncate/mprotect、wake 与入队、I/O 中 exit_group、fd 关闭/复用、内存耗尽与退出回收。
- [ ] 每一核数记录固定输入、重复次数、失败种类、泄漏/重复释放和同任务双核运行检查；保留单 hart 对照定位通用 ABI 与跨核错误。
- [ ] 正确性收口后进入 P7 并行构建；不先替换为 EEVDF。只有饥饿/公平性或性能证据要求时，再比较调度策略。

### P6g 真实内核栈 guard（可单 hart 先做）

- [ ] 比较栈虚拟映射方案，明确未映射 guard 与 direct-map 别名的保护范围、栈页 owner 和映射失败回滚；不能把连续物理栈底 canary 当 guard page。
- [ ] 用受控越界验证 fault 可诊断，保留 canary、高水位、compiler stack usage 和实际调用链预算；合法退出切到可信栈后才回收执行栈，zombie 不持有它。

**验证与退出**：拟新增 `tests/userland/smp_memory.c`、`tests/userland/smp_wait.c`、`tests/riscv/ipi_tlb_main.c` 与对应多 hart runner；保留 `test-riscv`、`test-userland-riscv`、`test-stack-usage` 的单 hart 门禁。2/4/8 核各自有重复正确性和资源基线证据，不能由某一核数或 multi-hart boot 推定其余配置。

## N：socket 与网络支线

**依赖与入口**：P1 的 OFD/后端边界可用于单 hart 网络，不等待 SMP/编译全绿。现有 `fs/files/{poll.c,epoll.c}` 接就绪；拟新增 `net/`、socket OFD 后端和 `kernel/syscall/socket.c`，在接口与栈选型后才物化。

### N1 对象与协议栈选型（待确认）

- [ ] 先定义 socket/OFD、endpoint、缓冲、等待者、close/shutdown 的 owner，及阻塞/非阻塞、用户复制、负 errno 的 ABI 边界；协议栈不能隐式决定这些语义。
- [ ] 比较自写有界协议子集与成熟 C 栈适配：实际协议能力、缓冲生命周期、可睡眠性、并发、内存成本、许可证/维护/可测试性；固定版本与实际消费者，不为跟随其他项目改变内核语言。

### N2 本地与 loopback 链路

- [ ] AF_UNIX/socketpair 与 AF_INET loopback TCP/UDP 按真实调用优先推进；原 socket entry 的实际协议从源码/日志确认，不能只凭测试名猜。
- [ ] bind/connect/listen/accept、send/recv、非阻塞 EAGAIN、半关闭、EOF、失败连接、poll/epoll 和信号打断逐项验收；失败连接不能假装建立 endpoint。
- [ ] sendmsg/recvmsg 与 SCM_RIGHTS 明确被传 fd 的 OFD 引用、用户复制失败和消息未接收/对端退出时回收；不能只传可被关闭复用的整数 fd。
- [ ] 原静态/动态 socket entry 闭环，进程退出后端口、缓冲、fd 和等待节点回到基线。`/proc/net` 文本不是网络实现。

### N3 网卡与真实服务

- [ ] VirtIO-net 实际数据路径、ARP/IP 与宿主双向报文，再验证多个连接、异常断连、半关闭、并发请求及停止回收的固定真实服务。
- [ ] VF2/LS2K 后端分别核对 DMA/cache、MDIO、checksum、IRQ、路由和链路故障；内存 loopback 不算实网卡完成。
- [ ] DNS/TLS 属于用户态时独立验证其随机、时间、文件/证书和网络依赖；TCP 通不等于 HTTPS 可用。

**交付证据**：固定客户端/服务输入、双方日志与实际外部报文，分别报告 loopback、QEMU 网卡、每块实板；所有权和失败场景与性能分开。

## L：LoongArch 与实板支线

**入口与资料**：`arch/riscv/`、架构头与 Makefile；后续拟新增 `arch/loongarch/` 和 `kernel-la`。先读 `references/README.md` 与清单中的 LoongArch 手册/文档、Linux、QEMU、VF2/2K1000LA 资料，记录具体 commit/tag/文档版本或 SHA-256。

### L0 架构依赖盘点

- [ ] 列出通用 MM/调度对 RV 头、satp、SFENCE.VMA、trap frame、页大小和寄存器布局的直接依赖，按实际消费者提取架构操作；不复制 `arch/riscv/mm.c` 中通用 VMA/文件页策略。
- [ ] 保留架构 MMU/context/trap 与平台 DTB/MMIO/DMA 的区分，新增接口由第二实现验证，不预建空泛 HAL。

### L1 最小启动与用户态

- [ ] 串口→trap→timer→物理页→TLB/页表→高地址映射→一个真实 U-mode exit，每步有独立启动/故障/回收证据。
- [ ] 延续已选 LA64 16 KiB/三级页表配置，页大小是架构构建期事实；不把目标配置描述为硬件唯一能力。只生成 `kernel-la` 不算用户态通过。

### L2 ABI 与映像

- [ ] ELF 段对齐、BSS 尾页、auxv、用户栈、stat/signal 结构及 clone 寄存器逐项核对；不能只换汇编入口却保留 RV ABI 编码。
- [ ] 同一用户源码分别编译 RV/LA ELF，每架构内部用同一 ELF 对照 Linux 与 BoarOS；共享测试语义，隔离寄存器/页表差异，不拿 RV ELF 验证 LA。

### L3 扩大真实用户空间

- [ ] 静态 musl→动态 musl/DSO/TLS→fork/COW/信号→共享映射→glibc→真实应用，逐层保留错误与资源回收结果。
- [ ] 维持 RV/LA 同口径功能矩阵，缺能力记录阻塞，不让新平台回退到固定输出或修改过的用户程序。

### L4 两块实板

- [ ] 每块板分别验证固件交接/DTB→RAM 保留区→timer→块设备→根盘读写→shell→IRQ→网络→SMP；QEMU、VF2、2K1000LA 结果分列。
- [ ] 记录 DMA 地址/缓存一致性、设备限速/超时、烧录校验、固件/DTB/镜像哈希与可回退启动配置；先确认镜像和网段，不把环境错误先归因 ABI。
- [ ] 每板最小交付是可重复启动和真实根文件系统读写，之后单独标记网络、多核、复杂应用及性能；未拿到设备或固件输入时明确阻塞。

## P7：多核真实编译与性能

**依赖**：P5/P6；实板性能另需 L4。代码优化落到被测模块，先保留基准和成本解释。

### P7a 可比的负载与测量

- [ ] 固定源码、工具链、镜像副本初态、核数、内存、日志、计时起止、冷/热缓存；明确数据写到 RAM、介质缓存还是已经同步。
- [ ] 真实 C/Rust `-j1/-j2/-j4/-j8` 全部构建成功且产物运行正确，再报告多次中位数/范围和失败。Linux/BoarOS 保持相同口径，不跨 ISA/机器/持久性模型直接比快慢。
- [ ] 观察加速曲线中的串行部分和竞争，不要求所有负载核数翻倍性能翻倍；QEMU 的功能证据与板上吞吐分开。

### P7b 测量驱动的优化候选

- [ ] 收集 fault/major fault、重复读取、缓存命中、全 LRU node 失效扫描、inode 查找、写回流量、块请求大小/队列深度、堆锁、调度/唤醒、TLB 同步与栈高水位。
- [ ] 有证据后比较 node 缓存页索引、inode 哈希、批量 I/O/预读、锁外 I/O、每核缓存、deadline 结构、ASID/批量 shootdown 与调度公平性；精确失效不代表查找成本已优化，也不能未经测量宣称它是主瓶颈。
- [ ] 每次只改变可归因机制，写清内存/复制/锁/延迟成本与回退；保留负收益。性能改动仍须满足 ABI、owner、并发与资源耗尽回归。

## P8：系统交付与现场复现

### P8a 离线交付包

- [ ] 新目录或新机器按固定输入离线构建，校验产物/镜像身份，不依赖开发目录残留；保留依赖许可证与版本。
- [ ] 提供内核 commit、编译器/依赖、磁盘/DTB/固件哈希、平台启动命令、已支持/未支持范围、失败复现、回归日志与恢复/回退方式。
- [ ] 验证目录 rootfs 重新制盘、整镜像校验、串口抓日志、双网卡网段/路由和最小应用探针。真实设备操作遵循人的发布/烧录授权。

### P8b 组合与现场演练

- [ ] 从未知组合脚本/服务逐层定位设备→根盘→用户态→应用的首个失败，形成最小复现修通用机制；不能识别脚本名称输出答案。
- [ ] 默认引导、交互模式和评测 runner 分开，由同一内核能力支撑；保留一个已验证可回退版本。
- [ ] 数据库恢复、网络服务、多核编译、两个 QEMU 架构和每块板分别给验收结果，不能合成“全面支持 Linux”。他人能据列出的输入和命令复现才收口。

## 实施前需要确认的路线

以下是可行候选与调查方向，不是批准记录。实施任务时继续补全固定资料、接口、失败路径与 owner 的证据，再由维护者确认。明显违反契约的“fsync 空返回”“只共享已有 PTE”“用本地关中断代替跨核同步”不列作候选。

| 取舍 / 当前证据 | 可行候选与正确性、演进、复杂度、成本 |
|---|---|
| P1b VFS：原 fs context 借用单根 mount、cwd 字符串；OFD 已独立 | 已选择并完成②：最小 mount+路径对象/后端操作。路径持有 mount、inode 与父链引用，ext4 字符节点按 `rdev` 分派；共享可改名目录项与普通/NOREPLACE rename 已完成；多挂载仍留后续。 |
| P4a 共享匿名 | 已选择并交付②：专用对象按索引惰性发布页，保持稀疏分配并提供稳定身份；急切分配会改变 lazy/OOM 成本。共享文件页与跨 MM futex 各自继续设计。 |
| P3b 持久化 | 用户已选择并交付逐 inode dirty/error、定向写回和真实 flush；共享事务可提交关联元数据，不主动全量写回无关文件。 |
| P3d 恢复 | 用户已选择本批启用并验证 journal/replay 与持久 orphan；故障模型、限制和复现命令见 VFS 模块。 |
| P6 SMP：当前 SIE 串行化，缺远端 TLB 确认 | ① 进程态对象先用粗粒度可睡眠锁、IRQ/队列另设短锁，验证较少但并行有限；② MM/OFD/cache/队列对象锁直接演进，锁顺序/取消成本更高。先盘点消费者和睡眠边界再选，临时启动大锁有退出条件。 |
| P6g 栈 guard：连续物理栈、canary/高水位 | ① 独立虚拟栈区映射已有页，便于未映射 guard，但需页表与回收接口；② 调整内核现有映射形成受保护栈区域，初始接口可能更少，但别名/大页拆分与 direct-map 消费者成本须实测。先验证真实越界保护范围再选。 |
| N1 协议栈：无 socket/传输链 | ① 自写明确范围的 C 协议子集，owner 易控制但协议/互操作验证成本高；② 适配成熟 C 栈，可复用协议实现，但缓冲/定时器/并发模型、许可与长期更新成本需核对。socket ABI 由 BoarOS 持有，先比较实际消费者。 |

调度暂保留简单 FIFO：先完成唯一运行、唤醒和 TLB 回收，再凭公平性/负载证据比较策略。第二架构按连续小里程碑推进；不等待 RV “全部完成”，也不复制整套通用内核。

## 每个任务的统一验收与现有命令

| 维度 | 提交前应有的证据 |
|---|---|
| 触发与契约 | 独立最小输入、声明支持范围、固定参考路径与版本；正常返回、errno、部分成功与 fault 可观察 |
| 失败前后 | 同输入的真实失败与修复结果，Linux 参考有效；未改上游答案、未宽泛归一化错误 |
| 所有权与安全 | 对象由谁持有、睡眠期间谁 pin、发布/失败/退出谁释放；真实 I/O 错误 owner 与分配器 fatal 不变量分开 |
| 并发 | 单 hart 临界区与跨 hart 同步分列；等待登记、可见性、发布/回收顺序和竞争探针 |
| 资源与错误 | OOM/超时/真实 I/O 返回所属层状态；合法释放完成即返回，非法释放/引用损坏 fatal，不能建立无 owner 重试链 |
| 性能 | 记录成本边界；无数据时称候选瓶颈。风格建议不混入 ABI/安全/并发结论 |
| 验证与文档 | 最窄→真实 U-mode→相关架构/全量；已知缺能力写阻塞。检查 README、modules、learning；TODO 更新具体完成条目 |

现有入口如下，子任务按依赖选择；上文拟新增文件不代表已经有同名 Make 目标：

```sh
make test-riscv
make test-userland-riscv
make test-diff-abi-riscv
make test-lwext4-host
make test-stack-usage
make test-program-inventory-host test-diff-abi-host
make inventory-userland-riscv
```

`inventory-userland-riscv` 默认成功只说明清单生成成功。全量 228 项仍有明确缺口，严格模式失败不是自动产生的新回归；`--case` 与 `--require-pass` 只严格判定本次选择集合，未选项目保留历史结果或 `not-run`，选择集合写入状态供恢复报告解释。完整 Harness 缺 `kernel-la` 或其他能力时保留阻塞原因。

## 范围与交付边界

暂不进主线：NUMA、swap、透明大页、完整 namespace/cgroup、seccomp/eBPF/ftrace、Linux 内核模块 ABI、复杂可加载框架和 io_uring。它们可另立任务，当前失败无需先完成这些能力。不因未来可能有用引入第三方框架或 Agent 编排平台。

`final-2025` 沿用只是规划假设；比赛事实只据本地固定规则/Harness，不能预测未来赛题。许可证和正式发布策略仍待决定。正常开发沿当前分支；提交围绕可说明/可验证的问题，AI 贡献按 CONTRIBUTING 添加 trailer；不提交会话材料、JSONL、秘密或运行镜像，发布权限遵循 [AGENTS.md](../AGENTS.md)。
