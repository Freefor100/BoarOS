# 内核 MM 模块

本文描述任务地址空间的通用所有权接口和当前 RISC-V Sv39 后端。页表格式、映射规则和硬件切换见 [RISC-V Sv39 分页模块](riscv-sv39.md)，地址空间与任务身份分离的背景见[内存管理学习总结](../learning/memory-management.md)。

## 范围与入口

| 文件 | 当前职责 |
|---|---|
| `include/kernel/mm.h` | 定义跨架构 MM 句柄、权限、状态和引用操作 |
| `include/arch/riscv/mm.h`、`arch/riscv/mm.c` | 用一张记录页封装 Sv39 用户地址空间，并实现当前构建所选的通用 MM 操作 |
| `tests/riscv/mm_cases.c`、`tests/mm-riscv.sh` | 验证创建、共享引用、移动、查询和可重试回收 |

稳定接口为：

```c
enum kernel_mm_status riscv_kernel_mm_create(
    struct kernel_mm *mm,
    struct riscv_sv39_user_space *space);

enum kernel_mm_status kernel_mm_acquire(
    struct kernel_mm *destination,
    const struct kernel_mm *source);

enum kernel_mm_status kernel_mm_fork(
    struct kernel_mm *destination,
    const struct kernel_mm *source);

enum kernel_mm_status kernel_mm_move(
    struct kernel_mm *destination,
    struct kernel_mm *source);

enum kernel_mm_status kernel_mm_lookup(
    const struct kernel_mm *mm,
    uint64_t virtual_address,
    struct kernel_mm_mapping *mapping);

enum kernel_mm_status kernel_mm_release(struct kernel_mm *mm);
```

`riscv_kernel_mm_satp()` 是 RISC-V scheduler 创建任务时使用的架构入口。通用层不暴露 Sv39 对象，也没有运行期 vtable；RISC-V 构建直接链接 RISC-V 实现，LoongArch 构建以后为同一通用接口提供 16 KiB/三级页表后端。这样任务和 syscall 层依赖 MM 语义，而不依赖页表格式，调用热路径也没有间接分派。

## 句柄、引用与状态

`struct kernel_mm` 是一个可移动的拥有型引用，不是地址空间本体。RISC-V 后端用 `record_page_address` 指向一张物理记录页；记录页保存引用计数、清理阶段和唯一的 `riscv_sv39_user_space`。多个 LIVE 句柄可以指向同一记录页：

- `acquire` 增加引用计数并发布一个新的独立 owner；
- `fork` 创建独立地址空间，复制用户映射、页内容和权限，不共享用户叶子页；
- `move` 转移一个 owner，不改变引用计数；
- `release` 消耗一个 owner，非末引用只减计数，末引用才销毁页表树和记录页；
- `lookup` 把架构权限翻译为 `KERNEL_MM_READ/WRITE/EXECUTE/USER`，不让通用调用者依赖 RISC-V PTE 位值。

输入目标必须是全零 `EMPTY` 句柄。成功移动后源进入 `MOVED`，成功释放后进入 `RELEASED`；二者都不再拥有资源。失败时不会凭错误码暗示所有权，句柄状态仍精确说明调用者应重试还是继续持有。

RISC-V 创建先分配并解析记录页，最后才把 LIVE Sv39 空间移入记录；成功后输入空间成为 MOVED。记录页已经分配但无法立即释放时，输出成为 CLEANUP owner，从而不会遗失唯一物理地址。

## fork 与后续 COW 边界

`kernel_mm_fork()` 当前使用 eager copy：遍历源 Sv39 用户树，为每个 4 KiB 用户叶子分配新物理页并复制内容，同时重建相同 VA 和权限的私有页表。内核高半区根项仍只借用稳定内核映射，不复制也不参与子 MM 的释放。成功后父子地址空间初始字节相同，但任一方写入都不会改变另一方；源 MM 始终保持 LIVE。

创建失败时，目标句柄要么仍为 EMPTY，要么成为持有精确剩余 owner 的 CLEANUP；调用者必须按状态继续释放，不能只按错误码假定没有分配。测试同时核对映射、物理地址不等、双向写隔离、父先释放后子仍可用以及最终页数回到基线。

Eager copy 的时间和新增物理内存都与已提交用户页数线性相关，当前还在关中断的 clone 临界区内执行，因此大进程 fork 会增加中断延迟并产生明显复制开销。这个实现建立的是正确的普通进程语义；以后可在同一 `kernel_mm_fork()` 接口内改为 COW：父子叶子改成共享只读引用，写 fault 再复制，并补充页引用、PTE 原子更新、TLB shootdown 与失败回滚。Scheduler 和 syscall ABI 不需要因 COW 改变。

## 末引用回收

末引用只能在其 `satp` 已不活动时释放。回收按两个阶段执行：

```text
LIVE(last ref)
  -> destroy private user leaves and page tables
  -> release MM record page
  -> RELEASED
```

Sv39 销毁一旦发生部分提交，MM 转入 `CLEANUP_SPACE`；后续调用只继续地址空间回收。地址空间已经销毁而记录页释放失败时转入 `CLEANUP_RECORD`；后续调用只重试记录页。两种 CLEANUP 都可 move，不能 acquire、lookup 或生成 `satp`。非末引用释放不触碰页表树。

用户退出路径先切到内核根页表，再在仍有效的任务内核栈上调用 `kernel_mm_release()`；失败 owner 交给 idle reaper 在后续重试。因此不会销毁硬件当前仍在使用的用户根。MM 完成后才允许任务成为 zombie，zombie 只保留身份、亲缘、wait status 和任务页。

## 并发与性能边界

当前引用计数和记录内容由单 hart、关 SIE 的 scheduler 生命周期串行化，不是 SMP 原子操作。接入 SMP 时必须在 MM 引用和末引用判定处加入锁或原子协议，并与页表修改、TLB shootdown 协调；公共句柄接口不需要因此改变。

Scheduler 在用户任务创建时解析 MM、验证入口与栈权限，并缓存目标 `satp`。tick、current 校验和 context switch 直接读取任务前缀中的缓存，不解析记录页、不增减 MM 引用，也不遍历用户页表。地址空间切换的当前主要成本仍是 ASID 0 下的 `satp`/全局 `SFENCE.VMA`。

## 验证与限制

```sh
make test-mm-riscv
make test-user-riscv
make test-user-elf-riscv
make test-riscv
```

MM 聚焦测试覆盖创建失败原子性、共享引用、移动、eager fork 的内容/权限复制与写隔离，以及页表部分回收、记录页访问/释放失败后的阶段化重试。用户与生产进程测试覆盖 scheduler 接管、父子分别退出和最终物理页计数复原。

当前只有 RISC-V 后端；映射仍由 Sv39/4 KiB 用户页实现。普通 clone 已使用独立 MM，但尚无 COW 或 `CLONE_VM` 共享进程；文件表、信号处理表和其他进程资源不属于 MM。
