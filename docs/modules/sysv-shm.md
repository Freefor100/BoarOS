# System V 共享内存（SysV SHM）模块

本文描述 BoarOS 中 System V 共享内存子系统（`shmget`、`shmctl`、`shmat`、`shmdt`）的实现契约、生命周期管理与内存管理集成。

## 范围与入口

| 文件 | 职责 |
|---|---|
| `include/kernel/ipc.h` | 与信号量共用的 `ipc64_perm`、IPC常数及身份/权限规则 |
| `include/kernel/shm.h` | `shmid64_ds` 等SHM原生布局、常数、内核段结构与接口声明 |
| `mm/shm.c` | 全局段表、`IPC_PRIVATE` 与命名 key 映射、代次 sequence、延迟销毁（`IPC_RMID`）与 `nattch` 引用计数 |
| `kernel/syscall/shm.c` | 系统调用参数解码、用户指针有效性校验与 Linux ABI 返回值映射 |
| `include/kernel/vma.h`、`mm/vma.c` | `KERNEL_VMA_KIND_SYSV_SHM` VMA 描述符定义与不合并（no-merge）约束 |
| `include/kernel/mm.h`、`mm/mm.c` | `kernel_mm_shmat`/`kernel_mm_shmdt`、Sv39 共享按需缺页、fork 继承及 munmap/release 清理钩子 |
| `tests/riscv/scale_main.c` | 裸机内核规模测试中的 SysV SHM 生命周期与多附加验证 |
| `tests/diff-abi/shm.c` | 针对固定 Linux 参考内核的双侧差分 ABI 测试 |
| `tests/userland/sysv_shm.h` | 真实 musl 用户态多进程 fork 与读写一致性验证 |

## 核心不变量与生命周期

1. **唯一段所有权与统一后备对象**：
   - 共享内存段使用 `struct kernel_memory_object` 提供物理页后备，与匿名共享映射基础设施完全统一，按需分配物理页。
   - 段所有权由内核段表统一管理，MM registry 持有后备对象引用；每次逻辑附加另有 `kernel_shm_attachment` owner，持有后备引用、固定 shmid 身份和原始起止地址。VMA 片段拥有 attachment 引用。
   - `shmat` 给 MM registry 新取得的内存对象引用先由局部 owner 持有，再移动给 registry；分配或映射失败只消费临时 owner，不改变段表引用。`test-scale-riscv` 在附加元数据分配处注入 OOM，要求重试附加成功并最终回到资源基线。
2. **段标识符与代次序列隔离**：
   - 最大段数量由 `KERNEL_SHMMNI`（128）固定。
   - `shmid = slot_index + seq * KERNEL_SHMMNI`。
   - 段彻底销毁时对应槽位的 `seq` 计数自增，避免悬空 ID 与 ABA 别名冲突。
3. **延迟销毁语义（`IPC_RMID`）**：
   - 调用 `shmctl(shmid, IPC_RMID, NULL)` 时：
     - 若当前片段计数 `nattch == 0` 且逻辑 attachment owner 数 `attachments == 0`，立即释放物理页与内存对象，槽位复位。
     - 否则标记 `marked_for_deletion = 1`，并将 key 改为 `IPC_PRIVATE`，使 `IPC_STAT` 和 proc 列表与命名查找的撤销一致。当前后续对该段的 `shmat` 拒绝附加（返回 `-EIDRM`），`shmget` 不再能按原 key 查找到该段。已有附加的虚拟内存映射保持有效并可正常读写。Linux 允许对仍存活的 RMID 段再次附加；该既有限制仍存在。
     - 随 `shmdt`、`munmap`、进程 `exit` 或 `execve` 解除附加，当最后一个片段及临时 attachment owner 消失时，触发物理资源与段槽位的彻底释放。
4. **`IPC_STAT` 与 `SHM_DEST` 标志**：
   - 当段已被 `IPC_RMID` 标记删除但仍有附加时，`shmctl(..., IPC_STAT, ...)` 仍可成功查询，并在 `shm_perm.mode` 中反映 `SHM_DEST`（01000）标志位，符合 Linux 原生行为。

创建时保存调用者 effective UID/GID 到 owner 和 creator 字段。按 key 查找、shmat 和 IPC_STAT 分别检查所需读写/执行位，CAP_IPC_OWNER 允许越过访问位；IPC_SET/RMID 要求 owner、creator 或 CAP_SYS_ADMIN。IPC_SET 先完整导入用户结构，再检查对象及管理权限，成功后更新 owner/group/mode；数字 UID/GID 采用初始用户命名空间的有效值。

`/proc/sysvipc/shm` 与 `IPC_STAT` 共用段描述符，并从实际 memory object 读取 resident 页数，按客机页尺寸换算为字节；当前没有 swap 后备。仍被附加持有的 RMID 段保留列表项及 `SHM_DEST`，最后释放后撤下。列表读取与其他 IPC 列表的共同规则见 [procfs 模块](procfs.md)。

## 限额、用量与槽查询

IPC_INFO 输出当前配置，SHM_INFO 输出真实活跃段数、按客机页尺寸取整的总页数和后备对象已驻留页数；两者返回最高活跃槽位，空表返回0。当前无swap，交换页数和交换尝试/成功次数均为0。已RMID但仍有附件的段继续计入，最后使用者释放后从统计撤下。

IPC_STAT 按完整ID检查槽和代次；SHM_STAT/SHM_STAT_ANY按槽索引取得当前对象，返回带当前代次的完整ID。STAT执行访问位与CAP_IPC_OWNER检查，STAT_ANY按Linux提供与`/proc/sysvipc/shm`相同的枚举可见性。段表在既有CPU0控制位置受保护，查询先取得有界结构快照，用户复制在保护外完成；复制期间原段删除不会改变这次快照，短复制保留实际输出并返回EFAULT。

SHM_LOCK/UNLOCK的真实UID账户、MEMLOCK和延迟归还见[MM模块](kernel-mm.md#madvise-与共享段锁定)。查询直接消费者为`tests/workloads/resource/shm_query.c`，宿主4/16 KiB页检查与锁定回归共用`tests/shm-lock-host.sh`。

## VMA 与架构集成

命名 key 查找先检查已存在段、`CREAT|EXCL` 冲突及请求大小是否超过原段；`size=0` 可查找已有段。缺失 key 且无 `CREAT` 返回 `ENOENT`；只有新建段才检查 `SHMMIN/SHMMAX`。固定 ABI 依据为本地 `references/linux/ipc/shm.c` 的 `newseg`/`shm_more_checks` 与 `references/linux/ipc/util.c`，commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e`。

- VMA 使用 `KERNEL_VMA_KIND_SYSV_SHM` 与 `KERNEL_VMA_FAULT_ANON_SHARED`。
- `mm/vma.c` 的 `can_merge()` 显式禁止合并 SysV SHM VMA，确保各附加段的边界、起始地址与生命周期独立。
- `mm/vma.c` 在成功插入、split、fork、remove/replace 和 destroy 时统一调用 attachment open/close；prepare 不改变引用，commit 不再分配。`nattch` 是已提交 VMA 片段数，不是每进程或每次 `shmat` 固定计一。临时 attachment owner 保住槽身份但不增加 `nattch`，所有权错误触发 fatal。
- `shmdt` 从原始附加地址查找稳定 attachment，即使首段已撤销也能移除全部剩余片段；不会移除洞内被匿名、文件或其他 SHM 附加替换的新映射。fork 只为继承的 VMA 增引用，DONTFORK 片段不进入子 MM；后续 fork OOM 经统一 destroy 回滚。
- `SHM_RDONLY` 的 VMA 上限为 RX，禁止经 `mprotect` 提升 WRITE；当前权限与上限分开，允许先降到 NONE 再恢复 READ。分裂和 fork 保留该限制。
- 页表 fork 的 SHARED 决定保留 SysV 共享叶的写权限；DONTFORK 对应 SKIP，既不创建子叶，也不对父叶施加 COW。

## 验证与回归

- **裸机规模与生命周期测试**：`make test-scale-riscv` 覆盖 `IPC_PRIVATE` 分配、跨地址多 attach、读写数据比对、`IPC_STAT`、`IPC_RMID` 延迟销毁、命名 key 冲突与消除，并验证进程退出后 `physical_page_available == baseline` 零泄漏。
- **Linux 差分 ABI 测试**：`make test-diff-abi-riscv` 覆盖 key/size/flags、片段计数/替换以及只读附件改权；截至权限上限阶段，1077 条全套记录与参考 Linux 内核一致。
- **真实 musl U-mode 测试**：`make test-userland-riscv` 在真实 musl libc 运行环境下验证跨 fork 数据传递、`shmdt`、销毁后再次 attach 拒绝等完整场景。
- **栈预算校验**：`make test-stack-usage` 确保所有新增函数均在栈安全边界内。
