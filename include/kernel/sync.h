#ifndef BOAROS_KERNEL_SYNC_H
#define BOAROS_KERNEL_SYNC_H

#include <kernel/cost.h>
#include <kernel/scheduler.h>
#include <kernel/raw_lock.h>
#include <stdint.h>

struct kernel_io_context;
struct kernel_lock_guard;
struct kernel_lock_waiter;
/* 任务级可睡眠对象锁；独立rank30 raw保护内部资格，登记时嵌套rank40调度锁。
 * rank/key、不可递归和guard owner随任务保留。释放内部raw后park，先授资格再wake；
 * 尚未运行的获得者也计入占用。读者批次保留handoff owner及原边界，每段最多16项，
 * 后来的读者不越过已排writer。对象存活到全部guard/等待者/handoff结束。
 * 等待不可中断；没有timed/interruptible rwlock。初始化/移动仅允许未发布实例。 */
struct kernel_rwlock {
    struct kernel_raw_lock metadata;
    struct kernel_wait_queue waiters;
    struct kernel_io_context *handoff_owner;
    struct kernel_lock_waiter *pending_head, *pending_tail;
    struct kernel_io_context *writer;
    uint32_t readers;
    uint32_t writers_waiting;
    uint32_t rank;
    uintptr_t key;
};
struct kernel_lock_guard {
    struct kernel_rwlock *lock;
    struct kernel_io_context *owner;
    struct kernel_lock_guard *previous;
    int write;
#if BOAROS_COST_DIAGNOSTICS
    uint64_t cost_start, cost_registered;
#endif
};
struct kernel_io_context {
    struct kernel_lock_guard *locks;
    unsigned allocation_depth;
    uint32_t reclaim_depth;
    unsigned backend_depth;
    unsigned background_reclaim;
    int backend_read;
    struct kernel_lock_guard backend_guard;
};
struct kernel_mutex { struct kernel_rwlock lock; };

/* Single-hart task context, or the bootstrap context before scheduling. */
struct kernel_io_context *kernel_io_context_current(void);
void kernel_rwlock_init(struct kernel_rwlock *lock, uint32_t rank, uintptr_t key);
int kernel_rwlock_try_read(struct kernel_rwlock *lock, struct kernel_lock_guard *guard);
void kernel_rwlock_read(struct kernel_rwlock *lock, struct kernel_lock_guard *guard);
void kernel_rwlock_write(struct kernel_rwlock *lock, struct kernel_lock_guard *guard);
void kernel_lock_release(struct kernel_lock_guard *guard);
int kernel_lock_held(const struct kernel_rwlock *lock, int write);
static inline struct kernel_io_context *kernel_no_reclaim_io_begin(void)
{
    struct kernel_io_context *context = kernel_io_context_current();
    if (context->allocation_depth == UINT32_MAX) __builtin_trap();
    context->allocation_depth++;
    return context;
}
static inline void kernel_no_reclaim_io_end(struct kernel_io_context **context)
{
    if (*context != kernel_io_context_current() || !(*context)->allocation_depth) __builtin_trap();
    (*context)->allocation_depth--;
}
#define KERNEL_NO_RECLAIM_IO struct kernel_io_context *reclaim_context __attribute__((cleanup(kernel_no_reclaim_io_end))) = kernel_no_reclaim_io_begin()

static inline void kernel_lock_scope_release(struct kernel_lock_guard *guard)
{ if (guard->lock) kernel_lock_release(guard); }
#define KERNEL_LOCK_SCOPE(name) struct kernel_lock_guard name __attribute__((cleanup(kernel_lock_scope_release))) = {0}
static inline void kernel_mutex_init(struct kernel_mutex *lock, uint32_t rank, uintptr_t key)
{ kernel_rwlock_init(&lock->lock, rank, key); }
static inline void kernel_mutex_lock(struct kernel_mutex *lock, struct kernel_lock_guard *guard)
{ kernel_rwlock_write(&lock->lock, guard); }
#endif
