#ifndef BOAROS_KERNEL_MM_BACKEND_H
#define BOAROS_KERNEL_MM_BACKEND_H
#include <arch/mmu.h>
#include <kernel/mm.h>
/* Successful creation consumes one backend space; failure preserves it. */
enum kernel_mm_status kernel_mm_create(struct kernel_mm *, struct arch_mmu_user_space *);
enum kernel_mm_status kernel_mm_context(const struct kernel_mm *, uint64_t *);
enum kernel_mm_status kernel_mm_concurrent_context(const struct kernel_mm *, uint64_t *);
/* 调度域外调用；next拥有MM引用，旧执行owner覆盖旧record借用。
 * NULL选择稳定kernel_context；并发域即使同SATP也做本地失效/取指同步。 */
enum kernel_mm_status kernel_mm_activate(const struct kernel_mm *next, uint64_t kernel_context);
/* CPU0私有构造完成后开放匿名并发域；须独占、无文件/外部后备及压力回调。
 * 共享匿名对象须属于同heap/allocator且无budget；tmpfs/SysV业务仍不开放。
 * CPU1仅接受任务已绑定的同一记录；这不是普通用户态SMP开关。 */
enum kernel_mm_status kernel_mm_enable_concurrency(struct kernel_mm *);
/* 私有构造期开放远端用户执行，保留普通CPU0业务及后备/压力契约。
 * 不启用并行内核修改gate；已有远端能力可重复核对而不改变owner。 */
enum kernel_mm_status kernel_mm_enable_remote_user_execution(struct kernel_mm *);
struct kernel_mm_statistics {
    uint64_t resident_probes, protect_visits;
    uint64_t protect_address_flushes, protect_global_flushes;
};
void kernel_mm_get_statistics(const struct kernel_mm *, struct kernel_mm_statistics *);
struct kernel_mm_concurrency_state {
    uint32_t references, users, pins, write_pins, active_cpus, pending_retirement;
    uint32_t record_bytes, pin_bytes;
    uint64_t generation, pin_acquisitions, prepared_pages, fault_retries;
    uint64_t retirement_batches, retired_pages, pin_waits;
};
/* 持LIVE MM owner的值快照，不能在调度锁内调用；不新增引用。 */
void kernel_mm_get_concurrency_state(const struct kernel_mm *, struct kernel_mm_concurrency_state *);
#endif
