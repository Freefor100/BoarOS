# System V 共享内存（SysV SHM）模块

本文描述 BoarOS 中 System V 共享内存子系统（`shmget`、`shmctl`、`shmat`、`shmdt`）的实现契约、生命周期管理与内存管理集成。

## 范围与入口

| 文件 | 职责 |
|---|---|
| `include/kernel/shm.h` | Linux UAPI 数据结构（`struct ipc64_perm`, `struct shmid64_ds`）、常数、内核段结构与接口声明 |
| `mm/shm.c` | 全局段表、`IPC_PRIVATE` 与命名 key 映射、代次 sequence、延迟销毁（`IPC_RMID`）与 `nattch` 引用计数 |
| `kernel/syscall/shm.c` | 系统调用参数解码、用户指针有效性校验与 Linux ABI 返回值映射 |
| `include/kernel/vma.h`、`mm/vma.c` | `KERNEL_VMA_KIND_SYSV_SHM` VMA 描述符定义与不合并（no-merge）约束 |
| `include/kernel/mm.h`、`arch/riscv/mm.c` | `kernel_mm_shmat`/`kernel_mm_shmdt`、Sv39 共享按需缺页、fork 继承及 munmap/release 清理钩子 |
| `tests/riscv/scale_main.c` | 裸机内核规模测试中的 SysV SHM 生命周期与多附加验证 |
| `tests/diff-abi/shm.c` | 针对固定 Linux 参考内核的双侧差分 ABI 测试 |
| `tests/userland/sysv_shm.h` | 真实 musl 用户态多进程 fork 与读写一致性验证 |

## 核心不变量与生命周期

1. **唯一段所有权与统一后备对象**：
   - 共享内存段使用 `struct kernel_memory_object` 提供物理页后备，与匿名共享映射基础设施完全统一，按需分配物理页。
   - 段所有权由内核段表统一管理，VMA 持有对 `kernel_memory_object` 的后备引用与对 `struct kernel_shm_segment` 的附加引用。
2. **段标识符与代次序列隔离**：
   - 最大段数量由 `KERNEL_SHMMNI`（128）固定。
   - `shmid = slot_index + seq * KERNEL_SHMMNI`。
   - 段彻底销毁时对应槽位的 `seq` 计数自增，避免悬空 ID 与 ABA 别名冲突。
3. **延迟销毁语义（`IPC_RMID`）**：
   - 调用 `shmctl(shmid, IPC_RMID, NULL)` 时：
     - 若当前附加计数 `nattch == 0`，立即释放物理页与内存对象，槽位复位。
     - 若当前 `nattch > 0`，标记 `marked_for_deletion = 1`。后续对该段的 `shmat` 拒绝附加（返回 `-EINVAL`），`shmget` 不再能按 key 查找到该段。已有附加的虚拟内存映射保持有效并可正常读写。
     - 随 `shmdt`、`munmap`、进程 `exit` 或 `execve` 接触附加，当最后一次附加脱落且 `nattch == 0` 时，触发物理资源与段槽位的彻底释放。
4. **`IPC_STAT` 与 `SHM_DEST` 标志**：
   - 当段已被 `IPC_RMID` 标记删除但仍有附加时，`shmctl(..., IPC_STAT, ...)` 仍可成功查询，并在 `shm_perm.mode` 中反映 `SHM_DEST`（01000）标志位，符合 Linux 原生行为。

## VMA 与架构集成

- VMA 使用 `KERNEL_VMA_KIND_SYSV_SHM` 与 `KERNEL_VMA_FAULT_ANON_SHARED`。
- `mm/vma.c` 的 `can_merge()` 显式禁止合并 SysV SHM VMA，确保各附加段的边界、起始地址与生命周期独立。
- `arch/riscv/mm.c` 在 `kernel_mm_fork()` 时遍历继承的 VMA 递增 `segment->nattch`；在 `kernel_mm_munmap()` 与 `kernel_mm_release()` 时对被移除的 SysV SHM VMA 调用 `kernel_shm_on_vma_detach()` 递减 `nattch` 并触发末引用清理。
- `shared_anon_leaf()` 将 `KERNEL_VMA_KIND_SYSV_SHM` 视作共享叶子页表项，`kernel_mm_fork()` 克隆页表时共享 PTE 而不作写保护。

## 验证与回归

- **裸机规模与生命周期测试**：`make test-scale-riscv` 覆盖 `IPC_PRIVATE` 分配、跨地址多 attach、读写数据比对、`IPC_STAT`、`IPC_RMID` 延迟销毁、命名 key 冲突与消除，并验证进程退出后 `physical_page_available == baseline` 零泄漏。
- **Linux 差分 ABI 测试**：`make test-diff-abi-riscv` 中 19 个 SysV SHM 测例（`shm.*`）与参考 Linux 内核比对，1031 条记录 100% 一致。
- **真实 musl U-mode 测试**：`make test-userland-riscv` 在真实 musl libc 运行环境下验证跨 fork 数据传递、`shmdt`、销毁后再次 attach 拒绝等完整场景。
- **栈预算校验**：`make test-stack-usage` 确保所有新增函数均在栈安全边界内。
