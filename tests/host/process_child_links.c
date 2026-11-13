/* Exercise the production child-link edits without exposing private APIs. */
#include "../../kernel/sched/process.c"
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
_Thread_local uintptr_t sync_test_irq;
void *sync_test_cpu(void) { return &cpu; }
void *sync_test_current_task(void) { return cpu.current; }

static struct kernel_task parent, children[65], adoptive;

static void setup(void)
{
    parent = adoptive = (struct kernel_task){0};
    memset(children, 0, sizeof(children));
    for (unsigned i = 0; i < 65; i++) child_append_locked(&parent, &children[i]);
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
    for (unsigned i = 0; i < sizeof(tasks)/sizeof(tasks[0]); i++) {
        tasks[i]->group_leader = tasks[i]->group_next = tasks[i];
        tasks[i]->state = KERNEL_THREAD_STATE_RUNNING;
    }
    scheduler.init_task = init;
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
    }
    puts("PASS creator sibling, live member of dead-leader subreaper, older ancestor, init and teardown");
}

int main(void)
{
    struct rlimit no_core = {0};
    assert(!setrlimit(RLIMIT_CORE, &no_core));
    kernel_cpu_initialize(&cpu, 0, NULL);
    kernel_raw_lock_init(&kernel_wait_domain, KERNEL_RAW_RANK_SCHEDULER);
    reaper_selection();
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
