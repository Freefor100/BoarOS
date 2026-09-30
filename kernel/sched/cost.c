#include "private.h"
#include <kernel/cost.h>
#if BOAROS_COST_DIAGNOSTICS
#include <kernel/errno.h>
#include <kernel/time.h>
#include <string.h>
uint64_t kernel_cost_clock(void) { uint64_t t; __asm__ volatile("rdtime %0" : "=r"(t)); return t; }
uint64_t kernel_cost_lock(void)
{ uint64_t s; __asm__ volatile("csrrc %0, sstatus, %1" : "=r"(s) : "r"((uint64_t)2) : "memory"); return s; }
void kernel_cost_unlock(uint64_t s)
{ if (s & 2) __asm__ volatile("csrsi sstatus, 2" ::: "memory"); }
static struct kernel_cost_task bootstrap_cost;
struct kernel_cost_task *kernel_cost_current(void)
{
    if (!scheduler.current) return &bootstrap_cost;
    if (scheduler.current->io_context.background_reclaim) scheduler.current->cost.wait_flags |= 64;
    else scheduler.current->cost.wait_flags &= (uint8_t)~64U;
    return &scheduler.current->cost;
}
void kernel_cost_syscall(uint64_t number, int64_t fd)
{
    struct kernel_task *task = scheduler.current;
    if (!task) return;
    kernel_cost_account(&task->cost);
    if ((number >= 62 && number <= 70) || number == 57)
        task->cost.suppress = kernel_files_cost_descriptor(&task->files, fd);
}
static int belongs(struct kernel_task *task, uint64_t owner)
{
    while (task) {
        if (process_identity_generation(task) == owner) return 1;
        struct kernel_task *leader = task->group_leader;
        task = leader ? leader->parent : task->parent;
    }
    return 0;
}
int kernel_cost_control(const char *command, size_t size)
{
    struct kernel_task *task = scheduler.current;
    if (!task || !task->group_leader) return -KERNEL_EINVAL;
    uint64_t owner = process_identity_generation(task);
    if (size == 6 && !memcmp(command, "begin\n", 6)) {
        int result = kernel_cost_begin(owner, kernel_cost_timebase(), 0, 1);
        if (!result) for (struct kernel_task *t = scheduler.all_tasks; t; t = t->all_next) {
            if (belongs(t, owner)) kernel_cost_join(&t->cost);
            else kernel_cost_rebase(&t->cost);
            if (t->state == KERNEL_THREAD_STATE_BLOCKED) t->cost.blocked_start = kernel_cost_clock();
            if (t->state == KERNEL_THREAD_STATE_READY) kernel_cost_ready(&t->cost);
        }
        return result;
    }
    if (size == 4 && !memcmp(command, "end\n", 4)) {
        /* End excludes its complete control syscall, including pre-callback copies. */
        return kernel_cost_end(owner, 1);
    }
    return -KERNEL_EINVAL;
}
void kernel_cost_user_return(void)
{
    struct kernel_cost_task *task = kernel_cost_current();
    if (task) kernel_cost_account(task);
    kernel_cost_boundary();
    if (task) { task->suppress = 0; task->run_start = kernel_cost_clock(); }
}
void kernel_cost_task_exit(void)
{
    struct kernel_task *task = scheduler.current;
    if (!task) return;
    kernel_cost_account(&task->cost);
    if (task->cost.wait_rank) kernel_cost_add_tag(kernel_cost_task_tag(&task->cost),
        (enum kernel_cost_metric)(COST_LOCK10_CANCELLED+(task->cost.wait_rank-1)*8),1);
    kernel_cost_cancel(&task->cost);
    if (task->group_leader && (task->group_leader->group_exiting ||
        task->group_leader->group_members <= 1))
        kernel_cost_abort(process_identity_generation(task));
}
#endif
