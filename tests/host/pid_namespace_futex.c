/* Production PI word admission with real hierarchical PID registries.
 * Task scheduling, page pinning and wait notification are controlled seams. */
#include "../../kernel/sched/futex_pi.c"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
_Thread_local uintptr_t sync_test_irq;
static struct kernel_cpu cpu={.initialized=KERNEL_CPU_INITIALIZED};
struct kernel_scheduler scheduler;
struct kernel_raw_lock kernel_wait_domain;
static struct kernel_task tasks[4];
static unsigned notices;
void *sync_test_cpu(void){return &cpu;}
void *sync_test_current_task(void){return cpu.current;}
void kernel_cpu_flush_notifications(void){assert(!cpu.outbound);}
struct kernel_pid *process_identity(const struct kernel_task *task,enum kernel_pid_role role)
{return task->identities[role].identity;}
struct kernel_pid_registry *process_pid_active(const struct kernel_task *task)
{return process_identity(task,KERNEL_PID_TID)->registry;}
void scheduler_task_get_locked(struct kernel_task *task)
{assert(cpu.raw_locks);task->storage_borrows++;}
void scheduler_task_put(struct kernel_task *task)
{assert(task->storage_borrows);task->storage_borrows--;}
struct kernel_task *scheduler_task_lookup_in_locked(struct kernel_pid_registry *view,kernel_pid_t tid,
    enum kernel_pid_role role,uint64_t generation)
{
    assert(cpu.raw_locks);struct kernel_pid *id=kernel_pid_find(view,tid);
    if(!id || (generation&&generation!=id->generation) || !id->members[role])return NULL;
    struct kernel_task *task=id->members[role]->task;scheduler_task_get_locked(task);return task;
}
void scheduler_pi_priority_locked(struct kernel_task *task,unsigned base,unsigned inherited)
{task->scheduling.priority=(int)(base>inherited?base:inherited);}
int kernel_wait_notify_locked(const struct kernel_wait_token *token,uint32_t reason)
{assert(token->task && reason==KERNEL_WAIT_WOKEN);notices++;return 1;}
struct futex_bucket futex_buckets[FUTEX_BUCKETS];
struct kernel_wait_queue futex_wait_queues[FUTEX_WAIT_QUEUES];
static uint32_t shared_word;
static unsigned pins,operations;
enum kernel_mm_status kernel_mm_prepare_futex(struct kernel_mm *mm,const uint64_t *addresses,
    unsigned count,int shared,unsigned pin_index,uint32_t access,struct kernel_futex_key *keys,
    struct kernel_mm_user_pin *pin)
{
    (void)shared;assert(count==1 && !addresses[0] && !pin_index && access==KERNEL_MM_WRITE);
    *keys=(struct kernel_futex_key){0};*pin=(struct kernel_mm_user_pin){.mm=mm,.page=&shared_word};pins++;return 0;
}
void kernel_mm_unpin_user_page(struct kernel_mm_user_pin *pin)
{assert(pins && pin->mm);pins--;*pin=(struct kernel_mm_user_pin){0};}
int kernel_vfs_node_release(struct kernel_vfs_node **node){(void)node;abort();}
void kernel_memory_object_release(struct kernel_memory_object **object){(void)object;abort();}
void kernel_hugetlb_map_release(struct kernel_hugetlb_map **map){(void)map;abort();}
__attribute__((noinline)) enum kernel_uaccess_status kernel_copy_from_user(struct kernel_mm *mm,void *out,uint64_t address,
    size_t bytes,size_t *copied)
{(void)mm;memset(out,0,bytes);assert(!address&&bytes==sizeof(shared_word));memcpy(out,&shared_word,sizeof(shared_word));*copied=bytes;return 0;}
enum kernel_task_status kernel_task_user_tid(const struct kernel_task *task,kernel_pid_t *tid)
{*tid=(kernel_pid_t)pi_tid_in(task,task);return 0;}
void kernel_rwlock_write(struct kernel_rwlock *lock,struct kernel_lock_guard *guard)
{assert(!operations++);guard->lock=lock;}
void kernel_lock_release(struct kernel_lock_guard *guard)
{assert(operations==1&&guard->lock);operations--;guard->lock=NULL;}
void kernel_cpu_notification_batch_begin(struct kernel_cpu_notification_batch *batch){(void)batch;assert(operations);}
void kernel_cpu_notification_batch_end(struct kernel_cpu_notification_batch *batch){(void)batch;assert(!operations);}
void kernel_wait_cursor_begin(struct kernel_wait_queue *queue,struct kernel_wait_cursor *cursor)
{(void)queue;*cursor=(struct kernel_wait_cursor){0};}
void kernel_wait_cursor_advance(struct kernel_wait_cursor *cursor){(void)cursor;abort();}
void kernel_wait_cursor_end(struct kernel_wait_cursor *cursor){assert(!cursor->node);}
static void cross_view_handoff(void)
{
    struct futex_bucket *bucket=&futex_buckets[0];
    struct kernel_futex_pi_state state={.bucket=bucket,.references=1,.attached=1};
    struct kernel_futex_pi_request waiter={.task=&tasks[2],.state=&state,.queued=1,
        .token={.task=&tasks[2]}};
    {KERNEL_RAW_SCOPE(g,&kernel_wait_domain);assert(!owner_set(&state,&tasks[0]));}
    bucket->pi_states=&state;state.head=state.tail=&waiter;
    struct kernel_futex_pi_request unlock={.task=&tasks[0]};
    enum kernel_scheduler_status status=0;shared_word=1|FUTEX_WAITERS;
    assert(!pi_unlock(&unlock,&status)&&!status);
    assert(state.owner==&tasks[2] && waiter.granted==1 && !waiter.queued && notices==1);
    assert(shared_word==((uint32_t)tasks[2].tid|FUTEX_WAITERS));
    assert((shared_word&FUTEX_TID_MASK)!=2 && !pins && !operations);
    /* Linux wrote the new owner in the unlocker's view, not its own. The
     * shared ABI cannot make a later local-TID unlock transparent. */
    struct kernel_futex_pi_request local={.task=&tasks[2]};
    assert(pi_unlock(&local,&status)==-KERNEL_EPERM);
    shared_word=FUTEX_OWNER_DIED|FUTEX_WAITERS;state.needs_fixup=1;
    assert(!fixup_dead_owner(&waiter,&status) && !status);
    assert(shared_word==(2|FUTEX_OWNER_DIED|FUTEX_WAITERS) && !state.needs_fixup && !pins);
    assert(kernel_futex_pi_robust(&tasks[2],0,2));
    assert(shared_word==(FUTEX_OWNER_DIED|FUTEX_WAITERS) && !pins);
    struct kernel_task *old;
    {KERNEL_RAW_SCOPE(g,&kernel_wait_domain);
     state_detach(&state);old=owner_set(&state,NULL);assert(old==&tasks[2]);
     struct kernel_task *unused=NULL;
     assert(state.references==2 && !state_drop_locked(&state,&unused) && !unused);}
    scheduler_task_put(old);unlock.state=NULL;
}

static enum pi_action claim(struct kernel_task *beneficiary,struct kernel_task *actor,
    struct futex_bucket *bucket,uint32_t *word,int expected,int force)
{
    struct kernel_futex_pi_request request={.task=beneficiary,.key={0}};
    int result=0;enum pi_action action;
    {KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
     action=claim_locked(&request,bucket,word,actor,0,1,force,&result);}
    assert(result==expected);
    if(request.exiting)scheduler_task_put(request.exiting);
    assert(!request.state);
    return action;
}
int main(void)
{
    kernel_raw_lock_init(&kernel_wait_domain,KERNEL_RAW_RANK_SCHEDULER);
    struct kernel_pid_allocator alloc[3]={0};uint64_t bitmap[3][KERNEL_PID_BITMAP_WORDS(64)]={0};
    struct kernel_pid_registry children[2]={0};
    for(unsigned i=0;i<3;i++)assert(!kernel_pid_allocator_init(&alloc[i],bitmap[i],64));
    assert(!kernel_pid_registry_init(&scheduler.identities,&alloc[0],NULL));
    for(unsigned i=0;i<2;i++)assert(!kernel_pid_registry_init(&children[i],&alloc[i+1],&scheduler.identities));
    struct kernel_pid_registry *views[]={&scheduler.identities,&children[0],&children[0],&children[1]};
    struct kernel_pid *ids[4]={0};
    for(unsigned i=0;i<4;i++){
        size_t bytes=kernel_pid_storage_size(views[i]);ids[i]=calloc(1,bytes);assert(ids[i]);
        tasks[i].arch.user_mode=1;
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        assert(!kernel_pid_reserve_numbers(views[i],ids[i],bytes,NULL,0));
        assert(!kernel_pid_publish_reserved(ids[i]));
        kernel_pid_attach(&tasks[i].identities[KERNEL_PID_TID],ids[i],KERNEL_PID_TID,&tasks[i]);
        tasks[i].tid=kernel_pid_number_in(ids[i],&scheduler.identities);
    }
    struct futex_bucket bucket={0};uint32_t word=0;
    assert(claim(&tasks[2],&tasks[2],&bucket,&word,0,0)==FINISHED && word==2);
    assert(claim(&tasks[2],&tasks[2],&bucket,&word,-KERNEL_EDEADLK,0)==FINISHED);
    /* Same shared word has no namespace tag. Sibling local 1 is interpreted
     * as self, not as the real owner in another namespace (Linux limitation). */
    word=1;assert(claim(&tasks[2],&tasks[2],&bucket,&word,-KERNEL_EAGAIN,0)==FINISHED);
    word=1;assert(claim(&tasks[3],&tasks[3],&bucket,&word,-KERNEL_EDEADLK,0)==FINISHED);
    word=2;assert(claim(&tasks[3],&tasks[3],&bucket,&word,-KERNEL_ESRCH,0)==FINISHED);
    assert(word==(FUTEX_WAITERS|2));
    /* A proxy requeue uses its actor, not the sleeping beneficiary's view. */
    word=0;assert(claim(&tasks[2],&tasks[0],&bucket,&word,0,0)==FINISHED);
    assert(word==(uint32_t)tasks[2].tid && word!=2);
    word=0;assert(claim(&tasks[2],&tasks[3],&bucket,&word,-KERNEL_EDEADLK,0)==FINISHED);
    struct kernel_futex_pi_state state={.owner=&tasks[2],.references=1,.attached=1,.bucket=&bucket};
    bucket.pi_states=&state;
    word=(uint32_t)tasks[2].tid;
    assert(claim(&tasks[0],&tasks[0],&bucket,&word,-KERNEL_EAGAIN,0)==FINISHED);
    assert(claim(&tasks[1],&tasks[1],&bucket,&word,-KERNEL_EINVAL,0)==FINISHED);
    word=2;assert(claim(&tasks[1],&tasks[1],&bucket,&word,-KERNEL_EAGAIN,0)==FINISHED);
    word=FUTEX_OWNER_DIED;assert(claim(&tasks[1],&tasks[1],&bucket,&word,-KERNEL_EAGAIN,0)==FINISHED);
    word=0;assert(claim(&tasks[1],&tasks[1],&bucket,&word,-KERNEL_EINVAL,0)==FINISHED);
    /* Zero is also a valid translated owner number when the existing
     * canonical owner is invisible to this actor, even without OWNER_DIED. */
    state.owner=&tasks[0];word=FUTEX_WAITERS;
    assert(claim(&tasks[1],&tasks[1],&bucket,&word,-KERNEL_EAGAIN,0)==FINISHED);
    struct kernel_futex_pi_request attached={.task=&tasks[1]};int error=0;
    {KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
     assert(claim_locked(&attached,&bucket,&word,&tasks[1],0,0,0,&error)==WAITING && !error);
     assert(attached.state==&state && state.owner==&tasks[0] && state.references==2);
     struct kernel_task *unused=NULL;assert(!state_drop_locked(&state,&unused) && !unused);}
    assert(claim(&tasks[3],&tasks[3],&bucket,&word,-KERNEL_EAGAIN,0)==FINISHED);
    state.owner=&tasks[2];word=FUTEX_WAITERS;
    assert(claim(&tasks[3],&tasks[3],&bucket,&word,-KERNEL_EAGAIN,0)==FINISHED);
    assert(claim(&tasks[1],&tasks[1],&bucket,&word,-KERNEL_EINVAL,0)==FINISHED);
    state.owner=NULL;
    assert(claim(&tasks[3],&tasks[3],&bucket,&word,-KERNEL_EINVAL,0)==FINISHED);
    bucket.pi_states=NULL;
    cross_view_handoff();
    for(unsigned i=0;i<4;i++){
        assert(!tasks[i].storage_borrows);
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        kernel_pid_detach(&tasks[i].identities[KERNEL_PID_TID]);kernel_pid_put(ids[i]);
    }
    for(;;) {
        struct kernel_pid *id;
        {KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);id=kernel_pid_take_retired(&scheduler.identities);}
        if(!id)break;
        free(id);
    }
    {KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
     for(unsigned i=0;i<2;i++)kernel_pid_registry_put(&children[i]);
     while(kernel_pid_take_retired_namespace(&scheduler.identities));}
    for(unsigned i=0;i<3;i++)assert(!alloc[i].allocated);
    assert(scheduler.identities.references==1 && notices==1);
    puts("PASS PI caller/proxy views, sibling collisions, existing owner validation handoff, robust repair and zero-word rules");
}
