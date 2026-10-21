#include <arch/context.h>
#include <arch/timer.h>
#include <kernel/sync.h>
#include <kernel/time.h>
#include <kernel/wait_internal.h>
#include "private.h"

struct kernel_lock_waiter {
    struct kernel_lock_waiter *next;
    struct kernel_io_context *owner;
    unsigned write, granted;
    struct kernel_wait_token token;
#if BOAROS_COST_DIAGNOSTICS
    uint64_t grant_ticks;
#endif
};
struct kernel_io_context *kernel_io_context_current(void)
{
    /* I/O ownership follows the executing stack/tp, including the interval
     * after cpu.current publishes the next task but before the stack switch.
     * It needs no borrowed CPU record or interrupt-masked task lookup. */
    struct kernel_task *task = arch_current_thread_get();
    return task ? &task->io_context : kernel_cpu_current()->bootstrap_io;
}

void kernel_rwlock_init(struct kernel_rwlock *lock, uint32_t rank, uintptr_t key)
{
    *lock = (struct kernel_rwlock){ .rank = rank, .key = key };
    kernel_wait_queue_init(&lock->waiters);
    kernel_raw_lock_init(&lock->metadata, KERNEL_RAW_RANK_OBJECT);
}
/* 在对象raw内一次发布登记与FIFO资格owner；本函数不睡眠。 */
static __attribute__((noinline)) void register_waiter(struct kernel_rwlock *lock,
    struct kernel_lock_waiter *waiter)
{
    if (kernel_wait_prepare(&lock->waiters, 0, 0, &waiter->token) != KERNEL_SCHEDULER_STATUS_OK)
        __builtin_trap();
    if (lock->pending_tail) lock->pending_tail->next = waiter;
    else lock->pending_head = waiter;
    lock->pending_tail = waiter;
}
#if BOAROS_MAX_CPUS > 1
static uint64_t queued_spin_budget(struct kernel_rwlock *lock,struct kernel_lock_waiter *waiter)
{
    KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
    unsigned online=0;
    unsigned local=kernel_cpu_current()->logical_id;
    int running_writer=0;
    for(struct kernel_cpu *cpu=scheduler.cpus;cpu;cpu=cpu->scheduler_next) {
        if(!kernel_cpu_online(cpu))continue;
        online++;
        struct kernel_task *task=cpu->current;
        if(lock->pending_head==waiter && lock->writer && cpu->logical_id!=local &&
           task && task->state==KERNEL_THREAD_STATE_RUNNING && task->wait.on_cpu &&
           &task->io_context==lock->writer)running_writer=1;
    }
    /* CPU current can name the next stack before its switch. This snapshot
     * only selects a bounded wait, never grants ownership or pins that task. */
    return online<2?0:running_writer?UINT64_C(1000000):UINT64_C(50000);
}
#endif
/* waiter仅竞争时驻留慢路径栈；返回前结束token，且对象raw仍由调用者持有。 */
static __attribute__((noinline)) uint64_t acquire_waiter(struct kernel_rwlock *lock,
    struct kernel_io_context *owner, int write, struct kernel_raw_guard *metadata,uintptr_t irq
#if BOAROS_COST_DIAGNOSTICS
    , unsigned rank, enum kernel_cost_metric metric
#endif
    )
{
    struct kernel_lock_waiter waiter = {.owner = owner, .write = write};
    register_waiter(lock, &waiter);
#if BOAROS_MAX_CPUS > 1
    uint64_t spin=queued_spin_budget(lock,&waiter);
#else
    (void)irq;
#endif
    while (!waiter.granted) {
        struct kernel_wait_token parked = waiter.token;
        kernel_raw_lock_release(metadata);
#if BOAROS_MAX_CPUS > 1
        if(spin) {
            uint64_t budget=spin;
            spin=0;
            /* The FIFO waiter remains registered and owns its stack. Restore
             * the caller's IRQ state while a remote owner can finish; neither
             * a later arrival nor this poll may steal another waiter's grant. */
            arch_interrupt_restore(irq);
            uint64_t now=kernel_time_monotonic_ns(),deadline;
            if(now<=UINT64_MAX-budget && kernel_time_deadline_from_monotonic(
                now+budget,&deadline)==KERNEL_TIME_STATUS_OK) {
                while(!__atomic_load_n(&waiter.granted,__ATOMIC_ACQUIRE) &&
                      (int64_t)(arch_time_read()-deadline)<0) {}
            }
            (void)arch_interrupt_save();
        }
#endif
        enum kernel_wait_wake_reason reason;
#if BOAROS_COST_DIAGNOSTICS
        uint64_t cost_irq=kernel_cost_lock();
        struct kernel_cost_task *task = kernel_cost_current();
        if (task) {
            if (task->wait_flags & 1) kernel_cost_add((enum kernel_cost_metric)(metric+4),1);
            task->wait_flags &= (uint8_t)~1U; task->wait_rank = (uint8_t)(rank+1);
        }
        kernel_cost_add((enum kernel_cost_metric)(metric+2),1);
        kernel_cost_unlock(cost_irq);
#endif
        if (kernel_wait_park(&parked, &reason) != KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
        /* pending中的token只在对象raw内更新；finish清局部副本，允许间隙授资格。 */
        if (kernel_wait_finish(&parked) != KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
        kernel_raw_lock_acquire(&lock->metadata, metadata);
        /* 通知不等于资格；虚唤醒只换token，不重排FIFO的业务等待者。 */
        if (!waiter.granted && kernel_wait_prepare(&lock->waiters, 0, 0, &waiter.token) !=
            KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
    }
    /* 资格授予同时摘pending；局部waiter不得逃出慢路径的存活区间。 */
    if (lock->pending_head == &waiter || lock->pending_tail == &waiter) __builtin_trap();
#if BOAROS_COST_DIAGNOSTICS
    return waiter.grant_ticks;
#else
    return 0;
#endif
}
static int acquire(struct kernel_rwlock *lock, struct kernel_lock_guard *guard, int write, int try_only)
{
    kernel_assert_can_block();
    COST_SCOPE(acquire_cost, OPERATION_TICKS);
    uintptr_t irq = arch_interrupt_save();
    struct kernel_io_context *owner = kernel_io_context_current();
    if (!lock || !guard || guard->lock || !lock->waiters.initialized) __builtin_trap();
    /* Held keys increase strictly. The top check also rejects reacquiring
     * any earlier lock, so acquisition never needs another full-stack scan. */
    if (owner->locks) {
        const struct kernel_rwlock *outer = owner->locks->lock;
        if (outer->rank > lock->rank ||
            (outer->rank == lock->rank && outer->key >= lock->key)) __builtin_trap();
    }
#if BOAROS_COST_DIAGNOSTICS
    unsigned rank = kernel_cost_rank(lock->rank);
    enum kernel_cost_metric metric = kernel_cost_lock_metric(rank);
    uint64_t wait_start = kernel_cost_clock();
    kernel_cost_add(metric,1);
#endif
    struct kernel_raw_guard metadata = {0};
    kernel_raw_lock_acquire(&lock->metadata, &metadata);
    int busy = lock->writer || lock->pending_head || lock->handoff_owner || (write && lock->readers);
    if (try_only && busy) {
        kernel_raw_lock_release(&metadata);
        arch_interrupt_restore(irq);
        return 0;
    }
#if BOAROS_COST_DIAGNOSTICS
    uint64_t grant_ticks;
#endif
    if (busy) {
#if BOAROS_COST_DIAGNOSTICS
        grant_ticks = acquire_waiter(lock, owner, write, &metadata, irq, rank, metric);
#else
        (void)acquire_waiter(lock, owner, write, &metadata, irq);
#endif
    } else {
        if (write) lock->writer = owner;
        else {
            if (lock->readers == UINT32_MAX) __builtin_trap();
            lock->readers++;
        }
#if BOAROS_COST_DIAGNOSTICS
        grant_ticks = kernel_cost_clock();
#endif
    }
    kernel_raw_lock_release(&metadata);
    *guard = (struct kernel_lock_guard){lock, owner, owner->locks, write
#if BOAROS_COST_DIAGNOSTICS
        ,0,0
#endif
    };
#if BOAROS_COST_DIAGNOSTICS
    uint64_t cost_irq=kernel_cost_lock();
    struct kernel_cost_task *task = kernel_cost_current();
    if (task) { task->wait_rank = 0; task->wait_flags &= (uint8_t)~1U; }
    kernel_cost_add((enum kernel_cost_metric)(metric+1),1);
    kernel_cost_sample((enum kernel_cost_metric)(metric+5),grant_ticks-wait_start);
    struct kernel_cost_scope hold = kernel_cost_enter((enum kernel_cost_metric)(metric+6));
    guard->cost_start = grant_ticks; guard->cost_registered = hold.actor != 0;
    kernel_cost_unlock(cost_irq);
#endif
    owner->locks = guard;
    arch_interrupt_restore(irq);
    return 1;
}
int kernel_rwlock_try_read(struct kernel_rwlock *lock, struct kernel_lock_guard *guard)
{ return acquire(lock, guard, 0, 1); }
int kernel_rwlock_try_write(struct kernel_rwlock *lock, struct kernel_lock_guard *guard)
{ return acquire(lock, guard, 1, 1); }
void kernel_rwlock_read(struct kernel_rwlock *lock, struct kernel_lock_guard *guard)
{ (void)acquire(lock, guard, 0, 0); }
void kernel_rwlock_write(struct kernel_rwlock *lock, struct kernel_lock_guard *guard)
{ (void)acquire(lock, guard, 1, 0); }
/* RV/LA 函数体不足 1 KiB；入口对齐避免频繁释放时跨代码页拆分短指令序列。 */
__attribute__((aligned(1024))) void kernel_lock_release(struct kernel_lock_guard *guard)
{
    uintptr_t irq = arch_interrupt_save();
    struct kernel_io_context *owner = kernel_io_context_current();
    if (!guard || !guard->lock || guard->owner != owner || owner->locks != guard)
        __builtin_trap();
    struct kernel_rwlock *lock = guard->lock;
#if BOAROS_COST_DIAGNOSTICS
    struct kernel_cost_scope hold = {
        .start=guard->cost_start,
        .actor=guard->cost_registered ? kernel_cost_current() : 0,
        .metric=(enum kernel_cost_metric)(kernel_cost_lock_metric(kernel_cost_rank(lock->rank))+6)};
    uint64_t released;
#endif
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
    released = kernel_cost_clock();
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
                    if (lock->readers || lock->writer) __builtin_trap();
                    lock->writer = next->owner;
                } else {
                    if (lock->readers == UINT32_MAX) __builtin_trap();
                    lock->readers++;
                }
#if BOAROS_COST_DIAGNOSTICS
                next->grant_ticks = kernel_cost_clock();
#endif
                __atomic_store_n(&next->granted,1,__ATOMIC_RELEASE);
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
            /* handoff_owner保留原队列边界；分段之间允许原本开启的中断响应。 */
            arch_interrupt_restore(irq);
            irq = arch_interrupt_save();
            kernel_raw_lock_acquire(&lock->metadata, &metadata);
            if (lock->handoff_owner != owner) __builtin_trap();
        }
    }
    kernel_raw_lock_release(&metadata);
    arch_interrupt_restore(irq);
#if BOAROS_COST_DIAGNOSTICS
    /* Qualification has ended; observer publication must not extend its hold. */
    kernel_cost_leave_at(&hold,released);
#endif
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
