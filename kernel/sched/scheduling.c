#include "private.h"
#include <arch/timer.h>
#include <kernel/errno.h>
#include <kernel/time.h>

static int is_rt(const struct kernel_task *task)
{
    return task && !task->idle && task->scheduling.priority != 0;
}

void scheduler_cpu_initialize(struct kernel_cpu *cpu, struct kernel_task *idle,
                              uint64_t now_ns)
{
    if (!cpu || !idle || cpu->initialized != KERNEL_CPU_INITIALIZED || cpu->idle ||
        !scheduler.rt_configuration.period_ns) __builtin_trap();
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    for (struct kernel_cpu *p = scheduler.cpus; p; p = p->scheduler_next)
        if (p == cpu || p->hardware_id == cpu->hardware_id) __builtin_trap();
    /* CPU记录在在线期常驻；私有初始化后才发布到调度域。 */
    cpu->rt_bandwidth.period_ns = scheduler.rt_configuration.period_ns;
    cpu->rt_bandwidth.runtime_ns = scheduler.rt_configuration.runtime_ns;
    cpu->rt_bandwidth.period_start_ns = cpu->rt_bandwidth.last_account_ns = now_ns;
    cpu->rt_bandwidth.consumed_ns = 0;
    cpu->idle = idle;
    idle->cpu = cpu;
    idle->idle = 1;
    cpu->scheduler_next = scheduler.cpus;
    scheduler.cpus = cpu;
}

static void account_cpu(struct kernel_cpu *cpu, uint64_t now)
{
#if BOAROS_COST_DIAGNOSTICS
    if (cpu->current) kernel_cost_account(&cpu->current->cost);
#endif
    if (now < cpu->rt_bandwidth.last_account_ns) __builtin_trap();
    uint64_t delta = now - cpu->rt_bandwidth.last_account_ns;
    if (cpu->current && !cpu->current->idle)
        kernel_sched_policy_charge(&cpu->current->scheduling, delta);
    kernel_rt_bandwidth_account(&cpu->rt_bandwidth, now, is_rt(cpu->current));
}

void scheduler_account_runtime_locked(void)
{ account_cpu(kernel_cpu_current(), kernel_time_monotonic_ns()); }

void scheduler_account_runtime(void)
{
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    scheduler_account_runtime_locked();
}

struct kernel_task *ready_first(void)
{ return scheduler.runqueue.first ? scheduler.runqueue.first->owner : 0; }
struct kernel_task *ready_next(const struct kernel_task *task)
{ return task->ready_node.next ? task->ready_node.next->owner : 0; }
struct kernel_task *ready_best_locked(void)
{
    struct kernel_sched_node *node = kernel_sched_pick(&scheduler.runqueue,
        kernel_rt_bandwidth_eligible(&kernel_cpu_current()->rt_bandwidth));
    return node ? node->owner : 0;
}
void ready_remove_locked(struct kernel_task *task)
{ kernel_sched_dequeue(&scheduler.runqueue, &task->ready_node); }
void ready_enqueue_locked(struct kernel_task *task, int head)
{
#if BOAROS_COST_DIAGNOSTICS
    kernel_cost_ready(&task->cost);
#endif
    if (!task->accounted) scheduler_task_publish_locked(task);
    task->next = 0;
    kernel_sched_enqueue(&scheduler.runqueue, &task->ready_node, task,
                         (unsigned)task->scheduling.priority, head);
    struct kernel_cpu *target = task->cpu ? task->cpu : kernel_cpu_current();
    if (!target->current || target->current->idle ||
        task->scheduling.priority > target->current->scheduling.priority)
        kernel_cpu_request_schedule(target);
}
void ready_remove(struct kernel_task *task)
{ KERNEL_RAW_SCOPE(guard, &kernel_wait_domain); ready_remove_locked(task); }
void ready_enqueue(struct kernel_task *task, int head)
{
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    if (task->wait.on_cpu) {
        if (task->wait.ready_pending) __builtin_trap();
        task->wait.ready_pending = 1;
        task->wait.ready_head = head;
    } else ready_enqueue_locked(task, head);
}
void ready_append(struct kernel_task *task) { ready_enqueue(task, 0); }
static struct kernel_task *claim_next_locked(struct kernel_cpu *cpu,
    struct kernel_task *previous, struct kernel_task *next)
{
    if (!cpu || cpu != kernel_cpu_current() || !previous || cpu->current != previous ||
        cpu->switch_previous || !previous->wait.on_cpu || !cpu->raw_locks ||
        cpu->raw_locks->lock != &kernel_wait_domain) __builtin_trap();
    if (!next) next = cpu->idle;
    if (next == previous) return 0;
    if (!next || next->wait.on_cpu || (next->idle && next->cpu != cpu)) __builtin_trap();
    if (!next->idle) {
        ready_remove_locked(next);
        next->state = KERNEL_THREAD_STATE_RUNNING;
    }
    /* 摘队列与取得执行owner是同一发布点，旧owner由新栈交接释放。 */
    next->wait.on_cpu = 1;
    next->cpu = cpu;
    cpu->current = next;
    cpu->switch_previous = previous;
    cpu->rotate_other = 0;
    return next;
}

struct kernel_task *scheduler_select_next_locked(struct kernel_cpu *cpu,
    struct kernel_task *previous, int ready_allowed)
{
    return claim_next_locked(cpu, previous, ready_allowed ? ready_best_locked() : 0);
}

void scheduler_rearm_timer(void)
{
    uintptr_t irq = arch_interrupt_save();
    struct kernel_cpu *cpu = kernel_cpu_current();
    uint64_t deadline;
    {
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    uint64_t delay = kernel_rt_bandwidth_delay(&cpu->rt_bandwidth,
        is_rt(cpu->current),
        scheduler.runqueue.first && scheduler.runqueue.first->priority != 0);
    if (cpu->current && cpu->current != cpu->idle &&
        cpu->current->scheduling.policy == KERNEL_SCHED_RR) {
        uint64_t slice = cpu->current->scheduling.rr_remaining_ns;
        if (!slice) slice = 1;
        if (!delay || slice < delay) delay = slice;
    }
    deadline = 0;
    if (delay && kernel_time_is_initialized()) {
        uint64_t now = cpu->rt_bandwidth.last_account_ns;
        uint64_t target = UINT64_MAX - now < delay ? UINT64_MAX : now + delay;
        enum kernel_time_status status = kernel_time_deadline_from_monotonic(target, &deadline);
        if (status == KERNEL_TIME_STATUS_DEADLINE_PASSED) deadline = arch_time_read() + 1U;
        else if (status != KERNEL_TIME_STATUS_OK) __builtin_trap();
    }
    /* 最早阻塞期限与 RT/时间片预算同为 tick 域；取最小者编入硬件事件。 */
    struct kernel_task *blocked = deadline_index_first();
    if (blocked != 0 && (!deadline || blocked->wakeup_deadline < deadline))
        deadline = blocked->wakeup_deadline;
    cpu->armed_deadline = deadline;
    }
    enum arch_timer_status status = arch_timer_set_scheduler_deadline(deadline);
    if (status != ARCH_TIMER_STATUS_OK && status != ARCH_TIMER_STATUS_NOT_STARTED)
        __builtin_trap();
    arch_interrupt_restore(irq);
}

enum kernel_scheduler_status scheduler_reschedule(int rotate_other, int voluntary, int check_queues)
{
    /* 调用者保持IRQ关闭；CPU借用在切换前结束，不跨睡眠或恢复点。 */
    struct kernel_cpu *cpu = kernel_cpu_current();
    if (cpu->preempt_depth) {
        {
            KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
            if (check_queues) {
                enum kernel_scheduler_status status = validate_queues_locked();
                if (status != KERNEL_SCHEDULER_STATUS_OK) return status;
            }
            scheduler_account_runtime_locked();
        }
        if (voluntary) __builtin_trap();
        kernel_cpu_request_schedule(cpu);
        cpu->rotate_other |= rotate_other != 0;
        scheduler_rearm_timer();
        return KERNEL_SCHEDULER_STATUS_OK;
    }
    struct kernel_task *current = cpu->current;
    struct kernel_task *selected;
    {
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    if (check_queues) {
        enum kernel_scheduler_status status = validate_queues_locked();
        if (status != KERNEL_SCHEDULER_STATUS_OK) return status;
    }
    (void)kernel_cpu_consume_schedule(cpu);
    rotate_other |= cpu->rotate_other;
    cpu->rotate_other = 0;
    scheduler_account_runtime_locked();
    struct kernel_task *next = ready_best_locked();
    int throttled = is_rt(current) && !kernel_rt_bandwidth_eligible(&cpu->rt_bandwidth);
    int expired = current != cpu->idle && kernel_sched_policy_expired(&current->scheduling);
    int higher = next && (current == cpu->idle ||
                         next->scheduling.priority > current->scheduling.priority);
    int rotate = voluntary || expired ||
                 (rotate_other && current->scheduling.policy == KERNEL_SCHED_OTHER);
    /* 当前任务尚未入队；yield/片尾只可轮转同级，不能把低优先级当成替代者。 */
    int same_priority = next && next->scheduling.priority == current->scheduling.priority;
    if (expired) kernel_sched_policy_rotate(&current->scheduling);
    if (!throttled && !higher && !(rotate && same_priority)) {
        kernel_raw_lock_release(&guard);
        scheduler_rearm_timer();
        return KERNEL_SCHEDULER_STATUS_OK;
    }
    if (current != cpu->idle) {
        current->state = KERNEL_THREAD_STATE_READY;
        /* 高优先级抢占/节流保留同级队首；只有yield/时间片结束移到队尾。 */
        if (current->wait.ready_pending) __builtin_trap();
        current->wait.ready_pending = 1;
        current->wait.ready_head = !rotate;
    } else cpu->idle_context_saved = 1U;
    selected = claim_next_locked(cpu, current, next);
    }
    if (!selected) __builtin_trap();
    return scheduler_switch_claimed(current, selected);
}

void kernel_scheduler_prepare_idle_return(void)
{
    if (scheduler.initialized != KERNEL_SCHEDULER_INITIALIZED ||
        kernel_cpu_current()->current != kernel_cpu_current()->idle || !kernel_cpu_schedule_requested(kernel_cpu_current())) return;
    /* 仅空闲栈是 IRQ 返回的内核抢占点；启动/清理持锁期间继续延后。 */
    if (kernel_cpu_current()->idle->io_context.locks || kernel_cpu_current()->idle->io_context.backend_depth) return;
    if (scheduler_reschedule(0, 0, 0) != KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
}

static struct kernel_task *sched_target(struct kernel_task *caller, int32_t pid)
{
    struct kernel_task *task = pid ? process_find_identity(pid, KERNEL_PID_TID) : caller;
    return task && task->accounted && task->arch.user_mode && task->tid_owned ? task : 0;
}

int kernel_task_sched_get(struct kernel_task *caller, int32_t pid,
                           struct kernel_sched_policy *state)
{
    if (pid < 0) return -KERNEL_EINVAL;
    int result = -KERNEL_ESRCH;
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        struct kernel_task *task = sched_target(caller, pid);
        if (task) { *state = task->scheduling; result = 0; }
    }
    return result;
}

int kernel_task_sched_set(struct kernel_task *caller, int32_t pid,
                           int policy, int priority, int keep_policy)
{
    if (pid < 0) return -KERNEL_EINVAL;
    uintptr_t irq = arch_interrupt_save();
    int result = -KERNEL_ESRCH;
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        struct kernel_task *task = sched_target(caller, pid);
        if (task) {
            struct kernel_cpu *cpu = kernel_cpu_current();
            uint64_t now = kernel_time_monotonic_ns();
            account_cpu(cpu, now);
            if (task->wait.on_cpu && task->cpu != cpu) {
                if (!task->cpu || task->cpu->current != task) __builtin_trap();
                account_cpu(task->cpu, now);
            }
            struct kernel_sched_policy updated = task->scheduling;
            result = kernel_sched_policy_set(&updated, policy, priority, keep_policy);
            if (!result) {
                int old_priority = task->scheduling.priority;
                int move = updated.priority != old_priority;
                int queued = task->ready_node.queued;
                if (move && queued) ready_remove_locked(task);
                task->scheduling = updated;
                if (move && queued) ready_enqueue_locked(task, updated.priority < old_priority);
                kernel_cpu_request_schedule(task->cpu ? task->cpu : cpu);
            }
        }
    }
    if (!result) scheduler_rearm_timer();
    arch_interrupt_restore(irq);
    return result;
}

void kernel_scheduler_rt_bandwidth_get(int64_t *period_us, int64_t *runtime_us)
{
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    if (scheduler.initialized != KERNEL_SCHEDULER_INITIALIZED) {
        *period_us = 1000000; *runtime_us = 950000;
    } else {
        *period_us = (int64_t)(scheduler.rt_configuration.period_ns / 1000U);
        *runtime_us = scheduler.rt_configuration.runtime_ns < 0 ? -1 :
                      scheduler.rt_configuration.runtime_ns / 1000;
    }
}

int kernel_scheduler_rt_bandwidth_set(int runtime_field, int64_t value)
{
    uintptr_t irq = arch_interrupt_save();
    if (scheduler.initialized != KERNEL_SCHEDULER_INITIALIZED) {
        arch_interrupt_restore(irq); return -KERNEL_EAGAIN;
    }
    int result;
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        uint64_t now = kernel_time_monotonic_ns();
        uint64_t old_period = scheduler.rt_configuration.period_ns;
        for (struct kernel_cpu *cpu = scheduler.cpus; cpu; cpu = cpu->scheduler_next)
            account_cpu(cpu, now);
        struct kernel_rt_bandwidth proposed = {
            .period_ns = scheduler.rt_configuration.period_ns,
            .runtime_ns = scheduler.rt_configuration.runtime_ns,
            .period_start_ns = now, .last_account_ns = now
        };
        result = kernel_rt_bandwidth_set(&proposed, runtime_field, value, now, 0);
        if (!result) scheduler.rt_configuration = (struct kernel_rt_configuration){
            .period_ns = proposed.period_ns, .runtime_ns = proposed.runtime_ns };
        if (!result) {
            for (struct kernel_cpu *cpu = scheduler.cpus; cpu; cpu = cpu->scheduler_next) {
                cpu->rt_bandwidth.period_ns = scheduler.rt_configuration.period_ns;
                cpu->rt_bandwidth.runtime_ns = scheduler.rt_configuration.runtime_ns;
                /* 改配置不赠送预算；只有周期长度变化才重建周期起点。 */
                if (old_period != scheduler.rt_configuration.period_ns)
                    cpu->rt_bandwidth.period_start_ns = now;
                kernel_cpu_request_schedule(cpu);
            }
        }
    }
    if (!result) scheduler_rearm_timer();
    arch_interrupt_restore(irq);
    return result;
}
