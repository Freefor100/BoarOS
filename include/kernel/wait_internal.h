#ifndef BOAROS_KERNEL_WAIT_INTERNAL_H
#define BOAROS_KERNEL_WAIT_INTERNAL_H
#include <kernel/scheduler.h>
#include <kernel/raw_lock.h>

enum kernel_wait_phase { KERNEL_WAIT_FINISHED, KERNEL_WAIT_PREPARED,
    KERNEL_WAIT_COMMITTED, KERNEL_WAIT_NOTIFIED };
/* Scheduler-domain members; caller owns task and bound nodes through finish. */
struct kernel_wait_record {
    uint64_t generation, deadline;
    struct kernel_wait_node *nodes;
    uint32_t phase, reason, interruptible;
    uint32_t on_cpu, ready_pending, ready_head, borrows;
};
extern struct kernel_raw_lock kernel_wait_domain;
void kernel_wait_domain_init(void);
void kernel_wait_record_init(struct kernel_wait_record *);
/* Initialized backend, valid queue/token, caller owns wait domain. */
enum kernel_scheduler_status kernel_wait_prepare_locked(struct kernel_wait_queue *,uint64_t,int,struct kernel_wait_token *);
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
enum kernel_scheduler_status kernel_wait_backend_switch(struct kernel_task *);
void kernel_wait_backend_quiesce(void);
#endif
