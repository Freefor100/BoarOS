/* Production priority operations with real PID indexes; traversal/heap edges modeled. */
#include "../../kernel/sched/ioprio.c"
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
static struct kernel_heap heap;
static unsigned allocations;
static int deny;
const struct kernel_credentials *kernel_credentials_current(void){return cpu.current->credentials;}
const struct kernel_credentials *kernel_credentials_task_locked(const struct kernel_task *t){assert(cpu.raw_locks);return t->credentials;}
int kernel_credentials_capable(const struct kernel_credentials *c,unsigned cap){(void)c;(void)cap;return !deny;}
enum kernel_heap_status kernel_heap_allocate_zeroed(struct kernel_heap *h,size_t n,size_t size,void **out)
{assert(h==&heap&&!cpu.raw_locks);*out=calloc(n,size);assert(*out);allocations++;return KERNEL_HEAP_STATUS_OK;}
void kernel_heap_release(struct kernel_heap *h,void *p){assert(h==&heap&&!cpu.raw_locks&&allocations);free(p);allocations--;}
struct kernel_task *scheduler_task_lookup_in_locked(struct kernel_pid_registry *view,kernel_pid_t number,enum kernel_pid_role role,uint64_t generation)
{
    assert(cpu.raw_locks&&!generation);struct kernel_pid *id=kernel_pid_find(view,number);
    struct kernel_task *task=id&&id->members[role]?id->members[role]->task:NULL;
    if(task)task->storage_borrows++;
    return task;
}
void scheduler_task_put(struct kernel_task *task){assert(!cpu.raw_locks&&task->storage_borrows);task->storage_borrows--;}
void scheduler_task_cursor_begin(struct scheduler_task_cursor *c){c->task=scheduler.all_tasks;}
void scheduler_task_cursor_next(struct scheduler_task_cursor *c){c->task=c->task->all_next;}
void scheduler_task_cursor_end(struct scheduler_task_cursor *c){c->task=NULL;}
void exited_append_locked(struct kernel_task *task){(void)task;abort();}
void kernel_scheduler_notify_cleanup(void){abort();}
int main(void)
{
    kernel_cpu_initialize(&cpu,0,NULL);kernel_raw_lock_init(&kernel_wait_domain,KERNEL_RAW_RANK_SCHEDULER);
    struct kernel_pid_registry inner={0},sibling={0};struct kernel_pid_allocator alloc[3]={0};
    struct kernel_pid_registry *views[]={&scheduler.identities,&inner,&sibling};
    uint64_t bits[3][KERNEL_PID_BITMAP_WORDS(64)]={0};
    for(unsigned i=0;i<3;i++){assert(!kernel_pid_allocator_init(&alloc[i],bits[i],64));assert(!kernel_pid_registry_init(views[i],&alloc[i],i?views[0]:NULL));}
    struct {struct kernel_pid id;struct kernel_pid_binding ancestor;} ids[3]={0};struct kernel_task tasks[3]={0};
    struct kernel_credentials creds[3]={{.uid=123,.euid=123},{.uid=123,.euid=123},{.uid=123,.euid=123}};
    for(unsigned i=0;i<3;i++){
        struct kernel_task *t=&tasks[i];kernel_pid_t numbers[]={1,(kernel_pid_t)(10*i+1)};
        t->group_leader=t;t->arch.user_mode=t->tid_owned=t->accounted=1;
        t->credentials=&creds[i];t->files.heap=&heap;t->all_next=i<2?&tasks[i+1]:NULL;
        assert(!kernel_pid_reserve_numbers(views[i],&ids[i].id,sizeof(ids[i]),numbers,i?2:1));
        kernel_pid_attach(&t->identities[KERNEL_PID_TID],&ids[i].id,KERNEL_PID_TID,t);
        kernel_pid_attach(&t->identities[KERNEL_PID_TGID],&ids[i].id,KERNEL_PID_TGID,t);
        kernel_pid_attach(&t->identities[KERNEL_PID_PGID],&ids[0].id,KERNEL_PID_PGID,t);
        assert(!kernel_pid_publish_reserved(&ids[i].id));
    }
    scheduler.all_tasks=tasks;cpu.current=&tasks[1];
    assert(!kernel_ioprio_call(&tasks[1],1,1,1,(2<<13)|6));
    assert(tasks[1].io_priority&&!tasks[0].io_priority&&!tasks[2].io_priority);
    assert(kernel_ioprio_call(&tasks[1],0,1,21,0)==-KERNEL_ESRCH);
    assert(kernel_ioprio_call(&tasks[1],0,2,1,0)==-KERNEL_ESRCH);
    assert(kernel_ioprio_call(&tasks[1],0,3,123,0)==((2<<13)|6));
    assert(kernel_ioprio_call(&tasks[1],0,2,0,0)==((2<<13)|4));
    assert(!kernel_ioprio_call(&tasks[1],1,3,123,(2<<13)|7));
    assert(tasks[1].io_priority->priority==((2<<13)|7)&&!tasks[0].io_priority&&!tasks[2].io_priority);
    assert(!kernel_ioprio_call(&tasks[1],1,2,0,(2<<13)|5));
    for(unsigned i=0;i<3;i++)assert(tasks[i].io_priority->priority==((2<<13)|5));
    cpu.current=&tasks[0];assert(!kernel_ioprio_call(&tasks[0],1,1,21,(2<<13)|2));
    assert(tasks[2].io_priority->priority==((2<<13)|2));
    cpu.current=&tasks[2];assert(kernel_ioprio_call(&tasks[2],0,1,1,0)==((2<<13)|2));
    creds[2].uid=456;cpu.current=&tasks[0];deny=1;
    assert(kernel_ioprio_call(&tasks[0],1,1,21,(2<<13)|3)==-KERNEL_EPERM);
    assert(tasks[2].io_priority->priority==((2<<13)|2));
    for(unsigned i=0;i<3;i++){assert(!tasks[i].storage_borrows);kernel_ioprio_exit(&tasks[i]);}
    {KERNEL_RAW_SCOPE(g,&kernel_wait_domain);
     for(unsigned i=0;i<3;i++){for(unsigned r=0;r<3;r++)kernel_pid_detach(&tasks[i].identities[r]);kernel_pid_put(&ids[i].id);}
     unsigned n=0;while(kernel_pid_take_retired(views[0]))n++;assert(n==3);
     kernel_pid_registry_put(&inner);kernel_pid_registry_put(&sibling);
     n=0;while(kernel_pid_take_retired_namespace(views[0]))n++;assert(n==2);kernel_pid_registry_put(views[0]);}
    assert(!allocations);puts("PASS ioprio caller PID/PGID, visible UID, inherited real group and owners");return 0;
}
