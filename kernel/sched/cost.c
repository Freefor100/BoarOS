#include "private.h"
#include <kernel/cost.h>
#if BOAROS_COST_DIAGNOSTICS
#include <kernel/errno.h>
#include <kernel/time.h>
#include <string.h>
uint64_t kernel_cost_clock(void) { uint64_t t; __asm__ volatile("rdtime %0" : "=r"(t)); return t; }
uint64_t kernel_cost_irq_save(void)
{
    uint64_t s; __asm__ volatile("csrrc %0, sstatus, %1" : "=r"(s) : "r"((uint64_t)2) : "memory");
    return !!(s & 2);
}
void kernel_cost_irq_restore(uint64_t enabled)
{ if (enabled) __asm__ volatile("csrsi sstatus, 2" ::: "memory"); }
struct kernel_cost_cpu *kernel_cost_cpu_current(void)
{ return &kernel_cpu_current()->cost; }
static struct kernel_task *executing_task(void)
{
    if (!kernel_cpu_current()->current) return 0;
    /* current may already name the next task before the physical stack handoff. */
    return arch_current_thread_get();
}
struct kernel_cost_task *kernel_cost_current(void)
{
    uint64_t irq=kernel_cost_lock();
    struct kernel_cpu *cpu=kernel_cpu_current();
    struct kernel_task *task=executing_task();
    struct kernel_cost_task *result=task ? &task->cost : &cpu->cost.bootstrap;
    if (task && task->io_context.background_reclaim) result->wait_flags |= 64;
    else result->wait_flags &= (uint8_t)~64U;
    kernel_cost_unlock(irq);
    return result;
}
void kernel_cost_syscall(uint64_t number, int64_t fd)
{
    struct kernel_task *task = executing_task();
    if (!task) return;
    kernel_cost_account(&task->cost);
    /* Descriptor lookup takes files locks, so do it before the COST lock. */
    int suppress=((number >= 62 && number <= 70) || number == 57) ?
        kernel_files_cost_descriptor(&task->files,fd) : 0;
    uint64_t irq=kernel_cost_lock();
    task->cost.suppress=(uint8_t)suppress;
    if (suppress) kernel_cost_irq_suppress();
    kernel_cost_unlock(irq);
}
int kernel_cost_control(const char *command, size_t size)
{
    struct kernel_task *task = executing_task();
    if (!task || !task->group_leader) return -KERNEL_EINVAL;
    uint64_t owner = process_identity_generation(task);
    if (size == 6 && !memcmp(command, "begin\n", 6)) {
        int result = kernel_cost_begin(owner, kernel_cost_timebase(), 0, 1);
        if (!result) {
            {
                KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
                for (struct kernel_cpu *cpu=scheduler.cpus;cpu;cpu=cpu->scheduler_next) {
                    uint64_t irq=kernel_cost_lock();
                    kernel_cost_register_cpu(&cpu->cost);
                    cpu->cost.running=cpu->accounting_owner ? &cpu->accounting_owner->cost :
                        cpu->current ? &cpu->current->cost : &cpu->cost.bootstrap;
                    kernel_cost_unlock(irq);
                }
            }
            struct scheduler_task_cursor cursor;
            scheduler_task_cursor_begin(&cursor);
            while (cursor.task) {
                int belongs = scheduler_task_descends_from(cursor.task, 0, owner);
                {
                    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
                    struct kernel_task *t = cursor.task;
                    if (t->accounted) {
                        uint64_t irq=kernel_cost_lock();
                        if (belongs) kernel_cost_join(&t->cost);
                        else kernel_cost_rebase(&t->cost);
                        if (t->state == KERNEL_THREAD_STATE_BLOCKED) t->cost.blocked_start = kernel_cost_clock();
                        if (t->state == KERNEL_THREAD_STATE_READY) kernel_cost_ready(&t->cost);
                        kernel_cost_unlock(irq);
                    }
                }
                scheduler_task_cursor_next(&cursor);
            }
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
    uint64_t irq=kernel_cost_lock();
    struct kernel_cost_task *task = kernel_cost_current();
    if (task) kernel_cost_account(task);
    kernel_cost_boundary();
    if (task) { task->suppress = 0; task->run_start = kernel_cost_clock(); task->wait_flags |= 8; }
    kernel_cost_unlock(irq);
}
void kernel_cost_task_exit(void)
{
    struct kernel_task *task = executing_task();
    if (!task) return;
    uint64_t irq=kernel_cost_lock();
    kernel_cost_account(&task->cost);
    if (task->cost.wait_rank) kernel_cost_add_tag(kernel_cost_task_tag(&task->cost),
        (enum kernel_cost_metric)(kernel_cost_lock_metric(task->cost.wait_rank-1)+7),1);
    kernel_cost_cancel(&task->cost);
    kernel_cost_unlock(irq);
    if (task->group_leader && (task->group_leader->group_exiting ||
        task->group_leader->group_members <= 1))
        kernel_cost_abort(process_identity_generation(task));
}
#endif
