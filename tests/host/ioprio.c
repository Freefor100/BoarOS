/* Production I/O-priority operations with a held task across exit. */
#include "../../kernel/sched/ioprio.c"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

_Thread_local uintptr_t sync_test_irq;
static _Thread_local struct kernel_cpu cpu={.initialized=KERNEL_CPU_INITIALIZED};
struct kernel_scheduler scheduler;
struct kernel_raw_lock kernel_wait_domain;
static struct kernel_task caller,target;
static struct kernel_credentials credentials;
static struct kernel_heap heap;
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t event=PTHREAD_COND_INITIALIZER;
static int lookup_ready,lookup_continue,allocations,fail_alloc,borrows;
static int64_t result;
static int set_operation;
void *sync_test_cpu(void){return &cpu;}
void *sync_test_current_task(void){return &caller;}
void kernel_cpu_flush_notifications(void){assert(!cpu.outbound);}
const struct kernel_credentials *kernel_credentials_current(void){return &credentials;}
const struct kernel_credentials *kernel_credentials_task_locked(const struct kernel_task *task)
{assert(cpu.raw_locks);return task->credentials;}
int kernel_credentials_capable(const struct kernel_credentials *c,unsigned cap)
{(void)c;(void)cap;return 1;}
enum kernel_heap_status kernel_heap_allocate_zeroed(struct kernel_heap *h,size_t n,size_t size,void **out)
{
    assert(h==&heap&&!cpu.raw_locks);
    if(fail_alloc)return KERNEL_HEAP_STATUS_EMPTY;
    *out=calloc(n,size);assert(*out);allocations++;return KERNEL_HEAP_STATUS_OK;
}
void kernel_heap_release(struct kernel_heap *h,void *p)
{assert(h==&heap && p && !cpu.raw_locks && allocations);allocations--;free(p);}
struct kernel_task *scheduler_task_lookup(kernel_pid_t pid,enum kernel_pid_role role,uint64_t generation)
{
    assert(pid==42 && role==KERNEL_PID_TID && !generation);
    __atomic_add_fetch(&borrows,1,__ATOMIC_RELAXED);
    assert(!pthread_mutex_lock(&gate));lookup_ready=1;assert(!pthread_cond_broadcast(&event));
    while(!lookup_continue)assert(!pthread_cond_wait(&event,&gate));
    assert(!pthread_mutex_unlock(&gate));return &target;
}
void scheduler_task_put(struct kernel_task *task)
{assert(task==&target && __atomic_sub_fetch(&borrows,1,__ATOMIC_RELAXED)>=0);}
void scheduler_task_cursor_begin(struct scheduler_task_cursor *cursor){cursor->task=0;}
void scheduler_task_cursor_next(struct scheduler_task_cursor *cursor){(void)cursor;abort();}
void scheduler_task_cursor_end(struct scheduler_task_cursor *cursor){assert(!cursor->task);}
struct kernel_pid *process_identity(const struct kernel_task *task,enum kernel_pid_role role)
{(void)task;(void)role;return 0;}
struct kernel_pid *kernel_pid_find(struct kernel_pid_registry *r,kernel_pid_t pid)
{(void)r;(void)pid;return 0;}
void kernel_pid_get(struct kernel_pid *id){(void)id;abort();}
void kernel_task_identity_put(struct kernel_pid *id){(void)id;abort();}
static void *query(void *unused)
{
    (void)unused;cpu.current=&caller;cpu.logical_id=1;
    result=kernel_ioprio_call(&caller,set_operation,1,42,(2<<13)|3);return 0;
}
static void exit_during_lookup(int setting)
{
    lookup_ready=lookup_continue=0;set_operation=setting;target.tid_owned=1;target.proc_exiting=0;
    assert(!kernel_ioprio_call(&caller,1,1,0,(2<<13)|4));
    assert(!kernel_ioprio_clone(&caller,&target,0));
    pthread_t t;assert(!pthread_create(&t,0,query,0));
    assert(!pthread_mutex_lock(&gate));while(!lookup_ready)assert(!pthread_cond_wait(&event,&gate));
    assert(!pthread_mutex_unlock(&gate));
    kernel_ioprio_exit(&target);
    {KERNEL_RAW_SCOPE(g,&kernel_wait_domain);target.proc_exiting=1;target.tid_owned=0;}
    assert(!pthread_mutex_lock(&gate));lookup_continue=1;assert(!pthread_cond_broadcast(&event));assert(!pthread_mutex_unlock(&gate));
    assert(!pthread_join(t,0));assert(result==0 && !target.io_priority && !borrows);
    kernel_ioprio_exit(&caller);assert(!allocations);
}
int main(void)
{
    cpu.current=&caller;kernel_raw_lock_init(&kernel_wait_domain,KERNEL_RAW_RANK_SCHEDULER);
    caller.tid_owned=1;caller.files.heap=&heap;caller.credentials=&credentials;
    target.tid_owned=1;target.files.heap=&heap;target.credentials=&credentials;
    exit_during_lookup(0);exit_during_lookup(1);
    assert(!kernel_ioprio_call(&caller,1,1,0,(2<<13)|4));
    fail_alloc=1;assert(kernel_ioprio_clone(&caller,&target,0)==-KERNEL_ENOMEM && !target.io_priority);fail_alloc=0;
    assert(!kernel_ioprio_clone(&caller,&target,1));kernel_ioprio_exit(&caller);
    assert(target.io_priority && target.io_priority->priority==((2<<13)|4));
    kernel_ioprio_exit(&target);assert(!allocations);
    puts("ioprio: held target exit, OOM and last shared owner passed");return 0;
}
