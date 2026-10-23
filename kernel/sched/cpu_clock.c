#include "private.h"
#include <kernel/accounting.h>
#include <kernel/errno.h>
#include <kernel/tick.h>
#include <kernel/task.h>

static int thread_clock_visible(const struct kernel_task *task)
{
    return task->group_leader == task ||
        (task->state != KERNEL_THREAD_STATE_EXITED && task->state != KERNEL_THREAD_STATE_GROUP_DEAD);
}

/* Resolver returns a real identity reference; all users sample outside the
 * scheduler domain and revalidate the resolved role after sampling. */
int scheduler_cpu_clock_resolve(struct kernel_task *caller,int32_t clock,int strict,
    struct scheduler_cpu_clock *out)
{
    if(clock==2)clock=-6;else if(clock==3)clock=-2;else if(clock>=0)return -KERNEL_EINVAL;
    unsigned kind=(uint32_t)clock&3U;int thread=((uint32_t)clock&4U)!=0;
    kernel_pid_t number=~(clock>>3);if(kind==3U)return -KERNEL_EINVAL;
    enum kernel_pid_role role=thread?KERNEL_PID_TID:KERNEL_PID_TGID;
    KERNEL_RAW_SCOPE(g,&kernel_wait_domain);
    struct kernel_pid *identity;
    if(!number || (!thread&&!strict&&number==caller->tid))identity=process_identity(caller,role);
    else identity=kernel_pid_find(&scheduler.identities,number);
    struct kernel_task *target=identity&&identity->members[role]?identity->members[role]->task:0;
    if(!target || (thread&&(target->group_leader!=caller->group_leader||!thread_clock_visible(target))))return -KERNEL_EINVAL;
    kernel_pid_get(identity);*out=(struct scheduler_cpu_clock){identity,kind,(uint32_t)thread};return 0;
}
int scheduler_cpu_clock_sample(const struct scheduler_cpu_clock *clock,struct kernel_task_usage *usage)
{
    enum kernel_pid_role role=clock->thread?KERNEL_PID_TID:KERNEL_PID_TGID;
    for(;;) {
        SCHEDULER_TASK_BORROW(target,0);
        {KERNEL_RAW_SCOPE(g,&kernel_wait_domain);
         target=clock->identity->members[role]?clock->identity->members[role]->task:0;
         if(!target || (clock->thread&&!thread_clock_visible(target))){target=0;return -KERNEL_ESRCH;}
         scheduler_task_get_locked(target);}
        if(clock->thread){KERNEL_RAW_SCOPE(g,&kernel_wait_domain);scheduler_usage_thread_locked(target,usage);}
        else scheduler_group_usage_snapshot(target,0,usage);
        {KERNEL_RAW_SCOPE(g,&kernel_wait_domain);
         if(!clock->identity->members[role])return -KERNEL_ESRCH;
         if(clock->identity->members[role]->task!=target)continue;
         return 0;}
    }
}
int kernel_task_cpu_clock(struct kernel_task *caller,int32_t clock,int resolution,uint64_t *nanoseconds)
{
    struct scheduler_cpu_clock target={0};int result=scheduler_cpu_clock_resolve(caller,clock,resolution,&target);
    if(result)return result;
    if(resolution)*nanoseconds=target.kind==2?1U:UINT64_C(1000000000)/KERNEL_TICKS_PER_SECOND;
    else {
        struct kernel_task_usage usage;result=scheduler_cpu_clock_sample(&target,&usage);
        if(!result)*nanoseconds=usage.user_ns+(target.kind==1?0:usage.kernel_ns);
        else result=-KERNEL_EINVAL;
    }
    kernel_task_identity_put(target.identity);return result;
}
