#ifndef BOAROS_KERNEL_WAIT_INTERNAL_H
#define BOAROS_KERNEL_WAIT_INTERNAL_H
#include <kernel/scheduler.h>
#include <kernel/raw_lock.h>

enum kernel_wait_phase { KERNEL_WAIT_FINISHED, KERNEL_WAIT_PREPARED,
    KERNEL_WAIT_COMMITTED, KERNEL_WAIT_NOTIFIED };
/* The wait owner retains bound nodes through finish. Queue links, borrows and
 * scheduler ownership remain protected by the scheduler domain. */
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
    uint32_t on_cpu, ready_pending, ready_head, borrows;
};
#if BOAROS_MAX_CPUS > 1
#define KERNEL_WAIT_GENERATION_MAX (UINT64_MAX >> 4)
static inline uint64_t kernel_wait_state(uint64_t generation, unsigned phase, unsigned reason)
{ return (generation << 4) | ((uint64_t)reason << 2) | phase; }
static inline uint64_t kernel_wait_record_load(const struct kernel_wait_record *record)
{ return __atomic_load_n(&record->state, __ATOMIC_ACQUIRE); }
static inline uint64_t kernel_wait_record_generation(const struct kernel_wait_record *record)
{ return kernel_wait_record_load(record) >> 4; }
static inline unsigned kernel_wait_record_phase(const struct kernel_wait_record *record)
{ return (unsigned)(kernel_wait_record_load(record) & 3U); }
static inline unsigned kernel_wait_record_reason(const struct kernel_wait_record *record)
{ return (unsigned)((kernel_wait_record_load(record) >> 2) & 3U); }
static inline void kernel_wait_record_store(struct kernel_wait_record *record,
    uint64_t generation, unsigned phase, unsigned reason)
{ __atomic_store_n(&record->state, kernel_wait_state(generation,phase,reason), __ATOMIC_RELEASE); }
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
extern struct kernel_raw_lock kernel_wait_domain;
void kernel_wait_domain_init(void);
void kernel_wait_record_init(struct kernel_wait_record *);
/* Initialized backend, valid queue/token, caller owns wait domain. */
enum kernel_scheduler_status kernel_wait_prepare_locked(struct kernel_wait_queue *,uint64_t,int,struct kernel_wait_token *);
enum kernel_scheduler_status kernel_wait_node_bind_locked(struct kernel_wait_queue *,struct kernel_wait_node *,const struct kernel_wait_token *);
int kernel_wait_notify_locked(const struct kernel_wait_token *, uint32_t);
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
int kernel_wait_cursor_prepare_event_locked(struct kernel_wait_queue *,struct kernel_wait_cursor *,uint64_t,struct kernel_task *);
int kernel_wait_cursor_prepare_locked(struct kernel_wait_queue *, struct kernel_wait_cursor *, int);
void kernel_wait_cursor_wake(struct kernel_wait_cursor *, int);
/* Caller keeps the queue alive and holds the condition lock. Waiters register
 * under that lock, or recheck the condition under it after binding. */
int kernel_wait_cursor_prepare(struct kernel_wait_queue *, struct kernel_wait_cursor *);
void kernel_wait_cursor_begin(struct kernel_wait_queue *, struct kernel_wait_cursor *);
void kernel_wait_cursor_advance(struct kernel_wait_cursor *);
void kernel_wait_cursor_end(struct kernel_wait_cursor *);
void kernel_wait_task_pin(struct kernel_task *);
void kernel_wait_task_unpin(struct kernel_task *);
/* wait_domain held; node borrows ended, task borrow protects revalidation/move. */
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
/* Success returns on this task's CPU after notification and scheduler handoff.
 * Its NOTIFIED reason is stable until the current owner finishes the token. */
enum kernel_scheduler_status kernel_wait_backend_switch(struct kernel_task *);
void kernel_wait_backend_quiesce(void);
#endif
