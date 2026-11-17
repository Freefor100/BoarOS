#include "../../kernel/sched/private.h"
#include <kernel/errno.h>
#include <kernel/namespace.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

struct kernel_scheduler scheduler;
struct kernel_raw_lock kernel_wait_domain;
_Thread_local uintptr_t sync_test_irq;
static _Thread_local struct kernel_cpu cpu;
static unsigned allocations;
static unsigned fail_allocation;
void *sync_test_cpu(void) { return &cpu; }
void *sync_test_current_task(void) { return cpu.current; }
enum kernel_heap_status kernel_heap_allocate_zeroed(struct kernel_heap *heap,
    size_t count, size_t size, void **owner)
{
    (void)heap; assert(!cpu.raw_locks);
    if(fail_allocation && !--fail_allocation) return KERNEL_HEAP_STATUS_EMPTY;
    *owner = calloc(count, size); if (!*owner) return KERNEL_HEAP_STATUS_EMPTY;
    __atomic_fetch_add(&allocations,1,__ATOMIC_RELAXED); return KERNEL_HEAP_STATUS_OK;
}
void kernel_heap_release(struct kernel_heap *heap, void *owner)
{ (void)heap; assert(!cpu.raw_locks); assert(__atomic_fetch_sub(&allocations,1,__ATOMIC_RELAXED)); free(owner); }
void kernel_task_tty_clear(struct kernel_task *task)
{ assert(!cpu.raw_locks && !task->controlling_tty); }
static void publish(struct kernel_task *task, struct kernel_task *parent)
{
    task->group_leader = task; task->parent = parent;
    task->group_members=1;task->group_next=task->group_previous=task;
    task->magic=KERNEL_THREAD_MAGIC;task->arch.user_mode=1;
    assert(!process_identity_create(task, parent, 0));
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    assert(!process_identity_admit_locked(task));
    process_identity_commit_locked(task);
}
static void release(struct kernel_task *task)
{ process_identity_release(task); process_identity_collect(); }
static pthread_barrier_t snapshot_barrier;
static struct kernel_pid_registry *concurrent_space;
static struct kernel_namespace *shared_snapshot;
static void *snapshot_worker(void *argument)
{
    unsigned index=(unsigned)(uintptr_t)argument;
    kernel_cpu_initialize(&cpu,index,NULL);
    for(unsigned round=0;round<16;round++) {
        struct kernel_namespace *snapshot=NULL;
        assert(!process_pid_namespace_snapshot(concurrent_space,&scheduler.identity_heap,&snapshot));
        kernel_namespace_get(shared_snapshot);
        assert(snapshot->id==shared_snapshot->id);
        pthread_barrier_wait(&snapshot_barrier);
        if(!index) {
            KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
            assert(concurrent_space->references==6); /* Creation + shared + four snapshots. */
            assert(shared_snapshot->references==5);
        }
        pthread_barrier_wait(&snapshot_barrier);
        kernel_namespace_put(shared_snapshot);
        kernel_namespace_put(snapshot);
        pthread_barrier_wait(&snapshot_barrier);
    }
    return NULL;
}
static void snapshot_threads_case(void)
{
    unsigned baseline=allocations;
    uint32_t refs=scheduler.identities.references;
    assert(!process_pid_namespace_create(&scheduler.identities,&scheduler.identity_heap,&concurrent_space));
    assert(!process_pid_namespace_snapshot(concurrent_space,&scheduler.identity_heap,&shared_snapshot));
    pthread_t threads[4];
    assert(!pthread_barrier_init(&snapshot_barrier,NULL,4));
    for(unsigned i=0;i<4;i++)assert(!pthread_create(&threads[i],NULL,snapshot_worker,(void *)(uintptr_t)i));
    for(unsigned i=0;i<4;i++)assert(!pthread_join(threads[i],NULL));
    assert(!pthread_barrier_destroy(&snapshot_barrier));
    kernel_namespace_put(shared_snapshot);
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        assert(concurrent_space->references==1);
        kernel_pid_registry_put(concurrent_space);
    }
    process_identity_collect();
    assert(allocations==baseline && scheduler.identities.references==refs);
    assert(!scheduler.identities.retired && !scheduler.identities.retired_namespaces);
}

static unsigned cleanup_notifications;
enum kernel_scheduler_status kernel_wait_queue_wake_all(struct kernel_wait_queue *queue)
{
    assert(!cpu.raw_locks && queue==&scheduler.cleanup_queue);
    __atomic_fetch_add(&cleanup_notifications,1,__ATOMIC_RELAXED);
    return KERNEL_SCHEDULER_STATUS_OK;
}
static struct kernel_task *reservation_parent;
static pthread_barrier_t reservation_barrier;
static void *reservation_worker(void *unused)
{
    (void)unused;
    kernel_cpu_initialize(&cpu,1,NULL);
    struct kernel_task reserved={0};
    assert(!process_identity_create(&reserved,reservation_parent,0));
    pthread_barrier_wait(&reservation_barrier);
    pthread_barrier_wait(&reservation_barrier);
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        assert(process_identity_admit_locked(&reserved)==-KERNEL_ENOMEM);
    }
    release(&reserved);
    return NULL;
}
static void deferred_shutdown_case(struct kernel_task *parent)
{
    unsigned baseline=allocations,notifications=cleanup_notifications;
    uint32_t refs=scheduler.identities.references;
    struct kernel_pid_registry *space=NULL,*sibling=NULL;
    assert(!process_pid_namespace_create(&scheduler.identities,&scheduler.identity_heap,&space));
    assert(!process_pid_namespace_create(&scheduler.identities,&scheduler.identity_heap,&sibling));
    struct kernel_task init={0},inside={0},outside={0};
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        assert(!process_pid_children_install_locked(parent,space));
    }
    publish(&init,parent);publish(&inside,&init);
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        assert(!process_pid_children_install_locked(parent,sibling));
    }
    publish(&outside,parent);
    inside.accounted=outside.accounted=1;
    inside.state=outside.state=KERNEL_THREAD_STATE_READY;
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        kernel_pid_t after=1;
        assert(process_pid_namespace_kill_target_locked(space,&after)==&inside);
        assert(!process_pid_namespace_kill_target_locked(space,&after));
        inside.state=KERNEL_THREAD_STATE_ZOMBIE;inside.group_members=1;
        after=1;assert(!process_pid_namespace_kill_target_locked(space,&after));
        /* Leader exit precedes cleanup; its sibling can still run. The TGID
         * role continues to designate that EXITED leader until exec/adoption. */
        struct kernel_task member={.group_leader=&inside,.group_next=&inside,
            .group_previous=&inside,.accounted=1,.state=KERNEL_THREAD_STATE_READY};
        inside.group_next=inside.group_previous=&member;
        inside.group_members=2;inside.state=KERNEL_THREAD_STATE_EXITED;
        after=1;assert(process_pid_namespace_kill_target_locked(space,&after)==&inside);
        inside.group_exiting=1;
        after=1;assert(!process_pid_namespace_kill_target_locked(space,&after));
        inside.group_exiting=0;inside.state=KERNEL_THREAD_STATE_GROUP_DEAD;
        after=1;assert(process_pid_namespace_kill_target_locked(space,&after)==&inside);
        kernel_pid_transfer(&inside.identities[KERNEL_PID_TGID],
            &member.identities[KERNEL_PID_TGID],&member);
        member.group_leader=member.group_next=member.group_previous=&member;member.group_members=1;
        after=1;assert(process_pid_namespace_kill_target_locked(space,&after)==&member);
        kernel_pid_transfer(&member.identities[KERNEL_PID_TGID],
            &inside.identities[KERNEL_PID_TGID],&inside);
        inside.group_members=1;inside.group_next=inside.group_previous=&inside;
        after=1;assert(!process_pid_namespace_kill_target_locked(space,&after));
        inside.state=KERNEL_THREAD_STATE_EXITED;
        after=1;assert(!process_pid_namespace_kill_target_locked(space,&after));
        assert(!process_pid_children_install_locked(parent,&scheduler.identities));
    }
    release(&inside);release(&outside);
    reservation_parent=&init;
    pthread_t worker;
    assert(!pthread_barrier_init(&reservation_barrier,NULL,2));
    assert(!pthread_create(&worker,NULL,reservation_worker,NULL));
    pthread_barrier_wait(&reservation_barrier);
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        assert(space->numbers->allocated==2);
        kernel_pid_registry_disable(space);
        space->shutdown_reaper=&init;
        init.state=KERNEL_THREAD_STATE_EXITED;init.reaping=1;
        exited_append_locked(&init);
        assert(process_pid_namespace_defer_locked(&init));
        assert(!init.reaping && !init.exit_queued && scheduler.pid_shutdowns==space);
    }
    process_identity_collect();
    assert(!init.exit_queued && cleanup_notifications==notifications);
    pthread_barrier_wait(&reservation_barrier);
    assert(!pthread_join(worker,NULL));
    assert(!pthread_barrier_destroy(&reservation_barrier));
    assert(init.exit_queued && scheduler.exited_head==&init && !scheduler.pid_shutdowns);
    assert(cleanup_notifications==notifications+1);
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        exited_remove_locked(&init);
    }
    release(&init);
    assert(!space->child_reaper && !space->shutdown_reaper);
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        kernel_pid_registry_put(space);kernel_pid_registry_put(sibling);
    }
    process_identity_collect();
    assert(allocations==baseline && scheduler.identities.references==refs);
    assert(!scheduler.exited_head && !scheduler.exited_tail && !scheduler.pid_shutdowns);
    assert(!scheduler.identities.retired && !scheduler.identities.retired_namespaces);
}

static void heap_namespace_case(struct kernel_task *parent)
{
    unsigned baseline=allocations;
    uint32_t refs=scheduler.identities.references;
    struct kernel_pid_registry *space=NULL;
    fail_allocation=1;
    assert(process_pid_namespace_create(&scheduler.identities,&scheduler.identity_heap,&space)==-KERNEL_ENOMEM);
    assert(!space && allocations==baseline && scheduler.identities.references==refs);
    assert(!process_pid_namespace_create(&scheduler.identities,&scheduler.identity_heap,&space));
    struct kernel_namespace *first=NULL,*second=NULL;
    fail_allocation=1;
    assert(process_pid_namespace_snapshot(space,&scheduler.identity_heap,&first)==-KERNEL_ENOMEM);
    assert(!first && space->references==1);
    assert(!process_pid_namespace_snapshot(space,&scheduler.identity_heap,&first));
    assert(!process_pid_namespace_snapshot(space,&scheduler.identity_heap,&second));
    assert(first!=second && first->id==second->id && first->type==KERNEL_NS_PID);
    assert(process_pid_namespace_registry(first)==space && space->references==3);
    kernel_namespace_get(first);
    assert(first->references==2 && space->references==3);
    kernel_namespace_put(first);
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        assert(!process_pid_children_install_locked(parent,space));
        kernel_pid_registry_put(space); /* Creation owner; FD/snapshot plus task remain. */
    }
    struct kernel_task init={0},failed={0};
    publish(&init,parent);
    assert(space->child_reaper==process_identity(&init,KERNEL_PID_TGID));
    release(&init);
    assert(!space->child_reaper && !space->accepting && !space->numbers->allocated);
    assert(process_identity_create(&failed,parent,0)==KERNEL_PID_STATUS_NAMESPACE_DEAD);
    kernel_namespace_put(first);
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        assert(!process_pid_children_install_locked(parent,&scheduler.identities));
        assert(space->references==1); /* Last independent snapshot keeps dead space alive. */
    }
    assert(process_pid_namespace_registry(second)==space);
    kernel_namespace_put(second);
    assert(allocations==baseline && scheduler.identities.references==refs);
    assert(!scheduler.identities.retired && !scheduler.identities.retired_namespaces);
}

int main(void)
{
    kernel_cpu_initialize(&cpu, 0, NULL);
    kernel_raw_lock_init(&kernel_wait_domain, KERNEL_RAW_RANK_SCHEDULER);
    uint64_t root_bits[1], child_bits[1], sibling_bits[1];
    struct kernel_pid_allocator child_numbers = {0}, sibling_numbers = {0};
    struct kernel_pid_registry child = {0}, sibling = {0};
    assert(!kernel_pid_allocator_init(&scheduler.pid_allocator, root_bits, 64));
    assert(!kernel_pid_registry_init(&scheduler.identities, &scheduler.pid_allocator, NULL));
    assert(!kernel_pid_allocator_init(&child_numbers, child_bits, 64));
    assert(!kernel_pid_registry_init(&child, &child_numbers, &scheduler.identities));
    assert(!kernel_pid_allocator_init(&sibling_numbers, sibling_bits, 64));
    assert(!kernel_pid_registry_init(&sibling, &sibling_numbers, &scheduler.identities));
    struct kernel_task init = {0}, child_init = {0}, ordinary = {0}, failed = {0};
    publish(&init, NULL); cpu.current = &init;
    assert(init.signal_unkillable);
    assert(init.tid == 1 && process_pid_active(&init) == &scheduler.identities);
    /* NEWPID owns its destination; failure must not install a for-children
     * override or leave an ancestor reservation behind. */
    {
        struct kernel_task fresh = {0};
        kernel_pid_t ids[] = {1, 41};
        unsigned refs = scheduler.identities.references, live = allocations;
        for (unsigned fail = 1; fail <= 2; ++fail) {
            fail_allocation = fail;
            assert(process_identity_create_new_namespace(&fresh, &init,
                ids, 2, 1) == KERNEL_PID_STATUS_NO_MEMORY);
            assert(allocations == live && scheduler.identities.references == refs);
        }
        ids[1] = 1;
        assert(process_identity_create_new_namespace(&fresh, &init,
            ids, 2, 1) == KERNEL_PID_STATUS_EXISTS);
        assert(allocations == live && scheduler.identities.references == refs);
        ids[1] = 41;
        assert(!process_identity_create_new_namespace(&fresh, &init, ids, 2, 1));
        struct kernel_pid_registry *space = process_pid_active(&fresh);
        assert(space->parent == process_pid_active(&init) && fresh.tid == 41);
        {
            KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
            assert(process_pid_children_locked(&init) == &scheduler.identities);
            assert(kernel_pid_find(&scheduler.identities, 41) == NULL);
            assert(!process_identity_admit_locked(&fresh));
            kernel_pid_registry_disable(space);
            assert(process_identity_admit_locked(&fresh) == -KERNEL_ENOMEM);
        }
        release(&fresh);
        assert(allocations == live && scheduler.identities.references == refs);
        assert(scheduler.pid_allocator.allocated == 1);
        assert(process_identity_create_new_namespace(&fresh, &init, ids, 2, 0) ==
               KERNEL_PID_STATUS_PERMISSION);
        assert(allocations == live && scheduler.identities.references == refs);
        fresh.group_leader = &fresh; fresh.parent = &init;
        assert(!process_identity_create_new_namespace(&fresh, &init, ids, 2, 1));
        {
            KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
            assert(!process_identity_admit_locked(&fresh));
            process_identity_commit_locked(&fresh);
            assert(process_pid_reaper_locked(process_pid_active(&fresh)) == &fresh);
            assert(process_pid_children_locked(&fresh) == process_pid_active(&fresh));
        }
        struct kernel_task nested = {0};
        assert(!process_identity_create_new_namespace(&nested, &fresh, NULL, 0, 0));
        {
            KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
            kernel_pid_registry_disable(process_pid_active(&fresh));
            assert(process_identity_admit_locked(&nested) == -KERNEL_ENOMEM);
        }
        release(&nested); release(&fresh);
        assert(allocations == live && scheduler.identities.references == refs);
    }
    {
        struct kernel_task *chain = calloc(KERNEL_PID_NAMESPACE_MAX_DEPTH, sizeof(*chain));
        assert(chain);
        struct kernel_task *parent = &init;
        unsigned live = allocations, refs = scheduler.identities.references;
        for (unsigned depth = 0; depth < KERNEL_PID_NAMESPACE_MAX_DEPTH; ++depth) {
            struct kernel_task *task = &chain[depth];
            task->parent = parent; task->group_leader = task;
            assert(!process_identity_create_new_namespace(task, parent, NULL, 0, 0));
            KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
            assert(!process_identity_admit_locked(task));
            process_identity_commit_locked(task);
            parent = task;
        }
        assert(process_identity_create_new_namespace(&failed, parent, NULL, 0, 0) ==
               KERNEL_PID_STATUS_DEPTH_LIMIT);
        for (unsigned n = KERNEL_PID_NAMESPACE_MAX_DEPTH; n; --n) release(&chain[n-1]);
        free(chain);
        assert(allocations == live && scheduler.identities.references == refs);
    }
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        assert(process_pid_reaper_locked(&scheduler.identities) == &init);
        assert(!process_pid_children_install_locked(&init, &child));
        assert(process_pid_active(&init) == &scheduler.identities);
        assert(process_pid_children_locked(&init) == &child);
    }
    assert(process_identity_create_new_namespace(&failed, &init, NULL, 0, 0) == KERNEL_PID_STATUS_INVALID);
    /* A new namespace cannot begin with a requested non-init TID. Failure must
     * release the prepared heap object and the temporary namespace reference. */
    assert(process_identity_create_numbers(&failed, &init, 0, (kernel_pid_t[]){2}, 1, 1) == KERNEL_PID_STATUS_INVALID);
    assert(!child_numbers.allocated && allocations == 1);
    assert(process_identity_create(&failed, &init, 1) == KERNEL_PID_STATUS_INVALID);
    kernel_pid_t conflict[] = {1, 1}, requested[] = {1, 23};
    fail_allocation = 1;
    assert(process_identity_create(&failed, &init, 0) == KERNEL_PID_STATUS_NO_MEMORY);
    kernel_pid_t outside[] = {1, 65}, invalid_inner[] = {65, 23};
    assert(process_identity_create_numbers(&failed,&init,0,outside,2,0)==KERNEL_PID_STATUS_PERMISSION);
    assert(process_identity_create_numbers(&failed,&init,0,invalid_inner,2,0)==KERNEL_PID_STATUS_INVALID);
    assert(process_identity_create_numbers(&failed,&init,0,outside,2,1)==KERNEL_PID_STATUS_INVALID);
    assert(process_identity_create_numbers(&failed, &init, 0, conflict, 2, 1) == KERNEL_PID_STATUS_EXISTS);
    assert(!child_numbers.allocated && allocations == 1);
    child_init.group_leader = &child_init; child_init.parent = &init;
    child_init.group_members=1;child_init.magic=KERNEL_THREAD_MAGIC;child_init.arch.user_mode=1;
    assert(!process_identity_create_numbers(&child_init, &init, 0, requested, 2, 1));
    /* All requested ranges precede the first allocation: the already held
     * local 1 must not hide the out-of-range outer request. */
    assert(process_identity_create_numbers(&failed,&init,0,outside,2,1)==KERNEL_PID_STATUS_INVALID);
    struct kernel_task before_init = {0};
    assert(process_identity_create(&before_init, &init, 0) == KERNEL_PID_STATUS_INVALID);
    assert(child_numbers.allocated == 1 && scheduler.pid_allocator.allocated == 2);
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        assert(!process_identity_admit_locked(&child_init));
        process_identity_commit_locked(&child_init);
    }
    struct kernel_pid *id = process_identity(&child_init, KERNEL_PID_TID);
    assert(child_init.tid == 23 && id->number == 1 && child_init.signal_unkillable);
    assert(process_pid_active(&child_init) == &child && child.child_reaper == id);
    kernel_pid_t visible,internal;
    cpu.current=&child_init;
    assert(!kernel_task_user_tid(&child_init,&visible) && visible==1);
    assert(!kernel_task_tid(&child_init,&internal) && internal==23);
    assert(!kernel_task_user_tgid(&child_init,&visible) && visible==1);
    assert(!kernel_task_tgid(&child_init,&internal) && internal==23);
    assert(!kernel_task_ppid(&child_init,&visible) && visible==0);
    assert(!kernel_task_set_tid_address(&child_init,0x1234,&visible) && visible==1);
    assert(child_init.clear_tid_address==0x1234);
    cpu.current=&init;
    assert(!kernel_task_user_tid(&child_init,&visible) && visible==1);
    assert(!kernel_task_user_tgid(&child_init,&visible) && visible==1);
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        assert(process_find_identity(1,KERNEL_PID_TID)==&init);
        assert(process_find_identity_in(1,KERNEL_PID_TID,&child)==&child_init);
        assert(!process_find_identity_in(23,KERNEL_PID_TID,&child));
        assert(kernel_pid_number_in(id,process_pid_active(&init))==23);
    }
    /* A third level accepts complete or partial inner-first requests. Errors
     * in an outer level must not consume the leaf's unpublished init slot. */
    {
        struct kernel_pid_registry *leaf = NULL;
        struct kernel_task grand = {0}, other = {0};
        assert(!process_pid_namespace_create(&child, &scheduler.identity_heap, &leaf));
        {
            KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
            assert(!process_pid_children_install_locked(&child_init, leaf));
        }
        kernel_pid_t vector[] = {1, 17, 23};
        assert(process_identity_create_numbers(&grand,&child_init,0,vector,3,1)==KERNEL_PID_STATUS_EXISTS);
        assert(!leaf->numbers->allocated && child.numbers->allocated==1);
        vector[2]=45;
        grand.group_leader=&grand; grand.parent=&child_init;
        assert(!process_identity_create_numbers(&grand,&child_init,0,vector,3,1));
        {
            KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
            assert(!process_identity_admit_locked(&grand));
            process_identity_commit_locked(&grand);
        }
        assert(grand.tid==45 && process_identity(&grand,KERNEL_PID_TID)->number==1);
        assert(kernel_pid_number_in(process_identity(&grand,KERNEL_PID_TID),&child)==17);
        vector[0]=9;
        assert(!process_identity_create_numbers(&other,&grand,0,vector,1,1));
        assert(process_identity(&other,KERNEL_PID_TID)->number==9 && other.tid>0);
        release(&other); release(&grand);
        {
            KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
            assert(!process_pid_children_install_locked(&child_init,&child));
            kernel_pid_registry_put(leaf);
        }
        process_identity_collect();
    }
    struct kernel_pid *outer_group = process_identity(&init, KERNEL_PID_PGID);
    struct kernel_pid *outer_session = process_identity(&init, KERNEL_PID_SID);
    assert(process_identity(&child_init, KERNEL_PID_PGID) == outer_group);
    assert(process_identity(&child_init, KERNEL_PID_SID) == outer_session);
    assert(!kernel_pid_number_in(outer_group, &child));
    assert(!kernel_pid_number_in(outer_session, &child));
    assert(!child_init.session_leader);
    child_init.accounted=init.accounted=1;
    assert(kernel_task_getpgid(&child_init,0)==0 && kernel_task_getsid(&child_init,1)==0);
    assert(kernel_task_getpgid(&child_init,23)==-KERNEL_ESRCH);
    assert(kernel_task_getpgid(&init,23)==1 && kernel_task_getsid(&init,23)==1);

    assert(kernel_task_setsid(&child_init) == 1);
    assert(kernel_task_getpgid(&init,23)==23 && kernel_task_getsid(&init,23)==23);
    assert(kernel_task_getpgid(&child_init,0)==1 && kernel_task_getsid(&child_init,0)==1);
    assert(process_identity(&child_init, KERNEL_PID_PGID) == id);
    assert(process_identity(&child_init, KERNEL_PID_SID) == id);
    assert(kernel_pid_number_in(id, &child) == 1);
    assert(kernel_task_setsid(&child_init) == -KERNEL_EPERM);
    /* Entering an existing child namespace affects PID visibility, not the
     * real parent's process group/session, even after its init ran setsid. */
    struct kernel_task entered = {0}, inside = {0};
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        assert(!process_pid_children_install_locked(&init, &scheduler.identities));
        assert(!process_pid_children_install_locked(&init, &child));
        assert(process_pid_active(&init) == &scheduler.identities);
    }
    publish(&entered, &init);
    assert(process_pid_active(&entered) == &child);
    assert(process_identity(&entered, KERNEL_PID_PGID) == outer_group);
    assert(process_identity(&entered, KERNEL_PID_SID) == outer_session);
    assert(!kernel_pid_number_in(process_identity(&entered, KERNEL_PID_PGID), &child));
    assert(!kernel_pid_number_in(process_identity(&entered, KERNEL_PID_SID), &child));
    publish(&inside, &child_init);
    cpu.current=&inside;
    assert(!kernel_task_ppid(&inside,&visible) && visible==1);
    cpu.current=&init;
    assert(process_identity(&inside, KERNEL_PID_PGID) == id);
    assert(process_identity(&inside, KERNEL_PID_SID) == id);
    entered.accounted=inside.accounted=1;inside.fork_no_exec=1;
    kernel_pid_t local=kernel_pid_number_in(process_identity(&inside,KERNEL_PID_TGID),&child);
    assert(kernel_task_getpgid(&inside,0)==1 && kernel_task_getsid(&inside,0)==1);
    assert(!kernel_task_setpgid(&child_init,local,0));
    assert(kernel_task_getpgid(&inside,0)==local);
    assert(kernel_task_setsid(&inside)==-KERNEL_EPERM);
    assert(!kernel_task_setpgid(&inside,0,1));
    assert(kernel_task_getpgid(&inside,0)==1);
    assert(kernel_task_setpgid(&inside,0,kernel_pid_number_in(process_identity(&entered,KERNEL_PID_TGID),&child))==-KERNEL_EPERM);
    assert(kernel_task_getpgid(&entered,0)==0 && kernel_task_getsid(&entered,0)==0);
    entered.accounted=inside.accounted=child_init.accounted=init.accounted=0;

    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        assert(process_pid_children_locked(&child_init) == &child);
        assert(process_pid_children_install_locked(&child_init, &scheduler.identities) == -KERNEL_EINVAL);
        assert(process_pid_children_install_locked(&child_init, &sibling) == -KERNEL_EINVAL);
        assert(!child_init.pid_for_children);
        assert(!process_pid_children_install_locked(&init, &scheduler.identities));
    }
    publish(&ordinary, &init);
    assert(process_pid_active(&ordinary) == &scheduler.identities);
    assert(!process_identity_create(&failed, &child_init, 0));
    kernel_pid_t failed_root = failed.tid;
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        process_pid_reaper_disable_locked(&ordinary);
        assert(scheduler.identities.accepting);
        process_pid_reaper_disable_locked(&child_init);
        assert(process_identity_admit_locked(&failed) == -KERNEL_ENOMEM);
        assert(!kernel_pid_find(&scheduler.identities, failed_root));
        assert(!failed.accounted && !failed.parent && !failed.cgroup && !failed.ptrace);
        assert(!failed.identities[KERNEL_PID_PGID].identity && !failed.identities[KERNEL_PID_SID].identity);
        assert(process_pid_reaper_locked(&child) == &child_init);
    }
    release(&failed);
    failed = (struct kernel_task){0};
    assert(process_identity_create(&failed, &child_init, 0) == KERNEL_PID_STATUS_NAMESPACE_DEAD);
    release(&inside); release(&entered); release(&child_init);
    assert(!child.child_reaper && !child.accepting && !child_numbers.allocated);
    release(&ordinary);
    heap_namespace_case(&init);
    deferred_shutdown_case(&init);
    snapshot_threads_case();
    release(&init);
    assert(!scheduler.identities.child_reaper && !allocations);
    assert(child.references == 1 && sibling.references == 1 && scheduler.identities.references == 3);
    kernel_pid_registry_put(&child); kernel_pid_registry_put(&sibling);
    assert(kernel_pid_take_retired_namespace(&scheduler.identities) == &sibling);
    assert(kernel_pid_take_retired_namespace(&scheduler.identities) == &child);
    assert(!scheduler.identities.retired_namespaces && !scheduler.identities.retired);
    assert(scheduler.identities.references == 1);
    kernel_pid_registry_put(&scheduler.identities);
    puts("PID lifecycle active/children init identity and rejected publication PASS");
}
