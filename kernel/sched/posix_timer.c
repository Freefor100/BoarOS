#include "private.h"
#include "../signal_pending.h"
#include <kernel/posix_timer.h>
#include <kernel/errno.h>
#include <kernel/avl.h>
#include <kernel/time.h>
#include <arch/timer.h>
#if BOAROS_MAX_CPUS > 1
#include <arch/riscv/smp.h>
#endif
#include <string.h>

_Static_assert(sizeof(struct kernel_posix_event)==64,"Linux sigevent");
_Static_assert(sizeof(struct kernel_posix_itimerspec)==32,"Linux itimerspec64");
#define TIMER_BUCKETS 64U
struct kernel_posix_timer_registry {
    struct kernel_heap *heap;
    struct kernel_posix_timer *buckets[TIMER_BUCKETS];
    uint32_t next_id,references;
    struct kernel_posix_timer *parked;
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
    struct kernel_avl_node deadline;
    struct kernel_posix_timer *park_next;
    uint64_t queued_generation,queued_expiry,claim_generation,claim_expiry,claim_interval;
    struct kernel_signal_wait_info *claim_output;
    uint32_t indexed,armed,queued,busy,deferred,parked;

};
static struct kernel_avl_node *wall_root;
static uint64_t wall_next;
static void timer_changed(void)
{
#if BOAROS_MAX_CPUS > 1
    if(kernel_cpu_current()->logical_id){kernel_cpu_notify(riscv_smp_cpu(0),RISCV_IPI_DEADLINE);return;}
#endif
    kernel_scheduler_deadline_changed();
}
static void wall_publish_locked(void)
{
    struct kernel_avl_node *node=kernel_avl_first(wall_root);
    __atomic_store_n(&wall_next,node?node->key:0,__ATOMIC_RELEASE);
}
uint64_t kernel_posix_timer_deadline(void)
{return __atomic_load_n(&wall_next,__ATOMIC_ACQUIRE);}
static uint64_t add_time(uint64_t a,uint64_t b)
{return a>(uint64_t)INT64_MAX-b?INT64_MAX:a+b;}
static uint64_t forward_time(uint64_t expires,uint64_t interval,uint64_t now,uint64_t *overrun)
{
    uint64_t periods=now>=expires?(now-expires)/interval+1:1;
    if(overrun)*overrun=periods-1;
    uint64_t delta=periods>(uint64_t)INT64_MAX/interval?INT64_MAX:periods*interval;
    return add_time(expires,delta);
}
static uint64_t timer_now(struct kernel_posix_timer *t)
{return t->clock==0?kernel_time_realtime_ns():kernel_time_monotonic_ns();}
static void unpark_locked(struct kernel_posix_timer *t)
{
    if(!t->parked)return;
    struct kernel_posix_timer **link=&t->owner->parked;
    while(*link!=t){if(!*link)__builtin_trap();link=&(*link)->park_next;}
    *link=t->park_next;t->park_next=0;t->parked=0;
}
static void deactivate_locked(struct kernel_posix_timer *t)
{
    if(t->indexed){wall_root=kernel_avl_remove_node(wall_root,&t->deadline);t->indexed=0;wall_publish_locked();}
    t->armed=0;unpark_locked(t);
}
static void arm_locked(struct kernel_posix_timer *t)
{
    if(t->dead||t->event.notify==1||!t->expires)return;
    if(t->cpu.identity)return; /* CPU target thresholds are supplied by task 3. */
    uint64_t mono=t->expires;
    if(!t->clock){uint64_t offset=kernel_time_boot_realtime_offset();mono=mono<=offset?0:mono-offset;}
    t->deadline=(struct kernel_avl_node){.height=1};
    enum kernel_time_status status=kernel_time_deadline_from_monotonic(mono,&t->deadline.key);
    if(status==KERNEL_TIME_STATUS_DEADLINE_PASSED)t->deadline.key=arch_time_read()+1;
    else if(status)__builtin_trap();
    t->armed=1;t->indexed=1;wall_root=kernel_avl_insert_node(wall_root,&t->deadline);wall_publish_locked();
}
static void snapshot_locked(struct kernel_posix_timer *t,uint64_t now,struct kernel_posix_itimerspec *out)
{
    *out=(struct kernel_posix_itimerspec){.interval={t->interval/1000000000,t->interval%1000000000}};
    uint64_t remaining=0;
    int pending=(t->queued&&t->queued_generation==t->generation)||
        (t->busy&&t->claim_generation==t->generation)||t->parked;
    if(t->event.notify==1||t->armed||pending) {
        if(t->interval&&!t->armed&&t->expires<=now)t->expires=forward_time(t->expires,t->interval,now,0);
        remaining=t->expires>now?t->expires-now:((t->event.notify!=1&&t->expires)?1:0);
    }
    out->value=(struct kernel_posix_timespec){remaining/1000000000,remaining%1000000000};
}
static void queue_locked(struct kernel_posix_timer *t,struct kernel_wait_cursor *cursor,struct kernel_task **wake)
{
    t->armed=0;
    if(t->busy){t->deferred=1;t->queued_expiry=t->expires;return;}
    t->queued_generation=t->generation;t->queued_expiry=t->expires;
    if(t->queued)return;
    struct signal_queue_record *r=t->notification.record;
    r->info=(struct kernel_signal_wait_info){.signal=(uint32_t)t->event.signal,.code=-2,.sender=t->id,.value=t->event.value};
    int status=signal_timer_post_locked(t->recipient,t->event.notify==4,r,cursor,wake);
    if(status==1){t->references++;t->queued=1;}
    else if(status==2&&t->interval&&!t->parked){t->parked=1;t->park_next=t->owner->parked;t->owner->parked=t;}
}
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
    *link=t->hash_next;t->hash_next=0;t->valid=0;t->dead=1;t->generation++;deactivate_locked(t);
}
static void registry_put(struct kernel_posix_timer_registry *r)
{
    int free_it;
    {KERNEL_RAW_SCOPE(g,&kernel_wait_domain);if(!r->references)__builtin_trap();free_it=!--r->references;}
    if(free_it)kernel_heap_release(r->heap,r);
}
static int timer_put_locked(struct kernel_posix_timer *t)
{
    if(!t->references)__builtin_trap();
    if(--t->references)return 0;
    if(t->charged){signal_pending_unreserve_locked();t->charged=0;}
    return 1;
}
static void timer_free(struct kernel_posix_timer *t)
{
    struct kernel_posix_timer_registry *r=t->owner;
    if(t->notification.record)t->notification.record->timer=0;
    signal_pending_finish(&t->notification);
    if(t->recipient)kernel_task_identity_put(t->recipient);
    if(t->cpu.identity)kernel_task_identity_put(t->cpu.identity);
    kernel_heap_release(t->heap,t);
    if(r)registry_put(r);
}
static void timer_put(struct kernel_posix_timer *t)
{
    int free_it;{KERNEL_RAW_SCOPE(g,&kernel_wait_domain);free_it=timer_put_locked(t);}
    if(free_it)timer_free(t);
}
void posix_timer_record_free(struct signal_queue_record *record)
{timer_free(record->timer);}
static void retire_record_locked(struct kernel_posix_timer *t,struct signal_pending_prepared *p)
{
    if(timer_put_locked(t)){struct signal_queue_record *r=t->notification.record;r->next=p->retired;p->retired=r;}
}
int posix_timer_record_take_locked(struct signal_queue_record *record,struct kernel_signal_wait_info *out,struct signal_pending_prepared *p)
{
    struct kernel_posix_timer *t=record->timer;t->queued=0;
    if(t->dead||t->queued_generation!=t->generation){out->signal=0;retire_record_locked(t,p);return 0;}
    if(t->busy)__builtin_trap();
    t->busy=1;t->claim_generation=t->generation;t->claim_expiry=t->queued_expiry;
    t->claim_interval=t->interval;t->claim_output=out;
    record->next=p->completed;p->completed=record;
    return 1;
}
void posix_timer_record_clear_locked(struct signal_queue_record *record,int ignored,struct signal_pending_prepared *p)
{
    struct kernel_posix_timer *t=record->timer;t->queued=0;
    if(ignored&&!t->dead&&t->queued_generation==t->generation&&t->interval&&!t->parked){
        t->parked=1;t->park_next=t->owner->parked;t->owner->parked=t;
    }
    retire_record_locked(t,p);
}
void posix_timer_record_complete(struct signal_queue_record *record)
{
    struct kernel_posix_timer *t=record->timer;
    uint64_t now=timer_now(t);struct kernel_wait_cursor cursor={0};struct kernel_task *wake=0;
    int free_it;
    {
        KERNEL_RAW_SCOPE(g,&kernel_wait_domain);uint64_t overrun=0,next=0;
        if(t->claim_interval)next=forward_time(t->claim_expiry,t->claim_interval,now,&overrun);
        int over=overrun>INT32_MAX?INT32_MAX:(int)overrun;
        t->claim_output->uid=(uint32_t)over;t->claim_output=0;t->busy=0;
        if(!t->dead&&t->claim_generation==t->generation){t->overrun=over;t->expires=next;if(next)arm_locked(t);}
        if(!t->dead&&t->deferred){t->deferred=0;queue_locked(t,&cursor,&wake);}
        free_it=timer_put_locked(t);
    }
    if(cursor.queue)kernel_wait_cursor_wake(&cursor,1);
    if(wake){(void)kernel_scheduler_wake_signal(wake);scheduler_task_put(wake);}
    timer_changed();if(free_it)timer_free(t);
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
    enum kernel_heap_status allocation=kernel_heap_allocate_zeroed(heap,1,sizeof(*t),(void **)&t);
    if(allocation==KERNEL_HEAP_STATUS_EMPTY)return -KERNEL_EAGAIN;
    if(allocation)__builtin_trap();
    t->heap=heap;t->references=1;t->clock=clock;t->generation=1;
    if(clock<0||clock==2||clock==3) {
        int error=scheduler_cpu_clock_resolve(caller,clock,1,&t->cpu);
        if(error){timer_put(t);return error;}
    }
    struct kernel_posix_timer_registry *candidate=0;int need_registry;
    {KERNEL_RAW_SCOPE(g,&kernel_wait_domain);need_registry=!caller->group_leader->posix_timers;}
    if(need_registry) {
        candidate=0;
        allocation=kernel_heap_allocate_zeroed(heap,1,sizeof(*candidate),(void **)&candidate);
        if(allocation==KERNEL_HEAP_STATUS_EMPTY){timer_put(t);return -KERNEL_EAGAIN;}
        if(allocation)__builtin_trap();
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
        int target_exited=t->event.notify==4&&target->proc_exiting;
        if(t->event.notify==4&&(!t->recipient->members[KERNEL_PID_TID]||t->recipient->members[KERNEL_PID_TID]->task!=target))error=-KERNEL_EINVAL;
        else if(!t->notification.record || (t->event.notify!=1&&!target_exited&&!pending->queue&&!t->notification.index))error=-KERNEL_EAGAIN;
        else error=signal_pending_reserve_locked(caller);
        if(!error) {
            t->charged=1;t->notification.record->timer=t;
            if(t->event.notify!=1&&!target_exited&&!pending->queue)signal_pending_install_index_locked(target,t->event.notify!=4,&t->notification);
            t->hash_next=t->owner->buckets[t->id%TIMER_BUCKETS];t->owner->buckets[t->id%TIMER_BUCKETS]=t;
            t->references++; /* private creator plus the not-yet-valid namespace entry */
        }
    }
    if(error){timer_put(t);return error;}
    struct signal_pending_prepared unused={.index=t->notification.index};t->notification.index=0;
    signal_pending_finish(&unused);
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
    timer_put(t);timer_changed();return removed?0:-KERNEL_EINVAL;
}
int kernel_posix_timer_get(struct kernel_task *caller,int32_t id,struct kernel_posix_itimerspec *out)
{
    struct kernel_posix_timer *t=timer_get(caller,id);if(!t)return -KERNEL_EINVAL;
    int error=0;
    {KERNEL_RAW_SCOPE(g,&kernel_wait_domain);if(!t->valid)error=-KERNEL_EINVAL;else snapshot_locked(t,timer_now(t),out);}
    timer_put(t);return error;
}
int kernel_posix_timer_overrun(struct kernel_task *caller,int32_t id)
{
    struct kernel_posix_timer *t=timer_get(caller,id);if(!t)return -KERNEL_EINVAL;
    int result;{KERNEL_RAW_SCOPE(g,&kernel_wait_domain);result=t->valid?t->overrun:-KERNEL_EINVAL;}
    timer_put(t);return result;
}
int kernel_posix_timer_set(struct kernel_task *caller,int32_t id,int flags,
    const struct kernel_posix_itimerspec *in,struct kernel_posix_itimerspec *old)
{
    if(in->value.seconds<0||in->interval.seconds<0||
        (uint64_t)in->value.nanoseconds>=1000000000||(uint64_t)in->interval.nanoseconds>=1000000000)
        return -KERNEL_EINVAL;
    struct kernel_posix_timer *t=timer_get(caller,id);if(!t)return -KERNEL_EINVAL;
    if(t->cpu.identity){timer_put(t);return -KERNEL_EOPNOTSUPP;}
    uint64_t now=timer_now(t);
    uint64_t value=in->value.seconds>INT64_MAX/1000000000?INT64_MAX:add_time((uint64_t)in->value.seconds*1000000000,(uint64_t)in->value.nanoseconds);
    uint64_t interval=in->interval.seconds>INT64_MAX/1000000000?INT64_MAX:add_time((uint64_t)in->interval.seconds*1000000000,(uint64_t)in->interval.nanoseconds);
    int error=0;
    {
        KERNEL_RAW_SCOPE(g,&kernel_wait_domain);
        if(!t->valid)error=-KERNEL_EINVAL;
        else {
            snapshot_locked(t,now,old);deactivate_locked(t);t->generation++;t->deferred=0;t->overrun=0;
            t->interval=value?interval:0;
            if(value){t->expires=(flags&1)?value:add_time(now,value);arm_locked(t);}
            else if(t->event.notify!=1)t->expires=0;
        }
    }
    timer_put(t);timer_changed();return error;
}
void kernel_posix_timer_expire(uint64_t now)
{
    for(;;) {
        struct kernel_posix_timer *t;struct kernel_wait_cursor cursor={0};struct kernel_task *wake=0;
        {
            KERNEL_RAW_SCOPE(g,&kernel_wait_domain);struct kernel_avl_node *node=kernel_avl_first(wall_root);
            if(!node||(int64_t)(now-node->key)<0)break;
            t=(void *)((char *)node- offsetof(struct kernel_posix_timer,deadline));t->references++;
            deactivate_locked(t);queue_locked(t,&cursor,&wake);
        }
        if(cursor.queue)kernel_wait_cursor_wake(&cursor,1);
        if(wake){(void)kernel_scheduler_wake_signal(wake);scheduler_task_put(wake);}
        timer_put(t);
    }
}
void kernel_posix_timer_unignore(struct kernel_task *task,unsigned sig)
{
    for(;;) {
        struct kernel_posix_timer *t=0;struct kernel_wait_cursor cursor={0};struct kernel_task *wake=0;
        {
            KERNEL_RAW_SCOPE(g,&kernel_wait_domain);
            struct kernel_posix_timer_registry *r=task->group_leader->posix_timers;
            for(t=r?r->parked:0;t;t=t->park_next) {
                if((unsigned)t->event.signal==sig&&!signal_timer_disposition_ignored_locked(t->recipient,t->event.notify==4,sig))break;
            }
            if(!t)break;
            t->references++;unpark_locked(t);t->expires=t->queued_expiry;queue_locked(t,&cursor,&wake);
        }
        if(cursor.queue)kernel_wait_cursor_wake(&cursor,1);
        if(wake){(void)kernel_scheduler_wake_signal(wake);scheduler_task_put(wake);}
        timer_put(t);
    }
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
    registry_put(r);timer_changed();
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
