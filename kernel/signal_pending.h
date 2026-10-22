#ifndef BOAROS_SIGNAL_PENDING_H
#define BOAROS_SIGNAL_PENDING_H
#include <kernel/signal.h>
struct signal_queue_record;
struct signal_pending_prepared {
    struct kernel_signal_queue *index;
    struct signal_queue_record *record, *retired;
};
void signal_pending_prepare(struct kernel_task *, int shared,
    const struct kernel_signal_wait_info *, int supplied, struct signal_pending_prepared *);
/* 0 publishes an event, 1 coalesces an existing standard signal, <0 errno. */
int signal_pending_post_locked(struct kernel_task *, int shared,
    const struct kernel_signal_wait_info *, int supplied, struct signal_pending_prepared *);
void signal_pending_take_locked(struct kernel_task *, int shared, unsigned,
    struct kernel_signal_wait_info *, struct signal_pending_prepared *);
void signal_pending_clear_locked(struct kernel_task *, int shared, uint64_t,
    struct signal_pending_prepared *);
void signal_pending_finish(struct signal_pending_prepared *);
#endif
