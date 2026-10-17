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

int main(void)
{
    struct rlimit no_core = {0};
    assert(!setrlimit(RLIMIT_CORE, &no_core));
    kernel_cpu_initialize(&cpu, 0, NULL);
    kernel_raw_lock_init(&kernel_wait_domain, KERNEL_RAW_RANK_SCHEDULER);
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
