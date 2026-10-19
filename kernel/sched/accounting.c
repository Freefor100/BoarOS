#include "private.h"
#include <kernel/time.h>

enum { ACCOUNT_KERNEL, ACCOUNT_USER, ACCOUNT_INTERRUPT };

void scheduler_cpu_time_account_locked(struct kernel_cpu *cpu, uint64_t now)
{
    if (!cpu->accounting_owner) {
        cpu->accounting_owner = cpu->current;
        cpu->accounting_started_ns = now;
        return;
    }
    if (now < cpu->accounting_started_ns) __builtin_trap();
    struct kernel_task *task = cpu->accounting_owner;
    uint64_t elapsed = now - cpu->accounting_started_ns;
    if (!task->idle && task != cpu->idle) {
        if (task->accounting_mode == ACCOUNT_USER) task->user_ns += elapsed;
        else if (task->accounting_mode == ACCOUNT_KERNEL) task->kernel_ns += elapsed;
    }
    cpu->accounting_started_ns = now;
}

void scheduler_cpu_time_switch(struct kernel_task *previous, struct kernel_task *next)
{
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    struct kernel_cpu *cpu = kernel_cpu_current();
    uint64_t now = kernel_time_monotonic_ns();
    if (!cpu->accounting_owner) {
        cpu->accounting_owner = previous;
        cpu->accounting_started_ns = now;
    }
    if (cpu->accounting_owner != previous) __builtin_trap();
    scheduler_cpu_time_account_locked(cpu, now);
    /* current已发布next，旧栈的执行借用仍覆盖previous，到真正切栈前才转移记账。 */
    cpu->accounting_owner = next;
}

void scheduler_task_cpu_time_locked(const struct kernel_task *task, uint64_t *user, uint64_t *system)
{
    *user = task->user_ns;
    *system = task->kernel_ns;
    uint64_t now = kernel_time_monotonic_ns();
    for (struct kernel_cpu *cpu = scheduler.cpus; cpu; cpu = cpu->scheduler_next) {
        if (cpu->accounting_owner != task || task->idle || task == cpu->idle) continue;
        if (now < cpu->accounting_started_ns) __builtin_trap();
        uint64_t elapsed = now - cpu->accounting_started_ns;
        if (task->accounting_mode == ACCOUNT_USER) *user += elapsed;
        else if (task->accounting_mode == ACCOUNT_KERNEL) *system += elapsed;
    }
}

void kernel_cpu_account_trap_begin(struct kernel_cpu_account_scope *scope, int user, int interrupt)
{
    scope->task = 0;
    if ((!user && !interrupt) || scheduler.initialized != KERNEL_SCHEDULER_INITIALIZED ||
        !kernel_time_is_initialized()) return;
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    struct kernel_cpu *cpu = kernel_cpu_current();
    struct kernel_task *task = cpu->current;
    if (!task) return;
    scheduler_cpu_time_account_locked(cpu, kernel_time_monotonic_ns());
    scope->task = task;
    /* U trap的后续返回准备仍是内核工作，最终USER切换在架构返回入口完成。 */
    scope->restore_mode = user ? ACCOUNT_KERNEL : task->accounting_mode;
    task->accounting_mode = interrupt ? ACCOUNT_INTERRUPT : ACCOUNT_KERNEL;
}

void kernel_cpu_account_trap_end(struct kernel_cpu_account_scope *scope)
{
    if (!scope->task) return;
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    struct kernel_cpu *cpu = kernel_cpu_current();
    if (cpu->current != scope->task) __builtin_trap();
    scheduler_cpu_time_account_locked(cpu, kernel_time_monotonic_ns());
    scope->task->accounting_mode = scope->restore_mode;
}

void kernel_cpu_account_user_return(void)
{
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    struct kernel_cpu *cpu = kernel_cpu_current();
    scheduler_cpu_time_account_locked(cpu, kernel_time_monotonic_ns());
    cpu->current->accounting_mode = ACCOUNT_USER;
}
