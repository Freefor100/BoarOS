#include "private.h"
#include <kernel/time.h>
#include <kernel/errno.h>
#include <kernel/irq.h>
#include <kernel/accounting.h>

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
        if (task->accounting_mode == ACCOUNT_USER) task->usage.user_ns += elapsed;
        else if (task->accounting_mode == ACCOUNT_KERNEL) task->usage.kernel_ns += elapsed;
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
    if (previous->arch.user_mode) {
        if (previous->state == KERNEL_THREAD_STATE_BLOCKED || previous->state == KERNEL_THREAD_STATE_STOPPED ||
            previous->state == KERNEL_THREAD_STATE_EXITED) previous->usage.voluntary++;
        else previous->usage.involuntary++;
    }
    cpu->accounting_owner = next;
}

void scheduler_task_cpu_time_locked(const struct kernel_task *task, uint64_t *user, uint64_t *system)
{
    *user = task->usage.user_ns;
    *system = task->usage.kernel_ns;
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
    if (!scope->task || scope->task->accounting_mode == scope->restore_mode) return;
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

void scheduler_usage_add(struct kernel_task_usage *to, const struct kernel_task_usage *from)
{
    to->user_ns += from->user_ns; to->kernel_ns += from->kernel_ns;
    to->minor_faults += from->minor_faults; to->major_faults += from->major_faults;
    to->voluntary += from->voluntary; to->involuntary += from->involuntary;
    to->read_bytes += from->read_bytes; to->write_bytes += from->write_bytes;
    if (to->maxrss_pages < from->maxrss_pages) to->maxrss_pages = from->maxrss_pages;
}

void scheduler_usage_thread_locked(const struct kernel_task *task, struct kernel_task_usage *usage)
{
    *usage = (struct kernel_task_usage){
        .minor_faults = __atomic_load_n(&task->usage.minor_faults, __ATOMIC_RELAXED),
        .major_faults = __atomic_load_n(&task->usage.major_faults, __ATOMIC_RELAXED),
        .read_bytes = __atomic_load_n(&task->usage.read_bytes, __ATOMIC_RELAXED),
        .write_bytes = __atomic_load_n(&task->usage.write_bytes, __ATOMIC_RELAXED),
        .maxrss_pages = task->usage.maxrss_pages,
        .voluntary = task->usage.voluntary, .involuntary = task->usage.involuntary,
    };
    scheduler_task_cpu_time_locked(task, &usage->user_ns, &usage->kernel_ns);
}

void scheduler_usage_save_mm(struct kernel_task *task)
{
    if (task->mm.state != KERNEL_MM_LIVE && task->mm.state != KERNEL_MM_CLEANUP) return;
    uint64_t peak = kernel_mm_peak_resident_pages(&task->mm);
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    if (task->usage.maxrss_pages < peak) task->usage.maxrss_pages = peak;
}

/* 数值快照不触及MM句柄，CPU时钟和times可在发起CPU查询。 */
void scheduler_usage_snapshot(const struct kernel_task *task, int who,
                              struct kernel_task_usage *usage)
{
    struct kernel_task *leader;
    int single = 0;
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        leader = task->group_leader;
        if (!leader) __builtin_trap();
        if (who == -1) { *usage = leader->child_usage; return; }
        if (who == 1) {
            scheduler_usage_thread_locked(task, usage);
            uint64_t peak = leader->dead_usage.maxrss_pages;
            if (peak < leader->usage.maxrss_pages) peak = leader->usage.maxrss_pages;
            if (usage->maxrss_pages < peak) usage->maxrss_pages = peak;
        } else if (leader->group_members == 1) {
            struct kernel_task_usage own;
            scheduler_usage_thread_locked(leader, &own);
            *usage = leader->dead_usage;
            scheduler_usage_add(usage, &own);
            if (who == -2) scheduler_usage_add(usage, &leader->child_usage);
            single = 1;
        }
    }
    if (who == 1 || single) return;
    struct scheduler_group_cursor cursor;
    scheduler_group_cursor_begin(leader, &cursor);
    uint64_t epoch = UINT64_MAX;
    while (cursor.task) {
        struct kernel_task *member = cursor.task;
        {
            KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
            if (epoch != cursor.epoch) {
                *usage = leader->dead_usage;
                if (who == -2) scheduler_usage_add(usage, &leader->child_usage);
                epoch = cursor.epoch;
            }
            if (leader->group_epoch == cursor.epoch && member->group_leader == leader) {
                struct kernel_task_usage part;
                scheduler_usage_thread_locked(member, &part);
                scheduler_usage_add(usage, &part);
            }
        }
        scheduler_group_cursor_next(&cursor);
    }
    scheduler_group_cursor_end(&cursor);
}

/* CPU0与IRQ关闭保证借用中的task MM句柄不会被cleanup并发move/release；
 * MM raw只固定页计数，没有分配、I/O或等待。MM与调度域分别取锁。 */
void scheduler_usage_snapshot_mm(const struct kernel_task *task, int who,
                                 struct kernel_task_usage *usage)
{
    scheduler_usage_snapshot(task, who, usage);
    if (who == -1) return;
    if (who == 1) {
        if (task->mm.state == KERNEL_MM_LIVE) {
            uint64_t peak = kernel_mm_peak_resident_pages(&task->mm);
            if (usage->maxrss_pages < peak) usage->maxrss_pages = peak;
        }
        return;
    }
    struct scheduler_group_cursor cursor;
    scheduler_group_cursor_begin(task->group_leader, &cursor);
    while (cursor.task) {
        if (cursor.task->mm.state == KERNEL_MM_LIVE) {
            uint64_t peak = kernel_mm_peak_resident_pages(&cursor.task->mm);
            if (usage->maxrss_pages < peak) usage->maxrss_pages = peak;
        }
        scheduler_group_cursor_next(&cursor);
    }
    scheduler_group_cursor_end(&cursor);
}

void scheduler_usage_format(const struct kernel_task_usage *usage, struct kernel_linux_rusage *r)
{
    *r = (struct kernel_linux_rusage){
        .ru_utime = {(int64_t)(usage->user_ns / 1000000000U), (int64_t)(usage->user_ns % 1000000000U / 1000U)},
        .ru_stime = {(int64_t)(usage->kernel_ns / 1000000000U), (int64_t)(usage->kernel_ns % 1000000000U / 1000U)},
        .ru_maxrss = (int64_t)(usage->maxrss_pages * (BOAROS_PAGE_SIZE / 1024U)),
        .ru_minflt = (int64_t)usage->minor_faults, .ru_majflt = (int64_t)usage->major_faults,
        .ru_nvcsw = (int64_t)usage->voluntary, .ru_nivcsw = (int64_t)usage->involuntary,
        .ru_inblock = (int64_t)(usage->read_bytes / 512U), .ru_oublock = (int64_t)(usage->write_bytes / 512U),
    };
}

int kernel_task_getrusage(struct kernel_task *task, int32_t who, struct kernel_linux_rusage *r)
{
    if (who != 0 && who != 1 && who != -1) return -KERNEL_EINVAL;
    KERNEL_SERVICE_SCOPE(service);
    KERNEL_IRQ_SCOPE(irq);
    struct kernel_task_usage usage = {0};
    scheduler_usage_snapshot_mm(task, who, &usage);
    scheduler_usage_format(&usage, r);
    return 0;
}

static struct kernel_task *io_account_task(void)
{
    struct kernel_task *task = kernel_io_context_current()->account_task;
    if (!task) task = kernel_task_current();
    return task && task->arch.user_mode ? task : 0;
}
void kernel_task_account_read(uint64_t bytes)
{
    struct kernel_task *task = io_account_task();
    if (task) __atomic_fetch_add(&task->usage.read_bytes, bytes, __ATOMIC_RELAXED);
}
void kernel_task_account_write(uint64_t bytes)
{
    struct kernel_task *task = io_account_task();
    if (task) __atomic_fetch_add(&task->usage.write_bytes, bytes, __ATOMIC_RELAXED);
}
uint64_t kernel_task_read_bytes(const struct kernel_task *task)
{ return task ? __atomic_load_n(&task->usage.read_bytes, __ATOMIC_RELAXED) : 0; }
void kernel_task_account_fault(struct kernel_task *task, int major)
{
    if (!task || !task->arch.user_mode) return;
    __atomic_fetch_add(major ? &task->usage.major_faults : &task->usage.minor_faults,
                       1, __ATOMIC_RELAXED);
}
