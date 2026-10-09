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
    /* 争用时先轮询，避免每次重试都向共享cacheline发出写操作。 */
    do {
        while (__atomic_load_n(&lock->word, __ATOMIC_RELAXED)) {}
    } while (__atomic_exchange_n(&lock->word, 1U, __ATOMIC_ACQUIRE));
    guard->lock = lock;
    guard->cpu = cpu;
    guard->previous = cpu->raw_locks;
    cpu->raw_locks = guard;
}
void kernel_raw_lock_release(struct kernel_raw_guard *guard)
{
    struct kernel_cpu *cpu = kernel_cpu_current();
    if (!guard || !guard->lock || guard->cpu != cpu || cpu->raw_locks != guard ||
        cpu->preempt_depth != guard->previous_depth + 1U ||
        __atomic_load_n(&guard->lock->word, __ATOMIC_RELAXED) != 1U) __builtin_trap();
    cpu->raw_locks = guard->previous;
    cpu->preempt_depth = guard->previous_depth;
    __atomic_store_n(&guard->lock->word, 0U, __ATOMIC_RELEASE);
    guard->lock = 0;
    arch_interrupt_restore(guard->interrupts);
}
