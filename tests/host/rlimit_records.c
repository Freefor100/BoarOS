#include "../../kernel/sched/private.h"
#include <kernel/task_state.h>
#include <kernel/task.h>
#include <kernel/heap.h>
#include <kernel/wait_internal.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
_Thread_local uintptr_t sync_test_irq=1;
static _Thread_local struct kernel_cpu cpu={.initialized=KERNEL_CPU_INITIALIZED};
struct kernel_raw_lock kernel_wait_domain={.rank=KERNEL_RAW_RANK_SCHEDULER,.initialized=KERNEL_RAW_INITIALIZED};
void *sync_test_cpu(void){return &cpu;}
void kernel_cpu_flush_notifications(void){assert(!cpu.outbound);}
void scheduler_cpu_timer_update_locked(struct kernel_task *t,unsigned kind){(void)t;(void)kind;}
void kernel_posix_timer_changed(void){}
static struct kernel_heap heap;
static unsigned live,fail;
enum kernel_heap_status kernel_heap_allocate_zeroed(struct kernel_heap *h,size_t n,size_t size,void **out)
{ assert(h==&heap&&!cpu.raw_locks);if(fail)return KERNEL_HEAP_STATUS_EMPTY;*out=calloc(n,size);assert(*out);live++;return 0; }
void kernel_heap_release(struct kernel_heap *h,void *p)
{ assert(h==&heap&&!cpu.raw_locks&&live);free(p);live--; }
int main(void)
{
    struct kernel_task task={.magic=KERNEL_THREAD_MAGIC,.tid=1,.tid_owned=1,.group_members=1};
    task.group_leader=&task;task.limits=kernel_resource_limits_create(&heap,0);assert(task.limits&&live==1);
    const uint64_t defaults[16]={UINT64_MAX,UINT64_MAX,UINT64_MAX,0x800000,0,UINT64_MAX,4096,1024,0x800000,UINT64_MAX,UINT64_MAX,4096,819200,0,0,UINT64_MAX};
    for(unsigned i=0;i<16;i++){
        struct kernel_rlimit64 value;
        assert(!kernel_task_get_rlimit(&task,i,&value)&&value.current==defaults[i]);
    }
    struct kernel_task sibling=task;sibling.group_leader=&task;
    sibling.limits=kernel_resource_limits_get(task.limits);
    struct kernel_task child=task;child.group_leader=&child;
    child.limits=kernel_resource_limits_create(&heap,task.limits);assert(child.limits&&live==2);
    struct kernel_rlimit64 changed={37,4096},value;
    assert(!kernel_task_set_rlimit(&sibling,KERNEL_RLIMIT_NPROC,&changed));
    assert(!kernel_task_get_rlimit(&task,KERNEL_RLIMIT_NPROC,&value)&&value.current==37);
    assert(!kernel_task_get_rlimit(&child,KERNEL_RLIMIT_NPROC,&value)&&value.current==4096);
    changed=(struct kernel_rlimit64){UINT64_MAX,UINT64_MAX};
    assert(!kernel_task_set_rlimit(&task,KERNEL_RLIMIT_RSS,&changed));
    assert(kernel_task_get_rlimit(&task,16,&value)==KERNEL_TASK_STATUS_INVALID_ARGUMENT);
    changed=(struct kernel_rlimit64){2,1};assert(kernel_task_set_rlimit(&task,KERNEL_RLIMIT_LOCKS,&changed)==KERNEL_TASK_STATUS_INVALID_ARGUMENT);
    fail=1;assert(!kernel_resource_limits_create(&heap,task.limits)&&live==2);fail=0;
    kernel_resource_limits_put(task.limits);assert(live==2);
    assert(!kernel_task_get_rlimit(&sibling,KERNEL_RLIMIT_NPROC,&value)&&value.current==37);
    kernel_resource_limits_put(sibling.limits);kernel_resource_limits_put(child.limits);assert(!live);
    puts("RLIMIT records defaults16/share/fork-copy/query/set/OOM/owners PASS");
}
