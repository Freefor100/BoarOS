#ifndef BOAROS_KERNEL_RAW_LOCK_H
#define BOAROS_KERNEL_RAW_LOCK_H
#include <kernel/cpu.h>
#include <kernel/cost.h>
#include <stdint.h>
#define KERNEL_RAW_INITIALIZED UINT32_C(0x52415731)

enum kernel_raw_rank {
    KERNEL_RAW_RANK_HEAP = 10, KERNEL_RAW_RANK_MM = 15,
    KERNEL_RAW_RANK_PAGE = 20,
    KERNEL_RAW_RANK_OBJECT = 30, KERNEL_RAW_RANK_FILE_MAPPING = 35,
    KERNEL_RAW_RANK_SCHEDULER = 40
};
/* 初始化/移动仅允许未发布实例；UP 以中断和抢占嵌套互斥，SMP 使用原子字。
 * CPU 持锁链总是保留；线性递归与等级审计只在 LOCK_DEBUG 中执行。 */
struct kernel_raw_lock { uint32_t word, rank, initialized; };
struct kernel_raw_guard {
    struct kernel_raw_lock *lock;
    struct kernel_cpu *cpu;
    struct kernel_raw_guard *previous;
    uintptr_t interrupts;
    uint32_t previous_depth;
#if BOAROS_COST_DIAGNOSTICS
    struct kernel_cost_tag cost;
    uint64_t wait_ticks, hold_start;
#endif
};
void kernel_raw_lock_init(struct kernel_raw_lock *, uint32_t rank);
void kernel_raw_lock_acquire(struct kernel_raw_lock *, struct kernel_raw_guard *);
void kernel_raw_lock_release(struct kernel_raw_guard *);
static inline void kernel_raw_scope_release(struct kernel_raw_guard *guard)
{ if (guard->lock) kernel_raw_lock_release(guard); }
#define KERNEL_RAW_SCOPE(name, object) \
    struct kernel_raw_guard name __attribute__((cleanup(kernel_raw_scope_release))); \
    name.lock = 0; \
    kernel_raw_lock_acquire((object), &name)

/* Object state used only by tasks needs no IRQ masking in a UP build.
 * IRQ-shared state also masks local interrupts. Both reject blocking and retain
 * full raw ownership/rank/cost records in SMP or diagnostic configurations. */
#if BOAROS_MAX_CPUS > 1 || BOAROS_LOCK_DEBUG || BOAROS_COST_DIAGNOSTICS
typedef struct kernel_raw_guard kernel_task_lock_guard;
typedef struct kernel_raw_guard kernel_irq_lock_guard;
#define kernel_task_lock_acquire kernel_raw_lock_acquire
#define kernel_task_lock_release kernel_raw_lock_release
#define kernel_irq_lock_acquire kernel_raw_lock_acquire
#define kernel_irq_lock_release kernel_raw_lock_release
#define KERNEL_TASK_LOCK_SCOPE KERNEL_RAW_SCOPE
#define KERNEL_IRQ_LOCK_SCOPE KERNEL_RAW_SCOPE
#else
typedef struct {
    struct kernel_cpu *cpu;
    uint32_t previous_depth;
} kernel_task_lock_guard;
static inline void kernel_task_lock_acquire(struct kernel_raw_lock *lock,
    kernel_task_lock_guard *guard)
{
    struct kernel_cpu *cpu=kernel_cpu_current();
    if (!lock || guard->cpu ||
        cpu->preempt_depth==UINT32_MAX) __builtin_trap();
    guard->cpu=cpu;
    guard->previous_depth=cpu->preempt_depth++;
    /* UP has no competing CPU. Preemption exclusion protects task-only state;
     * IRQ-shared callers also mask interrupts. No per-object lock word is
     * needed. Full raw ownership diagnostics remain in SMP/debug builds. */
    __asm__ volatile("" ::: "memory");
}
static inline void kernel_task_lock_release(kernel_task_lock_guard *guard)
{
    struct kernel_cpu *cpu=guard->cpu;
    if (!cpu || guard->previous_depth==UINT32_MAX ||
        cpu->preempt_depth!=guard->previous_depth+1U)
        __builtin_trap();
    guard->cpu=0;
    __asm__ volatile("" ::: "memory");
    cpu->preempt_depth=guard->previous_depth;
}
static inline void kernel_task_lock_scope_release(kernel_task_lock_guard *guard)
{ if (guard->cpu) kernel_task_lock_release(guard); }
#define KERNEL_TASK_LOCK_SCOPE(name, object) \
    kernel_task_lock_guard name __attribute__((cleanup(kernel_task_lock_scope_release))); \
    name.cpu=0; \
    kernel_task_lock_acquire((object),&name)
typedef struct {
    kernel_task_lock_guard task;
    uintptr_t interrupts;
} kernel_irq_lock_guard;
static inline void kernel_irq_lock_acquire(struct kernel_raw_lock *lock,
    kernel_irq_lock_guard *guard)
{
    uintptr_t irq=arch_interrupt_save();
    kernel_task_lock_acquire(lock,&guard->task);
    guard->interrupts=irq;
}
static inline void kernel_irq_lock_release(kernel_irq_lock_guard *guard)
{
    kernel_task_lock_release(&guard->task);
    arch_interrupt_restore(guard->interrupts);
}
static inline void kernel_irq_lock_scope_release(kernel_irq_lock_guard *guard)
{ if (guard->task.cpu) kernel_irq_lock_release(guard); }
#define KERNEL_IRQ_LOCK_SCOPE(name, object) \
    kernel_irq_lock_guard name __attribute__((cleanup(kernel_irq_lock_scope_release))); \
    name.task.cpu=0; \
    kernel_irq_lock_acquire((object),&name)
#endif
#endif
