/* Exercise the production child-link edits without exposing private APIs. */
#define kernel_raw_lock_acquire child_test_lock_acquire
#include "../../kernel/sched/process.c"
#undef kernel_raw_lock_acquire
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

struct kernel_scheduler scheduler;
struct kernel_raw_lock kernel_wait_domain;
static struct kernel_cpu cpu;
static unsigned child_domain_acquires;
_Thread_local uintptr_t sync_test_irq;
void *sync_test_cpu(void) { return &cpu; }
void *sync_test_current_task(void) { return cpu.current; }
void kernel_raw_lock_acquire(struct kernel_raw_lock *, struct kernel_raw_guard *);
void child_test_lock_acquire(struct kernel_raw_lock *lock, struct kernel_raw_guard *guard)
{
    if (lock == &kernel_wait_domain) child_domain_acquires++;
    kernel_raw_lock_acquire(lock, guard);
}

void scheduler_task_get_locked(struct kernel_task *task) { task->storage_borrows++; }
void scheduler_task_put(struct kernel_task *task)
{ assert(task->storage_borrows); task->storage_borrows--; }
uint32_t kernel_ptrace_tracer_tid_locked(const struct kernel_task *task)
{ assert(!task->ptrace); return 0; }
int ptrace_select(struct kernel_task *task, int which, const struct kernel_pid *target,
    unsigned options, struct kernel_task **selected, uint32_t *event, int *matching)
{
    (void)which; (void)target; (void)options; (void)selected; (void)event; (void)matching;
    assert(!task->ptrace);
    return 0;
}
int ptrace_select_target_locked(struct kernel_task *parent, struct kernel_task *task,
    unsigned options, struct kernel_task **selected, uint32_t *event, int *matching)
{
    (void)options; (void)selected; (void)event; (void)matching;
    assert(!parent->ptrace && !task->ptrace);
    return 0;
}

/* Pause the production report at its first potentially sleeping user copy. */
static struct kernel_task *copy_transfer_from, *copy_transfer_to;
static unsigned report_copies;
enum kernel_uaccess_status kernel_copy_to_user(struct kernel_mm *mm,
    uint64_t destination, const void *source, size_t size, size_t *copied)
{
    (void)mm;
    if (copy_transfer_from) {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        kernel_pid_transfer(&copy_transfer_from->identities[KERNEL_PID_TID],
                            &copy_transfer_to->identities[KERNEL_PID_TID], copy_transfer_to);
        copy_transfer_from = NULL;
    }
    memcpy((void *)(uintptr_t)destination, source, size);
    *copied = size;
    report_copies++;
    return KERNEL_UACCESS_STATUS_OK;
}
void scheduler_usage_snapshot_mm(const struct kernel_task *task, int who,
                                 struct kernel_task_usage *usage)
{ (void)task; (void)who; memset(usage, 0, sizeof(*usage)); }
void scheduler_usage_format(const struct kernel_task_usage *usage,
                            struct kernel_linux_rusage *rusage)
{ (void)usage; memset(rusage, 0, sizeof(*rusage)); }

/* Classification fixture: model traversal only, retaining production orphan
 * predicates and namespace/role selection. No retirement runs in these cases. */
void scheduler_task_cursor_begin(struct scheduler_task_cursor *c){c->task=scheduler.all_tasks;}
void scheduler_task_cursor_next(struct scheduler_task_cursor *c){c->task=c->task->all_next;}
void scheduler_task_cursor_end(struct scheduler_task_cursor *c){c->task=NULL;}
static void orphan_classification(void)
{
    struct kernel_task global={0},reaper={0},member={0};
    struct kernel_pid rootid={0},parentid={0},groupid={0};
    struct kernel_pid_registry inner={.depth=1,.parent=&scheduler.identities};
    global.group_leader=&global;reaper.group_leader=&reaper;member.group_leader=&member;
    rootid.registry=&scheduler.identities;parentid.registry=groupid.registry=&inner;
    global.identities[KERNEL_PID_TGID].identity=&rootid;global.identities[KERNEL_PID_TGID].task=&global;
    rootid.members[KERNEL_PID_TGID]=&global.identities[KERNEL_PID_TGID];
    reaper.identities[KERNEL_PID_TGID].identity=&parentid;reaper.identities[KERNEL_PID_TGID].task=&reaper;
    parentid.members[KERNEL_PID_TGID]=&reaper.identities[KERNEL_PID_TGID];
    reaper.identities[KERNEL_PID_TID].identity=&parentid;member.identities[KERNEL_PID_TID].identity=&groupid;
    global.identities[KERNEL_PID_PGID].identity=&rootid;reaper.identities[KERNEL_PID_PGID].identity=&parentid;
    member.identities[KERNEL_PID_PGID].identity=&groupid;
    global.identities[KERNEL_PID_SID].identity=reaper.identities[KERNEL_PID_SID].identity=member.identities[KERNEL_PID_SID].identity=&rootid;
    scheduler.identities.child_reaper=&rootid;inner.child_reaper=&parentid;
    member.accounted=1;member.group_members=1;member.parent=&reaper;scheduler.all_tasks=&member;
    assert(!group_orphaned(&groupid,NULL));
    assert(group_orphaned(&groupid,&member));
    member.state=KERNEL_THREAD_STATE_EXITED;assert(group_orphaned(&groupid,NULL));
    member.group_members=2;assert(!group_orphaned(&groupid,NULL));
    member.state=KERNEL_THREAD_STATE_READY;member.parent=&global;assert(group_orphaned(&groupid,NULL));
    scheduler.all_tasks=NULL;scheduler.identities.child_reaper=NULL;
    puts("PASS orphan classification: nested init connects session groups; only global init is excluded");
}

static struct kernel_task parent, children[65], adoptive;

static void setup(void)
{
    parent = adoptive = (struct kernel_task){0};
    memset(children, 0, sizeof(children));
    for (unsigned i = 0; i < 65; i++) child_append_locked(&parent, &children[i]);
}

static void exact_child_selection(void)
{
    setup();
    parent.group_leader = &parent;
    parent.magic = KERNEL_THREAD_MAGIC;
    parent.tid_owned = 1;
    for (unsigned i=0;i<65;i++) {
        children[i].magic=KERNEL_THREAD_MAGIC;
        children[i].arch.user_mode=1;
        children[i].tid_owned=children[i].accounted=1;
        children[i].tid=i+2;
        children[i].exit_signal=17;
        children[i].state=KERNEL_THREAD_STATE_BLOCKED;
    }
    struct kernel_task *last=&children[64], *selected;
    struct kernel_pid identity={.registry=&scheduler.identities,.references=1};
    uint32_t event;
    int matching;
    kernel_pid_attach(&last->identities[KERNEL_PID_TID],&identity,KERNEL_PID_TID,last);
    child_domain_acquires=0;
    assert(!select_wait_child(&parent,LINUX_P_PID,&identity,LINUX_WEXITED,&selected,&event,&matching));
    assert(!selected && !event && matching && child_domain_acquires==1);

    last->group_stopped=1;
    assert(!select_wait_child(&parent,LINUX_P_PIDFD,&identity,LINUX_WSTOPPED,&selected,&event,&matching));
    assert(selected==last && event==2 && matching && last->reaping && last->storage_borrows==1);
    assert(!select_wait_child(&parent,LINUX_P_PID,&identity,LINUX_WSTOPPED,&selected,&event,&matching));
    assert(!selected && !event && matching && last->storage_borrows==1);
    last->reaping=0;
    scheduler_task_put(last);
    last->child_creator=&identity;
    assert(!select_wait_child(&parent,LINUX_P_PID,&identity,LINUX_WEXITED|LINUX___WNOTHREAD,
        &selected,&event,&matching));
    assert(!selected && !event && !matching);

    child_remove_locked(&parent,last);
    child_append_locked(&adoptive,last);
    assert(!select_wait_child(&parent,LINUX_P_PIDFD,&identity,LINUX_WSTOPPED,&selected,&event,&matching));
    assert(!selected && !event && !matching);
    kernel_pid_detach(&last->identities[KERNEL_PID_TID]);
    puts("PASS exact wait: one domain acquisition, event claims, creator filter and adoption");
}

static void broken_edit(unsigned which)
{
    setup();
    switch (which) {
    case 0: children[31].next_sibling = &children[33]; child_remove_locked(&parent, &children[32]); break;
    case 1: children[33].previous_sibling = &children[31]; child_remove_locked(&parent, &children[32]); break;
    case 2: children[32].parent = &adoptive; child_remove_locked(&parent, &children[32]); break;
    case 3: parent.last_child->next_sibling = &children[0]; child_append_locked(&parent, &adoptive); break;
    case 4: child_append_locked(&adoptive, &children[32]); break;
    default: abort();
    }
}

static void reaper_selection(void)
{
    struct kernel_task *init = &children[0], *outer = &children[1], *inner = &children[2];
    struct kernel_task *worker = &children[3], *sibling = &children[4];
    memset(children, 0, sizeof(children));
    parent = (struct kernel_task){0};
    struct kernel_task *tasks[] = {init, outer, inner, worker, sibling, &parent};
    struct kernel_pid identities[6] = {0};
    struct kernel_pid_registry nested = {.parent=&scheduler.identities, .depth=1};
    for (unsigned i = 0; i < sizeof(tasks)/sizeof(tasks[0]); i++) {
        tasks[i]->group_leader = tasks[i]->group_next = tasks[i];
        tasks[i]->state = KERNEL_THREAD_STATE_RUNNING;
        identities[i].registry = &scheduler.identities;
        tasks[i]->identities[KERNEL_PID_TID].identity = &identities[i];
        tasks[i]->identities[KERNEL_PID_TGID].identity = &identities[i];
        tasks[i]->identities[KERNEL_PID_TGID].task = tasks[i];
        identities[i].members[KERNEL_PID_TGID] = &tasks[i]->identities[KERNEL_PID_TGID];
    }
    scheduler.init_task = init;
    scheduler.identities.child_reaper = &identities[0];
    outer->parent = init;
    inner->parent = outer;
    parent.parent = inner;
    parent.group_next = sibling;
    sibling->group_next = sibling->group_leader = &parent;
    inner->group_next = worker;
    worker->group_next = worker->group_leader = inner;
    inner->state = KERNEL_THREAD_STATE_GROUP_DEAD;
    kernel_task_set_subreaper(outer, 1);
    kernel_task_set_subreaper(worker, 1);
    assert(kernel_task_get_subreaper(inner));
    assert(!kernel_task_get_subreaper(sibling));
    assert(!kernel_task_set_parent_death_signal(worker, 64));
    assert(!kernel_task_get_parent_death_signal(inner));
    assert(kernel_task_set_parent_death_signal(worker, UINT64_C(0x100000001)) == -KERNEL_EINVAL);
    assert(kernel_task_get_parent_death_signal(worker) == 64);
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        assert(child_reaper_locked(&parent) == sibling);
        sibling->proc_exiting = 1;
        assert(child_reaper_locked(&parent) == worker);
        inner->group_exiting = 1;
        assert(child_reaper_locked(&parent) == outer);
        outer->terminate_requested = 1;
        assert(child_reaper_locked(&parent) == init);
        init->state = KERNEL_THREAD_STATE_ZOMBIE;
        assert(!child_reaper_locked(&parent));
        /* Same task ancestry, different PID view: an outer subreaper must not
         * steal the inner namespace orphan when its own init is exiting. */
        for (unsigned i = 2; i < 6; i++) identities[i].registry = &nested;
        nested.child_reaper = &identities[2];
        outer->terminate_requested = 0; init->state = KERNEL_THREAD_STATE_RUNNING;
        inner->group_exiting = 0;
        assert(child_reaper_locked(&parent) == worker);
        inner->group_exiting = 1;
        assert(child_reaper_locked(&parent)==inner);
        inner->state=KERNEL_THREAD_STATE_EXITED;
        assert(child_reaper_locked(&parent)==inner);
    }
    puts("PASS creator sibling, dead-leader subreaper, namespace-local init and no outer reaper escape");
    scheduler.identities.child_reaper = NULL;
}

static void wait_view_selection(void)
{
    uint64_t root_bits[1],nested_bits[1];
    struct kernel_pid_allocator root_numbers={0},nested_numbers={0};
    struct kernel_pid_registry nested={0};
    struct { struct kernel_pid id; struct kernel_pid_binding ancestor; } ids[3]={0};
    struct kernel_task tasks[3]={0};
    assert(!kernel_pid_allocator_init(&root_numbers,root_bits,64));
    assert(!kernel_pid_allocator_init(&nested_numbers,nested_bits,64));
    scheduler.identities=(struct kernel_pid_registry){0};
    assert(!kernel_pid_registry_init(&scheduler.identities,&root_numbers,NULL));
    assert(!kernel_pid_registry_init(&nested,&nested_numbers,&scheduler.identities));
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        for(unsigned i=0;i<3;i++) {
            kernel_pid_t numbers[2]={i? (kernel_pid_t)i:1,i? (kernel_pid_t)(22+i):0};
            assert(!kernel_pid_reserve_numbers(i?&nested:&scheduler.identities,
                &ids[i].id,sizeof(ids[i]),numbers,i?2:1));
            tasks[i].group_leader=&tasks[i];
            kernel_pid_attach(&tasks[i].identities[KERNEL_PID_TID],&ids[i].id,KERNEL_PID_TID,&tasks[i]);
            kernel_pid_attach(&tasks[i].identities[KERNEL_PID_TGID],&ids[i].id,KERNEL_PID_TGID,&tasks[i]);
            assert(!kernel_pid_publish_reserved(&ids[i].id));
            kernel_pid_put(&ids[i].id);
        }
    }
    struct kernel_pid *target=wait_target_get(&tasks[1],LINUX_P_PID,2);
    assert(target==&ids[2].id);
    assert(!wait_target_get(&tasks[1],LINUX_P_PID,24));
    struct kernel_pid *outer=wait_target_get(&tasks[0],LINUX_P_PID,24);
    assert(outer==target);
    kernel_pid_t visible_pid;
    struct kernel_task replacement = {0};
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        visible_pid=kernel_pid_number_in(process_identity(&tasks[2],KERNEL_PID_TID),
                                         process_pid_active(&tasks[1]));
    }
    copy_transfer_from=&tasks[2]; copy_transfer_to=&replacement;
    uint32_t reported=0;
    struct kernel_linux_rusage rusage;
    int64_t result=-1;
    assert(report_wait_event(&tasks[1],&tasks[2],LINUX_WAIT_CONTINUED,visible_pid,
        (uintptr_t)&reported,(uintptr_t)&rusage,&result)==KERNEL_SCHEDULER_STATUS_OK);
    assert(result==2 && reported==LINUX_WAIT_CONTINUED && report_copies==2);
    assert(!process_identity(&tasks[2],KERNEL_PID_TID));
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        kernel_pid_transfer(&replacement.identities[KERNEL_PID_TID],
                            &tasks[2].identities[KERNEL_PID_TID],&tasks[2]);
    }
    puts("PASS wait report preserves locked PID snapshot across user-copy role transfer");
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        kernel_pid_put(target);kernel_pid_put(outer);
        for(unsigned i=0;i<3;i++) {
            kernel_pid_detach(&tasks[i].identities[KERNEL_PID_TID]);
            kernel_pid_detach(&tasks[i].identities[KERNEL_PID_TGID]);
        }
        unsigned count=0;while(kernel_pid_take_retired(&scheduler.identities))count++;
        assert(count==3);
        kernel_pid_registry_put(&nested);
        assert(kernel_pid_take_retired_namespace(&scheduler.identities)==&nested);
        assert(!scheduler.identities.retired && !scheduler.identities.retired_namespaces);
        kernel_pid_registry_put(&scheduler.identities);
    }
    scheduler.identities=(struct kernel_pid_registry){0};
    puts("PASS wait target uses caller PID view and retains the canonical identity");
}

int main(void)
{
    struct rlimit no_core = {0};
    assert(!setrlimit(RLIMIT_CORE, &no_core));
    kernel_cpu_initialize(&cpu, 0, NULL);
    kernel_raw_lock_init(&kernel_wait_domain, KERNEL_RAW_RANK_SCHEDULER);
    orphan_classification();
    reaper_selection();
    wait_view_selection();
    exact_child_selection();
    setup();
    child_remove_locked(&parent, &children[32]);
    assert(children[31].next_sibling == &children[33] && children[33].previous_sibling == &children[31]);
    assert(!children[32].parent && !children[32].next_sibling && !children[32].previous_sibling);
    child_append_locked(&adoptive, &children[32]);
    assert(adoptive.first_child == &children[32] && adoptive.last_child == &children[32]);
    for (unsigned i = 0; i < 65; i++) {
        if (i == 32) continue;
        child_remove_locked(&parent, &children[i]);
        child_append_locked(&adoptive, &children[i]);
    }
    assert(!parent.first_child && !parent.last_child && parent.children_epoch == 130);
    while (adoptive.first_child) child_remove_locked(&adoptive, adoptive.first_child);
    assert(!adoptive.last_child && adoptive.children_epoch == 130);
    for (unsigned which = 0; which < 5; which++) {
        pid_t pid = fork(); assert(pid >= 0);
        if (!pid) { broken_edit(which); _exit(0); }
        int status; assert(waitpid(pid, &status, 0) == pid);
        assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGILL);
    }
    puts("PASS production child links: middle/head/tail removal, adoption, and five invalid edits rejected");
    return 0;
}
