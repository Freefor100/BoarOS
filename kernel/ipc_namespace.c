#include <kernel/ipc_namespace.h>
#include <kernel/heap.h>
#include <kernel/errno.h>
#include <kernel/shm.h>
#include <string.h>
/* An open sysctl retains this small record instead of owning the namespace. */
struct kernel_shm_limits {
    struct kernel_heap *heap;
    uint32_t references;
    uint32_t retired;
    uint64_t values[KERNEL_SHM_LIMIT_COUNT];
};

static const uint64_t shm_defaults[KERNEL_SHM_LIMIT_COUNT] = {
    [KERNEL_SHM_LIMIT_MAX] = KERNEL_SHMMAX,
    [KERNEL_SHM_LIMIT_ALL] = KERNEL_SHMALL,
    [KERNEL_SHM_LIMIT_MNI] = KERNEL_SHMMNI,
};

void kernel_ipc_shm_limits_put(struct kernel_shm_limits *limits)
{
    if (limits && !__atomic_sub_fetch(&limits->references, 1, __ATOMIC_ACQ_REL))
        kernel_heap_release(limits->heap, limits);
}

int kernel_ipc_shm_limits_open(struct kernel_namespace *base, struct kernel_shm_limits **owner)
{
    struct kernel_ipc_namespace *ns = kernel_ipc_namespace_state(base);
    struct kernel_shm_limits *limits = __atomic_load_n(&ns->shm_limits, __ATOMIC_ACQUIRE);
    if (!limits) {
        struct kernel_shm_limits *candidate;
        if (kernel_heap_allocate_zeroed(base->heap, 1, sizeof(*candidate), (void **)&candidate))
            return -KERNEL_ENOMEM;
        candidate->heap = base->heap;
        candidate->references = 1;
        memcpy(candidate->values, shm_defaults, sizeof(shm_defaults));
        {
            KERNEL_RAW_SCOPE(guard, &ns->lock);
            if (!ns->shm_limits)
                __atomic_store_n(&ns->shm_limits, candidate, __ATOMIC_RELEASE);
            limits = ns->shm_limits;
        }
        if (limits != candidate) kernel_ipc_shm_limits_put(candidate);
    }
    /* The opener still owns base, so the namespace's reference is live. */
    __atomic_add_fetch(&limits->references, 1, __ATOMIC_RELAXED);
    *owner = limits;
    return 0;
}

int kernel_ipc_shm_limit_read(const struct kernel_shm_limits *limits, enum kernel_shm_limit which,
    uint64_t *value)
{
    if (__atomic_load_n(&limits->retired, __ATOMIC_ACQUIRE)) return -KERNEL_ENOENT;
    *value = __atomic_load_n(&limits->values[which], __ATOMIC_RELAXED);
    return 0;
}

int kernel_ipc_shm_limit_set(struct kernel_shm_limits *limits, enum kernel_shm_limit which,
    uint64_t value)
{
    if (__atomic_load_n(&limits->retired, __ATOMIC_ACQUIRE)) return -KERNEL_ENOENT;
    if (which == KERNEL_SHM_LIMIT_MNI && value > KERNEL_SHMMNI_MAX) return -KERNEL_EINVAL;
    __atomic_store_n(&limits->values[which], value, __ATOMIC_RELAXED);
    return 0;
}

uint64_t kernel_ipc_shm_limit(const struct kernel_ipc_namespace *ns, enum kernel_shm_limit which)
{
    const struct kernel_shm_limits *limits = __atomic_load_n(&ns->shm_limits, __ATOMIC_ACQUIRE);
    return limits ? __atomic_load_n(&limits->values[which], __ATOMIC_RELAXED) : shm_defaults[which];
}

static struct kernel_ipc_namespace initial = {
    .base = {.references=1,.type=KERNEL_NS_IPC,.id=KERNEL_NS_INITIAL_IPC_ID},
};
struct kernel_namespace *kernel_ipc_initial(void) { return &initial.base; }
struct kernel_ipc_namespace *kernel_ipc_namespace_state(struct kernel_namespace *base)
{
    if(!base || base->type!=KERNEL_NS_IPC)__builtin_trap();
    return (struct kernel_ipc_namespace *)base;
}
int kernel_ipc_namespace_boot(struct kernel_heap *heap)
{
    if(!heap)return -KERNEL_EINVAL;
    if(initial.base.heap)return initial.base.heap==heap?0:-KERNEL_EINVAL;
    kernel_raw_lock_init(&initial.lock,24);initial.base.heap=heap;return 0;
}
static void ipc_release_tables(struct kernel_ipc_namespace *ns)
{
    struct kernel_sem_table *sem;
    struct kernel_msg_table *msg;
    struct kernel_shm_table *shm;
    struct kernel_shm_limits *limits;
    struct kernel_mqueue_tree *mq;
    {
        KERNEL_RAW_SCOPE(guard,&ns->lock);
        sem=ns->sem;msg=ns->msg;shm=ns->shm;mq=ns->mq;
        limits=ns->shm_limits;ns->shm_limits=NULL;
        if (limits) __atomic_store_n(&limits->retired, 1, __ATOMIC_RELEASE);
        ns->sem=0;ns->msg=0;ns->shm=0;ns->mq=0;
    }
    kernel_ipc_shm_limits_put(limits);
    kernel_sem_namespace_destroy(sem);
    kernel_msg_namespace_destroy(msg);
    kernel_shm_namespace_destroy(shm);
    kernel_mqueue_namespace_destroy(mq);
}
static void ipc_destroy(struct kernel_namespace *base)
{
    ipc_release_tables(kernel_ipc_namespace_state(base));
    kernel_heap_release(base->heap,base);
}
int kernel_ipc_initial_shutdown(void)
{
    if(__atomic_load_n(&initial.base.references,__ATOMIC_ACQUIRE)!=1)
        return -KERNEL_EBUSY;
    if(initial.base.heap)ipc_release_tables(&initial);
    return 0;
}
int kernel_ipc_namespace_create(struct kernel_heap *heap,struct kernel_namespace **owner)
{
    if(!heap||!owner||*owner)return -KERNEL_EINVAL;
    struct kernel_ipc_namespace *ns=0;
    enum kernel_heap_status s=kernel_heap_allocate_zeroed(heap,1,sizeof(*ns),(void **)&ns);
    if(s)return s==KERNEL_HEAP_STATUS_EMPTY?-KERNEL_ENOMEM:-KERNEL_EIO;
    kernel_namespace_init(&ns->base,KERNEL_NS_IPC,heap,ipc_destroy);
    kernel_raw_lock_init(&ns->lock,24);*owner=&ns->base;return 0;
}
