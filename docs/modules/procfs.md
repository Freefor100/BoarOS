# 进程文件系统（procfs）

**进程文件系统（Process Filesystem，procfs）**是 Linux 将内核运行状态、进程与线程视图、内存映射和运行时调优参数以虚拟文件树形式暴露给用户空间的核心接口。用户工具（如 `ps`、`top`、`mount`、`sysctl`、容器运行时与调试器）均依赖 `/proc` 读取或调整内核状态。本文基于 `fs/procfs.c` 的实际代码，梳理 BoarOS `procfs` 的完整目录树、PID 命名空间挂载视图、快照与控制文件语义以及逐页内存观测能力。

---

## 一、源码组织与目录全景

`fs/procfs.c` 将 `procfs` 实现为可动态创建与多实例挂载的 VFS 后端（文件系统魔数 `PROC_SUPER_MAGIC = 0x9fa0`），其完整节点树如下：

| 路径 | 类型 / 权限 | 核心内容与语义 |
| :--- | :--- | :--- |
| `/proc/cpuinfo` | 只读快照（`0444`） | 按逻辑处理器编号列出当前在线处理器。RISC-V 和 LoongArch 分别使用对应架构的字段。 |
| `/proc/meminfo`、`/proc/uptime` | 只读快照（`0444`） | 物理内存快照（KiB：`MemTotal/Free/Available/Buffers/Cached/Shmem/Dirty/Writeback` 等）与系统运行/空闲时长 |
| `/proc/filesystems`、`/proc/mounts` | 只读快照 / 符号链接 | 已注册文件系统类型表（非块设备带 `nodev`）；`/proc/mounts` 指向 `self/mounts` |
| `/proc/self`、`/proc/thread-self` | 动态符号链接 | 每次解析动态返回调用者的 `<TGID>` 或 `<TGID>/task/<TID>`（在当前挂载的 PID 命名空间不可见时返回 `-ENOENT`） |
| `/proc/kpageflags` | 二进制只读（属主可读） | 按物理页帧号（PFN × 8）查询 `KPF_DIRTY`、`KPF_WRITEBACK`、`KPF_BUDDY` 三项真实标志 |
| `/proc/sysvipc/{shm,sem,msg}` | 只读快照（`0444`） | 打开时捕获当前 IPC 与 PID 命名空间，按槽位枚举共享内存、信号量与消息队列元数据 |
| `/proc/net/route` | 只读快照（`0444`） | 当前网络接口的默认路由与直连子网路由表 |
| `/proc/sys/kernel/*` | 控制文件（`0644`；`pid_max` 为 `0444`） | `sched_rt_period_us`、`sched_rt_runtime_us`、`sched_rr_timeslice_ms`、`pid_max`、`unprivileged_bpf_disabled`、`hostname`、`domainname` |
| `/proc/sys/kernel/{shmmax,shmall,shmmni}` | 控制文件（`0644`） | 当前进程间通信命名空间的 System V 共享内存单段字节数、总基础页面数与段数上限。打开的文件描述符保留原配置，命名空间销毁后访问返回 `ENOENT`。 |
| `/proc/sys/kernel/keys/{maxkeys,maxbytes,root_maxkeys,root_maxbytes}` | 控制文件（`0644`） | 按真实用户标识执行的密钥数量和计费字节上限，分别配置普通用户和用户标识为 0 的账户。 |
| `/proc/sys/vm/*` | 控制文件（`0644`；`drop_caches` 为 `0200`） | `nr_hugepages`、`drop_caches`（1 回收干净页缓存、2 回收目录项与后端元数据、3 两者皆回收）、`vfs_cache_pressure`（默认 100，设为 0 停用自动元数据回收） |
| `/proc/sys/fs/*` | 控制文件（`aio-nr` `0444`、`aio-max-nr` `0644`） | 异步 I/O 全局在途请求计数与上限控制 |
| `/proc/<pid>/*` | 进程目录与子项 | `stat`（前 41 个标准字段）、`status`、`comm`、`maps`、`io`、`pagemap`、`mounts`、`mountinfo`、`cgroup`、`coredump_filter`、对象链接 `exe`/`cwd`/`root`、目录 `fd/`、`fdinfo/`、`ns/`、`task/` |
| `/proc/<pid>/task/<tid>/*` | 线程目录与子项 | 除 `task/` 与 `coredump_filter` 外与进程目录一致，所有统计与资源精确绑定目标线程 `TID` |
| `/proc/boaros_*`（仅诊断构建） | 只读 / 控制文件 | 开启 `BOAROS_COST_DIAGNOSTICS` 时提供 `boaros_cost`、`boaros_cost_control`、`boaros_net_stats`、`boaros_mem_stats`（详见 [成本观测模块](kernel-cost.md)） |

---

## 二、PID 命名空间挂载视图与防复用编码

在容器与多命名空间场景下，同一个进程在不同 **PID 命名空间**中拥有不同的本地编号。BoarOS 让每个 `procfs` 挂载实例在创建时（`fsopen("proc")` 或传统 `mount`）固定捕获创建者的活跃 PID 命名空间视图 `procfs_mount.pid_view`，后续挂载传播、绑定挂载或文件描述符继承均不改变该视图。

```mermaid
flowchart LR
    Path["用户查找 /proc/<local_pid>/..."]
    View["挂载 PID 视图 (pid_view)"]
    Inode["64 位动态 inode 编码: identity(34b) | kind(4b) | fd(10b) | root_pid(16b)"]
    Check["查询与打开复核: PID 代次 + 凭据 + dumpable + MM 身份"]

    Path -- "本地号转规范 root_pid 与全局代次" --> View
    View --> Inode
    Inode -- "进程退出或 PID 复用即失效 (ESRCH / ENOENT)" --> Check
```

- **动态 `inode` 编入分配代次**：路径解析立即把挂载视图内的本地 PID/TID 转换为全局规范编号和单调递增的分配代次（`identity`），编码进 64 位 `inode` 编号的高位；根目录与 `task/` 目录直接沿该命名空间的 PID 位图跳过空槽枚举，以本地号作为目录续读 `cookie`，不扫描全局任务表。
- **彻底杜绝 PID 复用误命中**：一旦目标进程退出并被 `wait` 回收，即使新进程复用了相同的整数 PID，旧的已打开目录或文件节点因代次不匹配会自动失效（已打开的旧进程目录继续查找返回 `-ESRCH`，旧线程目录返回 `-ENOENT`），绝不会误读或篡改新进程的资源。
- **跨命名空间与追踪者展示**：`stat`/`status` 在身份锁内原子捕获 `PID/TGID/PPID/PGID/SID` 与 `TracerPid`，在当前挂载视图不可见的外层进程显示为 `0`；`NSpid`/`NStgid`/`NSpgid`/`NSsid` 从挂载所在层向下展开至目标活跃层。`pidfd` 的 `fdinfo` 按同一挂载视图输出 `Pid` 与 `NSpid`（跨兄弟命名空间输出 `0`，已消亡输出 `-1`）。

---

## 三、两类虚拟文件模型：快照生成与动态控制

`procfs` 不进入磁盘页缓存，而是将虚拟文件明确分为**生成式快照文件**与**动态控制文件**两类语义：

| 文件类型 | 代表节点 | 读取与寻址（`lseek`）语义 | 写入与并发锁策略 |
| :--- | :--- | :--- | :--- |
| **生成式快照文件**（`seq_file` 语义） | `cpuinfo`、`meminfo`、`stat`、`status`、`maps`、`mounts`、`mountinfo`、`io`、`sysvipc/*` | 首次非空顺序读或非零 `lseek` 时生成堆缓冲并归属于当前 OFD；后续连续短读、`readv`/`pread` 直接复用快照；`lseek` 回 0 后释放并在下次读取重新采样；`SEEK_END` 返回 `-EINVAL`；`fstat` 报告 `st_size == 0` | 所有内核元数据在短锁内抓取后立即释放锁，文本格式化、堆分配与用户空间拷贝均在锁外执行，不持锁睡眠 |
| **动态控制文件**（`generated_control`） | `sched_rt_*`、`sched_rr_timeslice_ms`、`hostname`、`domainname`、`nr_hugepages`、`drop_caches`、`vfs_cache_pressure`、`aio-max-nr`、`unprivileged_bpf_disabled`、`comm`、`coredump_filter` | 不缓存 OFD 快照；数值型 sysctl 在偏移 0 实时读出当前值，非零偏移直接 EOF；字符串型（`hostname`/`domainname`）按当前偏移续读；`lseek` 采用 Linux `default_llseek`（`SEEK_END` 以 0 为基准，`SEEK_DATA/HOLE` 返回 `-ENXIO`） | 先在锁外完整拷贝用户数据，再在原子 setter 或短锁内校验并发布；`drop_caches` 校验权限后先释放 proc 重配置锁再遍历各挂载回收；`hostname`/`domainname` 按每次操作实时取调用者 UTS 命名空间且仅 `euid == 0` 可写 |

`cpuinfo` 依据已上线的处理器记录生成快照，枚举范围与读取线程的处理器亲和性无关。RISC-V 的 `hart` 为硬件线程编号，`isa` 列出各在线硬件线程共同支持且内核已探测的指令集架构（Instruction Set Architecture，ISA）扩展，`hart isa` 列出当前记录的扩展。`mmu` 显示 Sv39 分页模式，固件能提供厂商、架构和实现编号时才输出对应字段。LoongArch 输出逻辑编号、硬件编号 `global_id` 和指令集架构 `loongarch64`。

---

## 四、进程观测、对象链接与逐页查询

### 4.1 进程与线程的完整元数据视图

- **`stat` 与 `status`**：进程与线程共用统一快照路径。`stat` 输出前 41 个标准字段（含 `utime`/`stime`/`cutime`/`cstime` 100 Hz 节拍计数、`minflt`/`majflt` 缺页计数、真实 `priority`/`rt_priority`/`policy`、主 ELF 代码段边界 `start_code`/`end_code` 与初始栈 `start_stack`、`tty_nr`/`tpgid` 及 `wchan`）；`status` 输出 `Name`、`State`、`Tgid`、`Pid`、`PPid`、`Threads`、`VmSize`、`VmRSS`、`VmSwap`（无 swap 恒为 0）、`SigQ`、五组 64 位信号掩码、四组 `Uid`/`Gid`、`Groups`、五组能力集与 `NoNewPrivs`。
- **`maps` 内存映射表**：在地址空间门控（MM gate）内一次性捕获所有 VMA 元数据与后备 OFD 引用，随后在门控外渲染路径与文件标识；自动标注 `[heap]`、`[stack]`、`[vdso]` 与已删除文件的 `(deleted)` 后缀，换行转义为 `\012`。
- **`io` 输入输出记账**：进程目录汇总整个线程组（含已退出成员与已回收子进程），线程目录仅报告单线程，涵盖 `rchar`、`wchar`、`syscr`、`syscw`、`read_bytes`、`write_bytes` 与截断撤脏记账 `cancelled_write_bytes`。
- **`mounts` 与 `mountinfo`**：共享单次挂载拓扑快照，捕获目标进程根目录与挂载命名空间顶点的路径引用后无锁格式化；`mountinfo` 输出挂载 ID、父 ID、设备号、`bind` 根路径、相对挂载点及 `shared`/`master`/`propagate_from`/`unbindable` 真实传播属性，自动裁剪目标 `chroot` 根之外的挂载项。
- **对象链接（`exe`、`cwd`、`root`、`fd/`、`ns/`）**：
  - `exe`、`cwd`、`root` 在路径跟随时直接返回活 VFS 路径引用，即使原文件已被 `unlink` 仍可经链接重新打开原对象，`readlink` 仅用于展示并在已删除时追加 `(deleted)`。
  - `fd/` 按活文件表枚举 0 ～ 1023；普通文件与命名/匿名管道链接重新打开会建立独立 OFD（或新管道端点），套接字、`epoll`、`pidfd` 与 `fsopen`/`fspick` 配置描述符（`anon_inode:[fscontext]`）支持 `readlink` 但重新打开返回 `-ENXIO`。
  - `ns/{mnt,uts,ipc,pid,pid_for_children}` 打开后返回真实的命名空间文件描述符，持该描述符会保活对应命名空间，支持 `NS_GET_NSTYPE` 查询类型；对非用户命名空间调用 `NS_GET_OWNER_UID` 返回 `-EINVAL`（`O_PATH` 返回 `-EBADF`）。

### 4.2 逐页内存观测（`pagemap` 与 `kpageflags`）

为了支持调试器、性能分析工具和内存管理测试观察虚拟页与物理页帧的真实映射状态，BoarOS 实现了两个 8 字节对齐的二进制逐页查询接口：
- **`/proc/<pid>/pagemap`**（及线程 `task/<tid>/pagemap`）：字节偏移为 `虚拟页号 × 8`，分批检查页表项而不触发缺页，正确反映 `PROT_NONE` 已驻留页、大页内各基页真实偏移、文件/共享页标志（私有文件页经写时复制后清除文件页标志）。打开时校验跨进程观察权限并获取目标 MM 弱引用，只有打开者具备 `CAP_SYS_ADMIN` 时才暴露物理页帧号（PFN）；目标地址空间释放后读取返回 EOF，不会随 `exec` 或 PID 复用切换目标。
- **`/proc/kpageflags`**：字节偏移为 `PFN × 8`，输出物理页的 `KPF_DIRTY`（页缓存存在未写回修改）、`KPF_WRITEBACK`（正在写回）与 `KPF_BUDDY`（伙伴分配器空闲块首），物理空洞返回 0，越过最高受管页帧返回 EOF。

---

<details>
<summary>展开查看：跨进程访问控制细节与定向验证入口</summary>

### 跨进程权限校验与特殊规则

- **进程观察权限**：敏感的 `maps`、`io`、`pagemap` 以及 `exe`/`cwd`/`root`/`fd`/`fdinfo`/`ns` 链接复用跨任务访问校验（与 `get_robust_list` 同源）：同线程组直接放行，跨进程按调用者文件系统凭据（`fsuid`/`fsgid`）、目标三组凭据、可转储状态（`dumpable`）与能力集裁决；不可转储进程的非 `0555` 节点动态归属 `root`。获取对象前与获取后双次复核 PID 代次、凭据与执行安全状态，防止把 `exec` 前的权限沿用到新映像。
- **`/proc/<pid>/comm` 写入规则**：与 `prctl(PR_SET_NAME / PR_GET_NAME)` 共享 16 字节线程名（最多 15 字节 + 终止 NUL），仅允许同线程组写入，每次取前 15 字节（超长部分不访存但计入返回长度），定位写返回 `-ESPIPE`。
- **`/proc/sysvipc/*` 命名空间捕获**：普通 `open` 固定打开者当时的 IPC 与 PID 命名空间，后续 `unshare`/`setns` 不影响旧 FD 观察范围；`O_PATH` 不建立该引用。
- **已知边界**：`mounts`/`mountinfo` 当前在快照生成时读取目标实时根目录，尚未像 Linux 那样在 FD 打开时冻结根目录；`pagemap` 的软脏位、独占映射计数与 `ioctl` 扫描尚待实现。

### 定向验证入口

```sh
# 宿主三层/兄弟 PID 命名空间、pidfd、信号快照与 maps 索引回归
make test-proc-index-host
make test-proc-maps-host

# RISC-V / LoongArch 差分 ABI 与定向用户态测试
make test-diff-abi-riscv
python3 -B tests/diff-abi/harness.py --kernel kernel-rv --program build/diff-abi/proc-rv \
  --case-manifest tests/diff-abi/proc-cases.txt
```

- 相关源码与专项测试：`tests/diff-abi/proc.c`、`tests/diff-abi/rt_controls.c`、`tests/diff-abi/sched_stat.c`、`tests/host/proc_sysvipc.c`、`tests/workloads/ipc/proc_sysvipc.c`、`tests/riscv/sched-bandwidth.sh`；控制文件设计分析见 [proc 控制文件](../learning/proc-controls.md)。

</details>
