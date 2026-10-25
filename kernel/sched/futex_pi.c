#include "futex_internal.h"
#include <kernel/time.h>
#include <arch/timer.h>
#include <kernel/heap.h>
#include <string.h>

#define PI_CHAIN_LIMIT 1024U

static void waiter_remove(struct kernel_futex_pi_request *request)
{
    struct kernel_futex_pi_state *state=request->state;
    if(!request->queued)__builtin_trap();
    if(request->previous)request->previous->next=request->next;else state->head=request->next;
    if(request->next)request->next->previous=request->previous;else state->tail=request->previous;
    request->previous=request->next=0;request->queued=0;
}
static void waiter_insert(struct kernel_futex_pi_request *request)
{
    struct kernel_futex_pi_state *state=request->state;
    struct kernel_futex_pi_request *next=state->head,*previous=0;
    if(request->queued)__builtin_trap();
    while(next && (next->task->scheduling.priority>request->task->scheduling.priority ||
          (next->task->scheduling.priority==request->task->scheduling.priority && next->order<request->order))) {
        previous=next;next=next->next;
    }
    request->previous=previous;request->next=next;request->queued=1;
    if(previous)previous->next=request;else state->head=request;
    if(next)next->previous=request;else state->tail=request;
}
void kernel_futex_pi_recalculate_locked(struct kernel_task *task,int base)
{
    /* 新边已排除环；逐个末端追加可产生长于单次查环上限的既有链。 */
    while(task) {
        unsigned inherited=0;
        for(struct kernel_futex_pi_state *p=task->pi_owned;p;p=p->owner_next)
            if(p->head && (unsigned)p->head->task->scheduling.priority>inherited)
                inherited=(unsigned)p->head->task->scheduling.priority;
        unsigned requested=base<0?scheduler_base_priority(task):(unsigned)base;
        int old=task->scheduling.priority;
        scheduler_pi_priority_locked(task,requested,inherited);
        struct kernel_futex_pi_request *request=task->pi_request;
        if(old==task->scheduling.priority || !request || !request->queued)break;
        waiter_remove(request);waiter_insert(request);
        task=request->state->owner;base=-1;
    }
}

static uint64_t waiter_order;
static struct kernel_futex_pi_state *state_find(struct futex_bucket *bucket,
    const struct kernel_futex_key *key)
{
    for(struct kernel_futex_pi_state *p=bucket->pi_states;p;p=p->hash_next)
        if(futex_key_equal(&p->key,key))return p;
    return 0;
}
int futex_pi_conflict_locked(struct futex_bucket *bucket,const struct kernel_futex_key *key)
{return state_find(bucket,key)!=0;}
static void state_get(struct kernel_futex_pi_state *state)
{if(!state->references || state->references==UINT32_MAX)__builtin_trap();state->references++;}
static void state_detach(struct kernel_futex_pi_state *state)
{
    if(!state->attached)return;
    struct kernel_futex_pi_state **link=&state->bucket->pi_states;
    while(*link && *link!=state)link=&(*link)->hash_next;
    if(!*link)__builtin_trap();
    if(link==&state->bucket->pi_states)__atomic_store_n(link,state->hash_next,__ATOMIC_RELEASE);
    else *link=state->hash_next;
    state->hash_next=0;state->attached=0;
}
/* 返回旧任务的真实借用，调用者在放下短锁/桶资格后归还。 */
static struct kernel_task *owner_set(struct kernel_futex_pi_state *state,struct kernel_task *owner)
{
    struct kernel_task *old=state->owner;
    if(old==owner)return 0;
    if(owner)scheduler_task_get_locked(owner);
    if(old) {
        if(state->owner_previous)state->owner_previous->owner_next=state->owner_next;
        else {if(old->pi_owned!=state)__builtin_trap();old->pi_owned=state->owner_next;}
        if(state->owner_next)state->owner_next->owner_previous=state->owner_previous;
    }
    state->owner=owner;state->owner_previous=state->owner_next=0;
    if(owner) {
        state->owner_next=owner->pi_owned;
        if(owner->pi_owned)owner->pi_owned->owner_previous=state;
        owner->pi_owned=state;
    }
    if(old && old->futex_exit_state!=1)kernel_futex_pi_recalculate_locked(old,-1);
    if(owner)kernel_futex_pi_recalculate_locked(owner,-1);
    return old;
}
static int state_drop_locked(struct kernel_futex_pi_state *state,struct kernel_task **owner)
{
    if(!state->references)__builtin_trap();
    if(--state->references)return 0;
    if(state->head || state->tail)__builtin_trap();
    state_detach(state);*owner=owner_set(state,0);return 1;
}
static void state_release(struct kernel_futex_pi_state *state)
{futex_key_release(&state->key);kernel_heap_release(state->heap,state);}
static void state_put(struct kernel_futex_pi_state *state)
{
    struct kernel_task *old=0;int release;
    {KERNEL_RAW_SCOPE(g,&kernel_wait_domain);release=state_drop_locked(state,&old);}
    if(old)scheduler_task_put(old);
    if(release)state_release(state);
}
static int normal_waiter(struct futex_bucket *bucket,const struct kernel_futex_key *key)
{
    if(!__atomic_load_n(&bucket->waiters.registrations,__ATOMIC_RELAXED))return 0;
    int found=0;
    struct kernel_wait_cursor cursor;
    kernel_wait_cursor_begin(&bucket->waiters,&cursor);
    while(cursor.node) {
        {
            KERNEL_RAW_SCOPE(g,&kernel_wait_domain);
            struct kernel_wait_node *node=cursor.node;
            if(node->queue==&bucket->waiters && node->task &&
               !futex_entry(node)->woken && futex_key_equal(&futex_entry(node)->key,key))found=1;
        }
        if(found)break;
        kernel_wait_cursor_advance(&cursor);
    }
    kernel_wait_cursor_end(&cursor);
    return found;
}
static int cycle(struct kernel_task *owner,struct kernel_task *waiter)
{
    unsigned depth=0;
    while(owner) {
        if(owner==waiter || ++depth>=PI_CHAIN_LIMIT)return 1;
        struct kernel_futex_pi_request *request=owner->pi_request;
        if(!request || !request->queued)break;
        owner=request->state->owner;
    }
    return 0;
}
static void release_preparation(struct kernel_futex_pi_request *request)
{
    if(request->pin.mm)kernel_mm_unpin_user_page(&request->pin);
    if(request->source_pin.mm)kernel_mm_unpin_user_page(&request->source_pin);
    futex_key_release(&request->key);
}
static struct kernel_task *handoff(struct kernel_futex_pi_state *state,int fixup)
{
    struct kernel_futex_pi_request *next=state->head;
    if(!next) {
        state_detach(state);state->needs_fixup=0;
        return owner_set(state,0);
    }
    waiter_remove(next);
    struct kernel_task *old=owner_set(state,next->task);
    state->needs_fixup=(unsigned)fixup;
    __atomic_store_n(&next->granted,fixup?2U:1U,__ATOMIC_RELEASE);
    if(!fixup)state->word_tid=(uint32_t)next->task->tid;
    (void)kernel_wait_notify_locked(&next->token,KERNEL_WAIT_WOKEN);
    return old;
}
static void request_finish(struct kernel_futex_pi_request *request,int abandoned)
{
    struct kernel_task *old=0,*retired_owner=0;int release=0;
    KERNEL_LOCK_SCOPE(departure);
    struct kernel_cpu_notification_batch notifications;
    int departing=abandoned && request->granted && request->state;
    if(departing) {
        kernel_mutex_lock(&request->state->bucket->operation,&departure);
        kernel_cpu_notification_batch_begin(&notifications);
    }
    {
        KERNEL_RAW_SCOPE(g,&kernel_wait_domain);
        if(request->queued) {
            waiter_remove(request);
            if(request->state->owner)kernel_futex_pi_recalculate_locked(request->state->owner,-1);
        }
        if(abandoned && request->granted && request->state && request->state->owner==request->task)
            old=handoff(request->state,1);
        if(request->task->pi_request!=request)__builtin_trap();
        request->task->pi_request=0;
        /* 注销栈请求与归还它的状态引用一次发布，不重复进调度域。 */
        if(request->state)release=state_drop_locked(request->state,&retired_owner);
    }
    if(departing){kernel_lock_release(&departure);kernel_cpu_notification_batch_end(&notifications);}
    release_preparation(request);
    if(request->exiting){scheduler_task_put(request->exiting);request->exiting=0;}
    if(old)scheduler_task_put(old);
    if(retired_owner)scheduler_task_put(retired_owner);
    if(release)state_release(request->state);
    request->state=0;
    /* 未发布候选始终由本调用独占，无key、owner或等待者需要解挂。 */
    if(request->candidate)kernel_heap_release(request->candidate->heap,request->candidate);
    request->candidate=0;
}
void kernel_futex_pi_abort(struct kernel_task *task)
{
    struct kernel_futex_pi_request *request=task->pi_request;
    if(!request)return;
    /* process_begin_exit 已结束真实等待；旧栈仅剩未归还的操作owner。 */
    if(task->wait.phase!=KERNEL_WAIT_FINISHED)__builtin_trap();
    request_finish(request,1);
}
static int prepare_word(struct kernel_futex_pi_request *request,enum kernel_scheduler_status *status)
{
    return (int)futex_prepare(request->task,&request->address,1,request->operation,
        0,KERNEL_MM_WRITE,&request->key,&request->pin,status);
}
static uint32_t *request_word(struct kernel_futex_pi_request *request)
{return (void *)((unsigned char *)request->pin.page+(request->address&BOAROS_PAGE_MASK));}
static int fixup_dead_owner(struct kernel_futex_pi_request *request,enum kernel_scheduler_status *status)
{
    int result=prepare_word(request,status);
    if(result || *status)return result;
    uint32_t *word=request_word(request);
    {
        KERNEL_RAW_SCOPE(g,&kernel_wait_domain);
        struct kernel_futex_pi_state *state=request->state;
        if(state->owner!=request->task || !state->needs_fixup)__builtin_trap();
        uint32_t value=__atomic_load_n(word,__ATOMIC_ACQUIRE);
        uint32_t next=(uint32_t)request->task->tid|FUTEX_WAITERS|FUTEX_OWNER_DIED;
        if(!__atomic_compare_exchange_n(word,&value,next,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST))result=-KERNEL_EAGAIN;
        else {state->word_tid=(uint32_t)request->task->tid;state->needs_fixup=0;request->granted=1;}
    }
    release_preparation(request);
    return result;
}
static int wait_exiting_owner(struct kernel_futex_pi_request *request)
{
    struct kernel_task *owner=request->exiting;
    for(;;) {
        struct kernel_wait_token token={0};
        {
            KERNEL_RAW_SCOPE(g,&kernel_wait_domain);
            if(owner->futex_exit_state!=1 && (!owner->proc_exiting || owner->futex_exit_state==2))break;
            if(kernel_wait_prepare_locked(&owner->group_wait_queue,0,0,&token))__builtin_trap();
        }
        enum kernel_wait_wake_reason reason;
        if(kernel_wait_park(&token,&reason) || kernel_wait_finish(&token))__builtin_trap();
    }
    scheduler_task_put(owner);request->exiting=0;
    return 0;
}
enum pi_action { AGAIN,ALLOCATE,EXITING,WAITING,FINISHED };
static struct kernel_futex_pi_state *state_attach(struct kernel_futex_pi_request *request,
    struct futex_bucket *bucket,struct kernel_task *owner,uint32_t tid)
{
    struct kernel_futex_pi_state *state=request->candidate;
    request->candidate=0;request->state=state;
    state->key=request->key;request->key=(struct kernel_futex_key){0};
    state->bucket=bucket;state->hash_next=bucket->pi_states;
    __atomic_store_n(&bucket->pi_states,state,__ATOMIC_RELEASE);
    state->attached=1;state->word_tid=tid;
    (void)owner_set(state,owner);return state;
}
/* 普通LOCK与代理重排共用用户字/owner接纳，实际受益线程是request->task。 */
static enum pi_action claim_locked(struct kernel_futex_pi_request *request,
    struct futex_bucket *bucket,uint32_t *word,int mixed,int try_only,int force_state,int *result)
{
    struct kernel_task *task=request->task;
    uint32_t value=__atomic_load_n(word,__ATOMIC_ACQUIRE),tid=(uint32_t)task->tid;
    struct kernel_futex_pi_state *state=state_find(bucket,&request->key);
    if((value&FUTEX_TID_MASK)==tid)*result=-KERNEL_EDEADLK;
    else if(mixed)*result=-KERNEL_EINVAL;
    else if(state) {
        uint32_t owner=value&FUTEX_TID_MASK;
        if(!state->owner || (owner!=state->word_tid &&
           !((state->needs_fixup || state->owner->futex_exit_state==1) && !owner && (value&FUTEX_OWNER_DIED))))*result=-KERNEL_EINVAL;
        else if(try_only)*result=-KERNEL_EAGAIN;
        else if(cycle(state->owner,task))*result=-KERNEL_EDEADLK;
        else if(!__atomic_compare_exchange_n(word,&value,value|FUTEX_WAITERS,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST))return AGAIN;
        else {state_get(state);request->state=state;return WAITING;}
    } else if(!(value&FUTEX_TID_MASK)) {
        if(force_state && !request->candidate)return ALLOCATE;
        uint32_t next=(value&FUTEX_OWNER_DIED)|tid|(force_state?FUTEX_WAITERS:0);
        if(!__atomic_compare_exchange_n(word,&value,next,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST))return AGAIN;
        if(force_state)(void)state_attach(request,bucket,task,tid);
    } else {
        uint32_t desired=value|FUTEX_WAITERS;
        if(!__atomic_compare_exchange_n(word,&value,desired,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST))return AGAIN;
        struct kernel_task *owner=scheduler_task_lookup_locked((int32_t)(value&FUTEX_TID_MASK),KERNEL_PID_TID,0);
        request->exiting=owner;
        if(!owner || owner->futex_exit_state==2) {
            if(__atomic_load_n(word,__ATOMIC_ACQUIRE)!=desired)return AGAIN;
            *result=-KERNEL_ESRCH;
        } else if(!owner->arch.user_mode)*result=-KERNEL_EPERM;
        else if(owner->proc_exiting || owner->futex_exit_state==1)return EXITING;
        else if(try_only)*result=-KERNEL_EAGAIN;
        else if(cycle(owner,task))*result=-KERNEL_EDEADLK;
        else if(!request->candidate)return ALLOCATE;
        else {(void)state_attach(request,bucket,owner,value&FUTEX_TID_MASK);return WAITING;}
    }
    return FINISHED;
}
static int allocate_state(struct kernel_futex_pi_request *request)
{
    enum kernel_heap_status status=kernel_heap_allocate_zeroed(request->task->files.heap,1,
        sizeof(*request->candidate),(void **)&request->candidate);
    if(status)return status==KERNEL_HEAP_STATUS_EMPTY?-KERNEL_ENOMEM:-KERNEL_EIO;
    request->candidate->heap=request->task->files.heap;request->candidate->references=1;return 0;
}
static int pi_deadline(struct kernel_task *task,uint64_t timeout,int realtime,
    uint64_t *deadline,int *expired)
{
    if(!timeout)return 0;
    struct {int64_t seconds,nanoseconds;} ts;size_t copied;
    if(kernel_copy_from_user(&task->mm,&ts,timeout,sizeof(ts),&copied) || copied!=sizeof(ts))return -KERNEL_EFAULT;
    if(ts.seconds<0 || ts.nanoseconds<0 || ts.nanoseconds>=1000000000)return -KERNEL_EINVAL;
    __uint128_t wide=(__uint128_t)(uint64_t)ts.seconds*1000000000+(uint64_t)ts.nanoseconds;
    uint64_t ns=wide>INT64_MAX?INT64_MAX:(uint64_t)wide;
    if(realtime){uint64_t offset=kernel_time_boot_realtime_offset();ns=ns<=offset?0:ns-offset;}
    enum kernel_time_status converted=kernel_time_deadline_from_monotonic(ns,deadline);
    if(converted==KERNEL_TIME_STATUS_DEADLINE_PASSED)*expired=1;
    else if(converted!=KERNEL_TIME_STATUS_OK)__builtin_trap();
    return 0;
}
static int pi_unlock(struct kernel_futex_pi_request *request,enum kernel_scheduler_status *status)
{
    uint32_t value;size_t copied;
    if(kernel_copy_from_user(&request->task->mm,&value,request->address,sizeof(value),&copied) || copied!=sizeof(value))return -KERNEL_EFAULT;
    if((value&FUTEX_TID_MASK)!=(uint32_t)request->task->tid)return -KERNEL_EPERM;
    int result=prepare_word(request,status);
    if(result || *status)return result;
    struct futex_bucket *bucket=futex_bucket(&request->key);
    KERNEL_LOCK_SCOPE(operation);kernel_mutex_lock(&bucket->operation,&operation);
    int mixed=normal_waiter(bucket,&request->key);
    struct kernel_cpu_notification_batch notifications;
    kernel_cpu_notification_batch_begin(&notifications);
    struct kernel_task *old=0;
    {
        KERNEL_RAW_SCOPE(g,&kernel_wait_domain);
        uint32_t *word=request_word(request),actual=__atomic_load_n(word,__ATOMIC_ACQUIRE);
        struct kernel_futex_pi_state *state=state_find(bucket,&request->key);
        if(mixed || (state && state->owner!=request->task))result=-KERNEL_EINVAL;
        else if(actual!=value)result=-KERNEL_EAGAIN;
        else {
            uint32_t next=state && state->head ? FUTEX_WAITERS|(uint32_t)state->head->task->tid:0;
            if(!__atomic_compare_exchange_n(word,&actual,next,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST))result=-KERNEL_EAGAIN;
            else if(state) {
                state_get(state);request->state=state;
                old=handoff(state,0);
            }
        }
    }
    kernel_lock_release(&operation);
    kernel_cpu_notification_batch_end(&notifications);
    release_preparation(request);
    if(old)scheduler_task_put(old);
    return result;
}

int64_t kernel_futex_pi(struct kernel_task *task,uint64_t address,uint32_t operation,
    uint64_t timeout,enum kernel_scheduler_status *status)
{
    unsigned command=operation&~(FUTEX_PRIVATE|FUTEX_CLOCK_REALTIME);
    uint64_t deadline=0;int expired=0;
    if(timeout && (command==6 || command==13)) {
        int error=pi_deadline(task,timeout,command==6 || (operation&FUTEX_CLOCK_REALTIME),&deadline,&expired);
        if(error)return error;
    }
    if((operation&FUTEX_CLOCK_REALTIME) && command!=13)return -KERNEL_ENOSYS;
    struct kernel_futex_pi_request request={.task=task,.address=address,.operation=operation};
    {
        KERNEL_RAW_SCOPE(g,&kernel_wait_domain);
        if(task->pi_request)__builtin_trap();
        task->pi_request=&request;
    }
    int result=0;
    if(command==7) {
        do {result=pi_unlock(&request,status);} while(result==-KERNEL_EAGAIN);
        request_finish(&request,0);return result;
    }
    enum pi_action action;
    unsigned retries=0;
    for(;;) {
        result=prepare_word(&request,status);
        if(result || *status)break;
        struct futex_bucket *bucket=futex_bucket(&request.key);
        KERNEL_LOCK_SCOPE(operation_guard);kernel_mutex_lock(&bucket->operation,&operation_guard);
        int mixed=normal_waiter(bucket,&request.key);
        struct kernel_cpu_notification_batch notifications;
        kernel_cpu_notification_batch_begin(&notifications);
        action=FINISHED;
#if BOAROS_MAX_CPUS > 1
        int spin=0;
#endif
        {
            KERNEL_RAW_SCOPE(g,&kernel_wait_domain);
            action=claim_locked(&request,bucket,request_word(&request),mixed,command==8,0,&result);
            if(action==WAITING) {
                if(waiter_order==UINT64_MAX)__builtin_trap();
                request.order=++waiter_order;
                if(kernel_wait_prepare_locked(0,deadline,1,&request.token))__builtin_trap();
                waiter_insert(&request);kernel_futex_pi_recalculate_locked(request.state->owner,-1);
                if(expired)(void)kernel_wait_notify_locked(&request.token,KERNEL_WAIT_TIMEOUT);
#if BOAROS_MAX_CPUS > 1
                struct kernel_task *owner=request.state->owner;
                spin=!expired && request.state->head==&request && owner->wait.on_cpu &&
                    owner->state==KERNEL_THREAD_STATE_RUNNING && owner->cpu &&
                    owner->cpu!=kernel_cpu_current() && owner->cpu->current==owner;
#endif
            }
        }
        kernel_lock_release(&operation_guard);
        /* 与普通futex共用通知批次，远端收到IPI时桶已可取得。 */
        kernel_cpu_notification_batch_end(&notifications);
        release_preparation(&request);
        if(action!=EXITING && request.exiting) {scheduler_task_put(request.exiting);request.exiting=0;}
        if(action==FINISHED)break;
        if(action==ALLOCATE) {
            result=allocate_state(&request);if(result)break;
        } else if(action==EXITING)wait_exiting_owner(&request);
        else if(action==WAITING) {
#if BOAROS_MAX_CPUS > 1
            if(spin) {
                COST_SCOPE(spin_cost,LOCK_SPIN_TICKS);
                uint64_t now=kernel_time_monotonic_ns(),until;
                /* 队首和运行owner快照只决定有界等待；所有权仍仅由handoff授予。
                 * 锁外仅轮询本请求，不保留可能已退出的旧owner地址。 */
                if(now<=UINT64_MAX-50000 && kernel_time_deadline_from_monotonic(now+50000,&until)==KERNEL_TIME_STATUS_OK) {
                    if(deadline && deadline<until)until=deadline;
                    while(!__atomic_load_n(&request.granted,__ATOMIC_ACQUIRE) &&
                          !__atomic_load_n(&kernel_cpu_current()->need_resched,__ATOMIC_RELAXED) &&
                          (int64_t)(arch_time_read()-until)<0) {}
                }
            }
#endif
            enum kernel_wait_wake_reason reason;
            *status=kernel_wait_park(&request.token,&reason);
            {
                KERNEL_RAW_SCOPE(g,&kernel_wait_domain);
                if(request.queued) {waiter_remove(&request);kernel_futex_pi_recalculate_locked(request.state->owner,-1);}
            }
            if(kernel_wait_finish(&request.token))__builtin_trap();
            if(*status)break;
            if(request.granted==2) {
                do {result=fixup_dead_owner(&request,status);}while(result==-KERNEL_EAGAIN && !*status);
            } else if(!request.granted) {
                if(reason==KERNEL_WAIT_TIMEOUT)result=-KERNEL_ETIMEDOUT;
                else if(reason==KERNEL_WAIT_SIGNALLED) {
                    kernel_signal_note_syscall_restart(task);
                    task->syscall_restart.kind=KERNEL_SYSCALL_RESTART_NOINTR;
                    result=-KERNEL_ERESTARTSYS;
                } else __builtin_trap();
            }
            break;
        } else if(++retries%16==0) {
            if(kernel_scheduler_yield_current())__builtin_trap();
        }
    }
    /* 用户字修复EFAULT不等于线程退出；其他等待者仍借用这个真实owner。 */
    request_finish(&request,0);
    return result;
}

/* requeue_stage: 1在条件队列，2已交给PI队列，3正在完成。 */
int64_t kernel_futex_wait_requeue_pi(struct kernel_task *task,uint64_t address,
    uint32_t operation,uint32_t value,uint64_t timeout,uint64_t target,
    enum kernel_scheduler_status *status)
{
    uint64_t deadline=0;int expired=0;
    int result=pi_deadline(task,timeout,operation&FUTEX_CLOCK_REALTIME,&deadline,&expired);
    if(result)return result;
    if(address==target)return -KERNEL_EINVAL;
    struct kernel_futex_pi_request pi_request={.task=task,.address=target,.operation=operation,.requeue_stage=1};
    struct kernel_futex_entry entry={.node={.task=task},.kind=FUTEX_ENTRY_REQUEUE_PI,.bitset=UINT32_MAX,.pi=&pi_request};
    struct kernel_futex_request request={.task=task,.entries=&entry,.count=1};
    {KERNEL_RAW_SCOPE(g,&kernel_wait_domain);
     if(task->pi_request || task->futex_request)__builtin_trap();
     task->pi_request=&pi_request;task->futex_request=&request;}
    result=futex_prepare(task,&target,1,operation,0,(operation&FUTEX_PRIVATE)?0:KERNEL_MM_WRITE,
        &pi_request.key,&pi_request.pin,status);
    if(result || *status)goto out;
    if(pi_request.pin.mm)kernel_mm_unpin_user_page(&pi_request.pin);
    result=futex_prepare(task,&address,1,operation,0,KERNEL_MM_READ,&entry.key,&pi_request.source_pin,status);
    if(result || *status)goto out;
    if(futex_key_equal(&entry.key,&pi_request.key)){result=-KERNEL_EINVAL;goto out;}
    struct futex_bucket *bucket=futex_bucket(&entry.key);
    {
        KERNEL_LOCK_SCOPE(g);kernel_mutex_lock(&bucket->operation,&g);
        uint32_t *word=(void *)((unsigned char *)pi_request.source_pin.page+(address&BOAROS_PAGE_MASK));
        if(__atomic_load_n(word,__ATOMIC_ACQUIRE)!=value){result=-KERNEL_EAGAIN;goto out;}
        KERNEL_RAW_SCOPE(raw,&kernel_wait_domain);
        if(kernel_wait_prepare_locked(0,deadline,1,&pi_request.token))__builtin_trap();
        futex_bind_entry_locked(&bucket->waiters,&entry,&pi_request.token);
        if(expired)(void)kernel_wait_notify_locked(&pi_request.token,KERNEL_WAIT_TIMEOUT);
    }
    kernel_mm_unpin_user_page(&pi_request.source_pin);
    enum kernel_wait_wake_reason reason;
    *status=kernel_wait_park(&pi_request.token,&reason);
    unsigned stage;
    {
        KERNEL_RAW_SCOPE(raw,&kernel_wait_domain);
        stage=pi_request.requeue_stage;pi_request.requeue_stage=3;
        kernel_wait_node_remove_locked(&entry.node);
        if(pi_request.queued) {
            waiter_remove(&pi_request);
            kernel_futex_pi_recalculate_locked(pi_request.state->owner,-1);
        }
    }
    if(kernel_wait_finish(&pi_request.token))__builtin_trap();
    if(*status)goto out;
    /* 到期、信号与授予竞争同一状态；真实owner已经交接时完成修复。 */
    if(pi_request.granted==2) {
        release_preparation(&pi_request);
        do {result=fixup_dead_owner(&pi_request,status);}while(result==-KERNEL_EAGAIN && !*status);
    } else if(pi_request.granted)result=0;
    else if(reason==KERNEL_WAIT_TIMEOUT)result=-KERNEL_ETIMEDOUT;
    else if(reason==KERNEL_WAIT_SIGNALLED && stage==1) {
        kernel_signal_note_syscall_restart(task);task->syscall_restart.kind=KERNEL_SYSCALL_RESTART_NOINTR;
        result=-KERNEL_ERESTARTSYS;
    } else result=-KERNEL_EAGAIN;
out:
    request_finish(&pi_request,0);
    kernel_futex_request_release(&request);
    return result;
}

static int prepare_requeue_words(struct kernel_futex_pi_request *call,
    struct kernel_futex_entry *entries,uint64_t address,int target_ready,
    enum kernel_scheduler_status *status)
{
    for(;;) {
        uint64_t addresses[2]={address,call->address};struct kernel_futex_key keys[2]={0};
        int result=futex_prepare(call->task,addresses,2,call->operation,target_ready?1:0,
            target_ready?(uint32_t)target_ready:KERNEL_MM_READ,keys,
            target_ready?&call->pin:&call->source_pin,status);
        if(result || *status)return result;
        entries[0].key=keys[0];entries[1].key=keys[1];
        if(!target_ready)return 0;
        enum kernel_mm_status prepared=kernel_mm_try_futex_pin(&call->task->mm,address,
            !(call->operation&FUTEX_PRIVATE),KERNEL_MM_READ,&keys[0],&call->source_pin);
        if(!prepared)return 0;
        release_preparation(call);futex_key_release(&entries[0].key);futex_key_release(&entries[1].key);
        if(prepared!=KERNEL_MM_STATUS_CONFLICT){*status=KERNEL_SCHEDULER_STATUS_INVALID_STATE;return 0;}
        /* 两页均需访问时，任何缺页/等待先归还已取得的目标pin。 */
        result=futex_prepare(call->task,&address,1,call->operation,0,KERNEL_MM_READ,
            &entries[0].key,&call->source_pin,status);
        release_preparation(call);futex_key_release(&entries[0].key);
        if(result || *status)return result;
    }
}
int64_t kernel_futex_cmp_requeue_pi(struct kernel_task *task,uint64_t address,
    uint32_t operation,uint32_t wake_count,uint32_t move_count,uint64_t target,uint32_t expected,
    enum kernel_scheduler_status *status)
{
    if(operation&FUTEX_CLOCK_REALTIME)return -KERNEL_ENOSYS;
    if(wake_count!=1 || (int32_t)move_count<0 || address==target)return -KERNEL_EINVAL;
    struct kernel_futex_pi_request call={.task=task,.address=target,.operation=operation};
    struct kernel_futex_entry entries[2]={0};
    struct kernel_futex_request owner={.task=task,.entries=entries,.count=2};
    {KERNEL_RAW_SCOPE(g,&kernel_wait_domain);
     if(task->pi_request || task->futex_request)__builtin_trap();
     task->pi_request=&call;task->futex_request=&owner;}
    int target_ready=(operation&FUTEX_PRIVATE)?0:KERNEL_MM_WRITE;
    int result=allocate_state(&call);if(result)goto out;
    for(;;) {
        result=prepare_requeue_words(&call,entries,address,target_ready,status);
        if(result || *status)break;
        struct kernel_futex_key keys[2]={entries[0].key,entries[1].key};
        if(futex_key_equal(&keys[0],&keys[1])){result=-KERNEL_EINVAL;break;}
        struct futex_bucket *buckets[2];struct kernel_lock_guard guards[2]={0};
        futex_lock_buckets(keys,2,buckets,guards);
        struct kernel_cpu_notification_batch notifications;
        kernel_cpu_notification_batch_begin(&notifications);
        uint32_t *source=(void *)((unsigned char *)call.source_pin.page+(address&BOAROS_PAGE_MASK));
        int retry=0,prepare_target=0;unsigned moved=0;
        struct kernel_wait_cursor cursor={0};
        if(__atomic_load_n(source,__ATOMIC_ACQUIRE)!=expected){result=-KERNEL_EAGAIN;goto unlock;}
        /* Linux在寻找首个源等待者之前读取目标字；空源队列也检查目标可读。 */
        if(!call.pin.mm) {
            enum kernel_mm_status access=kernel_mm_try_futex_pin(&task->mm,target,
                0,KERNEL_MM_READ,&keys[1],&call.pin);
            if(access) {
                if(access!=KERNEL_MM_STATUS_CONFLICT)*status=KERNEL_SCHEDULER_STATUS_INVALID_STATE;
                else {retry=1;prepare_target=KERNEL_MM_READ;}
                goto unlock;
            }
        }
        (void)__atomic_load_n(request_word(&call),__ATOMIC_ACQUIRE);
        int target_mixed=normal_waiter(buckets[1],&keys[1]);
        kernel_wait_cursor_begin(&buckets[0]->waiters,&cursor);
        while(cursor.node && (uint64_t)moved<1+(uint64_t)move_count) {
            struct kernel_wait_node *node=cursor.node;
            struct kernel_futex_entry *entry=futex_entry(node);
            int selected=0;
            {
                KERNEL_RAW_SCOPE(g,&kernel_wait_domain);
                if(node->queue==&buckets[0]->waiters && futex_key_equal(&entry->key,&keys[0])) {
                    if(entry->kind!=FUTEX_ENTRY_REQUEUE_PI || !entry->pi ||
                       !futex_key_equal(&entry->pi->key,&keys[1]))result=-KERNEL_EINVAL;
                    else if(entry->pi->requeue_stage==1)selected=1;
                }
            }
            if(result)break;
            if(selected) {
                if(!(call.pin.access&KERNEL_MM_WRITE)) {
                    kernel_mm_unpin_user_page(&call.pin);
                    if(!(operation&FUTEX_PRIVATE))__builtin_trap();
                    enum kernel_mm_status prepared=kernel_mm_try_futex_pin(&task->mm,target,
                        !(operation&FUTEX_PRIVATE),KERNEL_MM_WRITE,&keys[1],&call.pin);
                    if(prepared) {
                        if(prepared!=KERNEL_MM_STATUS_CONFLICT)*status=KERNEL_SCHEDULER_STATUS_INVALID_STATE;
                        else {prepare_target=KERNEL_MM_WRITE;retry=1;}
                        break;
                    }
                }
                KERNEL_RAW_SCOPE(g,&kernel_wait_domain);
                /* 游标保活source entry；超时线程可能已摘队，发布前复核。 */
                if(node->queue==&buckets[0]->waiters && entry->pi->requeue_stage==1) {
                    struct kernel_futex_pi_request *request=entry->pi;
                    request->candidate=call.candidate;call.candidate=0;
                    enum pi_action action=claim_locked(request,buckets[1],request_word(&call),
                        target_mixed,0,move_count!=0,&result);
                    call.candidate=request->candidate;request->candidate=0;
                    if(request->exiting) {
                        if(call.exiting)__builtin_trap();
                        call.exiting=request->exiting;request->exiting=0;
                    }
                    if(action==ALLOCATE)__builtin_trap();
                    if(action==AGAIN || action==EXITING){retry=1;break;}
                    if(result)break;
                    if(request->state && !call.state){call.state=request->state;state_get(call.state);}
                    kernel_wait_node_remove_locked(node);request->requeue_stage=2;
                    if(action==WAITING) {
                        if(waiter_order==UINT64_MAX)__builtin_trap();
                        request->order=++waiter_order;
                        waiter_insert(request);kernel_futex_pi_recalculate_locked(request->state->owner,-1);
                    } else {
                        __atomic_store_n(&request->granted,1,__ATOMIC_RELEASE);
                        (void)kernel_wait_notify_locked(&request->token,KERNEL_WAIT_WOKEN);
                    }
                    moved++;
                }
            }
            kernel_wait_cursor_advance(&cursor);
        }
        kernel_wait_cursor_end(&cursor);
unlock:
        futex_unlock_buckets(guards);kernel_cpu_notification_batch_end(&notifications);
        release_preparation(&call);
        futex_key_release(&entries[0].key);futex_key_release(&entries[1].key);
        if(result || *status)break;
        if(!retry){result=(int)moved;break;}
        if(moved){result=-KERNEL_EAGAIN;break;}
        if(call.exiting)wait_exiting_owner(&call);
        if(prepare_target)target_ready=prepare_target;
    }
out:
    request_finish(&call,0);kernel_futex_request_release(&owner);return result;
}

void kernel_futex_pi_cleanup_begin(struct kernel_task *task)
{
    KERNEL_RAW_SCOPE(g,&kernel_wait_domain);
    if(task->futex_exit_state==1)__builtin_trap();
    task->futex_exit_state=1;
}
void kernel_futex_pi_cleanup_end(struct kernel_task *task)
{
    for(;;) {
        struct kernel_futex_pi_state *state;struct kernel_task *old=0;
        {
            KERNEL_RAW_SCOPE(g,&kernel_wait_domain);
            state=task->pi_owned;if(!state)break;
            state_get(state);
        }
        /* 重排发布持有目标桶；退出先等待它结束，保留后续等待者的PI状态。 */
        KERNEL_LOCK_SCOPE(operation);kernel_mutex_lock(&state->bucket->operation,&operation);
        struct kernel_cpu_notification_batch notifications;
        kernel_cpu_notification_batch_begin(&notifications);
        {KERNEL_RAW_SCOPE(g,&kernel_wait_domain);
         if(state->owner==task)old=handoff(state,1);}
        kernel_lock_release(&operation);kernel_cpu_notification_batch_end(&notifications);
        if(old)scheduler_task_put(old);
        state_put(state);
    }
    {
        KERNEL_RAW_SCOPE(g,&kernel_wait_domain);
        kernel_futex_pi_recalculate_locked(task,-1);
        task->futex_exit_state=task->proc_exiting?2U:0U;
    }
    (void)kernel_wait_queue_wake_all(&task->group_wait_queue);
}
int kernel_futex_pi_robust(struct kernel_task *task,uint64_t address,uint32_t owner_tid)
{
    struct kernel_futex_pi_request request={.task=task,.address=address};
    enum kernel_scheduler_status status=KERNEL_SCHEDULER_STATUS_OK;
    if(prepare_word(&request,&status) || status){release_preparation(&request);return 0;}
    struct futex_bucket *bucket=futex_bucket(&request.key);
    KERNEL_LOCK_SCOPE(operation);kernel_mutex_lock(&bucket->operation,&operation);
    int done=0;
    {
        KERNEL_RAW_SCOPE(g,&kernel_wait_domain);
        uint32_t *word=request_word(&request),value=__atomic_load_n(word,__ATOMIC_ACQUIRE);
        for(unsigned attempt=0;attempt<32;attempt++) {
            if((value&FUTEX_TID_MASK)!=owner_tid) {done=1;break;}
            if(__atomic_compare_exchange_n(word,&value,(value&FUTEX_WAITERS)|FUTEX_OWNER_DIED,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST)) {done=1;break;}
        }
    }
    kernel_lock_release(&operation);release_preparation(&request);
    /* 实际PI owner列表随后在同一退出流程中交接，普通WAKE不接管PI等待者。 */
    return done;
}
