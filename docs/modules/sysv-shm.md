# System V 共享内存（SysV SHM）模块

本文描述 BoarOS 中 System V 共享内存子系统（`shmget`、`shmctl`、`shmat`、`shmdt`）的实现契约、生命周期管理与内存管理集成。

## 范围与入口

| 文件 | 职责 |
|---|---|
| `include/kernel/ipc.h` | 与信号量共用的 `ipc64_perm`、IPC常数及身份/权限规则 |
| `include/kernel/shm.h` | `shmid64_ds` 等SHM原生布局、常数、内核段结构与接口声明 |
| `mm/shm.c` | 各IPC命名空间的段表、`IPC_PRIVATE` 与命名 key 映射、代次 sequence、延迟销毁（`IPC_RMID`）与 `nattch` 引用计数 |
| `kernel/syscall/shm.c` | 系统调用参数解码、用户指针有效性校验与 Linux ABI 返回值映射 |
| `include/kernel/vma.h`、`mm/vma.c` | `KERNEL_VMA_KIND_SYSV_SHM` VMA 描述符定义与不合并（no-merge）约束 |
| `include/kernel/mm.h`、`mm/mm.c` | `kernel_mm_shmat`/`kernel_mm_shmdt`、Sv39 共享按需缺页、fork 继承及 munmap/release 清理钩子 |
| `tests/diff-abi/shm.c` | 针对固定 Linux 参考内核的双侧差分 ABI 测试 |
| `tests/userland/sysv_shm.h` | 真实 musl 用户态多进程 fork 与读写一致性验证 |

`SHM_HUGETLB` 使用原生 hstate：RISC-V 2 MiB、LoongArch 32 MiB；`SHM_HUGE_SHIFT` 可选默认或该原生尺寸，其他尺寸返回 `EINVAL`。已有 key 的查找先执行大小、权限及 `CREAT|EXCL` 检查，不因创建标志改变语义。新建大页段要求 `CAP_IPC_LOCK` 或 root 组成员身份（Linux 初始 `hugetlb_shm_group=0`）；该组的可写 sysctl 尚未实现。

## 核心不变量与生命周期

1. **唯一段所有权与统一后备对象**：
   - 普通段使用 `struct kernel_memory_object` 提供基页后备；大页段使用 `kernel_hugetlb_map` 的共享后备，复用匿名共享大页的真实高阶物理页和 PMD 叶，不创建普通页伪装大页。两者均按需缺页。
   - 段所有权由内核段表统一管理，MM registry 持有后备对象引用，同一 MM 的同一大页段只登记一次，多个附加复用；每次逻辑附加另有 `kernel_shm_attachment` owner，持有后备引用、固定 shmid 身份和原始起止地址。VMA 片段拥有 attachment 引用。
   - `shmat` 给 MM registry 新取得的内存对象引用先由局部 owner 持有，再移动给 registry；分配或映射失败只消费临时 owner，不改变段表引用。
   - cpid/lpid 保留规范 TGID 身份，syscall 按当前读取者 PID namespace 输出，proc 按 OFD 打开时保留的 PID namespace 输出；退出与数字复用不改变旧 owner。异步 MM/VMA 销毁显式携带操作主体，不能记为 cleanup worker。共享 MM 非最后引用释放不更新 lpid；fork 的 VMA open 记录父任务。身份及锁外回收契约见 [PID namespace](pid-namespace.md#sysv-ipc-的-pid-身份与读取者视图)。
2. **段标识符与代次序列隔离**：
   - 最大段数量由 `KERNEL_SHMMNI`（128）固定。
   - `shmid = slot_index + seq * KERNEL_SHMMNI`。
   - 段彻底销毁时对应槽位的 `seq` 计数自增，避免悬空 ID 与 ABA 别名冲突。
3. **延迟销毁语义（`IPC_RMID`）**：
   - 调用 `shmctl(shmid, IPC_RMID, NULL)` 时：
     - 若当前片段计数 `nattch == 0` 且逻辑 attachment owner 数 `attachments == 0`，立即释放物理页与内存对象，槽位复位。
     - 否则标记 `marked_for_deletion = 1`，并将 key 改为 `IPC_PRIVATE`，使 `IPC_STAT` 和 proc 列表与命名查找的撤销一致。后续 `shmget` 不再能按原 key 查找到该段；只要该段仍存活，按完整 ID 的 `shmat` 可以再次附加。已有附加的虚拟内存映射保持有效并可正常读写。
     - 随 `shmdt`、`munmap`、进程 `exit` 或 `execve` 解除附加，当最后一个片段及临时 attachment owner 消失时，触发物理资源与段槽位的彻底释放。
4. **`IPC_STAT` 与 `SHM_DEST` 标志**：
   - 当段已被 `IPC_RMID` 标记删除但仍有附加时，`shmctl(..., IPC_STAT, ...)` 仍可成功查询，并在 `shm_perm.mode` 中反映 `SHM_DEST`（01000）标志位，符合 Linux 原生行为。

创建时保存调用者 effective UID/GID 到 owner 和 creator 字段。按 key 查找、shmat 和 IPC_STAT 分别检查所需读写/执行位，CAP_IPC_OWNER 允许越过访问位；IPC_SET/RMID 要求 owner、creator 或 CAP_SYS_ADMIN。IPC_SET 先完整导入用户结构，再检查对象及管理权限，成功后更新 owner/group/mode；数字 UID/GID 采用初始用户命名空间的有效值。

`/proc/sysvipc/shm` 与 `IPC_STAT` 共用段描述符，并从实际后备读取 resident 页数（大页共享 map 在首次成功发布时维护常数时间驻留计数），按客机页尺寸换算为字节；当前没有 swap 后备。仍被附加持有的 RMID 段保留列表项及 `SHM_DEST`，最后释放后撤下。列表读取与其他 IPC 列表的共同规则见 [procfs 模块](procfs.md)。

命名空间隔离及旧 attachment 在空间切换后的所有权见 [IPC 命名空间](ipc-namespace.md)。

## 限额、用量与槽查询

每个 IPC namespace 独立执行 `SHMMNI`、`SHMMAX` 和 `SHMALL` 限额。`SHMALL` 按用户请求大小向上取整为客机基页计费，不按大页实际后备尺寸计费；RMID 段的计费保留到最后销毁，创建失败不扣额度。

IPC_INFO 输出当前配置，SHM_INFO 输出真实活跃段数、按客机页尺寸取整的请求总页数和后备对象已驻留页数；两者返回最高活跃槽位，空表返回0。当前无swap，交换页数和交换尝试/成功次数均为0。已RMID但仍有附件的段继续计入，最后使用者释放后从统计撤下。

IPC_STAT 按完整ID检查槽和代次；SHM_STAT/SHM_STAT_ANY按槽索引取得当前对象，返回带当前代次的完整ID。STAT执行访问位与CAP_IPC_OWNER检查，STAT_ANY按Linux提供与`/proc/sysvipc/shm`相同的枚举可见性。段表在既有CPU0控制位置受保护，查询先取得有界结构快照，用户复制在保护外完成；复制期间原段删除不会改变这次快照，短复制保留实际输出并返回EFAULT。

SHM_LOCK/UNLOCK的真实UID账户、MEMLOCK和延迟归还见[MM模块](kernel-mm.md#madvise-与共享段锁定)。查询直接消费者为`tests/workloads/resource/shm_query.c`，宿主4/16 KiB页检查与锁定回归共用`tests/host/shm-lock.sh`。

## 大页预留与生命周期

大页后备向上取整到原生大页尺寸，但 `IPC_STAT.shm_segsz` 保留原请求值。`shmget` 一次性从现有池容量预留，不在每次 `shmat` 或 fork 重复预留；不足返回 `ENOMEM`。`SHM_NORESERVE` 仅跳过预留，实际 fault 仍必须取得真大页，耗尽时 `SIGBUS`。段表、attachment 与 MM registry 分别持有稳定后备引用；即使暂时没有映射，未 RMID 的段也保留数据和预留。最后销毁归还未用预留和驻留页，namespace 切换不改变旧 attachment 的来源。

`SHM_LOCK/UNLOCK` 对大页仍检查权限及零 MEMLOCK 条件，但不扣 UID 锁页额度、不设 `SHM_LOCKED`：大页本来就不可换出。`SHM_RND` 仍按架构 SHMLBA（当前基页）向下取整，取整后固定地址必须满足大页对齐；自动地址也按大页对齐。mprotect、munmap 只允许大页边界，shmdt 按稳定 attachment 移除剩余片段。大页 mremap 和 MADV_REMOVE 尚不支持；没有 hugetlbfs 挂载、多 hstate 或透明大页。

Linux 语义参考 `ipc/shm.c:newseg/do_shmat/shmctl_do_lock` 与 `fs/hugetlbfs/inode.c:hugetlb_file_setup`（v6.12）。

## VMA 与架构集成

命名 key 查找先检查已存在段、`CREAT|EXCL` 冲突及请求大小是否超过原段；`size=0` 可查找已有段。缺失 key 且无 `CREAT` 返回 `ENOENT`；只有新建段才检查 `SHMMIN/SHMMAX`。固定 ABI 依据为本地 `references/linux/ipc/shm.c` 的 `newseg`/`shm_more_checks` 与 `references/linux/ipc/util.c`，commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e`。

- 普通段 VMA 使用 `KERNEL_VMA_KIND_SYSV_SHM` 与 `KERNEL_VMA_FAULT_ANON_SHARED`；大页使用 `KERNEL_VMA_KIND_HUGETLB_SHARED` 与 `KERNEL_VMA_FAULT_HUGETLB`，同时保留 SysV role 和 attachment 生命周期。
- `mm/vma.c` 的 `can_merge()` 显式禁止合并带 attachment 的 SysV SHM VMA，确保各附加段的边界、起始地址与生命周期独立。
- `mm/vma.c` 在成功插入、split、fork、remove/replace 和 destroy 时统一调用 attachment open/close；prepare 不改变引用，commit 不再分配。`nattch` 是已提交 VMA 片段数，不是每进程或每次 `shmat` 固定计一。临时 attachment owner 保住槽身份但不增加 `nattch`，所有权错误触发 fatal。
- `shmdt` 从原始附加地址查找稳定 attachment，即使首段已撤销也能移除全部剩余片段；不会移除洞内被匿名、文件或其他 SHM 附加替换的新映射。fork 只为继承的 VMA 增引用，DONTFORK 片段不进入子 MM；后续 fork OOM 经统一 destroy 回滚。
- `SHM_RDONLY` 的 VMA 上限为 RX，禁止经 `mprotect` 提升 WRITE；当前权限与上限分开，允许先降到 NONE 再恢复 READ。分裂和 fork 保留该限制。
- 页表 fork 的 SHARED 决定保留 SysV 共享叶的写权限；DONTFORK 对应 SKIP，既不创建子叶，也不对父叶施加 COW。

## 验证与回归

- `tests/host/shm-hugetlb.sh` 链接真实 SHM、namespace、MM、hstate 和页表：检查原生高阶 pin、多个附加、SHM_REMAP、fork、RMID、只读权限、NORESERVE、分配失败回滚、namespace 旧附件及配额释放。
- `tests/userland/hugetlb.py` 的 RV/LA 定向 guest 检查与匿名大页共用池，覆盖大小取整、split/shmdt、共享 fork、RMID 重附加和真实 `SIGBUS`；`--shm-flags-only` 的独立源码也可在宿主 Linux 运行作无需修改池配置的差分。

- **Linux 差分 ABI 测试**：`make test-diff-abi-riscv` 覆盖 key/size/flags、片段计数/替换以及只读附件改权。
- **真实 musl U-mode 测试**：`make test-userland-riscv` 在真实 musl libc 运行环境下验证跨 fork 数据传递、`shmdt`、销毁后再次 attach 拒绝等完整场景。
- **栈预算校验**：`make test-stack-usage` 确保所有新增函数均在栈安全边界内。
