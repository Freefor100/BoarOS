#include <arch/context.h>
#include <kernel/sync.h>
#include "private.h"

static struct kernel_io_context bootstrap_io;
struct kernel_lock_waiter {
    struct kernel_lock_waiter *next;
    struct kernel_io_context *owner;
    unsigned write, granted;
#if BOAROS_COST_DIAGNOSTICS
    uint64_t grant_ticks;
#endif
};
struct kernel_io_context *kernel_io_context_current(void)
{
    struct kernel_task *task = kernel_task_current();
    return task ? &task->io_context : &bootstrap_io;
}
void kernel_rwlock_init(struct kernel_rwlock *lock, uint32_t rank, uintptr_t key)
{
    *lock = (struct kernel_rwlock){ .rank = rank, .key = key };
    kernel_wait_queue_init(&lock->waiters);
}
static int acquire(struct kernel_rwlock *lock, struct kernel_lock_guard *guard, int write, int try_only)
{
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
    if (try_only && (lock->writer || lock->pending_head)) {
        arch_interrupt_restore(irq);
        return 0;
    }
    struct kernel_lock_waiter waiter = {.owner = owner, .write = write};
    if (lock->writer || lock->pending_head || (write && lock->readers)) {
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
        enum kernel_wait_wake_reason reason;
#if BOAROS_COST_DIAGNOSTICS
        struct kernel_cost_task *task = kernel_cost_current();
        if (task) {
            if (task->wait_flags & 1) kernel_cost_add((enum kernel_cost_metric)(metric+4),1);
            task->wait_flags &= (uint8_t)~1U; task->wait_rank = (uint8_t)(rank+1);
        }
        kernel_cost_add((enum kernel_cost_metric)(metric+2),1);
#endif
        if (kernel_scheduler_block_current(&lock->waiters, 0, 0, &reason) !=
            KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
    }
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
    if (!lock->writer && !lock->readers) {
        while (lock->pending_head) {
            struct kernel_lock_waiter *next = lock->pending_head;
            lock->pending_head = next->next;
            if (!lock->pending_head) lock->pending_tail = 0;
            /* 先交接资格再唤醒；尚未运行的获得者也阻止新任务抢锁。 */
            if (next->write) {
                if (!lock->writers_waiting) __builtin_trap();
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
            if (!lock->waiters.head || kernel_wait_queue_wake_one(&lock->waiters) !=
                KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
            if (next->write || (lock->pending_head && lock->pending_head->write)) break;
        }
    }
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
