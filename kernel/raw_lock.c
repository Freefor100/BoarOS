#include <kernel/raw_lock.h>
#define KERNEL_RAW_INITIALIZED UINT32_C(0x52415731)
_Static_assert(__atomic_always_lock_free(4, 0), "raw locks require inline 32-bit atomics");

void kernel_raw_lock_init(struct kernel_raw_lock *lock, uint32_t rank)
{
    if (!lock || !rank) __builtin_trap();
    *lock = (struct kernel_raw_lock){ .rank = rank, .initialized = KERNEL_RAW_INITIALIZED };
}
void kernel_raw_lock_acquire(struct kernel_raw_lock *lock, struct kernel_raw_guard *guard)
{
    uintptr_t irq = arch_interrupt_save();
    struct kernel_cpu *cpu = kernel_cpu_current();
    if (!lock || !guard || guard->lock || lock->initialized != KERNEL_RAW_INITIALIZED ||
        cpu->preempt_depth == UINT32_MAX) __builtin_trap();
    for (struct kernel_raw_guard *held = cpu->raw_locks; held; held = held->previous)
        if (held->lock == lock) __builtin_trap();
    if (cpu->raw_locks) {
        struct kernel_raw_lock *outer = cpu->raw_locks->lock;
        if (outer->rank > lock->rank ||
            (outer->rank == lock->rank && (uintptr_t)outer >= (uintptr_t)lock)) __builtin_trap();
    }
    guard->interrupts = irq;
    guard->previous_depth = cpu->preempt_depth++;
#if BOAROS_COST_DIAGNOSTICS
    guard->cost = kernel_cost_capture();
    uint64_t wait_start = guard->cost.epoch ? kernel_cost_clock() : 0;
#endif
    /* 争用时先轮询，避免每次重试都向共享cacheline发出写操作。 */
    uint32_t observed;
    do {
        do {
            observed = __atomic_load_n(&lock->word, __ATOMIC_RELAXED);
            if (observed > 1U) __builtin_trap();
        } while (observed);
        observed = __atomic_exchange_n(&lock->word, 1U, __ATOMIC_ACQUIRE);
        if (observed > 1U) __builtin_trap();
    } while (observed);
#if BOAROS_COST_DIAGNOSTICS
    if (guard->cost.epoch) {
        guard->hold_start = kernel_cost_clock();
        guard->wait_ticks = guard->hold_start - wait_start;
    }
#endif
    guard->lock = lock;
    guard->cpu = cpu;
    guard->previous = cpu->raw_locks;
    cpu->raw_locks = guard;
}
void kernel_raw_lock_release(struct kernel_raw_guard *guard)
{
    struct kernel_cpu *cpu = kernel_cpu_current();
    if (!guard || !guard->lock || guard->cpu != cpu || cpu->raw_locks != guard ||
        !cpu->preempt_depth || guard->previous_depth == UINT32_MAX ||
        cpu->preempt_depth != guard->previous_depth + 1U ||
        __atomic_load_n(&guard->lock->word, __ATOMIC_RELAXED) != 1U) __builtin_trap();
#if BOAROS_COST_DIAGNOSTICS
    uint64_t hold = guard->cost.epoch ? kernel_cost_clock() - guard->hold_start : 0;
#endif
    cpu->raw_locks = guard->previous;
    cpu->preempt_depth = guard->previous_depth;
    __atomic_store_n(&guard->lock->word, 0U, __ATOMIC_RELEASE);
    guard->lock = 0;
    arch_interrupt_restore(guard->interrupts);
#if BOAROS_COST_DIAGNOSTICS
    if (guard->cost.epoch) kernel_cost_raw_lock(guard->cost, guard->wait_ticks, hold);
#endif
}
