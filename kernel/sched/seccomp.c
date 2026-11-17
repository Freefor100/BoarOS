#include "private.h"
#include <kernel/seccomp.h>
#include <kernel/errno.h>

static int seccomp_exec_waiting(struct kernel_task *caller,struct kernel_task *peer)
{
    KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
    return !caller->terminate_requested && !peer->proc_exiting &&
        kernel_task_exec_busy_locked(peer);
}

int64_t kernel_seccomp_install(struct kernel_task *task,
    struct kernel_seccomp_filter *filter,uint32_t flags)
{
    for(;;) {
        SCHEDULER_TASK_BORROW(busy,0);
        {
            KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
            if(task->terminate_requested)return -KERNEL_EINTR;
            if(flags&1) {
                for(struct kernel_task *peer=task->group_next;peer!=task;peer=peer->group_next) {
                    if(!peer->proc_exiting && kernel_task_exec_busy_locked(peer)) {
                        scheduler_task_get_locked(peer);busy=peer;task->seccomp_exec_waiting=1;break;
                    }
                }
            }
            /* Exec attaches its PREPARING transaction under this domain,
             * before reading no_new_privs. Never synchronize across a stale
             * set-ID credential decision; failure/commit wakes this queue. */
            if(!busy)return kernel_seccomp_attach_locked(task,filter,flags);
        }
        enum kernel_wait_wake_reason reason;
        if(KERNEL_WAIT_RECHECK(&busy->child_exit_queue,0,0,&reason,
            seccomp_exec_waiting(task,busy)))__builtin_trap();
        {
            KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
            task->seccomp_exec_waiting=0;
            if(task->terminate_requested)return -KERNEL_EINTR;
        }
    }
}

void kernel_seccomp_abort_wait_locked(struct kernel_task *task)
{
    /* TSYNC waits out exec credential capture without handling ordinary
     * signals. A successful peer exec must nevertheless be able to kill the
     * waiter before waiting for the group to shrink. Publication and this
     * fatal-only wake share the scheduler domain with its prepare/recheck. */
    if(task->seccomp_exec_waiting) {
        unsigned phase=kernel_wait_record_phase(&task->wait);
        if(phase==KERNEL_WAIT_PREPARED || phase==KERNEL_WAIT_COMMITTED) {
            struct kernel_wait_token token={task,kernel_wait_record_generation(&task->wait)};
            (void)kernel_wait_notify_locked(&token,KERNEL_WAIT_SIGNALLED);
        }
    }
}
