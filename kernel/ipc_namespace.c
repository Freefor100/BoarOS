#include <kernel/ipc_namespace.h>
#include <kernel/heap.h>
#include <kernel/errno.h>
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
    struct kernel_mqueue_tree *mq;
    {
        KERNEL_RAW_SCOPE(guard,&ns->lock);
        sem=ns->sem;msg=ns->msg;shm=ns->shm;mq=ns->mq;
        ns->sem=0;ns->msg=0;ns->shm=0;ns->mq=0;
    }
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
