#include "private.h"
#include <kernel/credentials.h>
#include <kernel/errno.h>
#include <kernel/ioprio.h>
#include <kernel/tty_task.h>

/* Linux's block-I/O identity is separate from the task's execution/lock state. */
struct kernel_io_priority_context {
    struct kernel_heap *heap;
    uint32_t references;
    uint16_t priority;
};

static int valid_priority(uint16_t priority)
{
    unsigned cls=(priority>>13)&7U;
    return cls>=1U && cls<=3U;
}

static void put_context(struct kernel_io_priority_context *context)
{
    if(!context)return;
    int last;
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        if(!context->references)__builtin_trap();
        last=!--context->references;
    }
    if(last)kernel_heap_release(context->heap,context);
}

int kernel_ioprio_clone(struct kernel_task *parent,struct kernel_task *child,int share)
{
    uint16_t inherited=0;
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        struct kernel_io_priority_context *context=parent->io_priority;
        if(!context)return 0;
        if(share) {
            if(context->references==UINT32_MAX)__builtin_trap();
            context->references++;child->io_priority=context;return 0;
        }
        inherited=context->priority;
    }
    /* Linux copies a private context only when the priority is explicit. */
    if(!valid_priority(inherited))return 0;
    struct kernel_io_priority_context *context=0;
    enum kernel_heap_status status=kernel_heap_allocate_zeroed(parent->files.heap,1,sizeof(*context),(void **)&context);
    if(status) {
        if(status!=KERNEL_HEAP_STATUS_EMPTY)__builtin_trap();
        return -KERNEL_ENOMEM;
    }
    context->heap=parent->files.heap;context->references=1;context->priority=inherited;
    child->io_priority=context;
    return 0;
}

void kernel_ioprio_exit(struct kernel_task *task)
{
    struct kernel_io_priority_context *context;
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        context=task->io_priority;task->io_priority=0;
    }
    put_context(context);
}

static int effective_locked(const struct kernel_task *task)
{
    uint16_t raw=task->io_priority?task->io_priority->priority:0;
    if(valid_priority(raw))return raw;
    return ((task->scheduling.policy==KERNEL_SCHED_FIFO || task->scheduling.policy==KERNEL_SCHED_RR)?1:2)*8192+4;
}

static int change_one(struct kernel_task *caller,struct kernel_task *target,uint16_t priority)
{
    struct kernel_io_priority_context *candidate=0;
    for(;;) {
        int result=0,need=0;
        {
            KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
            const struct kernel_credentials *from=kernel_credentials_task_locked(caller);
            const struct kernel_credentials *to=kernel_credentials_task_locked(target);
            if(to->uid!=from->euid && to->uid!=from->uid &&
                    !kernel_credentials_capable(from,KERNEL_CAP_SYS_NICE))result=-KERNEL_EPERM;
            else if(!target->io_priority &&
                    (target->proc_exiting || target->retirement || !target->tid_owned)) {
                /* A successful PID lookup already borrowed this task. Linux
                 * completes an exiting target without installing a new ioc. */
            } else if(!target->io_priority && !candidate)need=1;
            else {
                if(!target->io_priority) { target->io_priority=candidate;candidate=0; }
                target->io_priority->priority=priority;
            }
        }
        if(!need) {
            if(candidate)kernel_heap_release(candidate->heap,candidate);
            return result;
        }
        struct kernel_heap *heap=caller->files.heap;
        enum kernel_heap_status status=kernel_heap_allocate_zeroed(heap,1,sizeof(*candidate),(void **)&candidate);
        if(status) {
            if(status!=KERNEL_HEAP_STATUS_EMPTY)__builtin_trap();
            return -KERNEL_ENOMEM;
        }
        candidate->heap=heap;candidate->references=1;
    }
}

int64_t kernel_ioprio_call(struct kernel_task *caller,int setting,int32_t which,int32_t who,int32_t priority)
{
    if(setting) {
        unsigned cls=((uint32_t)priority>>13)&7U,level=(uint32_t)priority&7U;
        if(cls==1U) {
            const struct kernel_credentials *credentials=kernel_credentials_current();
            if(!kernel_credentials_capable(credentials,KERNEL_CAP_SYS_ADMIN) &&
               !kernel_credentials_capable(credentials,KERNEL_CAP_SYS_NICE))return -KERNEL_EPERM;
        } else if(cls>3U || (!cls && level))return -KERNEL_EINVAL;
    }
    if(which==1) {
        if(who<0)return -KERNEL_ESRCH;
        SCHEDULER_TASK_BORROW(borrow,who?scheduler_task_lookup(who,KERNEL_PID_TID,0):0);
        struct kernel_task *target=who?borrow:caller;
        if(!target)return -KERNEL_ESRCH;
        if(setting)return change_one(caller,target,(uint16_t)priority);
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        return target->io_priority?target->io_priority->priority:0;
    }
    if(which!=2 && which!=3)return -KERNEL_EINVAL;
    struct kernel_pid *group __attribute__((cleanup(kernel_task_identity_end)))=0;
    uint32_t uid=0;
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        if(which==2) {
            if(who<0)return -KERNEL_ESRCH;
            group=who?kernel_pid_find(&scheduler.identities,who):process_identity(caller,KERNEL_PID_PGID);
            if(!group)return -KERNEL_ESRCH;
            kernel_pid_get(group);
        } else {
            /* The fixed Linux set path matches the numeric UID argument even
             * when who==0; get uses the caller's real UID in that case. */
            uid=(!setting && !who)?kernel_credentials_task_locked(caller)->uid:(uint32_t)who;
            if(uid==UINT32_MAX)return -KERNEL_ESRCH;
        }
    }
    int result=-KERNEL_ESRCH;
    struct scheduler_task_cursor cursor;scheduler_task_cursor_begin(&cursor);
    while(cursor.task) {
        int match,value=0;
        {
            KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
            struct kernel_task *task=cursor.task;
            match=task->arch.user_mode && task->tid_owned && !task->retirement &&
                (which==2?process_identity(task,KERNEL_PID_PGID)==group:
                          kernel_credentials_task_locked(task)->uid==uid);
            if(match && !setting)value=effective_locked(task);
        }
        if(match) {
            if(setting) {
                result=change_one(caller,cursor.task,(uint16_t)priority);
                if(result)break;
            } else if(result==-KERNEL_ESRCH || value<result) result=value;
        }
        scheduler_task_cursor_next(&cursor);
    }
    scheduler_task_cursor_end(&cursor);
    return result;
}
