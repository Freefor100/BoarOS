#include <kernel/raw_lock.h>
_Static_assert(__atomic_always_lock_free(4, 0), "raw locks require inline 32-bit atomics");

void kernel_raw_lock_init(struct kernel_raw_lock *lock, uint32_t rank)
{
    if (!lock || !rank) __builtin_trap();
    *lock = (struct kernel_raw_lock){ .rank = rank, .initialized = KERNEL_RAW_INITIALIZED };
}
static inline uint64_t raw_enter(struct kernel_raw_lock *lock, struct kernel_raw_guard *guard)
{
    uintptr_t irq = arch_interrupt_save();
    struct kernel_cpu *cpu = kernel_cpu_current();
    if (!lock || !guard || guard->lock || lock->initialized != KERNEL_RAW_INITIALIZED ||
        cpu->preempt_depth == UINT32_MAX) __builtin_trap();
#if BOAROS_LOCK_DEBUG
    for (struct kernel_raw_guard *held = cpu->raw_locks; held; held = held->previous)
        if (held->lock == lock) __builtin_trap();
    if (cpu->raw_locks) {
        struct kernel_raw_lock *outer = cpu->raw_locks->lock;
        if (outer->rank > lock->rank ||
            (outer->rank == lock->rank && (uintptr_t)outer >= (uintptr_t)lock)) __builtin_trap();
    }
#endif
    guard->interrupts = irq;
    guard->previous_depth = cpu->preempt_depth++;
#if BOAROS_COST_DIAGNOSTICS
    guard->cost = kernel_cost_capture();
    uint64_t wait_start = guard->cost.epoch ? kernel_cost_clock() : 0;
#endif
    guard->cpu = cpu;
#if BOAROS_COST_DIAGNOSTICS
    return wait_start;
#else
    return 0;
#endif
}
static inline void raw_acquired(struct kernel_raw_lock *lock, struct kernel_raw_guard *guard,
    uint64_t wait_start)
{
#if BOAROS_COST_DIAGNOSTICS
    if (guard->cost.epoch) {
        guard->hold_start = kernel_cost_clock();
        guard->wait_ticks = guard->hold_start - wait_start;
    }
#else
    (void)wait_start;
#endif
    guard->lock = lock;
    guard->previous = guard->cpu->raw_locks;
    guard->cpu->raw_locks = guard;
}
void kernel_raw_lock_acquire(struct kernel_raw_lock *lock, struct kernel_raw_guard *guard)
{
    uint64_t wait_start = raw_enter(lock, guard);
#if BOAROS_MAX_CPUS > 1
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
#else
    if (lock->word) __builtin_trap();
    lock->word = 1;
    __asm__ volatile("" ::: "memory");
#endif
    raw_acquired(lock, guard, wait_start);
}
static inline void raw_validate_release(struct kernel_raw_guard *guard)
{
    struct kernel_cpu *cpu = kernel_cpu_current();
    if (!guard || !guard->lock || guard->cpu != cpu || cpu->raw_locks != guard ||
        !cpu->preempt_depth || guard->previous_depth == UINT32_MAX ||
        cpu->preempt_depth != guard->previous_depth + 1U) __builtin_trap();
}
static inline void raw_release_owner(struct kernel_raw_guard *guard)
{
    guard->cpu->raw_locks = guard->previous;
    guard->cpu->preempt_depth = guard->previous_depth;
}
static inline void raw_release_finish(struct kernel_raw_guard *guard, unsigned rank, uint64_t hold)
{
    struct kernel_cpu *cpu = guard->cpu;
    guard->lock = 0;
#if BOAROS_MAX_CPUS > 1
    /* outbound belongs to this CPU until IRQ restore; empty queues need no
     * firmware dispatch or scan of the other CPUs. */
    if (!cpu->raw_locks && cpu->outbound) kernel_cpu_flush_notifications();
#else
    (void)cpu;
#endif
    arch_interrupt_restore(guard->interrupts);
#if BOAROS_COST_DIAGNOSTICS
    if (guard->cost.epoch) kernel_cost_raw_lock(guard->cost, rank, guard->wait_ticks, hold);
#else
    (void)rank; (void)hold;
#endif
}
void kernel_raw_lock_release(struct kernel_raw_guard *guard)
{
    raw_validate_release(guard);
    if (__atomic_load_n(&guard->lock->word, __ATOMIC_RELAXED) != 1U) __builtin_trap();
    unsigned rank = 0;
    uint64_t hold = 0;
#if BOAROS_COST_DIAGNOSTICS
    rank = guard->lock->rank;
    hold = guard->cost.epoch ? kernel_cost_clock() - guard->hold_start : 0;
#endif
    raw_release_owner(guard);
#if BOAROS_MAX_CPUS > 1
    __atomic_store_n(&guard->lock->word, 0U, __ATOMIC_RELEASE);
#else
    __asm__ volatile("" ::: "memory");
    guard->lock->word = 0;
#endif
    raw_release_finish(guard, rank, hold);
}
#if BOAROS_MAX_CPUS > 1
void kernel_raw_rwlock_init(struct kernel_raw_rwlock *lock, uint32_t rank)
{
    if (!lock) __builtin_trap();
    *lock = (struct kernel_raw_rwlock){0};
    kernel_raw_lock_init(&lock->writer, rank);
}
void kernel_raw_rwlock_write_acquire(struct kernel_raw_rwlock *lock, struct kernel_raw_guard *guard)
{
    if (!lock) __builtin_trap();
    uint64_t wait_start = raw_enter(&lock->writer, guard);
    uint32_t *word = &lock->writer.word;
    uint32_t previous;
    do {
        while (__atomic_load_n(word, __ATOMIC_RELAXED) & 1U) {}
        previous = __atomic_fetch_or(word, 1U, __ATOMIC_ACQUIRE);
    } while (previous & 1U);
    /* Readers admitted before our RMW drain with release RMWs. New
     * readers cannot add to the count while bit 0 is set. */
    if (previous) while (__atomic_load_n(word, __ATOMIC_ACQUIRE) != 1U) {}
    raw_acquired(&lock->writer, guard, wait_start);
}
void kernel_raw_rwlock_read_acquire(struct kernel_raw_rwlock *lock, struct kernel_raw_read_guard *guard)
{
    if (!lock || !guard) __builtin_trap();
    uint64_t wait_start = raw_enter(&lock->writer, &guard->raw);
    uint32_t *word = &lock->writer.word;
    uint32_t state = __atomic_load_n(word, __ATOMIC_RELAXED);
    for (;;) {
        while (state & 1U) state = __atomic_load_n(word, __ATOMIC_RELAXED);
        if (state == UINT32_MAX - 1U) __builtin_trap();
        if (__atomic_compare_exchange_n(word, &state, state + 2U, 1,
            __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) break;
    }
    raw_acquired(&lock->writer, &guard->raw, wait_start);
}
void kernel_raw_rwlock_read_release(struct kernel_raw_read_guard *guard)
{
    if (!guard) __builtin_trap();
    struct kernel_raw_guard *raw = &guard->raw;
    raw_validate_release(raw);
    unsigned rank = 0;
    uint64_t hold = 0;
#if BOAROS_COST_DIAGNOSTICS
    rank = raw->lock->rank;
    hold = raw->cost.epoch ? kernel_cost_clock() - raw->hold_start : 0;
#endif
    raw_release_owner(raw);
    /* The last release may let a writer retire its containing object. Nothing
     * below this decrement may dereference lock, including cost attribution. */
    if (__atomic_fetch_sub(&raw->lock->word, 2U, __ATOMIC_RELEASE) < 2U) __builtin_trap();
    raw_release_finish(raw, rank, hold);
}
#endif
