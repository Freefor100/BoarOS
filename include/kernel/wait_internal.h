#ifndef BOAROS_KERNEL_WAIT_INTERNAL_H
#define BOAROS_KERNEL_WAIT_INTERNAL_H
#include <kernel/scheduler.h>
#include <kernel/raw_lock.h>

enum kernel_wait_phase { KERNEL_WAIT_FINISHED, KERNEL_WAIT_PREPARED,
    KERNEL_WAIT_COMMITTED, KERNEL_WAIT_NOTIFIED };
/* The request owner retains its node list through finish. Each event queue
 * protects links/cursors; the execution owner protects READY and stack handoff. */
struct kernel_wait_record {
#if BOAROS_MAX_CPUS > 1
    /* Generation, phase and notification reason are one atomic publication. */
    uint64_t state;
#else
    uint64_t generation;
#endif
    uint64_t deadline;
    struct kernel_wait_node *nodes;
#if BOAROS_MAX_CPUS == 1
    uint32_t phase, reason, interruptible;
#else
    uint32_t interruptible;
#endif
#if BOAROS_USER_SMP
    _Atomic uint32_t on_cpu;
#else
    uint32_t on_cpu;
#endif
    uint32_t ready_pending, ready_flags, borrows;
};
static inline uint32_t kernel_wait_record_borrows(const struct kernel_wait_record *record)
{
#if BOAROS_MAX_CPUS > 1
    return __atomic_load_n(&record->borrows,__ATOMIC_ACQUIRE);
#else
    return record->borrows;
#endif
}
#if BOAROS_MAX_CPUS > 1
#define KERNEL_WAIT_GENERATION_MAX (UINT64_MAX >> 5)
static inline uint64_t kernel_wait_state(uint64_t generation, unsigned phase, unsigned reason)
{ return (generation << 5) | ((uint64_t)reason << 2) | phase; }
static inline uint64_t kernel_wait_record_load(const struct kernel_wait_record *record)
{ return __atomic_load_n(&record->state, __ATOMIC_ACQUIRE); }
static inline uint64_t kernel_wait_record_generation(const struct kernel_wait_record *record)
{ return kernel_wait_record_load(record) >> 5; }
static inline unsigned kernel_wait_record_phase(const struct kernel_wait_record *record)
{ return (unsigned)(kernel_wait_record_load(record) & 3U); }
static inline unsigned kernel_wait_record_reason(const struct kernel_wait_record *record)
{ return (unsigned)((kernel_wait_record_load(record) >> 2) & 3U); }
static inline void kernel_wait_record_store(struct kernel_wait_record *record,
    uint64_t generation, unsigned phase, unsigned reason)
{ __atomic_store_n(&record->state, kernel_wait_state(generation,phase,reason) |
        ((uint64_t)record->interruptible<<4), __ATOMIC_RELEASE); }
static inline void kernel_wait_record_transition(struct kernel_wait_record *record,
    uint64_t generation, unsigned phase, unsigned reason)
{ kernel_wait_record_store(record,generation,phase,reason); }
#else
#define KERNEL_WAIT_GENERATION_MAX UINT64_MAX
static inline uint64_t kernel_wait_record_generation(const struct kernel_wait_record *record)
{ return record->generation; }
static inline unsigned kernel_wait_record_phase(const struct kernel_wait_record *record)
{ return record->phase; }
static inline unsigned kernel_wait_record_reason(const struct kernel_wait_record *record)
{ return record->reason; }
static inline void kernel_wait_record_store(struct kernel_wait_record *record,
    uint64_t generation, unsigned phase, unsigned reason)
{ record->generation=generation; record->phase=phase; record->reason=reason; }
static inline void kernel_wait_record_transition(struct kernel_wait_record *record,
    uint64_t generation, unsigned phase, unsigned reason)
{ (void)generation; record->reason=reason; record->phase=phase; }
#endif
static inline int kernel_wait_record_interruptible(const struct kernel_wait_record *record)
{
#if BOAROS_MAX_CPUS > 1
    return !!(kernel_wait_record_load(record)&16U);
#else
    return record->interruptible;
#endif
}
static inline uint64_t kernel_wait_signal_generation(const struct kernel_wait_record *record)
{
#if BOAROS_MAX_CPUS > 1
    uint64_t state=kernel_wait_record_load(record);
    unsigned phase=(unsigned)(state&3U);
    return (state&16U) && (phase==KERNEL_WAIT_PREPARED || phase==KERNEL_WAIT_COMMITTED)
        ? state>>5 : 0;
#else
    return record->interruptible && (record->phase==KERNEL_WAIT_PREPARED ||
        record->phase==KERNEL_WAIT_COMMITTED) ? record->generation : 0;
#endif
}
extern struct kernel_raw_lock kernel_wait_domain;
void kernel_wait_domain_init(void);
void kernel_wait_record_init(struct kernel_wait_record *);
/* Node operations require their queue lock. Release that lock before notifying
 * a task; completion and switch_finish need the actual execution protection. */
enum kernel_scheduler_status kernel_wait_node_bind_locked(struct kernel_wait_queue *,struct kernel_wait_node *,const struct kernel_wait_token *);
int kernel_wait_notify_locked(const struct kernel_wait_token *, uint32_t);
#if BOAROS_USER_SMP
/* 1: caller owns the returned run guard, 0: obsolete, -1: common path. */
int kernel_wait_backend_lock_local(const struct kernel_wait_token *,struct kernel_raw_guard *);
#endif
void kernel_wait_node_add_locked(struct kernel_wait_queue *, struct kernel_wait_node *);
void kernel_wait_node_remove_locked(struct kernel_wait_node *);
void kernel_wait_switch_finish_locked(struct kernel_task *);
struct kernel_wait_cursor {
    struct kernel_wait_queue *queue;
    struct kernel_wait_node *node;
    uint64_t sequence;
    uint64_t event;
    struct kernel_task *recipient;
};
int kernel_wait_cursor_prepare_event(struct kernel_wait_queue *,struct kernel_wait_cursor *,uint64_t,struct kernel_task *);
void kernel_wait_cursor_wake(struct kernel_wait_cursor *, int);
/* Caller keeps the queue alive and holds the condition lock. Waiters register
 * under that lock, or recheck the condition under it after binding. */
int kernel_wait_cursor_prepare(struct kernel_wait_queue *, struct kernel_wait_cursor *);
void kernel_wait_cursor_begin(struct kernel_wait_queue *, struct kernel_wait_cursor *);
void kernel_wait_cursor_advance(struct kernel_wait_cursor *);
void kernel_wait_cursor_end(struct kernel_wait_cursor *);
void kernel_wait_task_pin(struct kernel_task *);
void kernel_wait_task_unpin(struct kernel_task *);
/* Both queues held in address order; node borrows ended, task borrow protects move. */
enum kernel_scheduler_status kernel_wait_node_move_locked(struct kernel_wait_node *, struct kernel_wait_queue *);

/* 构建期绑定：真实任务、期限索引和切换在 wait.c；宿主仅替换机器边界。 */
struct kernel_wait_record *kernel_wait_record_of(struct kernel_task *);
struct kernel_wait_node *kernel_wait_default_node(struct kernel_task *);
struct kernel_task *kernel_wait_current_task(void);
int kernel_wait_backend_initialized(void);
int kernel_wait_backend_signal(struct kernel_task *);
uint64_t kernel_wait_backend_time(void);
void kernel_wait_backend_commit(struct kernel_task *, uint64_t, int);
void kernel_wait_backend_notify(struct kernel_task *, uint32_t);
void kernel_wait_backend_ready(struct kernel_task *);
/* Success resumes this owner with a stable NOTIFIED result until finish. */
enum kernel_scheduler_status kernel_wait_backend_switch(struct kernel_task *);
#if BOAROS_MAX_CPUS > 1
/* SMP consumes the commit guard before the physical stack switch. */
struct kernel_raw_guard;
enum kernel_scheduler_status kernel_wait_backend_switch_locked(struct kernel_task *, struct kernel_raw_guard *);
#endif
void kernel_wait_backend_quiesce(void);
#endif
