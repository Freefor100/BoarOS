#include <arch/context.h>
#include <kernel/sync.h>
#include <kernel/wait_internal.h>

struct kernel_lock_waiter {
    struct kernel_lock_waiter *next;
    struct kernel_io_context *owner;
    unsigned write, granted;
    struct kernel_wait_token token;
#if BOAROS_COST_DIAGNOSTICS
    uint64_t grant_ticks;
#endif
};
void kernel_rwlock_init(struct kernel_rwlock *lock, uint32_t rank, uintptr_t key)
{
    *lock = (struct kernel_rwlock){ .rank = rank, .key = key };
    kernel_wait_queue_init(&lock->waiters);
    kernel_raw_lock_init(&lock->metadata, KERNEL_RAW_RANK_OBJECT);
}
static int acquire(struct kernel_rwlock *lock, struct kernel_lock_guard *guard, int write, int try_only)
{
    kernel_assert_can_block();
    COST_SCOPE(acquire_cost, OPERATION_TICKS);
    uintptr_t irq = arch_interrupt_save();
    struct kernel_io_context *owner = kernel_io_context_current();
    if (!lock || !guard || guard->lock || !lock->waiters.initialized) __builtin_trap();
    for (struct kernel_lock_guard *held = owner->locks; held; held = held->previous)
        if (held->lock == lock) __builtin_trap();
    if (owner->locks) {
        const struct kernel_rwlock *outer = owner->locks->lock;
        if (outer->rank > lock->rank ||
            (outer->rank == lock->rank && outer->key >= lock->key)) __builtin_trap();
    }
#if BOAROS_COST_DIAGNOSTICS
    unsigned rank = kernel_cost_rank(lock->rank);
    enum kernel_cost_metric metric = (enum kernel_cost_metric)(COST_LOCK10_ATTEMPTS + rank*8);
    uint64_t wait_start = kernel_cost_clock();
    uint64_t hold_start = wait_start;
    kernel_cost_add(metric,1);
#endif
    struct kernel_raw_guard metadata = {0};
    kernel_raw_lock_acquire(&lock->metadata, &metadata);
    if (try_only && (lock->writer || lock->pending_head || lock->handoff_owner)) {
        kernel_raw_lock_release(&metadata);
        arch_interrupt_restore(irq);
        return 0;
    }
    struct kernel_lock_waiter waiter = {.owner = owner, .write = write};
    if (lock->writer || lock->pending_head || lock->handoff_owner || (write && lock->readers)) {
        if (kernel_wait_prepare(&lock->waiters, 0, 0, &waiter.token) != KERNEL_SCHEDULER_STATUS_OK)
            __builtin_trap();
        if (lock->pending_tail) lock->pending_tail->next = &waiter;
        else lock->pending_head = &waiter;
        lock->pending_tail = &waiter;
        if (write) {
            if (lock->writers_waiting == UINT32_MAX) __builtin_trap();
            lock->writers_waiting++;
        }
    } else {
        waiter.granted = 1;
        if (write) lock->writer = owner;
        else {
            if (lock->readers == UINT32_MAX) __builtin_trap();
            lock->readers++;
        }
#if BOAROS_COST_DIAGNOSTICS
        waiter.grant_ticks = hold_start;
#endif
    }
    while (!waiter.granted) {
        struct kernel_wait_token parked = waiter.token;
        kernel_raw_lock_release(&metadata);
        enum kernel_wait_wake_reason reason;
#if BOAROS_COST_DIAGNOSTICS
        struct kernel_cost_task *task = kernel_cost_current();
        if (task) {
            if (task->wait_flags & 1) kernel_cost_add((enum kernel_cost_metric)(metric+4),1);
            task->wait_flags &= (uint8_t)~1U; task->wait_rank = (uint8_t)(rank+1);
        }
        kernel_cost_add((enum kernel_cost_metric)(metric+2),1);
#endif
        if (kernel_wait_park(&parked, &reason) != KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
        /* pending中的token只在对象raw内更新；finish清局部副本，允许间隙授资格。 */
        if (kernel_wait_finish(&parked) != KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
        kernel_raw_lock_acquire(&lock->metadata, &metadata);
        /* 通知不等于资格；虚唤醒只换token，不重排FIFO的业务等待者。 */
        if (!waiter.granted && kernel_wait_prepare(&lock->waiters, 0, 0, &waiter.token) !=
            KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
    }
    kernel_raw_lock_release(&metadata);
    *guard = (struct kernel_lock_guard){lock, owner, owner->locks, write
#if BOAROS_COST_DIAGNOSTICS
        ,0,0
#endif
    };
#if BOAROS_COST_DIAGNOSTICS
    struct kernel_cost_task *task = kernel_cost_current();
    if (task) { task->wait_rank = 0; task->wait_flags &= (uint8_t)~1U; }
    kernel_cost_add((enum kernel_cost_metric)(metric+1),1);
    kernel_cost_sample((enum kernel_cost_metric)(metric+5),kernel_cost_clock()-wait_start);
    struct kernel_cost_scope hold = kernel_cost_enter((enum kernel_cost_metric)(metric+6));
    guard->cost_start = waiter.grant_ticks; guard->cost_registered = hold.actor != 0;
#endif
    owner->locks = guard;
    arch_interrupt_restore(irq);
    return 1;
}
int kernel_rwlock_try_read(struct kernel_rwlock *lock, struct kernel_lock_guard *guard)
{ return acquire(lock, guard, 0, 1); }
void kernel_rwlock_read(struct kernel_rwlock *lock, struct kernel_lock_guard *guard)
{ (void)acquire(lock, guard, 0, 0); }
void kernel_rwlock_write(struct kernel_rwlock *lock, struct kernel_lock_guard *guard)
{ (void)acquire(lock, guard, 1, 0); }
void kernel_lock_release(struct kernel_lock_guard *guard)
{
    uintptr_t irq = arch_interrupt_save();
    struct kernel_io_context *owner = kernel_io_context_current();
    if (!guard || !guard->lock || guard->owner != owner || owner->locks != guard)
        __builtin_trap();
    struct kernel_rwlock *lock = guard->lock;
    struct kernel_raw_guard metadata = {0};
    kernel_raw_lock_acquire(&lock->metadata, &metadata);
    if (guard->write) {
        if (lock->writer != owner || lock->readers) __builtin_trap();
        lock->writer = 0;
    } else {
        if (lock->writer || !lock->readers) __builtin_trap();
        lock->readers--;
    }
#if BOAROS_COST_DIAGNOSTICS
    struct kernel_cost_scope hold = {
        .tag={guard->cost_registered ? kernel_cost_epoch() : 0,0}, .start=guard->cost_start,
        .actor=guard->cost_registered ? kernel_cost_current() : 0,
        .metric=(enum kernel_cost_metric)(COST_LOCK10_HOLD_TICKS+kernel_cost_rank(lock->rank)*8)};
    kernel_cost_leave(&hold);
#endif
    owner->locks = guard->previous;
    *guard = (struct kernel_lock_guard){0};
    if (!lock->writer && !lock->readers && !lock->handoff_owner && lock->pending_head) {
        lock->handoff_owner = owner;
        struct kernel_lock_waiter *boundary = lock->pending_tail;
        int done = 0;
        while (!done) {
            for (unsigned n = 0; n < 16; n++) {
                struct kernel_lock_waiter *next = lock->pending_head;
                if (!next) __builtin_trap();
                lock->pending_head = next->next;
                if (!lock->pending_head) lock->pending_tail = 0;
                if (next->write) {
                    if (!lock->writers_waiting || lock->readers || lock->writer) __builtin_trap();
                    lock->writers_waiting--;
                    lock->writer = next->owner;
                } else {
                    if (lock->readers == UINT32_MAX) __builtin_trap();
                    lock->readers++;
                }
                next->granted = 1;
#if BOAROS_COST_DIAGNOSTICS
                next->grant_ticks = kernel_cost_clock();
#endif
                /* token定向通知；资格先于wake，非运行获得者也计入占用。 */
                if (kernel_wait_notify(&next->token, KERNEL_WAIT_WOKEN) != KERNEL_SCHEDULER_STATUS_OK)
                    __builtin_trap();
                if (next->write || next == boundary || (lock->pending_head && lock->pending_head->write)) {
                    done = 1;
                    break;
                }
            }
            if (done) { lock->handoff_owner = 0; break; }
            kernel_raw_lock_release(&metadata);
            kernel_raw_lock_acquire(&lock->metadata, &metadata);
            if (lock->handoff_owner != owner) __builtin_trap();
        }
    }
    kernel_raw_lock_release(&metadata);
    arch_interrupt_restore(irq);
}

int kernel_lock_held(const struct kernel_rwlock *lock, int write)
{
    for (struct kernel_lock_guard *g = kernel_io_context_current()->locks; g; g = g->previous)
        if (g->lock == lock) {
            if (write && !g->write) __builtin_trap();
            return 1;
        }
    return 0;
}
