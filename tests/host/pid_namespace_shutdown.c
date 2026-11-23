/* Real namespace shutdown scan, group termination, admission and collector.
 * Task resource cleanup is modeled explicitly; this is not a guest reaper. */
#include "../../kernel/sched/process.c"
#include <kernel/namespace.h>
#include <kernel/errno.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

extern void signal_owner_bind_cpu(struct kernel_cpu *);
extern void signal_owner_heap_empty(void);
extern void pid_namespace_ptrace_cases(struct kernel_task *,struct kernel_task *,struct kernel_task *,struct kernel_task *,struct kernel_task *);
uint64_t kernel_mm_peak_resident_pages(const struct kernel_mm *mm)
{ assert(mm->state==KERNEL_MM_RELEASED);return 0; }
unsigned kernel_stack_cache_reclaim(struct physical_page_allocator *allocator)
{
    assert(!allocator);
    return 0;
}
struct kernel_task *pid_namespace_disposal_target;
extern void pid_namespace_ptrace_final_link(struct kernel_task *,struct kernel_task *);
static struct kernel_cpu main_cpu, worker_cpu;
static pthread_barrier_t barrier;
static struct kernel_task *reservation_parent;

static void create(struct kernel_task *task, struct kernel_task *parent,
                   struct kernel_signal_table *table)
{
    *task=(struct kernel_task){.magic=KERNEL_THREAD_MAGIC,.accounted=1,
        .state=KERNEL_THREAD_STATE_READY,.parent=parent,.group_members=1};
    task->group_leader=task->group_next=task->group_previous=task;
    task->arch.user_mode=1;
    task->limits=kernel_resource_limits_create(&scheduler.identity_heap,NULL);
    assert(task->limits);
    kernel_wait_queue_init(&task->group_wait_queue);
    kernel_wait_queue_init(&task->child_exit_queue);
    memset(table,0,sizeof(*table));
    table->magic=KERNEL_SIGNAL_TABLE_MAGIC;table->references=1;
    kernel_wait_queue_init(&table->signalfd_waiters);
    task->signal_table_address=(uintptr_t)table;
    assert(!process_identity_create(task,parent,0));
    KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
    assert(!process_identity_admit_locked(task));
    process_identity_commit_locked(task);
}
static void cleaned_child(struct kernel_task *task,struct kernel_task *parent,uint64_t physical)
{
    *task=(struct kernel_task){.magic=KERNEL_THREAD_MAGIC,.parent=parent,
        .state=KERNEL_THREAD_STATE_ZOMBIE,.group_members=1,
        .stack_physical_address=KERNEL_THREAD_NO_PAGE,.physical_address=physical};
    task->arch.user_mode=1;task->mm.state=KERNEL_MM_RELEASED;
    task->group_leader=task->group_next=task->group_previous=task;
    task->limits=kernel_resource_limits_create(&scheduler.identity_heap,NULL);
    assert(task->limits);
    assert(!process_identity_create(task,parent,0));
    KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
    assert(!process_identity_admit_locked(task));scheduler_task_publish_locked(task);
    task->parent=NULL;child_append_locked(parent,task);
}
static void children(struct kernel_task *task,struct kernel_pid_registry *space)
{
    KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
    assert(!process_pid_children_install_locked(task,space));
}
static void release(struct kernel_task *task)
{
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        assert(!task->storage_borrows);
        if(task->exit_queued)exited_remove_locked(task);
        if(task->pid_for_children) {
            kernel_pid_registry_put(task->pid_for_children);task->pid_for_children=NULL;
        }
        process_identity_release_locked(task);
    }
    process_identity_collect();
    kernel_resource_limits_put(task->limits);
    task->limits=NULL;
}
static int drain(struct kernel_task *task)
{
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        task->state=KERNEL_THREAD_STATE_EXITED;task->reaping=1;
        if(!task->exit_queued)exited_append_locked(task);
    }
    return process_pid_namespace_drain(task);
}
static void *reserve_during_shutdown(void *unused)
{
    (void)unused;
    kernel_cpu_initialize(&worker_cpu,1,NULL);signal_owner_bind_cpu(&worker_cpu);
    struct kernel_task reserved={0};
    assert(!process_identity_create(&reserved,reservation_parent,0));
    pthread_barrier_wait(&barrier);
    pthread_barrier_wait(&barrier);
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        assert(process_identity_admit_locked(&reserved)==-KERNEL_ENOMEM);
    }
    release(&reserved);
    return NULL;
}

static void namespace_shutdown_case(enum kernel_reboot_action reboot,int init_caller)
{
    signal_owner_heap_empty();
    scheduler=(struct kernel_scheduler){0};
    kernel_cpu_initialize(&main_cpu,0,NULL);signal_owner_bind_cpu(&main_cpu);
    kernel_raw_lock_init(&kernel_wait_domain,KERNEL_RAW_RANK_SCHEDULER);
    kernel_wait_queue_init(&scheduler.cleanup_queue);
    uint64_t bitmap[KERNEL_PID_BITMAP_WORDS(128)]={0};
    struct kernel_pid_allocator numbers={0};
    assert(!kernel_pid_allocator_init(&numbers,bitmap,128));
    assert(!kernel_pid_registry_init(&scheduler.identities,&numbers,NULL));
    struct kernel_pid_registry *inner=NULL,*nested=NULL,*sibling=NULL;
    assert(!process_pid_namespace_create(&scheduler.identities,&scheduler.identity_heap,&inner));
    assert(!process_pid_namespace_create(inner,&scheduler.identity_heap,&nested));
    assert(!process_pid_namespace_create(&scheduler.identities,&scheduler.identity_heap,&sibling));
    struct kernel_task root,a,b,c,thread,foreign,peer,leaf,zombie,traced;
    struct kernel_signal_table tables[7];
    create(&root,NULL,&tables[0]);
    children(&root,inner);create(&a,&root,&tables[1]);
    children(&a,nested);create(&b,&a,&tables[2]);
    children(&a,inner);create(&c,&a,&tables[3]);
    create(&leaf,&b,&tables[4]);
    create(&foreign,&root,&tables[5]);foreign.state=KERNEL_THREAD_STATE_ZOMBIE;
    children(&root,sibling);create(&peer,&root,&tables[6]);
    children(&root,&scheduler.identities);
    thread=(struct kernel_task){.magic=KERNEL_THREAD_MAGIC,.accounted=1,
        .state=KERNEL_THREAD_STATE_READY,.group_leader=&c,.group_next=&c,.group_previous=&c};
    thread.limits=kernel_resource_limits_get(c.limits);
    assert(!process_identity_create(&thread,&c,1));
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        assert(!process_identity_admit_locked(&thread));process_identity_commit_locked(&thread);
        c.group_members=2;c.group_next=c.group_previous=&thread;
    }
    /* An already-cleaned zombie goes through the actual wait claim, identity
     * release and scheduler retirement transaction, not our release model. */
    cleaned_child(&zombie,&a,0xabc000);
    /* Real lifetime lookup and scheduler query boundaries: equal local PID 1
     * names different tasks; root-number guesses do not escape a view. */
    struct kernel_task *borrow=kernel_task_find_user_tid(&a,1);
    assert(borrow==&a);kernel_task_lookup_put(borrow);
    borrow=kernel_task_find_user_tid(&peer,1);
    assert(borrow==&peer);kernel_task_lookup_put(borrow);
    borrow=kernel_task_find_user_tid(&b,1);
    assert(borrow==&b);kernel_task_lookup_put(borrow);
    assert(!kernel_task_find_user_tid(&peer,a.tid));
    borrow=kernel_task_find_by_tid(a.tid);assert(borrow==&a);kernel_task_lookup_put(borrow);
    a.scheduling.nice=4;peer.scheduling.nice=9;b.scheduling.nice=7;
    struct kernel_sched_policy policy;
    assert(!kernel_task_sched_get(&a,1,&policy) && policy.nice==4);
    assert(!kernel_task_sched_get(&peer,1,&policy) && policy.nice==9);
    assert(!kernel_task_sched_get(&b,1,&policy) && policy.nice==7);
    assert(kernel_task_sched_get(&peer,a.tid,&policy)==-KERNEL_ESRCH);
    /* USER filters PID visibility, but inherited canonical PGID=0 does not.
     * Publish a controlled traversal without changing the shutdown fixture's
     * existing scheduler ownership list. */
    struct kernel_task *saved_all=scheduler.all_tasks;
    struct kernel_task *priority_tasks[]={&root,&a,&b,&peer};
    struct kernel_task *saved_next[4];
    struct kernel_credentials priority_creds={.uid=123,.euid=123};
    for(unsigned i=0;i<4;i++) {
        saved_next[i]=priority_tasks[i]->all_next;
        priority_tasks[i]->all_next=i<3?priority_tasks[i+1]:NULL;
        priority_tasks[i]->credentials=&priority_creds;
    }
    scheduler.all_tasks=&root;root.scheduling.nice=-10;
    assert(kernel_task_priority(&a,0,2,123,0)==16); /* a + descendant b */
    assert(kernel_task_priority(&peer,0,2,123,0)==11); /* sibling only */
    assert(kernel_task_priority(&root,0,2,123,0)==30);
    assert(kernel_task_priority(&a,0,1,0,0)==30); /* actual outer group */
    assert(kernel_task_priority(&a,0,1,1,0)==-KERNEL_ESRCH); /* local init is not PGID */
    assert(kernel_task_priority(&peer,0,0,a.tid,0)==-KERNEL_ESRCH);
    for(unsigned i=0;i<4;i++) {
        priority_tasks[i]->all_next=saved_next[i];priority_tasks[i]->credentials=NULL;
    }
    scheduler.all_tasks=saved_all;
    uint64_t affinity;
    assert(!kernel_task_affinity_get(&a,1,&affinity) && affinity==1);
    assert(kernel_task_affinity_get(&peer,a.tid,&affinity)==-KERNEL_ESRCH);
    assert(!kernel_task_affinity_set(&a,1,1));
    assert(kernel_task_affinity_set(&peer,a.tid,0)==-KERNEL_ESRCH);
    assert(kernel_task_sched_get(&peer,-1,&policy)==-KERNEL_EINVAL);
    assert(kernel_task_affinity_get(&peer,-1,&affinity)==-KERNEL_ESRCH);
    puts("PASS resource caller-view lifetime lookup and scheduler sibling isolation");
    pid_namespace_ptrace_cases(&root,&a,&b,&peer,&c);
    cleaned_child(&traced,&a,0xabd000);
    pid_namespace_ptrace_final_link(&root,&traced);
    c.state=KERNEL_THREAD_STATE_EXITED; /* Old leader still owns the TGID. */
    struct kernel_namespace *snapshot=NULL;
    assert(!process_pid_namespace_snapshot(nested,&scheduler.identity_heap,&snapshot));
    reservation_parent=&b;
    pthread_t worker;
    assert(!pthread_barrier_init(&barrier,NULL,2));
    assert(!pthread_create(&worker,NULL,reserve_during_shutdown,NULL));
    pthread_barrier_wait(&barrier);
    assert(!kernel_task_reboot_namespace(&root,reboot));
    assert(kernel_task_reboot_namespace(init_caller?&a:&thread,reboot)==1);
    assert(inner->reboot_signal==(reboot==KERNEL_REBOOT_RESTART?1U:2U));
    assert(a.completion.reason==KERNEL_THREAD_EXIT_SIGNAL&&a.completion.status==9);
    assert(kernel_reboot_action_requested()==KERNEL_REBOOT_NONE);
    assert(!inner->accepting && nested->accepting);
    struct kernel_task failed={0};
    assert(process_identity_create(&failed,&b,0)==KERNEL_PID_STATUS_NAMESPACE_DEAD);
    pid_namespace_disposal_target=&zombie;
    assert(drain(&a));
    assert(!pid_namespace_disposal_target);
    assert(a.first_child==&traced && a.last_child==&traced && !zombie.identities[KERNEL_PID_TID].identity);
    assert(zombie.retirement==3 && !zombie.storage_borrows && !zombie.reaping);
    assert(!scheduler.retired_tasks && !scheduler.retired_pending);
    assert(scheduler.task_count==1 && scheduler.all_tasks==&traced);
    assert(b.group_exiting && c.group_exiting && leaf.group_exiting);
    assert(b.terminate_requested && thread.terminate_requested && leaf.terminate_requested);
    assert(!c.terminate_requested && !foreign.terminate_requested);
    assert(!root.group_exiting && !peer.group_exiting && sibling->accepting);
    assert(!nested->accepting && tables[1].actions[16].handler==KERNEL_SIGNAL_IGN);
    assert(!a.storage_borrows && !b.storage_borrows && !c.storage_borrows && !thread.storage_borrows);
    assert(process_identity_create(&failed,&a,0)==KERNEL_PID_STATUS_NAMESPACE_DEAD);
    assert(process_identity_create(&failed,&b,0)==KERNEL_PID_STATUS_NAMESPACE_DEAD);
    /* The production syscall adapter maps this status to ENOMEM. */
    assert(drain(&b));
    release(&leaf);release(&thread);
    c.group_members=1;c.group_next=c.group_previous=&c;release(&c);
    assert(!a.exit_queued && !b.exit_queued); /* Reservation still owns a number. */
    pthread_barrier_wait(&barrier);
    assert(!pthread_join(worker,NULL));assert(!pthread_barrier_destroy(&barrier));
    assert(b.exit_queued && !a.exit_queued);
    assert(!drain(&b));
    release(&b);
    assert(!a.exit_queued && inner->numbers->allocated==3);
    /* A legitimate outside tracer retains FINAL wait ownership. Consume it
     * through the real trace wait and autoreap-to-cleanup handoff. */
    struct kernel_task *selected=NULL;
    uint32_t event=0;int matching=0;
    assert(select_wait_child(&root,LINUX_P_ALL,NULL,LINUX_WEXITED|LINUX___WALL,
        &selected,&event,&matching)==KERNEL_SCHEDULER_STATUS_OK);
    assert(selected==&traced && event==TRACE_WAIT_FINAL && matching);
    assert(!finish_traced_wait(&root,&traced));
    wait_claim_end(&selected);scheduler_task_put(selected);
    assert(!traced.parent && traced.exit_queued && traced.state==KERNEL_THREAD_STATE_EXITED);
    assert(!traced.ptrace->tracer && !traced.reaping && !traced.storage_borrows);
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        exited_remove_locked(&traced);
        struct kernel_wait_cursor unused;
        assert(!scheduler_forget_task_locked(&traced,&unused));
        scheduler_task_retire_locked(&traced);
    }
    process_identity_collect();
    pid_namespace_disposal_target=&traced;scheduler_task_collect();
    assert(!pid_namespace_disposal_target && !scheduler.retired_pending);
    assert(!scheduler.task_count && !scheduler.all_tasks);
    assert(!a.exit_queued && inner->numbers->allocated==2);
    kernel_ptrace_dispose(&root);root.ptrace=NULL;
    assert(foreign.parent==&root && foreign.state==KERNEL_THREAD_STATE_ZOMBIE);
    /* A real outside parent retains wait ownership; only its eventual reap
     * can make the outer namespace's deferred init ready. */
    release(&foreign);
    assert(a.exit_queued && !scheduler.pid_shutdowns);
    assert(!drain(&a));
    assert(a.completion.reason==KERNEL_THREAD_EXIT_SIGNAL&&a.completion.status==inner->reboot_signal);
    assert(a.wait_status==inner->reboot_signal&&a.wait_status==user_wait_status(&a.completion));
    release(&a);
    assert(!inner->child_reaper && !nested->child_reaper);
    children(&root,nested);
    assert(process_identity_create(&failed,&root,0)==KERNEL_PID_STATUS_NAMESPACE_DEAD);
    children(&root,&scheduler.identities);
    assert(process_pid_namespace_registry(snapshot)==nested && !nested->numbers->allocated);
    kernel_namespace_put(snapshot);
    release(&peer);release(&root);
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        kernel_pid_registry_put(nested);kernel_pid_registry_put(inner);kernel_pid_registry_put(sibling);
    }
    process_identity_collect();
    assert(!numbers.allocated && scheduler.identities.references==1);
    assert(!scheduler.identities.retired && !scheduler.identities.retired_namespaces);
    assert(!scheduler.exited_head && !scheduler.exited_tail && !scheduler.pid_shutdowns);
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        kernel_pid_registry_put(&scheduler.identities);
        scheduler.identities=(struct kernel_pid_registry){0};
    }
    signal_owner_heap_empty();
    puts("PID shutdown: production nested drain/exit requests, concurrent admission ENOMEM, zombie retirement and dead namespace FD ownership passed");
}

void pid_namespace_shutdown_cases(void)
{
    namespace_shutdown_case(KERNEL_REBOOT_RESTART,0);
    namespace_shutdown_case(KERNEL_REBOOT_HALT,1);
    namespace_shutdown_case(KERNEL_REBOOT_POWER_OFF,0);
}
