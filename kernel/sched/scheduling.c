#include "private.h"
#include <arch/timer.h>
#include <kernel/errno.h>
#include <kernel/time.h>

static int is_rt(const struct kernel_task *task)
{
    return task && task != &scheduler.idle && task->scheduling.priority != 0;
}

void scheduler_account_runtime(void)
{
#if BOAROS_COST_DIAGNOSTICS
    if (kernel_cpu_current()->current) kernel_cost_account(&kernel_cpu_current()->current->cost);
#endif
    uint64_t now = kernel_time_monotonic_ns();
    uint64_t delta = now - scheduler.rt_bandwidth.last_account_ns;
    if (kernel_cpu_current()->current && kernel_cpu_current()->current != &scheduler.idle)
        kernel_sched_policy_charge(&kernel_cpu_current()->current->scheduling, delta);
    kernel_rt_bandwidth_account(&scheduler.rt_bandwidth, now, is_rt(kernel_cpu_current()->current));
}

struct kernel_task *ready_first(void)
{ return scheduler.runqueue.first ? scheduler.runqueue.first->owner : 0; }
struct kernel_task *ready_next(const struct kernel_task *task)
{ return task->ready_node.next ? task->ready_node.next->owner : 0; }
struct kernel_task *ready_best_locked(void)
{
    struct kernel_sched_node *node = kernel_sched_pick(&scheduler.runqueue,
        kernel_rt_bandwidth_eligible(&scheduler.rt_bandwidth));
    return node ? node->owner : 0;
}
void ready_remove_locked(struct kernel_task *task)
{ kernel_sched_dequeue(&scheduler.runqueue, &task->ready_node); }
void ready_enqueue_locked(struct kernel_task *task, int head)
{
#if BOAROS_COST_DIAGNOSTICS
    kernel_cost_ready(&task->cost);
#endif
    if (!task->accounted) {
        task->accounted = 1;
        task->all_next = scheduler.all_tasks;
        scheduler.all_tasks = task;
    }
    task->next = 0;
    kernel_sched_enqueue(&scheduler.runqueue, &task->ready_node, task,
                         (unsigned)task->scheduling.priority, head);
    if (!kernel_cpu_current()->current || kernel_cpu_current()->current == &scheduler.idle ||
        task->scheduling.priority > kernel_cpu_current()->current->scheduling.priority)
        kernel_cpu_request_schedule(kernel_cpu_current());
}
struct kernel_task *ready_best(void)
{ KERNEL_RAW_SCOPE(guard, &kernel_wait_domain); return ready_best_locked(); }
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
struct kernel_task *ready_pop(void)
{
    struct kernel_task *task = ready_best();
    if (task) ready_remove(task);
    return task;
}

void scheduler_rearm_timer(void)
{
    uint64_t delay = kernel_rt_bandwidth_delay(&scheduler.rt_bandwidth,
        is_rt(kernel_cpu_current()->current),
        scheduler.runqueue.first && scheduler.runqueue.first->priority != 0);
    if (kernel_cpu_current()->current && kernel_cpu_current()->current != &scheduler.idle &&
        kernel_cpu_current()->current->scheduling.policy == KERNEL_SCHED_RR) {
        uint64_t slice = kernel_cpu_current()->current->scheduling.rr_remaining_ns;
        if (!slice) slice = 1;
        if (!delay || slice < delay) delay = slice;
    }
    uint64_t deadline = 0;
    if (delay && kernel_time_is_initialized()) {
        uint64_t now = scheduler.rt_bandwidth.last_account_ns;
        uint64_t target = UINT64_MAX - now < delay ? UINT64_MAX : now + delay;
        enum kernel_time_status status = kernel_time_deadline_from_monotonic(target, &deadline);
        if (status == KERNEL_TIME_STATUS_DEADLINE_PASSED) deadline = arch_time_read() + 1U;
        else if (status != KERNEL_TIME_STATUS_OK) __builtin_trap();
    }
    /* 最早阻塞期限与 RT/时间片预算同为 tick 域；取最小者编入硬件事件。 */
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        struct kernel_task *blocked = deadline_index_first();
        if (blocked != 0 && (!deadline || blocked->wakeup_deadline < deadline))
            deadline = blocked->wakeup_deadline;
    }
    scheduler.armed_deadline = deadline;
    enum arch_timer_status status = arch_timer_set_scheduler_deadline(deadline);
    if (status != ARCH_TIMER_STATUS_OK && status != ARCH_TIMER_STATUS_NOT_STARTED)
        __builtin_trap();
}

enum kernel_scheduler_status scheduler_reschedule(int rotate_other, int voluntary)
{
    scheduler_account_runtime();
    if (kernel_cpu_current()->preempt_depth) {
        if (voluntary) __builtin_trap();
        kernel_cpu_request_schedule(kernel_cpu_current());
        kernel_cpu_current()->rotate_other |= rotate_other != 0;
        scheduler_rearm_timer();
        return KERNEL_SCHEDULER_STATUS_OK;
    }
    (void)kernel_cpu_consume_schedule(kernel_cpu_current());
    rotate_other |= kernel_cpu_current()->rotate_other;
    kernel_cpu_current()->rotate_other = 0;
    struct kernel_task *current = kernel_cpu_current()->current;
    {
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    struct kernel_task *next = ready_best_locked();
    int throttled = is_rt(current) && !kernel_rt_bandwidth_eligible(&scheduler.rt_bandwidth);
    int expired = current != &scheduler.idle && kernel_sched_policy_expired(&current->scheduling);
    int higher = next && (current == &scheduler.idle ||
                         next->scheduling.priority > current->scheduling.priority);
    int rotate = voluntary || expired ||
                 (rotate_other && current->scheduling.policy == KERNEL_SCHED_OTHER);
    if (expired) kernel_sched_policy_rotate(&current->scheduling);
    if (!throttled && !higher && !(rotate && next)) {
        kernel_raw_lock_release(&guard);
        scheduler_rearm_timer();
        return KERNEL_SCHEDULER_STATUS_OK;
    }
    if (current != &scheduler.idle) {
        current->state = KERNEL_THREAD_STATE_READY;
        /* 高优先级抢占/节流保留同级队首；只有yield/时间片结束移到队尾。 */
        if (current->wait.ready_pending) __builtin_trap();
        current->wait.ready_pending = 1;
        current->wait.ready_head = !rotate;
    } else scheduler.idle_context_saved = 1U;
    }
    return scheduler_switch_current_away(current);
}

void kernel_scheduler_prepare_idle_return(void)
{
    if (scheduler.initialized != KERNEL_SCHEDULER_INITIALIZED ||
        kernel_cpu_current()->current != &scheduler.idle || !kernel_cpu_schedule_requested(kernel_cpu_current())) return;
    /* 仅空闲栈是 IRQ 返回的内核抢占点；启动/清理持锁期间继续延后。 */
    if (scheduler.idle.io_context.locks || scheduler.idle.io_context.backend_depth) return;
    if (scheduler_reschedule(0, 0) != KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
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
    uintptr_t irq = arch_interrupt_save();
    struct kernel_task *task = sched_target(caller, pid);
    if (task) *state = task->scheduling;
    arch_interrupt_restore(irq);
    return task ? 0 : -KERNEL_ESRCH;
}

int kernel_task_sched_set(struct kernel_task *caller, int32_t pid,
                           int policy, int priority, int keep_policy)
{
    if (pid < 0) return -KERNEL_EINVAL;
    uintptr_t irq = arch_interrupt_save();
    struct kernel_task *task = sched_target(caller, pid);
    if (!task) { arch_interrupt_restore(irq); return -KERNEL_ESRCH; }
    scheduler_account_runtime();
    struct kernel_sched_policy updated = task->scheduling;
    int result = kernel_sched_policy_set(&updated, policy, priority, keep_policy);
    if (!result) {
        int old_priority = task->scheduling.priority;
        int move = updated.priority != old_priority;
        int queued = task->ready_node.queued;
        if (move && queued) ready_remove(task);
        task->scheduling = updated;
        if (move && queued) ready_enqueue(task, updated.priority < old_priority);
        kernel_cpu_request_schedule(kernel_cpu_current());
        scheduler_rearm_timer();
    }
    arch_interrupt_restore(irq);
    return result;
}

void kernel_scheduler_rt_bandwidth_get(int64_t *period_us, int64_t *runtime_us)
{
    uintptr_t irq = arch_interrupt_save();
    if (scheduler.initialized != KERNEL_SCHEDULER_INITIALIZED) {
        *period_us = 1000000; *runtime_us = 950000;
    } else {
        *period_us = (int64_t)(scheduler.rt_bandwidth.period_ns / 1000U);
        *runtime_us = scheduler.rt_bandwidth.runtime_ns < 0 ? -1 :
                      scheduler.rt_bandwidth.runtime_ns / 1000;
    }
    arch_interrupt_restore(irq);
}

int kernel_scheduler_rt_bandwidth_set(int runtime_field, int64_t value)
{
    uintptr_t irq = arch_interrupt_save();
    if (scheduler.initialized != KERNEL_SCHEDULER_INITIALIZED) {
        arch_interrupt_restore(irq); return -KERNEL_EAGAIN;
    }
    scheduler_account_runtime();
    int result = kernel_rt_bandwidth_set(&scheduler.rt_bandwidth, runtime_field, value,
        scheduler.rt_bandwidth.last_account_ns, 0);
    if (!result) {
        kernel_cpu_request_schedule(kernel_cpu_current());
        scheduler_rearm_timer();
    }
    arch_interrupt_restore(irq);
    return result;
}
