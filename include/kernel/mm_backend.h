#ifndef BOAROS_KERNEL_MM_BACKEND_H
#define BOAROS_KERNEL_MM_BACKEND_H
#include <arch/mmu.h>
#include <kernel/mm.h>
/* Successful creation consumes one backend space; failure preserves it. */
enum kernel_mm_status kernel_mm_create(struct kernel_mm *, struct arch_mmu_user_space *);
void kernel_mm_drain_sources(const struct kernel_mm *);
enum kernel_mm_status kernel_mm_context(const struct kernel_mm *, uint64_t *);
/* 调度域外调用；next拥有MM引用，旧执行owner覆盖旧record借用。
 * NULL选择稳定kernel_context；并发域即使同SATP也做本地失效/取指同步。 */
enum kernel_mm_status kernel_mm_activate(const struct kernel_mm *next, uint64_t kernel_context);
/* 受控内核任务在创建处检查支持的后备消费者；不切换MM同步模型。 */
enum kernel_mm_status kernel_mm_check_task_domain(const struct kernel_mm *);
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
