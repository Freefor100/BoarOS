#include "private.h"
#include "../signal_pending.h"
#include <kernel/posix_timer.h>
#include <kernel/errno.h>
#include <string.h>

_Static_assert(sizeof(struct kernel_posix_event)==64,"Linux sigevent");
_Static_assert(sizeof(struct kernel_posix_itimerspec)==32,"Linux itimerspec64");
#define TIMER_BUCKETS 64U
struct kernel_posix_timer_registry {
    struct kernel_heap *heap;
    struct kernel_posix_timer *buckets[TIMER_BUCKETS];
    uint32_t next_id,references;
};
struct kernel_posix_timer {
    struct kernel_posix_timer *hash_next;
    struct kernel_posix_timer_registry *owner;
    struct kernel_heap *heap;
    struct kernel_pid *recipient;
    struct scheduler_cpu_clock cpu;
    struct signal_pending_prepared notification;
    struct kernel_posix_event event;
    uint32_t id,references,valid,dead,charged;
    int32_t clock,overrun;
    uint64_t expires,interval,generation;
};
static struct kernel_posix_timer *find_locked(struct kernel_posix_timer_registry *r,uint32_t id)
{
    for(struct kernel_posix_timer *t=r?r->buckets[id%TIMER_BUCKETS]:0;t;t=t->hash_next)
        if(t->id==id)return t;
    return 0;
}
static void unhash_locked(struct kernel_posix_timer *t)
{
    struct kernel_posix_timer **link=&t->owner->buckets[t->id%TIMER_BUCKETS];
    while(*link!=t){if(!*link)__builtin_trap();link=&(*link)->hash_next;}
    *link=t->hash_next;t->hash_next=0;t->valid=0;t->dead=1;
}
static void registry_put(struct kernel_posix_timer_registry *r)
{
    int free_it;
    {KERNEL_RAW_SCOPE(g,&kernel_wait_domain);if(!r->references)__builtin_trap();free_it=!--r->references;}
    if(free_it)kernel_heap_release(r->heap,r);
}
static void timer_put(struct kernel_posix_timer *t)
{
    int free_it;
    {
        KERNEL_RAW_SCOPE(g,&kernel_wait_domain);
        if(!t->references)__builtin_trap();
        free_it=!--t->references;
        if(free_it&&t->charged){signal_pending_unreserve_locked();t->charged=0;}
    }
    if(!free_it)return;
    struct kernel_posix_timer_registry *r=t->owner;
    signal_pending_finish(&t->notification);
    if(t->recipient)kernel_task_identity_put(t->recipient);
    if(t->cpu.identity)kernel_task_identity_put(t->cpu.identity);
    kernel_heap_release(t->heap,t);
    if(r)registry_put(r);
}
static struct kernel_posix_timer *timer_get(struct kernel_task *caller,int32_t id)
{
    KERNEL_RAW_SCOPE(g,&kernel_wait_domain);
    struct kernel_posix_timer *t=find_locked(caller->group_leader->posix_timers,(uint32_t)id);
    if(!t||!t->valid)return 0;
    if(t->references==UINT32_MAX)__builtin_trap();
    t->references++;return t;
}
int kernel_posix_timer_prepare(struct kernel_task *caller,int32_t clock,
    const struct kernel_posix_event *event,int32_t *id,struct kernel_posix_timer **out)
{
    struct kernel_heap *heap=caller->files.heap;
    struct kernel_posix_timer *t=0;
    if(clock>=0 && clock!=0 && clock!=1 && clock!=2 && clock!=3)
        return clock<=11?-KERNEL_EOPNOTSUPP:-KERNEL_EINVAL;
    if(kernel_heap_allocate_zeroed(heap,1,sizeof(*t),(void **)&t))return -KERNEL_EAGAIN;
    t->heap=heap;t->references=1;t->clock=clock;t->generation=1;
    if(clock<0||clock==2||clock==3) {
        int error=scheduler_cpu_clock_resolve(caller,clock,1,&t->cpu);
        if(error){timer_put(t);return error;}
    }
    struct kernel_posix_timer_registry *candidate=0;int need_registry;
    {KERNEL_RAW_SCOPE(g,&kernel_wait_domain);need_registry=!caller->group_leader->posix_timers;}
    if(need_registry) {
        candidate=0;
        if(kernel_heap_allocate_zeroed(heap,1,sizeof(*candidate),(void **)&candidate)){timer_put(t);return -KERNEL_EAGAIN;}
        candidate->heap=heap;candidate->references=1;
    }
    int error=0;SCHEDULER_TASK_BORROW(target,0);
    {
        KERNEL_RAW_SCOPE(g,&kernel_wait_domain);
        struct kernel_task *group=caller->group_leader;
        struct kernel_posix_timer_registry *r=group->posix_timers;
        if(!r){r=candidate;candidate=0;group->posix_timers=r;}
        if(!r)__builtin_trap();
        /* 注册表保存ID分配进度，线程间发布候选时只出现一个实例。 */
        r->references++;t->owner=r;
        uint32_t first=r->next_id;
        while(find_locked(r,r->next_id)) {
            r->next_id=(r->next_id+1)&INT32_MAX;
            if(r->next_id==first){error=-KERNEL_EAGAIN;break;}
        }
        t->id=r->next_id;r->next_id=(r->next_id+1)&INT32_MAX;
        t->event=event?*event:(struct kernel_posix_event){.signal=14,.notify=0,.value=t->id};
        int notify=t->event.notify;
        if(!error && notify!=0 && notify!=1 && notify!=2 && notify!=4)error=-KERNEL_EINVAL;
        if(!error && notify!=1 && (t->event.signal<=0 || t->event.signal>64))error=-KERNEL_EINVAL;
        if(!error) {
            target=notify==4?process_find_identity(t->event.tid,KERNEL_PID_TID):group;
            if(!target || target->group_leader!=group) {target=0;error=-KERNEL_EINVAL;}
            else {
                scheduler_task_get_locked(target);
                t->recipient=process_identity(target,notify==4?KERNEL_PID_TID:KERNEL_PID_TGID);
                kernel_pid_get(t->recipient);
            }
        }
    }
    if(candidate)kernel_heap_release(heap,candidate);
    if(error){timer_put(t);return error;}
    signal_pending_prepare_record(heap,&t->notification);
    if(t->event.notify!=1)signal_pending_prepare_index(target,t->event.notify!=4,heap,&t->notification);
    {
        KERNEL_RAW_SCOPE(g,&kernel_wait_domain);
        struct kernel_signal_pending_state *pending=t->event.notify==4?&target->signal_queue:&target->group_queue;
        if(!t->notification.record || (t->event.notify!=1&&!pending->queue&&!t->notification.index))error=-KERNEL_EAGAIN;
        else error=signal_pending_reserve_locked(caller);
        if(!error) {
            t->charged=1;
            if(t->event.notify!=1&&!pending->queue){pending->queue=t->notification.index;t->notification.index=0;}
            t->hash_next=t->owner->buckets[t->id%TIMER_BUCKETS];t->owner->buckets[t->id%TIMER_BUCKETS]=t;
            t->references++; /* private creator plus the not-yet-valid namespace entry */
        }
    }
    if(error){timer_put(t);return error;}
    *out=t;*id=(int32_t)t->id;return 0;
}
void kernel_posix_timer_publish(struct kernel_posix_timer *t,int success)
{
    {KERNEL_RAW_SCOPE(g,&kernel_wait_domain);if(success)t->valid=1;else unhash_locked(t);}
    if(!success)timer_put(t);
    timer_put(t);
}
int kernel_posix_timer_delete(struct kernel_task *caller,int32_t id)
{
    struct kernel_posix_timer *t=timer_get(caller,id);if(!t)return -KERNEL_EINVAL;
    int removed=0;
    {KERNEL_RAW_SCOPE(g,&kernel_wait_domain);if(t->valid){unhash_locked(t);removed=1;}}
    if(removed)timer_put(t);
    timer_put(t);return removed?0:-KERNEL_EINVAL;
}
int kernel_posix_timer_get(struct kernel_task *caller,int32_t id,struct kernel_posix_itimerspec *out)
{
    struct kernel_posix_timer *t=timer_get(caller,id);if(!t)return -KERNEL_EINVAL;
    *out=(struct kernel_posix_itimerspec){0};timer_put(t);return 0;
}
int kernel_posix_timer_overrun(struct kernel_task *caller,int32_t id)
{
    struct kernel_posix_timer *t=timer_get(caller,id);if(!t)return -KERNEL_EINVAL;
    int result;{KERNEL_RAW_SCOPE(g,&kernel_wait_domain);result=t->overrun;}
    timer_put(t);return result;
}
int kernel_posix_timer_set(struct kernel_task *caller,int32_t id,int flags,
    const struct kernel_posix_itimerspec *in,struct kernel_posix_itimerspec *old)
{
    (void)caller;(void)id;(void)flags;(void)in;(void)old;return -KERNEL_EOPNOTSUPP;
}
void kernel_posix_timers_exec(struct kernel_task *task)
{
    struct kernel_posix_timer_registry *r;
    {KERNEL_RAW_SCOPE(g,&kernel_wait_domain);r=task->group_leader->posix_timers;task->group_leader->posix_timers=0;}
    if(!r)return;
    for(unsigned i=0;i<TIMER_BUCKETS;i++)for(;;) {
        struct kernel_posix_timer *t;
        {KERNEL_RAW_SCOPE(g,&kernel_wait_domain);t=r->buckets[i];if(t)unhash_locked(t);}
        if(!t)break;
        timer_put(t);
    }
    registry_put(r);
}
void kernel_posix_timers_exit(struct kernel_task *task)
{
    if(task->group_leader==task && task->group_members<=1)kernel_posix_timers_exec(task);
}
void kernel_posix_timers_adopt_locked(struct kernel_task *to,struct kernel_task *from)
{
    if(to->posix_timers)__builtin_trap();
    to->posix_timers=from->posix_timers;from->posix_timers=0;
}
