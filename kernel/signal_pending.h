#ifndef BOAROS_SIGNAL_PENDING_H
#define BOAROS_SIGNAL_PENDING_H
#include <kernel/signal.h>
struct kernel_posix_timer;
struct signal_queue_record {
    struct kernel_signal_wait_info info;
    struct signal_queue_record *next;
    struct kernel_heap *heap;
    struct kernel_posix_timer *timer;
};
struct kernel_heap;
struct signal_pending_prepared {
    struct kernel_signal_queue *index;
    struct signal_queue_record *record, *retired, *completed;
};
void signal_pending_prepare_index(struct kernel_task *,int,struct kernel_heap *,struct signal_pending_prepared *);
void signal_pending_prepare_record(struct kernel_heap *,struct signal_pending_prepared *);
int signal_pending_reserve_locked(const struct kernel_task *);
void signal_pending_unreserve_locked(void);
void signal_pending_prepare(struct kernel_task *, int shared, struct kernel_heap *,
    const struct kernel_signal_wait_info *, int supplied, struct signal_pending_prepared *);
/* 0 publishes an event, 1 coalesces an existing standard signal, <0 errno. */
int signal_pending_post_locked(struct kernel_task *, int shared,
    const struct kernel_signal_wait_info *, int supplied, struct signal_pending_prepared *);
void signal_pending_take_locked(struct kernel_task *, int shared, unsigned,
    struct kernel_signal_wait_info *, struct signal_pending_prepared *);
int signal_pending_clear_locked(struct kernel_task *, int shared, uint64_t,
    struct signal_pending_prepared *);
int signal_pending_ignore_locked(struct kernel_task *,int,uint64_t,struct signal_pending_prepared *);
void signal_pending_install_index_locked(struct kernel_task *,int,struct signal_pending_prepared *);
void signal_pending_post_timer_locked(struct kernel_task *,int,struct signal_queue_record *);
int posix_timer_record_take_locked(struct signal_queue_record *,struct kernel_signal_wait_info *,struct signal_pending_prepared *);
void posix_timer_record_clear_locked(struct signal_queue_record *,int,struct signal_pending_prepared *);
void posix_timer_record_complete(struct signal_queue_record *);
void posix_timer_record_free(struct signal_queue_record *);
void signal_pending_flush_timers(struct kernel_task *);
void signal_pending_finish(struct signal_pending_prepared *);
#endif
