#include "futex_internal.h"
#include <kernel/time.h>
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
static void state_put(struct kernel_futex_pi_state *state)
{
    if(!state)return;
    struct kernel_task *old=0;int release=0;
    {
        KERNEL_RAW_SCOPE(g,&kernel_wait_domain);
        if(!state->references)__builtin_trap();
        if(!--state->references) {
            if(state->head || state->tail)__builtin_trap();
            state_detach(state);old=owner_set(state,0);release=1;
        }
    }
    if(old)scheduler_task_put(old);
    if(release) {futex_key_release(&state->key);kernel_heap_release(state->heap,state);}
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
               node->task->wait.phase!=KERNEL_WAIT_NOTIFIED && futex_key_equal(&node->task->futex_key,key))found=1;
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
    next->granted=fixup?2U:1U;
    if(!fixup)state->word_tid=(uint32_t)next->task->tid;
    (void)kernel_wait_notify_locked(&next->token,KERNEL_WAIT_WOKEN);
    return old;
}
static void request_finish(struct kernel_futex_pi_request *request,int abandoned)
{
    struct kernel_task *old=0;
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
    }
    release_preparation(request);
    if(request->exiting){scheduler_task_put(request->exiting);request->exiting=0;}
    if(old)scheduler_task_put(old);
    state_put(request->state);request->state=0;
    state_put(request->candidate);request->candidate=0;
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
    kernel_lock_release(&operation);release_preparation(request);
    if(old)scheduler_task_put(old);
    return result;
}

int64_t kernel_futex_pi(struct kernel_task *task,uint64_t address,uint32_t operation,
    uint64_t timeout,enum kernel_scheduler_status *status)
{
    unsigned command=operation&~(FUTEX_PRIVATE|FUTEX_CLOCK_REALTIME);
    uint64_t deadline=0;int expired=0;
    if(timeout && (command==6 || command==13)) {
        struct {int64_t seconds,nanoseconds;} ts;size_t copied;
        if(kernel_copy_from_user(&task->mm,&ts,timeout,sizeof(ts),&copied) || copied!=sizeof(ts))return -KERNEL_EFAULT;
        if(ts.seconds<0 || ts.nanoseconds<0 || ts.nanoseconds>=1000000000)return -KERNEL_EINVAL;
        __uint128_t wide=(__uint128_t)(uint64_t)ts.seconds*1000000000+(uint64_t)ts.nanoseconds;
        uint64_t ns=wide>INT64_MAX?INT64_MAX:(uint64_t)wide;
        if(command==6 || (operation&FUTEX_CLOCK_REALTIME)) {
            uint64_t offset=kernel_time_boot_realtime_offset();ns=ns<=offset?0:ns-offset;
        }
        enum kernel_time_status converted=kernel_time_deadline_from_monotonic(ns,&deadline);
        if(converted==KERNEL_TIME_STATUS_DEADLINE_PASSED)expired=1;
        else if(converted!=KERNEL_TIME_STATUS_OK)__builtin_trap();
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
    enum { AGAIN,ALLOCATE,EXITING,WAITING,FINISHED } action;
    unsigned retries=0;
    for(;;) {
        result=prepare_word(&request,status);
        if(result || *status)break;
        struct futex_bucket *bucket=futex_bucket(&request.key);
        KERNEL_LOCK_SCOPE(operation_guard);kernel_mutex_lock(&bucket->operation,&operation_guard);
        int mixed=normal_waiter(bucket,&request.key);
        action=FINISHED;
        {
            KERNEL_RAW_SCOPE(g,&kernel_wait_domain);
            uint32_t *word=request_word(&request),value=__atomic_load_n(word,__ATOMIC_ACQUIRE);
            uint32_t tid=(uint32_t)task->tid;
            struct kernel_futex_pi_state *state=state_find(bucket,&request.key);
            if((value&FUTEX_TID_MASK)==tid)result=-KERNEL_EDEADLK;
            else if(mixed)result=-KERNEL_EINVAL;
            else if(state) {
                uint32_t owner=value&FUTEX_TID_MASK;
                if(!state->owner || (owner!=state->word_tid &&
                    !((state->needs_fixup || state->owner->futex_exit_state==1) &&
                      !owner && (value&FUTEX_OWNER_DIED))))result=-KERNEL_EINVAL;
                else if(command==8)result=-KERNEL_EAGAIN;
                else if(cycle(state->owner,task))result=-KERNEL_EDEADLK;
                else if(!__atomic_compare_exchange_n(word,&value,value|FUTEX_WAITERS,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST))action=AGAIN;
                else {state_get(state);request.state=state;action=WAITING;}
            } else if(!(value&FUTEX_TID_MASK)) {
                uint32_t next=(value&FUTEX_OWNER_DIED)|tid;
                if(!__atomic_compare_exchange_n(word,&value,next,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST))action=AGAIN;
            } else {
                uint32_t desired=value|FUTEX_WAITERS;
                if(!__atomic_compare_exchange_n(word,&value,desired,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST))action=AGAIN;
                else {
                    struct kernel_task *owner=scheduler_task_lookup_locked((int32_t)(value&FUTEX_TID_MASK),KERNEL_PID_TID,0);
                    request.exiting=owner;
                    if(!owner || owner->futex_exit_state==2) {
                        if(__atomic_load_n(word,__ATOMIC_ACQUIRE)!=desired)action=AGAIN;
                        else result=-KERNEL_ESRCH;
                    } else if(!owner->arch.user_mode)result=-KERNEL_EPERM;
                    else if(owner->proc_exiting || owner->futex_exit_state==1)action=EXITING;
                    else if(command==8)result=-KERNEL_EAGAIN;
                    else if(cycle(owner,task))result=-KERNEL_EDEADLK;
                    else if(!request.candidate)action=ALLOCATE;
                    else {
                        state=request.candidate;request.candidate=0;request.state=state;
                        state->key=request.key;request.key=(struct kernel_futex_key){0};
                        state->bucket=bucket;state->hash_next=bucket->pi_states;
                        __atomic_store_n(&bucket->pi_states,state,__ATOMIC_RELEASE);
                        state->attached=1;state->word_tid=value&FUTEX_TID_MASK;
                        (void)owner_set(state,owner);action=WAITING;
                    }
                }
            }
            if(action==WAITING) {
                if(waiter_order==UINT64_MAX)__builtin_trap();
                request.order=++waiter_order;
                if(kernel_wait_prepare_locked(0,deadline,1,&request.token))__builtin_trap();
                waiter_insert(&request);kernel_futex_pi_recalculate_locked(request.state->owner,-1);
                if(expired)(void)kernel_wait_notify_locked(&request.token,KERNEL_WAIT_TIMEOUT);
            }
        }
        kernel_lock_release(&operation_guard);release_preparation(&request);
        if(action!=EXITING && request.exiting) {scheduler_task_put(request.exiting);request.exiting=0;}
        if(action==FINISHED)break;
        if(action==ALLOCATE) {
            enum kernel_heap_status allocated=kernel_heap_allocate_zeroed(task->files.heap,1,sizeof(*request.candidate),(void **)&request.candidate);
            if(allocated) {result=allocated==KERNEL_HEAP_STATUS_EMPTY?-KERNEL_ENOMEM:-KERNEL_EIO;break;}
            request.candidate->heap=task->files.heap;request.candidate->references=1;
        } else if(action==EXITING)wait_exiting_owner(&request);
        else if(action==WAITING) {
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

void kernel_futex_pi_cleanup_begin(struct kernel_task *task)
{
    KERNEL_RAW_SCOPE(g,&kernel_wait_domain);
    if(task->futex_exit_state==1)__builtin_trap();
    task->futex_exit_state=1;
}
void kernel_futex_pi_cleanup_end(struct kernel_task *task)
{
    for(;;) {
        struct kernel_futex_pi_state *state;struct kernel_task *old;
        {
            KERNEL_RAW_SCOPE(g,&kernel_wait_domain);
            state=task->pi_owned;if(!state)break;
            state_get(state);old=handoff(state,1);
        }
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
