#include "private.h"
#include <kernel/time.h>
#include <arch/timer.h>
#include <kernel/errno.h>
#include <kernel/irq.h>
#include <kernel/accounting.h>

enum { ACCOUNT_KERNEL, ACCOUNT_USER, ACCOUNT_INTERRUPT };

static inline uint64_t accounting_counter(void)
{
#if BOAROS_MAX_CPUS > 1
    return arch_time_read_ordered();
#else
    return arch_time_read();
#endif
}

/* 只有实际执行本栈的CPU能写执行片段，入口/返回/切栈均已关闭IRQ。
 * 远端查询持调度域固定owner及task存储；序列只覆盖数值发布，写者不等待。 */
static inline void accounting_write_begin(struct kernel_cpu *cpu)
{
#if BOAROS_MAX_CPUS > 1
    uint64_t sequence=__atomic_load_n(&cpu->accounting_sequence,__ATOMIC_RELAXED);
    __atomic_store_n(&cpu->accounting_sequence,sequence+1,__ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
#else
    (void)cpu;
#endif
}
static inline void accounting_write_end(struct kernel_cpu *cpu)
{
#if BOAROS_MAX_CPUS > 1
    uint64_t sequence=__atomic_load_n(&cpu->accounting_sequence,__ATOMIC_RELAXED);
    __atomic_store_n(&cpu->accounting_sequence,sequence+1,__ATOMIC_RELEASE);
#else
    (void)cpu;
#endif
}
static __attribute__((always_inline)) inline void account_time(struct kernel_cpu *cpu,uint64_t now)
{
    uint64_t started=__atomic_load_n(&cpu->accounting_started_ticks,__ATOMIC_RELAXED);
    if (now<started) __builtin_trap();
    struct kernel_task *task=cpu->accounting_owner;
    uint64_t elapsed=now-started;
    if (!task->idle && task!=cpu->idle) {
        uint32_t mode=__atomic_load_n(&task->accounting_mode,__ATOMIC_RELAXED);
        uint64_t *counter=mode==ACCOUNT_USER ? &cpu->accounting_user_ticks :
            mode==ACCOUNT_KERNEL ? &cpu->accounting_kernel_ticks : 0;
        if (counter) __atomic_store_n(counter,
            __atomic_load_n(counter,__ATOMIC_RELAXED)+elapsed,__ATOMIC_RELAXED);
    }
    __atomic_store_n(&cpu->accounting_started_ticks,now,__ATOMIC_RELAXED);
}
/* 冷启动首次绑定也沿调度域发布owner；常规trap无需重复取全局锁。 */
static int accounting_initialize(struct kernel_cpu *cpu)
{
    if (scheduler.initialized!=KERNEL_SCHEDULER_INITIALIZED || !kernel_time_is_initialized()) return 0;
    KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
    if (!cpu->current) return 0;
    cpu->accounting_owner=cpu->current;
    __atomic_store_n(&cpu->accounting_started_ticks,accounting_counter(),__ATOMIC_RELAXED);
    return 1;
}
void scheduler_cpu_time_switch(struct kernel_task *previous,struct kernel_task *next)
{
    KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
    struct kernel_cpu *cpu=kernel_cpu_current();
    uint64_t now=accounting_counter();
    if (!cpu->accounting_owner) {
        cpu->accounting_owner=previous;
        cpu->accounting_started_ticks=now;
    }
    if (cpu->accounting_owner!=previous) __builtin_trap();
    accounting_write_begin(cpu);
    account_time(cpu,now);
    /* task保存已结束执行片段的纳秒值；本CPU的硬件tick只在查询/交接换算。 */
    previous->usage.user_ns+=kernel_time_ticks_to_ns(cpu->accounting_user_ticks);
    previous->usage.kernel_ns+=kernel_time_ticks_to_ns(cpu->accounting_kernel_ticks);
    __atomic_store_n(&cpu->accounting_user_ticks,0,__ATOMIC_RELAXED);
    __atomic_store_n(&cpu->accounting_kernel_ticks,0,__ATOMIC_RELAXED);
    accounting_write_end(cpu);
    /* current已发布next，旧栈借用覆盖previous，真正切栈前才转移owner。 */
    if (previous->arch.user_mode) {
        if (previous->state==KERNEL_THREAD_STATE_BLOCKED || previous->state==KERNEL_THREAD_STATE_STOPPED ||
            previous->state==KERNEL_THREAD_STATE_EXITED) previous->usage.voluntary++;
        else previous->usage.involuntary++;
    }
    cpu->accounting_owner=next;
}
void scheduler_task_cpu_time_locked(const struct kernel_task *task,uint64_t *user,uint64_t *system)
{
    for (struct kernel_cpu *cpu=scheduler.cpus;cpu;cpu=cpu->scheduler_next) {
        if (cpu->accounting_owner!=task || task->idle || task==cpu->idle) continue;
        for (;;) {
#if BOAROS_MAX_CPUS > 1
            uint64_t sequence=__atomic_load_n(&cpu->accounting_sequence,__ATOMIC_ACQUIRE);
            if (sequence&1U) continue;
#endif
            uint64_t u=__atomic_load_n(&cpu->accounting_user_ticks,__ATOMIC_RELAXED);
            uint64_t k=__atomic_load_n(&cpu->accounting_kernel_ticks,__ATOMIC_RELAXED);
            uint64_t started=__atomic_load_n(&cpu->accounting_started_ticks,__ATOMIC_RELAXED);
            uint32_t mode=__atomic_load_n(&task->accounting_mode,__ATOMIC_RELAXED);
            uint64_t now=accounting_counter();
#if BOAROS_MAX_CPUS > 1
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            if (sequence!=__atomic_load_n(&cpu->accounting_sequence,__ATOMIC_RELAXED)) continue;
#endif
            if (now<started) __builtin_trap();
            uint64_t elapsed=now-started;
            *user=task->usage.user_ns+kernel_time_ticks_to_ns(u+(mode==ACCOUNT_USER ? elapsed : 0));
            *system=task->usage.kernel_ns+kernel_time_ticks_to_ns(k+(mode==ACCOUNT_KERNEL ? elapsed : 0));
            return;
        }
    }
    *user=__atomic_load_n(&task->usage.user_ns,__ATOMIC_RELAXED);
    *system=__atomic_load_n(&task->usage.kernel_ns,__ATOMIC_RELAXED);
}
void kernel_cpu_account_trap_begin(struct kernel_cpu_account_scope *scope,int user,int interrupt)
{
    scope->task=0;
    if (!user && !interrupt) return;
    struct kernel_cpu *cpu=kernel_cpu_current();
    if (!cpu->accounting_owner && !accounting_initialize(cpu)) return;
    struct kernel_task *task=cpu->accounting_owner;
    accounting_write_begin(cpu);
    account_time(cpu,accounting_counter());
    scope->task=task;
    /* U trap后续返回准备仍计内核工作；最终USER切换在架构返回入口完成。 */
    scope->restore_mode=user ? ACCOUNT_KERNEL : task->accounting_mode;
    __atomic_store_n(&task->accounting_mode,interrupt ? ACCOUNT_INTERRUPT : ACCOUNT_KERNEL,__ATOMIC_RELAXED);
    accounting_write_end(cpu);
}
void kernel_cpu_account_trap_end(struct kernel_cpu_account_scope *scope)
{
    if (!scope->task || scope->task->accounting_mode==scope->restore_mode) return;
    struct kernel_cpu *cpu=kernel_cpu_current();
    if (cpu->accounting_owner!=scope->task) __builtin_trap();
    accounting_write_begin(cpu);
    account_time(cpu,accounting_counter());
    __atomic_store_n(&scope->task->accounting_mode,scope->restore_mode,__ATOMIC_RELAXED);
    accounting_write_end(cpu);
}
void kernel_cpu_account_user_return(void)
{
    struct kernel_cpu *cpu=kernel_cpu_current();
    if (!cpu->accounting_owner && !accounting_initialize(cpu)) __builtin_trap();
    accounting_write_begin(cpu);
    account_time(cpu,accounting_counter());
    __atomic_store_n(&cpu->accounting_owner->accounting_mode,ACCOUNT_USER,__ATOMIC_RELAXED);
    accounting_write_end(cpu);
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

/* 显式group容器由调用者借用。退出/exec可清除其group_leader；
 * 此时只读该容器已保留的数值，由PID查询者检查身份并决定重试。 */
void scheduler_group_usage_snapshot(struct kernel_task *leader, int children,
                                   struct kernel_task_usage *usage)
{
    struct scheduler_group_cursor cursor = {0};
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        *usage = leader->dead_usage;
        if (children) scheduler_usage_add(usage, &leader->child_usage);
        if (leader->group_members <= 1 || leader->retirement || leader->group_leader != leader) {
            struct kernel_task_usage own;
            scheduler_usage_thread_locked(leader, &own);
            scheduler_usage_add(usage, &own);
            return;
        }
        scheduler_task_get_locked(leader);
        cursor.leader = cursor.task = leader;
        cursor.epoch = leader->group_epoch;
    }
    uint64_t epoch = UINT64_MAX;
    while (cursor.task) {
        struct kernel_task *member = cursor.task;
        {
            KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
            if (epoch != cursor.epoch) {
                *usage = leader->dead_usage;
                if (children) scheduler_usage_add(usage, &leader->child_usage);
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

/* 数值快照不触及MM句柄，CPU时钟和times可在发起CPU查询。 */
void scheduler_usage_snapshot(const struct kernel_task *task, int who,
                              struct kernel_task_usage *usage)
{
    struct kernel_task *leader;
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
            return;
        }
    }
    scheduler_group_usage_snapshot(leader, who == -2, usage);
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
