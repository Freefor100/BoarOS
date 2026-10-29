/* 静态生产停止操作由测试入口暴露；调度锁/claim/通知保持真实实现。 */
#define process_group_request_exit user_signal_unexpected_group_exit
#define kernel_user_thread_exit user_signal_unexpected_exit
#define process_group_is_orphaned user_signal_unexpected_orphan
#define scheduler_switch_current_away user_signal_unexpected_park
#define wake_waiting_parent user_signal_unexpected_parent
#include "../../kernel/sched/signal.c"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
void user_signal_unexpected_group_exit(struct kernel_task *task,enum kernel_thread_exit_reason reason,uint64_t status,uint64_t detail)
{ (void)task;(void)reason;(void)status;(void)detail;abort(); }
void user_signal_unexpected_exit(enum kernel_thread_exit_reason reason,uint64_t status,uint64_t detail)
{ (void)reason;(void)status;(void)detail;abort(); }
int user_signal_unexpected_orphan(const struct kernel_task *task) { (void)task;abort(); }
enum kernel_scheduler_status user_signal_unexpected_park(struct kernel_task *task)
{ (void)task;assert(!"SIGCONT was posted during migration, then an obsolete STOP still parked the group");abort(); }
void user_signal_unexpected_parent(struct kernel_task *task)
{ (void)task;assert(!"SIGCONT canceled the STOP during migration, so no stop notification may follow");abort(); }
void scheduler_task_disposal_notify(void) { abort(); }
/* 此模型不回收信号表页；若误入该路径，必须显式失败。 */
enum riscv_direct_map_status riscv_direct_map_va_to_pa(uint64_t address,uint64_t size,uint64_t *physical)
{ (void)address;(void)size;(void)physical;abort(); }
enum physical_page_status physical_page_release(struct physical_page_allocator *allocator,uint64_t address)
{ (void)allocator;(void)address;abort(); }

void user_stop_case(struct kernel_task *task) { signal_stop_member(task,SIGNAL_STOP); }
/* Standard-signal scheduler fixture: PID storage is static, while the
 * production group cursor and task borrows remain real. No queued payloads. */
static struct kernel_pid group_identity;
static struct kernel_pid_member group_member;
struct kernel_pid *process_identity(const struct kernel_task *task,enum kernel_pid_role role)
{ assert(role==KERNEL_PID_TGID);assert(task->group_leader==group_member.task);return &group_identity; }
void kernel_pid_get(struct kernel_pid *id){assert(id==&group_identity);id->references++;}
void kernel_task_identity_put(struct kernel_pid *id)
{ KERNEL_RAW_SCOPE(g,&kernel_wait_domain);assert(id==&group_identity&&id->references>1);id->references--; }
static struct kernel_task *signal_waiter;
static struct kernel_task *signal_receiver;
static struct kernel_task finishing_waiter;
static struct kernel_signal_table actions;
void user_signal_retarget_case(struct kernel_task *receiver,struct kernel_task *waiter)
{
    group_member=(struct kernel_pid_member){.task=receiver,.role=KERNEL_PID_TGID};
    group_identity=(struct kernel_pid){.number=receiver->tid,.references=1};group_identity.members[KERNEL_PID_TGID]=&group_member;
    actions=(struct kernel_signal_table){.magic=KERNEL_SIGNAL_TABLE_MAGIC,.references=2};
    kernel_wait_queue_init(&actions.signalfd_waiters);
    actions.actions[9]=(struct kernel_signal_action){.handler=0x1000,.mask=signal_mask(12)};
    actions.actions[11]=(struct kernel_signal_action){.handler=0x2000};
    receiver->magic=waiter->magic=KERNEL_THREAD_MAGIC;
    receiver->group_leader=waiter->group_leader=receiver;
    receiver->group_members=2;receiver->group_next=receiver->group_previous=waiter;
    waiter->group_next=waiter->group_previous=receiver;
    receiver->signal_table_address=waiter->signal_table_address=(uintptr_t)&actions;
    receiver->signal_blocked=receiver->signal_restore_mask=receiver->user_return_pending=0;
    receiver->signal_pending=0;receiver->group_pending=signal_mask(10)|signal_mask(12);
    waiter->signal_blocked=signal_mask(10);
    waiter->wait=(struct kernel_wait_record){.interruptible=1};
    kernel_wait_record_store(&waiter->wait,1,KERNEL_WAIT_COMMITTED,KERNEL_WAIT_WOKEN);
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        kernel_wait_backend_commit(waiter,0,1);
    }
    signal_waiter=waiter;signal_receiver=receiver;
    struct kernel_signal_delivery delivery={0};
    assert(kernel_signal_select(receiver,&delivery)==KERNEL_SIGNAL_SELECT_HANDLER && delivery.info.signal==10);
    assert(kernel_wait_record_phase(&waiter->wait)==KERNEL_WAIT_NOTIFIED && kernel_wait_record_reason(&waiter->wait)==KERNEL_WAIT_SIGNALLED);
    assert(waiter->state==KERNEL_THREAD_STATE_READY && waiter->ready_node.queued);
    assert(!receiver->storage_borrows && !waiter->storage_borrows);
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        ready_remove_locked(waiter);
        receiver->group_pending=0;receiver->signal_blocked=0;
        kernel_wait_record_store(&waiter->wait,kernel_wait_record_generation(&waiter->wait)+1,
                                 KERNEL_WAIT_COMMITTED,KERNEL_WAIT_WOKEN);
        kernel_wait_backend_commit(waiter,0,1);
    }
    assert(!kernel_signal_send(receiver,12,7));
    assert(kernel_wait_record_phase(&waiter->wait)==KERNEL_WAIT_COMMITTED);
    { KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);receiver->proc_exiting=1; }
    kernel_signal_task_exit(receiver);
    assert(kernel_wait_record_phase(&waiter->wait)==KERNEL_WAIT_NOTIFIED && kernel_wait_record_reason(&waiter->wait)==KERNEL_WAIT_SIGNALLED);
    assert(waiter->state==KERNEL_THREAD_STATE_READY && waiter->ready_node.queued);
    { KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
      assert(!signal_wants_signal_locked(receiver,12));receiver->proc_exiting=0; }
    puts("PASS an exiting receiver transfers its unconsumed shared signal before group teardown");

    /* 接收候选刚结束sigtimedwait；它的临时集合仍接收B，普通mask却屏蔽B。 */
    finishing_waiter=(struct kernel_task){.magic=KERNEL_THREAD_MAGIC,.arch.user_mode=1,
        .state=KERNEL_THREAD_STATE_READY,.group_leader=receiver,
        .signal_table_address=(uintptr_t)&actions,.signal_blocked=signal_mask(12),
        .signal_wait_mask=signal_mask(12)};
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        ready_remove_locked(waiter);
        receiver->group_pending=signal_mask(12);receiver->signal_blocked=signal_mask(12);
        receiver->group_members=3;receiver->group_next=&finishing_waiter;
        finishing_waiter.group_previous=receiver;finishing_waiter.group_next=waiter;
        waiter->group_previous=&finishing_waiter;
        kernel_wait_record_store(&waiter->wait,kernel_wait_record_generation(&waiter->wait)+1,
                                 KERNEL_WAIT_COMMITTED,KERNEL_WAIT_WOKEN);
        kernel_wait_backend_commit(waiter,0,1);
    }
    signal_retarget_shared(receiver,signal_mask(12));
    assert(kernel_wait_record_phase(&waiter->wait)==KERNEL_WAIT_COMMITTED);
    assert(finishing_waiter.user_return_pending & KERNEL_USER_RETURN_SIGNAL);
    kernel_signal_wait_end(&finishing_waiter);
    assert(!finishing_waiter.signal_wait_mask && kernel_wait_record_phase(&waiter->wait)==KERNEL_WAIT_NOTIFIED);
    assert(waiter->state==KERNEL_THREAD_STATE_READY && waiter->ready_node.queued);
    assert(!finishing_waiter.storage_borrows && !receiver->storage_borrows && !waiter->storage_borrows);
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        receiver->group_members=2;receiver->group_next=waiter;waiter->group_previous=receiver;
    }
    puts("PASS leaving sigtimedwait transfers a still-blocked shared signal to another real waiter");
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        ready_remove_locked(waiter);
        waiter->state=KERNEL_THREAD_STATE_STOPPED;stopped_append_locked(waiter);
        waiter->user_return_pending=0;
    }
    signal_prepare_group(receiver,SIGNAL_CONTINUE);
    assert(waiter->state==KERNEL_THREAD_STATE_READY && waiter->ready_node.queued);
    assert(waiter->user_return_pending & KERNEL_USER_RETURN_SIGNAL);
    puts("PASS SIGCONT restoring a READY member also exposes signals published while it was stopped");
}
void user_signal_take_retargeted(struct kernel_task *waiter)
{
    struct kernel_signal_delivery delivery={0};
    assert(kernel_signal_select(waiter,&delivery)==KERNEL_SIGNAL_SELECT_HANDLER && delivery.info.signal==12);
    assert(!waiter->group_leader->group_pending && !waiter->storage_borrows);
    signal_waiter=0;
    puts("PASS a handler blocking a shared signal transfers it to another real interruptible waiter");
}
void user_signal_cancel_stop(struct kernel_task *task)
{ signal_prepare_group(task,SIGNAL_CONTINUE); }
void user_signal_stop_selection(struct kernel_task *task)
{
    task->group_members=1;task->group_next=task->group_previous=task;
    task->signal_blocked=task->signal_restore_mask=task->user_return_pending=0;
    task->signal_pending=signal_mask(SIGNAL_STOP);task->group_pending=0;
    struct kernel_signal_delivery delivery={0};
    assert(kernel_signal_select(task,&delivery)==KERNEL_SIGNAL_SELECT_NONE);
    assert(task->state==KERNEL_THREAD_STATE_RUNNING && !task->signal_pending && !task->group_pending);
    puts("PASS SIGCONT during CPU0 handoff cancels a STOP before pending consumption");
}
/* 仅上面的真实waiter允许进入阻塞信号后端；原停止反例不经过该分支。 */
enum kernel_scheduler_status __real_kernel_scheduler_wake_signal(struct kernel_task *);
enum kernel_scheduler_status __wrap_kernel_scheduler_wake_signal(struct kernel_task *task)
{
    assert(task==signal_waiter || task==&finishing_waiter ||
           (task==signal_receiver && kernel_wait_record_phase(&task->wait)==KERNEL_WAIT_FINISHED));
    return __real_kernel_scheduler_wake_signal(task);
}

/* These standard-signal models never allocate queued payloads. Real queue
 * allocation, quota and retirement run in signal_pending.c's focused suite. */
enum kernel_heap_status kernel_heap_allocate_zeroed(struct kernel_heap *h,size_t n,size_t size,void **out)
{ (void)h;(void)n;(void)size;(void)out;abort(); }
void kernel_heap_release(struct kernel_heap *h,void *p)
{ (void)h;(void)p;abort(); }
