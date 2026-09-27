#include <arch/riscv/context.h>
#include <kernel/sync.h>
#include "private.h"

static struct kernel_io_context bootstrap_io;
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
    uintptr_t irq = riscv_interrupt_save();
    struct kernel_io_context *owner = kernel_io_context_current();
    if (!lock || !guard || guard->lock || !lock->waiters.initialized) __builtin_trap();
    for (struct kernel_lock_guard *held = owner->locks; held; held = held->previous)
        if (held->lock == lock) __builtin_trap();
    if (owner->locks) {
        const struct kernel_rwlock *outer = owner->locks->lock;
        if (outer->rank > lock->rank ||
            (outer->rank == lock->rank && outer->key >= lock->key)) __builtin_trap();
    }
    if (try_only && (lock->writer || lock->writers_waiting)) {
        riscv_interrupt_restore(irq);
        return 0;
    }
    if (write) {
        if (lock->writers_waiting == UINT32_MAX) __builtin_trap();
        lock->writers_waiting++;
    }
    while (lock->writer || (write ? lock->readers != 0 : lock->writers_waiting != 0)) {
        enum kernel_wait_wake_reason reason;
        if (kernel_scheduler_block_current(&lock->waiters, 0, 0, &reason) !=
            KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
    }
    if (write) {
        lock->writers_waiting--;
        lock->writer = owner;
    } else {
        if (lock->readers == UINT32_MAX) __builtin_trap();
        lock->readers++;
    }
    *guard = (struct kernel_lock_guard){lock, owner, owner->locks, write};
    owner->locks = guard;
    riscv_interrupt_restore(irq);
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
    uintptr_t irq = riscv_interrupt_save();
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
    owner->locks = guard->previous;
    *guard = (struct kernel_lock_guard){0};
    if (!lock->readers && lock->waiters.head && kernel_wait_queue_wake_all(&lock->waiters) !=
        KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
    riscv_interrupt_restore(irq);
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
