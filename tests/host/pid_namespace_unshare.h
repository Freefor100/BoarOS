#include <kernel/uts.h>
#include <kernel/ipc_namespace.h>
#include <kernel/credentials.h>

/* The transaction uses real UTS/IPC/PID owners. Model the FS prepare and
 * exchange boundary with one allocated record, and count undo consumption. */
static unsigned undo_exits,shared_fs,mount_copies;
void kernel_sem_exit(struct kernel_task *task) { assert(task==cpu.current);undo_exits++; }
void kernel_sem_namespace_destroy(struct kernel_sem_table *p){assert(!p);}
void kernel_msg_namespace_destroy(struct kernel_msg_table *p){assert(!p);}
void kernel_shm_namespace_destroy(struct kernel_shm_table *p){assert(!p);}
void kernel_mqueue_namespace_destroy(struct kernel_mqueue_tree *p){assert(!p);}
enum kernel_fs_context_status kernel_fs_context_fork(struct kernel_fs_context *to,const struct kernel_fs_context *from)
{
    assert(from==&cpu.current->fs);
    void *record=NULL;
    if(kernel_heap_allocate_zeroed(&scheduler.identity_heap,1,1,&record))return KERNEL_FS_CONTEXT_STATUS_NO_MEMORY;
    to->record=record;to->state=KERNEL_FS_CONTEXT_LIVE;return 0;
}
int kernel_fs_context_prepare_unshare(struct kernel_fs_context *to,const struct kernel_fs_context *from,int mount)
{
    if(mount)mount_copies++;
    else if(!shared_fs)return 0;
    return kernel_fs_context_fork(to,from)?-KERNEL_ENOMEM:0;
}
void kernel_fs_context_exchange(struct kernel_fs_context *to,struct kernel_fs_context *from,struct kernel_fs_context *old)
{*old=*to;*to=*from;*from=(struct kernel_fs_context){0};}
void kernel_fs_context_exchange_paths_namespace(struct kernel_fs_context *to,struct kernel_fs_context *from,struct kernel_fs_context *old)
{(void)to;(void)from;(void)old;assert(0);}
enum kernel_fs_context_status kernel_fs_context_release(struct kernel_fs_context *fs)
{assert(fs->record);kernel_heap_release(&scheduler.identity_heap,fs->record);*fs=(struct kernel_fs_context){0};return 0;}
static void unshare_case(struct kernel_task *parent)
{
    const uint64_t flags=KERNEL_NS_PID|KERNEL_NS_MNT|KERNEL_NS_UTS|KERNEL_NS_IPC;
    unsigned live=allocations,refs=scheduler.identities.references;
    struct kernel_credentials denied={0};
    scheduler.initialized=KERNEL_SCHEDULER_INITIALIZED;
    kernel_raw_lock_init(&parent->files_access,KERNEL_RAW_RANK_TASK_FILES);
    parent->files.heap=&scheduler.identity_heap;
    parent->uts_namespace=kernel_namespace_get(kernel_uts_initial());
    parent->ipc_namespace=kernel_namespace_get(kernel_ipc_initial());
    parent->credentials=&denied;parent->group_members=2;
    assert(kernel_task_unshare_namespaces(parent,0x10000)==-KERNEL_EINVAL);
    assert(kernel_task_unshare_namespaces(parent,flags|0x10000)==-KERNEL_EINVAL);
    assert(kernel_task_unshare_namespaces(parent,flags)==-KERNEL_EPERM);
    assert(!mount_copies && allocations==live);
    const uint64_t copy_flags[]={flags,KERNEL_NS_PID|0x200,KERNEL_NS_MNT,KERNEL_NS_UTS|0x200};
    for(shared_fs=0;shared_fs<=1;shared_fs++) for(unsigned i=0;i<sizeof(copy_flags)/sizeof(copy_flags[0]);i++) {
        fail_allocation=1;
        if((copy_flags[i]&KERNEL_NS_MNT) || shared_fs)
            assert(kernel_task_unshare_namespaces(parent,copy_flags[i])==-KERNEL_ENOMEM && !fail_allocation);
        else {
            assert(kernel_task_unshare_namespaces(parent,copy_flags[i])==-KERNEL_EPERM && fail_allocation==1);
            fail_allocation=0;
        }
        assert(kernel_task_unshare_namespaces(parent,copy_flags[i])==-KERNEL_EPERM);
        assert(!mount_copies && !undo_exits && !parent->fs.record && !parent->pid_for_children &&
            parent->uts_namespace==kernel_uts_initial() && parent->ipc_namespace==kernel_ipc_initial() &&
            allocations==live && scheduler.identities.references==refs);
    }
    shared_fs=0;
    parent->credentials=NULL;
    for(unsigned fail=1;fail<=4;fail++) {
        fail_allocation=fail;
        assert(kernel_task_unshare_namespaces(parent,flags)==-KERNEL_ENOMEM);
        assert(!parent->fs.record && parent->uts_namespace==kernel_uts_initial() &&
            parent->ipc_namespace==kernel_ipc_initial() && !parent->pid_for_children);
        assert(!undo_exits && allocations==live && scheduler.identities.references==refs);
    }
    assert(!kernel_task_unshare_namespaces(parent,flags));
    struct kernel_pid_registry *target=parent->pid_for_children;
    assert(target && target->parent==&scheduler.identities && !target->child_reaper &&
        process_pid_active(parent)==&scheduler.identities && parent->fs.record &&
        parent->uts_namespace!=kernel_uts_initial() && parent->ipc_namespace!=kernel_ipc_initial() && undo_exits==1);
    struct kernel_task child={0};
    unsigned after=allocations;
    struct kernel_namespace *uts=parent->uts_namespace,*ipc=parent->ipc_namespace;
    struct kernel_fs_context_record *paths=parent->fs.record;
    assert(kernel_task_unshare_namespaces(parent,flags)==-KERNEL_EINVAL);
    assert(parent->pid_for_children==target && parent->uts_namespace==uts &&
        parent->ipc_namespace==ipc && parent->fs.record==paths && undo_exits==1 &&
        allocations==after && target->references==1);
    publish(&child,parent);
    assert(process_pid_active(&child)==target && process_identity(&child,KERNEL_PID_TID)->number==1);
    release(&child);
    {KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
     assert(!process_pid_children_install_locked(parent,&scheduler.identities));}
    process_identity_collect();
    kernel_fs_context_release(&parent->fs);
    kernel_namespace_put(parent->uts_namespace);kernel_namespace_put(parent->ipc_namespace);
    parent->uts_namespace=parent->ipc_namespace=NULL;parent->group_members=1;
    scheduler.initialized=0;
    assert(allocations==live && scheduler.identities.references==refs);
}
