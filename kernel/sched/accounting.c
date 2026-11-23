#include "private.h"
#include <kernel/time.h>
#include <arch/timer.h>
#include <kernel/errno.h>
#include <kernel/irq.h>
#include <kernel/accounting.h>
#include <kernel/credentials.h>

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
static inline void account_interval(struct kernel_cpu *cpu, uint64_t now, uint64_t *counter)
{
    uint64_t started=__atomic_load_n(&cpu->accounting_started_ticks,__ATOMIC_RELAXED);
    if (now<started) __builtin_trap();
    if (counter) __atomic_store_n(counter,
        __atomic_load_n(counter,__ATOMIC_RELAXED) + (now - started), __ATOMIC_RELAXED);
    __atomic_store_n(&cpu->accounting_started_ticks,now,__ATOMIC_RELAXED);
}

static __attribute__((always_inline)) inline void account_time(struct kernel_cpu *cpu,uint64_t now)
{
    struct kernel_task *task=cpu->accounting_owner;
    uint64_t *counter = NULL;
    if (!task->idle && task!=cpu->idle) {
        uint32_t mode=__atomic_load_n(&task->accounting_mode,__ATOMIC_RELAXED);
        counter=mode==ACCOUNT_USER ? &cpu->accounting_user_ticks :
            mode==ACCOUNT_KERNEL ? &cpu->accounting_kernel_ticks : 0;
    }
    account_interval(cpu, now, counter);
}
/* 首次绑定有独立的控制资格和失败分支，保持在常规trap及用户返回的记账路径之外。 */
static __attribute__((noinline)) int accounting_initialize(struct kernel_cpu *cpu)
{
    if (scheduler.initialized!=KERNEL_SCHEDULER_INITIALIZED || !kernel_time_is_initialized()) return 0;
    KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
    if (!cpu->current) return 0;
    cpu->accounting_owner=cpu->current;
    uint64_t now=accounting_counter();
    cpu->accounting_dispatch_ticks=now;
    __atomic_store_n(&cpu->accounting_started_ticks,now,__ATOMIC_RELAXED);
    return 1;
}
/* Scheduling consumes the same real execution counters as CPU clocks, not
 * wall time between decisions (which can include interrupts or sleep). */
uint64_t scheduler_cpu_execution_ns(struct kernel_cpu *cpu,const struct kernel_task *task)
{
    for (;;) {
#if BOAROS_MAX_CPUS > 1
        uint64_t seq=__atomic_load_n(&cpu->accounting_sequence,__ATOMIC_ACQUIRE);
        if(seq&1U)continue;
#endif
        uint64_t total=task->usage.user_ns+task->usage.kernel_ns;
        if(cpu->accounting_owner==task) {
            uint64_t u=__atomic_load_n(&cpu->accounting_user_ticks,__ATOMIC_RELAXED);
            uint64_t k=__atomic_load_n(&cpu->accounting_kernel_ticks,__ATOMIC_RELAXED);
            uint64_t started=__atomic_load_n(&cpu->accounting_started_ticks,__ATOMIC_RELAXED);
            uint32_t mode=__atomic_load_n(&task->accounting_mode,__ATOMIC_RELAXED);
            uint64_t now=accounting_counter();
            if(now<started)__builtin_trap();
            if(mode==ACCOUNT_USER)u+=now-started;
            else if(mode==ACCOUNT_KERNEL)k+=now-started;
            total+=kernel_time_ticks_to_ns(u)+kernel_time_ticks_to_ns(k);
        }
#if BOAROS_MAX_CPUS > 1
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if(seq!=__atomic_load_n(&cpu->accounting_sequence,__ATOMIC_RELAXED))continue;
#endif
        return total;
    }
}
void scheduler_fair_account(struct kernel_task *task,uint64_t total)
{
    if(total<task->fair_runtime_seen)__builtin_trap();
    uint64_t delta=total-task->fair_runtime_seen;task->fair_runtime_seen=total;
    if(task->scheduling.policy==KERNEL_SCHED_FIFO||task->scheduling.policy==KERNEL_SCHED_RR)return;
    uint64_t advance=kernel_sched_fair_delta(&task->scheduling,delta);
    if(advance>UINT64_MAX-task->ready_node.fair.key)__builtin_trap();
    task->ready_node.fair.key+=advance;
    uint32_t used=task->scheduling.fair_slice_used;
    task->scheduling.fair_slice_used=delta>=KERNEL_SCHED_FAIR_MAX_NS-used
        ? KERNEL_SCHED_FAIR_MAX_NS : used+(uint32_t)delta;
}
void scheduler_cpu_time_switch(struct kernel_task *previous,struct kernel_task *next)
{
#if BOAROS_USER_SMP
    struct kernel_cpu *cpu=kernel_cpu_current();
    KERNEL_RAW_SCOPE(guard,kernel_run_lock(cpu));
#else
    KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
    struct kernel_cpu *cpu=kernel_cpu_current();
#endif
    uint64_t now=accounting_counter();
    if (!cpu->accounting_owner) {
        cpu->accounting_owner=previous;
        cpu->accounting_started_ticks=now;
        cpu->accounting_dispatch_ticks=now;
    }
    if (cpu->accounting_owner!=previous) __builtin_trap();
    accounting_write_begin(cpu);
    account_time(cpu,now);
    /* task保存已结束执行片段的纳秒值；本CPU的硬件tick只在查询/交接换算。 */
    previous->usage.user_ns+=kernel_time_ticks_to_ns(cpu->accounting_user_ticks);
    previous->usage.kernel_ns+=kernel_time_ticks_to_ns(cpu->accounting_kernel_ticks);
    if(!previous->idle)scheduler_fair_account(previous,previous->usage.user_ns+previous->usage.kernel_ns);
    __atomic_store_n(&cpu->accounting_user_ticks,0,__ATOMIC_RELAXED);
    __atomic_store_n(&cpu->accounting_kernel_ticks,0,__ATOMIC_RELAXED);
    accounting_write_end(cpu);
    /* current已发布next，旧栈借用覆盖previous，真正切栈前才转移owner。 */
    if (previous->arch.user_mode) {
        if (previous->state==KERNEL_THREAD_STATE_BLOCKED || previous->state==KERNEL_THREAD_STATE_STOPPED ||
            previous->state==KERNEL_THREAD_STATE_EXITED) previous->usage.voluntary++;
        else previous->usage.involuntary++;
    }
    /* PERF_COUNT_SW_CPU_CLOCK includes the whole dispatched interval. User
     * and system time above deliberately exclude interrupt handling. */
    previous->scheduled_ticks+=now-cpu->accounting_dispatch_ticks;
    cpu->accounting_dispatch_ticks=now;
    cpu->accounting_owner=next;
}

int kernel_task_perf_clock_authorize(const struct kernel_task *caller,
    const struct kernel_task *target, int perfmon)
{
    KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
    if (!target->accounted || target->retirement || !target->arch.user_mode ||
        target->state==KERNEL_THREAD_STATE_EXITED ||
        target->state==KERNEL_THREAD_STATE_ZOMBIE ||
        target->state==KERNEL_THREAD_STATE_GROUP_DEAD) return -KERNEL_ESRCH;
    return perfmon || kernel_credentials_may_access_locked(caller,target,0)
        ? 0 : -KERNEL_EACCES;
}

uint64_t kernel_task_scheduled_ns(const struct kernel_task *task)
{
    /* The context switch publishes the dispatch timestamp and owner under
     * the same protection. The hardware clock advances between switches. */
    KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
    KERNEL_RUN_SET(runs);
    uint64_t elapsed=task->scheduled_ticks;
    for (struct kernel_cpu *cpu=scheduler.cpus;cpu;cpu=cpu->scheduler_next) {
        if (cpu->accounting_owner!=task) continue;
        elapsed+=accounting_counter()-cpu->accounting_dispatch_ticks;
        break;
    }
    return kernel_time_ticks_to_ns(elapsed);
}

void scheduler_task_cpu_time_locked(const struct kernel_task *task,uint64_t *user,uint64_t *system)
{
    KERNEL_RUN_SET(runs);
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
    uint64_t now = accounting_counter();
    if (user) account_interval(cpu, now, &cpu->accounting_user_ticks);
    else account_time(cpu, now);
    /* Synchronous traps stay in kernel accounting through return preparation.
     * Only an interrupt needs a saved mode and an end-of-handler transition. */
    if (interrupt) {
        scope->task = task;
        scope->restore_mode = user ? ACCOUNT_KERNEL : task->accounting_mode;
    }
    __atomic_store_n(&task->accounting_mode,interrupt ? ACCOUNT_INTERRUPT : ACCOUNT_KERNEL,__ATOMIC_RELAXED);
    accounting_write_end(cpu);
}
void kernel_cpu_account_interrupt_end(struct kernel_cpu_account_scope *scope)
{
    if (scope->task->accounting_mode==scope->restore_mode) return;
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
    account_interval(cpu, accounting_counter(), &cpu->accounting_kernel_ticks);
    __atomic_store_n(&cpu->accounting_owner->accounting_mode,ACCOUNT_USER,__ATOMIC_RELAXED);
    accounting_write_end(cpu);
}

void scheduler_usage_add(struct kernel_task_usage *to, const struct kernel_task_usage *from)
{
    to->user_ns += from->user_ns; to->kernel_ns += from->kernel_ns;
    to->minor_faults += from->minor_faults; to->major_faults += from->major_faults;
    to->voluntary += from->voluntary; to->involuntary += from->involuntary;
    to->read_bytes += from->read_bytes; to->write_bytes += from->write_bytes;
    to->rchar += from->rchar; to->wchar += from->wchar;
    to->syscr += from->syscr; to->syscw += from->syscw;
    to->cancelled_write_bytes += from->cancelled_write_bytes;
    if (to->maxrss_pages < from->maxrss_pages) to->maxrss_pages = from->maxrss_pages;
}

void scheduler_usage_thread_locked(const struct kernel_task *task, struct kernel_task_usage *usage)
{
    KERNEL_RUN_SET(runs);
    *usage = (struct kernel_task_usage){
        .minor_faults = __atomic_load_n(&task->usage.minor_faults, __ATOMIC_RELAXED),
        .major_faults = __atomic_load_n(&task->usage.major_faults, __ATOMIC_RELAXED),
        .read_bytes = __atomic_load_n(&task->usage.read_bytes, __ATOMIC_RELAXED),
        .write_bytes = __atomic_load_n(&task->usage.write_bytes, __ATOMIC_RELAXED),
        .rchar = __atomic_load_n(&task->usage.rchar, __ATOMIC_RELAXED),
        .wchar = __atomic_load_n(&task->usage.wchar, __ATOMIC_RELAXED),
        .syscr = __atomic_load_n(&task->usage.syscr, __ATOMIC_RELAXED),
        .syscw = __atomic_load_n(&task->usage.syscw, __ATOMIC_RELAXED),
        .cancelled_write_bytes = __atomic_load_n(&task->usage.cancelled_write_bytes, __ATOMIC_RELAXED),
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
        *usage = leader->limits ? leader->limits->dead_usage : (struct kernel_task_usage){0};
        if (children && leader->limits) scheduler_usage_add(usage, &leader->limits->child_usage);
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
                *usage = leader->limits ? leader->limits->dead_usage : (struct kernel_task_usage){0};
                if (children && leader->limits) scheduler_usage_add(usage, &leader->limits->child_usage);
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
        if (who == -1) { *usage = leader->limits ? leader->limits->child_usage : (struct kernel_task_usage){0}; return; }
        if (who == 1) {
            scheduler_usage_thread_locked(task, usage);
            uint64_t peak = leader->limits ? leader->limits->dead_usage.maxrss_pages : 0;
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
void kernel_task_account_cancelled_write(uint64_t bytes)
{
    struct kernel_task *task = io_account_task();
    if (task) __atomic_fetch_add(&task->usage.cancelled_write_bytes, bytes, __ATOMIC_RELAXED);
}

void kernel_task_account_io_return(struct kernel_task *task, int64_t completed, unsigned directions)
{
    if (completed < 0 && !(directions & KERNEL_IO_ACCOUNT_VECTOR)) return;
    /* Only the task returning this system call writes its character and call
     * counters. Remote proc queries take atomic snapshots of these values. */
#define ACCOUNT_CALL(field, value) \
    __atomic_store_n(&task->usage.field, \
        __atomic_load_n(&task->usage.field, __ATOMIC_RELAXED) + (value), __ATOMIC_RELAXED)
    if (directions & KERNEL_IO_ACCOUNT_READ) {
        if (completed > 0) ACCOUNT_CALL(rchar, (uint64_t)completed);
        ACCOUNT_CALL(syscr, 1);
    }
    if (directions & KERNEL_IO_ACCOUNT_WRITE) {
        if (completed > 0) ACCOUNT_CALL(wchar, (uint64_t)completed);
        ACCOUNT_CALL(syscw, 1);
    }
#undef ACCOUNT_CALL
}
uint64_t kernel_task_read_bytes(const struct kernel_task *task)
{ return task ? __atomic_load_n(&task->usage.read_bytes, __ATOMIC_RELAXED) : 0; }
void kernel_task_account_fault(struct kernel_task *task, int major)
{
    if (!task || !task->arch.user_mode) return;
    __atomic_fetch_add(major ? &task->usage.major_faults : &task->usage.minor_faults,
                       1, __ATOMIC_RELAXED);
}
