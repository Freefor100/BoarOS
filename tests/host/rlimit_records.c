#include "../../kernel/sched/private.h"
#include <kernel/task_state.h>
#include <kernel/task.h>
#include <kernel/heap.h>
#include <kernel/credentials.h>
#include <kernel/errno.h>
#include <kernel/wait_internal.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
_Thread_local uintptr_t sync_test_irq=1;
static _Thread_local struct kernel_cpu cpu={.initialized=KERNEL_CPU_INITIALIZED};
struct kernel_scheduler scheduler={.initialized=KERNEL_SCHEDULER_INITIALIZED};
struct kernel_raw_lock kernel_wait_domain={.rank=KERNEL_RAW_RANK_SCHEDULER,.initialized=KERNEL_RAW_INITIALIZED};
void *sync_test_cpu(void){return &cpu;}
void *sync_test_current_task(void){return cpu.current;}
void kernel_cpu_flush_notifications(void){assert(!cpu.outbound);}
void scheduler_cpu_timer_update_locked(struct kernel_task *t,unsigned kind){(void)t;(void)kind;}
void kernel_posix_timer_changed(void){}
static struct kernel_heap heap;
static unsigned live,fail;
enum kernel_heap_status kernel_heap_allocate(struct kernel_heap *h,size_t size,void **out)
{ assert(h==&heap&&!cpu.raw_locks);if(fail)return KERNEL_HEAP_STATUS_EMPTY;*out=malloc(size);assert(*out);live++;return 0; }
enum kernel_heap_status kernel_heap_allocate_zeroed(struct kernel_heap *h,size_t n,size_t size,void **out)
{ enum kernel_heap_status status=kernel_heap_allocate(h,n*size,out);if(!status)memset(*out,0,n*size);return status; }
void kernel_heap_release(struct kernel_heap *h,void *p)
{ assert(h==&heap&&!cpu.raw_locks&&live);free(p);live--; }
static int change(struct kernel_task *task,unsigned resource,const struct kernel_rlimit64 *replacement,
    struct kernel_rlimit64 *before)
{
    assert(kernel_task_current()==cpu.current);
    return kernel_task_prlimit(task,resource,replacement,before,
        kernel_credentials_capable(kernel_credentials_current(),KERNEL_CAP_SYS_RESOURCE));
}
int main(void)
{
    struct kernel_task task={.magic=KERNEL_THREAD_MAGIC,.tid=1,.tid_owned=1,.group_members=1,
        .state=KERNEL_THREAD_STATE_RUNNING,.credentials=kernel_credentials_initial()};
    task.arch.user_mode=1;cpu.current=&task;
    assert(kernel_task_current()==&task && kernel_credentials_current()==task.credentials);
    task.group_leader=&task;task.limits=kernel_resource_limits_create(&heap,0);assert(task.limits&&live==1);
    const uint64_t defaults[16]={UINT64_MAX,UINT64_MAX,UINT64_MAX,0x800000,0,UINT64_MAX,4096,1024,0x800000,UINT64_MAX,UINT64_MAX,4096,819200,0,0,UINT64_MAX};
    for(unsigned i=0;i<16;i++){
        struct kernel_rlimit64 value;
        assert(!kernel_task_get_rlimit(&task,i,&value)&&value.current==defaults[i]);
    }
    struct kernel_rlimit64 stack;
    assert(!kernel_task_get_rlimit(&task,KERNEL_RLIMIT_STACK,&stack));
    assert(stack.maximum==KERNEL_RLIM_INFINITY);
    stack=(struct kernel_rlimit64){10*1024*1024,10*1024*1024};
    assert(!change(&task,KERNEL_RLIMIT_STACK,&stack,0));
    assert(!kernel_task_get_rlimit(&task,KERNEL_RLIMIT_STACK,&stack));
    assert(stack.current==10*1024*1024 && stack.maximum==10*1024*1024);
    stack=(struct kernel_rlimit64){KERNEL_RLIM_INFINITY,KERNEL_RLIM_INFINITY};
    assert(!change(&task,KERNEL_RLIMIT_STACK,&stack,0));
    assert(!kernel_task_get_rlimit(&task,KERNEL_RLIMIT_STACK,&stack));
    assert(stack.current==KERNEL_RLIM_INFINITY && stack.maximum==KERNEL_RLIM_INFINITY);
    struct kernel_task sibling=task;sibling.tid=2;sibling.state=KERNEL_THREAD_STATE_READY;sibling.group_leader=&task;task.group_members=2;
    sibling.limits=kernel_resource_limits_get(task.limits);
    struct kernel_task child=task;child.tid=3;child.state=KERNEL_THREAD_STATE_READY;child.group_members=1;child.group_leader=&child;
    child.limits=kernel_resource_limits_create(&heap,task.limits);assert(child.limits&&live==2);
    struct kernel_rlimit64 changed={37,4096},value;
    assert(!change(&sibling,KERNEL_RLIMIT_NPROC,&changed,&value));
    assert(value.current==4096 && !kernel_task_get_rlimit(&task,KERNEL_RLIMIT_NPROC,&value)&&value.current==37);
    assert(!kernel_task_get_rlimit(&child,KERNEL_RLIMIT_NPROC,&value)&&value.current==4096);
    changed=(struct kernel_rlimit64){UINT64_MAX,UINT64_MAX};
    assert(!change(&task,KERNEL_RLIMIT_RSS,&changed,0));
    assert(kernel_task_get_rlimit(&task,16,&value)==KERNEL_TASK_STATUS_INVALID_ARGUMENT);
    changed=(struct kernel_rlimit64){2,1};assert(change(&task,KERNEL_RLIMIT_LOCKS,&changed,0)==-KERNEL_EINVAL);
    assert(change(&task,16,0,&value)==-KERNEL_EINVAL);
    const struct kernel_credentials *root=task.credentials;
    struct kernel_credentials *unpriv=kernel_credentials_clone(&heap,root);assert(unpriv);
    assert(!kernel_credentials_set_account(unpriv,1000));
    unpriv->uid=unpriv->euid=unpriv->suid=unpriv->fsuid=1000;
    unpriv->gid=unpriv->egid=unpriv->sgid=unpriv->fsgid=1000;
    unpriv->effective=unpriv->permitted=0;
    task.credentials=unpriv;
    assert(kernel_credentials_current()==unpriv && !kernel_credentials_capable(unpriv,KERNEL_CAP_SYS_RESOURCE));
    changed=(struct kernel_rlimit64){64,64};
    assert(!change(&task,KERNEL_RLIMIT_NOFILE,&changed,&value) && value.current==1024 && value.maximum==1024);
    changed=(struct kernel_rlimit64){64,65};
    assert(change(&task,KERNEL_RLIMIT_NOFILE,&changed,0)==-KERNEL_EPERM);
    assert(!change(&task,KERNEL_RLIMIT_NOFILE,0,&value) && value.current==64 && value.maximum==64);
    assert(change(&child,KERNEL_RLIMIT_RSS,0,&value)==-KERNEL_EPERM);
    assert(change(&child,16,0,&value)==-KERNEL_EPERM); /* Permission precedes resource validation. */
    child.credentials=kernel_credentials_get(unpriv);
    assert(!change(&child,KERNEL_RLIMIT_NPROC,0,&value) && value.current==4096);
    const struct kernel_credentials *child_cred=child.credentials;child.credentials=root;
    task.credentials=root;kernel_credentials_put(unpriv);kernel_credentials_put(child_cred);
    assert(live==2 && kernel_credentials_capable(kernel_credentials_current(),KERNEL_CAP_SYS_RESOURCE));
    changed=(struct kernel_rlimit64){1024,1025};
    assert(change(&task,KERNEL_RLIMIT_NOFILE,&changed,0)==-KERNEL_EPERM);
    changed=(struct kernel_rlimit64){1024,1024};
    assert(!change(&task,KERNEL_RLIMIT_NOFILE,&changed,0));
    fail=1;assert(!kernel_resource_limits_create(&heap,task.limits)&&live==2);fail=0;
    kernel_resource_limits_put(task.limits);assert(live==2);
    assert(!kernel_task_get_rlimit(&sibling,KERNEL_RLIMIT_NPROC,&value)&&value.current==37);
    kernel_resource_limits_put(sibling.limits);kernel_resource_limits_put(child.limits);assert(!live);
    puts("RLIMIT records defaults16/share/fork-copy/query/atomic-set/credentials/hard-limit/OOM/owners PASS");
}
