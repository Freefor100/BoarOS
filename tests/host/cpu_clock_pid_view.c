/* Actual resolver/sample and identities; accounting and storage-borrow edges modeled. */
#include "../../kernel/sched/cpu_clock.c"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
struct kernel_scheduler scheduler;
struct kernel_raw_lock kernel_wait_domain;
static struct kernel_cpu cpu;
_Thread_local uintptr_t sync_test_irq;
void *sync_test_cpu(void){return &cpu;}
void *sync_test_current_task(void){return cpu.current;}
void kernel_cpu_flush_notifications(void){assert(!cpu.outbound);}
void scheduler_task_get_locked(struct kernel_task *t){assert(cpu.raw_locks);t->storage_borrows++;}
void scheduler_task_put(struct kernel_task *t){assert(!cpu.raw_locks&&t->storage_borrows);t->storage_borrows--;}
static struct kernel_task *transfer_to;
static unsigned samples;
void scheduler_usage_thread_locked(const struct kernel_task *t,struct kernel_task_usage *u){assert(cpu.raw_locks);*u=t->usage;}
void scheduler_group_usage_snapshot(struct kernel_task *t,int children,struct kernel_task_usage *u)
{
    assert(!cpu.raw_locks&&!children);samples++;*u=t->usage;
    if(transfer_to){KERNEL_RAW_SCOPE(g,&kernel_wait_domain);kernel_pid_transfer(&t->identities[KERNEL_PID_TGID],&transfer_to->identities[KERNEL_PID_TGID],transfer_to);transfer_to=NULL;}
}
void kernel_heap_release(struct kernel_heap *h,void *p){(void)h;(void)p;abort();}
void exited_append_locked(struct kernel_task *t){(void)t;abort();}
void kernel_scheduler_notify_cleanup(void){abort();}
static int32_t encoded(int pid,int thread){return (int32_t)((~(uint32_t)pid<<3)|(thread?4:0)|2);}
static void resolves(struct kernel_task *caller,int pid,int thread,int strict,struct kernel_pid *expected)
{
    struct scheduler_cpu_clock c={0};int e=scheduler_cpu_clock_resolve(caller,encoded(pid,thread),strict,&c);
    if(!expected){assert(e==-KERNEL_EINVAL&&!c.identity);return;}
    assert(!e&&c.identity==expected&&c.kind==2&&c.thread==(unsigned)thread);kernel_task_identity_put(c.identity);
}
int main(void)
{
    kernel_cpu_initialize(&cpu,0,NULL);kernel_raw_lock_init(&kernel_wait_domain,KERNEL_RAW_RANK_SCHEDULER);
    struct kernel_pid_registry inner={0},sibling={0};struct kernel_pid_allocator alloc[3]={0};
    struct kernel_pid_registry *views[]={&scheduler.identities,&inner,&sibling};
    uint64_t bits[3][KERNEL_PID_BITMAP_WORDS(64)]={0};
    for(unsigned i=0;i<3;i++){assert(!kernel_pid_allocator_init(&alloc[i],bits[i],64));assert(!kernel_pid_registry_init(views[i],&alloc[i],i?views[0]:NULL));}
    struct {struct kernel_pid id;struct kernel_pid_binding ancestor;} ids[4]={0};struct kernel_task tasks[4]={0};
    for(unsigned i=0;i<4;i++){
        unsigned ns=i==0?0:i==2?2:1;kernel_pid_t numbers[]={i==3?2:1,i==2?21:(kernel_pid_t)(10+i)};
        struct kernel_task *t=&tasks[i];t->group_leader=i==3?&tasks[1]:t;t->state=KERNEL_THREAD_STATE_READY;
        t->usage.user_ns=100+i;t->usage.kernel_ns=20+i;
        assert(!kernel_pid_reserve_numbers(views[ns],&ids[i].id,sizeof(ids[i]),numbers,i?2:1));
        kernel_pid_attach(&t->identities[KERNEL_PID_TID],&ids[i].id,KERNEL_PID_TID,t);
        if(i<3)kernel_pid_attach(&t->identities[KERNEL_PID_TGID],&ids[i].id,KERNEL_PID_TGID,t);
        assert(!kernel_pid_publish_reserved(&ids[i].id));t->tid=kernel_pid_number_in(&ids[i].id,views[0]);
    }
    cpu.current=&tasks[1];
    resolves(&tasks[1],1,0,1,&ids[1].id);resolves(&tasks[2],1,0,1,&ids[2].id);
    resolves(&tasks[1],21,0,0,NULL);resolves(&tasks[0],21,0,1,&ids[2].id);
    resolves(&tasks[1],2,1,1,&ids[3].id);resolves(&tasks[2],2,1,1,NULL);
    resolves(&tasks[0],13,1,0,NULL);resolves(&tasks[3],2,0,0,&ids[1].id);
    resolves(&tasks[3],2,0,1,NULL);resolves(&tasks[3],13,0,0,NULL);
    resolves(&tasks[3],0,0,1,&ids[1].id);resolves(&tasks[3],0,1,1,&ids[3].id);
    tasks[3].state=KERNEL_THREAD_STATE_EXITED;resolves(&tasks[1],2,1,1,NULL);tasks[3].state=KERNEL_THREAD_STATE_READY;
    uint64_t ns=0;assert(!kernel_task_cpu_clock(&tasks[3],encoded(2,0),0,&ns)&&ns==122);
    assert(kernel_task_cpu_clock(&tasks[3],encoded(2,0),1,&ns)==-KERNEL_EINVAL);
    struct scheduler_cpu_clock held={0};assert(!scheduler_cpu_clock_resolve(&tasks[1],encoded(1,0),1,&held));
    struct kernel_task replacement={.group_leader=&replacement,.usage={.user_ns=900,.kernel_ns=1}};
    transfer_to=&replacement;samples=0;struct kernel_task_usage usage;
    assert(!scheduler_cpu_clock_sample(&held,&usage)&&samples==2&&usage.user_ns==900);
    assert(!tasks[1].storage_borrows&&!replacement.storage_borrows);
    {KERNEL_RAW_SCOPE(g,&kernel_wait_domain);kernel_pid_transfer(&replacement.identities[KERNEL_PID_TGID],&tasks[1].identities[KERNEL_PID_TGID],&tasks[1]);}
    {KERNEL_RAW_SCOPE(g,&kernel_wait_domain);kernel_pid_detach(&tasks[1].identities[KERNEL_PID_TGID]);}
    assert(scheduler_cpu_clock_sample(&held,&usage)==-KERNEL_ESRCH);
    resolves(&tasks[0],11,0,1,NULL);
    /* A live TID still pins the number; restore only to release the fixture. */
    {KERNEL_RAW_SCOPE(g,&kernel_wait_domain);kernel_pid_attach(&tasks[1].identities[KERNEL_PID_TGID],held.identity,KERNEL_PID_TGID,&tasks[1]);}
    kernel_task_identity_put(held.identity);
    {KERNEL_RAW_SCOPE(g,&kernel_wait_domain);
     for(unsigned i=0;i<4;i++){kernel_pid_detach(&tasks[i].identities[KERNEL_PID_TID]);if(i<3)kernel_pid_detach(&tasks[i].identities[KERNEL_PID_TGID]);kernel_pid_put(&ids[i].id);}
     unsigned n=0;while(kernel_pid_take_retired(views[0]))n++;assert(n==4);
     kernel_pid_registry_put(&inner);kernel_pid_registry_put(&sibling);
     n=0;while(kernel_pid_take_retired_namespace(views[0]))n++;assert(n==2);kernel_pid_registry_put(views[0]);}
    puts("PASS CPU clock caller PID view, nonleader gettime exception, foreign thread denial and role-transfer resample");return 0;
}
