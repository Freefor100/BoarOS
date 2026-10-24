#include "../../kernel/sched/private.h"
#include <kernel/credentials.h>
#include <kernel/errno.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
_Thread_local uintptr_t sync_test_irq=1;
static _Thread_local struct kernel_cpu cpu={.initialized=KERNEL_CPU_INITIALIZED};
struct kernel_scheduler scheduler;
struct kernel_raw_lock kernel_wait_domain={.rank=KERNEL_RAW_RANK_SCHEDULER,.initialized=KERNEL_RAW_INITIALIZED};
static struct kernel_heap heap;
static unsigned live;
void *sync_test_cpu(void){return &cpu;}
void kernel_cpu_flush_notifications(void){assert(!cpu.outbound);}
enum kernel_heap_status kernel_heap_allocate_zeroed(struct kernel_heap *h,size_t n,size_t size,void **out)
{assert(h==&heap&&!cpu.raw_locks);*out=calloc(n,size);assert(*out);__atomic_add_fetch(&live,1,__ATOMIC_RELAXED);return 0;}
enum kernel_heap_status kernel_heap_allocate(struct kernel_heap *h,size_t size,void **out)
{return kernel_heap_allocate_zeroed(h,1,size,out);}
void kernel_heap_release(struct kernel_heap *h,void *p)
{assert(h==&heap&&!cpu.raw_locks&&p);free(p);assert(__atomic_fetch_sub(&live,1,__ATOMIC_RELAXED));}
static struct kernel_credentials *cred(unsigned uid)
{
    struct kernel_credentials *c=kernel_credentials_clone(&heap,kernel_credentials_initial());assert(c);
    assert(!kernel_credentials_set_account(c,uid));c->uid=c->euid=c->suid=c->fsuid=uid;c->effective=c->permitted=0;return c;
}
static void make_task(struct kernel_task *t,unsigned uid,uint64_t limit)
{
    *t=(struct kernel_task){0};t->credentials=cred(uid);t->limits=kernel_resource_limits_create(&heap,0);assert(t->limits);
    t->limits->values[KERNEL_RLIMIT_NPROC].current=limit;
}
static void release_charge(struct kernel_task *t)
{KERNEL_RAW_SCOPE(g,&kernel_wait_domain);kernel_nproc_release_locked(t);}
static void finish(struct kernel_task *t)
{release_charge(t);kernel_credentials_put(t->credentials);kernel_resource_limits_put(t->limits);*t=(struct kernel_task){0};}
static void boundary(void)
{
    struct kernel_task parent,child;make_task(&parent,1001,1);make_task(&child,1001,1);
    assert(!kernel_nproc_acquire(&parent,0));assert(parent.credentials->account->processes==1);
    assert(kernel_nproc_acquire(&child,&parent)==-KERNEL_EAGAIN);assert(!child.resource_flags);
    parent.limits->values[KERNEL_RLIMIT_NPROC].current=2;
    parent.resource_flags|=KERNEL_RESOURCE_NPROC_EXCEEDED|4U;
    assert(!kernel_nproc_acquire(&child,&parent));assert(!(parent.resource_flags&KERNEL_RESOURCE_NPROC_EXCEEDED)&&(parent.resource_flags&4));
    assert(child.resource_flags&KERNEL_RESOURCE_NPROC_EXCEEDED);
    /* A zombie still consumes its thread charge. Storage borrows do not after reap. */
    child.state=KERNEL_THREAD_STATE_ZOMBIE;parent.limits->values[KERNEL_RLIMIT_NPROC].current=1;
    assert(parent.credentials->account->processes==2);
    child.storage_borrows=1;child.stack_physical_address=KERNEL_THREAD_NO_PAGE;
    scheduler.retired_pending=0;
    {KERNEL_RAW_SCOPE(g,&kernel_wait_domain);scheduler_task_unlink_locked(&child);assert(parent.credentials->account->processes==1);scheduler_task_retire_locked(&child);}
    assert(child.retirement==2&&child.storage_borrows==1&&parent.credentials->account->processes==1);
    finish(&child);finish(&parent);assert(!live);
}
static void exemptions(void)
{
    for(unsigned i=0;i<3;i++){
        struct kernel_task t;make_task(&t,i?1002:0,0);
        struct kernel_credentials *c=(struct kernel_credentials *)t.credentials;
        if(i)c->effective=UINT64_C(1)<<(i==1?KERNEL_CAP_SYS_RESOURCE:KERNEL_CAP_SYS_ADMIN);
        assert(!kernel_nproc_acquire(&t,0)&&t.credentials->account->processes==1);
        finish(&t);assert(!live);
    }
}
static void transfer(void)
{
    struct kernel_task actor,holder;make_task(&actor,1003,1);make_task(&holder,1004,8);
    assert(!kernel_nproc_acquire(&actor,0)&&!kernel_nproc_acquire(&holder,0));
    struct kernel_uid_account *old=kernel_uid_account_get(&heap,1003);
    actor.limits->values[KERNEL_RLIMIT_NPROC].current=0;actor.resource_flags|=4;
    struct kernel_credentials *replacement=cred(1004);kernel_credentials_publish(&actor,replacement);
    assert(!old->processes&&holder.credentials->account->processes==2);
    assert(actor.resource_flags&KERNEL_RESOURCE_NPROC_EXCEEDED);
    assert(kernel_nproc_exec(&actor)==-KERNEL_EAGAIN);
    finish(&holder);assert(kernel_nproc_exec(&actor)==-KERNEL_EAGAIN);
    actor.limits->values[KERNEL_RLIMIT_NPROC].current=1;
    assert(!kernel_nproc_exec(&actor)&&!(actor.resource_flags&KERNEL_RESOURCE_NPROC_EXCEEDED)&&(actor.resource_flags&4));
    kernel_uid_account_put(old);finish(&actor);assert(!live);
    /* Linux marks using target usage before the setuid transfer, with strict >. */
    make_task(&actor,1005,1);make_task(&holder,1006,8);
    assert(!kernel_nproc_acquire(&actor,0)&&!kernel_nproc_acquire(&holder,0));
    kernel_credentials_publish(&actor,cred(1006));
    assert(actor.credentials->account->processes==2&&!(actor.resource_flags&KERNEL_RESOURCE_NPROC_EXCEEDED));
    assert(!kernel_nproc_exec(&actor));finish(&actor);finish(&holder);assert(!live);
}
static pthread_barrier_t gate;
struct attempt {struct kernel_task *parent;struct kernel_task task;int result;};
static void *admit(void *p)
{struct attempt *a=p;pthread_barrier_wait(&gate);a->result=kernel_nproc_acquire(&a->task,a->parent);return 0;}
static void concurrent(void)
{
    struct kernel_task parent;make_task(&parent,1007,2);assert(!kernel_nproc_acquire(&parent,0));
    struct attempt a[2]={{.parent=&parent},{.parent=&parent}};
    for(unsigned i=0;i<2;i++)make_task(&a[i].task,1007,2);
    assert(!pthread_barrier_init(&gate,0,3));pthread_t threads[2];
    for(unsigned i=0;i<2;i++)assert(!pthread_create(&threads[i],0,admit,&a[i]));
    pthread_barrier_wait(&gate);for(unsigned i=0;i<2;i++)assert(!pthread_join(threads[i],0));
    assert((a[0].result==0)+(a[1].result==0)==1);
    assert(parent.credentials->account->processes==2);
    for(unsigned i=0;i<2;i++){assert(a[i].result==0||a[i].result==-KERNEL_EAGAIN);finish(&a[i].task);}
    assert(parent.credentials->account->processes==1);finish(&parent);assert(!live);assert(!pthread_barrier_destroy(&gate));
}
int main(void)
{boundary();exemptions();transfer();concurrent();puts("NPROC quota/zombie/logical-reap/borrow/root/caps/UID-transfer/exec/concurrent/owners PASS");}
