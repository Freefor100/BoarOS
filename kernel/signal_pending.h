#ifndef BOAROS_SIGNAL_PENDING_H
#define BOAROS_SIGNAL_PENDING_H
#include <kernel/signal.h>
struct kernel_posix_timer;
struct signal_queue_record {
    struct kernel_signal_wait_info info;
    struct signal_queue_record *next;
    struct kernel_heap *heap;
    struct kernel_posix_timer *timer;
    struct kernel_uid_account *account;
};
struct kernel_heap;
struct signal_pending_prepared {
    struct kernel_signal_queue *index;
    struct signal_queue_record *record, *retired, *completed;
    struct kernel_uid_account *retired_accounts[2];
};
/* 从现有队列取独立空闲记录，再在锁外补备缺少的索引；不借用队列存储。 */
void signal_pending_prepare_queue(struct kernel_task *,int,struct kernel_heap *,struct signal_pending_prepared *);
void signal_pending_prepare_record(struct kernel_heap *,struct signal_pending_prepared *);
int signal_pending_reserve_locked(const struct kernel_task *,struct kernel_uid_account **);
void signal_pending_unreserve_locked(struct kernel_uid_account *);
/* 队列 owner 与计账目标可不同：进程信号存入组队列，按原目标 TID 计费。 */
void signal_pending_prepare(struct kernel_task *, int shared, const struct kernel_task *charge_target, struct kernel_heap *,
    const struct kernel_signal_wait_info *, int supplied, struct signal_pending_prepared *);
/* 0 publishes an event, 1 coalesces an existing standard signal, <0 errno. */
int signal_pending_needs_record_locked(struct kernel_task *,int,const struct kernel_task *charge_target,const struct kernel_signal_wait_info *,int supplied);
int signal_pending_post_locked(struct kernel_task *, int shared, const struct kernel_task *charge_target,
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
