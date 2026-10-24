#include <kernel/rlimit.h>
#include <kernel/heap.h>
#include <kernel/wait_internal.h>
#include <string.h>

struct kernel_resource_limits *kernel_resource_limits_create(struct kernel_heap *heap,
    const struct kernel_resource_limits *source)
{
    struct kernel_resource_limits *limits=0;
    if(kernel_heap_allocate_zeroed(heap,1,sizeof(*limits),(void **)&limits))return 0;
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
