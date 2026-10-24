#include <kernel/rlimit.h>
#include <kernel/heap.h>
#include <kernel/task_state.h>
#include <kernel/credentials.h>
#include <kernel/errno.h>
#include <kernel/wait_internal.h>
#include <string.h>

struct kernel_resource_limits *kernel_resource_limits_create(struct kernel_heap *heap,
    const struct kernel_resource_limits *source)
{
    struct kernel_resource_limits *limits=0;
    enum kernel_heap_status status=kernel_heap_allocate_zeroed(heap,1,sizeof(*limits),(void **)&limits);
    if(status==KERNEL_HEAP_STATUS_EMPTY)return 0;
    if(status!=KERNEL_HEAP_STATUS_OK)__builtin_trap();
    limits->heap=heap;limits->references=1;
    if(source){
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        memcpy(limits->values,source->values,sizeof(limits->values));
    }else{
        for(unsigned i=0;i<KERNEL_RLIMIT_COUNT;i++)
            limits->values[i]=(struct kernel_rlimit64){KERNEL_RLIM_INFINITY,KERNEL_RLIM_INFINITY};
        limits->values[KERNEL_RLIMIT_CORE].current=0;
        limits->values[KERNEL_RLIMIT_STACK]=(struct kernel_rlimit64){KERNEL_RLIMIT_STACK_CAP,KERNEL_RLIMIT_STACK_CAP};
        limits->values[KERNEL_RLIMIT_NOFILE]=(struct kernel_rlimit64){KERNEL_RLIMIT_NOFILE_CAP,KERNEL_RLIMIT_NOFILE_CAP};
        limits->values[KERNEL_RLIMIT_NPROC]=(struct kernel_rlimit64){4096,4096};
        limits->values[KERNEL_RLIMIT_SIGPENDING]=(struct kernel_rlimit64){KERNEL_RLIMIT_SIGPENDING_DEFAULT,KERNEL_RLIMIT_SIGPENDING_DEFAULT};
        limits->values[KERNEL_RLIMIT_MEMLOCK]=(struct kernel_rlimit64){UINT64_C(8)*1024*1024,UINT64_C(8)*1024*1024};
        limits->values[KERNEL_RLIMIT_MSGQUEUE]=(struct kernel_rlimit64){819200,819200};
        limits->values[KERNEL_RLIMIT_NICE]=limits->values[KERNEL_RLIMIT_RTPRIO]=(struct kernel_rlimit64){0,0};
    }
    return limits;
}
struct kernel_resource_limits *kernel_resource_limits_get(struct kernel_resource_limits *limits)
{
    uint32_t refs=__atomic_load_n(&limits->references,__ATOMIC_RELAXED);
    do { if(!refs||refs==UINT32_MAX)__builtin_trap(); }
    while(!__atomic_compare_exchange_n(&limits->references,&refs,refs+1,0,__ATOMIC_RELAXED,__ATOMIC_RELAXED));
    return limits;
}
void kernel_resource_limits_put(struct kernel_resource_limits *limits)
{
    if(!limits)return;
    uint32_t refs=__atomic_fetch_sub(&limits->references,1,__ATOMIC_ACQ_REL);
    if(!refs)__builtin_trap();
    if(refs==1)kernel_heap_release(limits->heap,limits);
}

int kernel_nproc_acquire(struct kernel_task *task,struct kernel_task *parent)
{
    const struct kernel_credentials *cred=task->credentials;
    struct kernel_uid_account *account=cred->account;
    const struct kernel_resource_limits *limits=parent?parent->limits:task->limits;
    KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
    if(task->resource_flags&KERNEL_RESOURCE_NPROC_CHARGED)__builtin_trap();
    if(account->processes==UINT64_MAX)__builtin_trap();
    if(account->processes>=limits->values[KERNEL_RLIMIT_NPROC].current && cred->uid!=0 &&
        !kernel_credentials_capable(cred,KERNEL_CAP_SYS_RESOURCE) &&
        !kernel_credentials_capable(cred,KERNEL_CAP_SYS_ADMIN)) return -KERNEL_EAGAIN;
    account->processes++;
    uint32_t inherited=parent?(parent->resource_flags&KERNEL_RESOURCE_NPROC_EXCEEDED):0;
    __atomic_fetch_or(&task->resource_flags,KERNEL_RESOURCE_NPROC_CHARGED|inherited,__ATOMIC_RELAXED);
    if(parent)__atomic_fetch_and(&parent->resource_flags,~KERNEL_RESOURCE_NPROC_EXCEEDED,__ATOMIC_RELAXED);
    return 0;
}
void kernel_nproc_release_locked(struct kernel_task *task)
{
    if(!(task->resource_flags&KERNEL_RESOURCE_NPROC_CHARGED))return;
    struct kernel_uid_account *account=task->credentials->account;
    if(!account->processes)__builtin_trap();
    account->processes--;
    __atomic_fetch_and(&task->resource_flags,~KERNEL_RESOURCE_NPROC_CHARGED,__ATOMIC_RELAXED);
}
void kernel_nproc_credentials_changed_locked(struct kernel_task *task,
    const struct kernel_credentials *old,const struct kernel_credentials *replacement)
{
    if(old->account==replacement->account ||
       !(task->resource_flags&KERNEL_RESOURCE_NPROC_CHARGED))return;
    uint64_t limit=task->limits->values[KERNEL_RLIMIT_NPROC].current;
    /* Linux在commit_creds转账前标记目标账户超限；setuid本身仍成功。 */
    if(replacement->uid!=0 && replacement->account->processes>limit)
        __atomic_fetch_or(&task->resource_flags,KERNEL_RESOURCE_NPROC_EXCEEDED,__ATOMIC_RELAXED);
    else __atomic_fetch_and(&task->resource_flags,~KERNEL_RESOURCE_NPROC_EXCEEDED,__ATOMIC_RELAXED);
    if(!old->account->processes || replacement->account->processes==UINT64_MAX)__builtin_trap();
    replacement->account->processes++;
    old->account->processes--;
}
int kernel_nproc_exec(struct kernel_task *task)
{
    KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
    if((task->resource_flags&KERNEL_RESOURCE_NPROC_EXCEEDED) &&
       task->credentials->account->processes>task->limits->values[KERNEL_RLIMIT_NPROC].current)
        return -KERNEL_EAGAIN;
    __atomic_fetch_and(&task->resource_flags,~KERNEL_RESOURCE_NPROC_EXCEEDED,__ATOMIC_RELAXED);
    return 0;
}
