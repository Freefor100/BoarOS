#include "private.h"
#include <arch/riscv/timer.h>
#include <kernel/errno.h>
#include <kernel/time.h>

static int is_rt(const struct kernel_task *task)
{
    return task && task != &scheduler.idle && task->scheduling.priority != 0;
}

void scheduler_account_runtime(void)
{
#if BOAROS_COST_DIAGNOSTICS
    if (scheduler.current) kernel_cost_account(&scheduler.current->cost);
#endif
    uint64_t now = kernel_time_monotonic_ns();
    uint64_t delta = now - scheduler.rt_bandwidth.last_account_ns;
    if (scheduler.current && scheduler.current != &scheduler.idle)
        kernel_sched_policy_charge(&scheduler.current->scheduling, delta);
    kernel_rt_bandwidth_account(&scheduler.rt_bandwidth, now, is_rt(scheduler.current));
}

struct kernel_task *ready_first(void)
{ return scheduler.runqueue.first ? scheduler.runqueue.first->owner : 0; }
struct kernel_task *ready_next(const struct kernel_task *task)
{ return task->ready_node.next ? task->ready_node.next->owner : 0; }
struct kernel_task *ready_best(void)
{
    struct kernel_sched_node *node = kernel_sched_pick(&scheduler.runqueue,
        kernel_rt_bandwidth_eligible(&scheduler.rt_bandwidth));
    return node ? node->owner : 0;
}
void ready_remove(struct kernel_task *task)
{ kernel_sched_dequeue(&scheduler.runqueue, &task->ready_node); }
void ready_enqueue(struct kernel_task *task, int head)
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
    if (!scheduler.current || scheduler.current == &scheduler.idle ||
        task->scheduling.priority > scheduler.current->scheduling.priority)
        scheduler.need_resched = 1;
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
        is_rt(scheduler.current),
        scheduler.runqueue.first && scheduler.runqueue.first->priority != 0);
    if (scheduler.current && scheduler.current != &scheduler.idle &&
        scheduler.current->scheduling.policy == KERNEL_SCHED_RR) {
        uint64_t slice = scheduler.current->scheduling.rr_remaining_ns;
        if (!slice) slice = 1;
        if (!delay || slice < delay) delay = slice;
    }
    uint64_t deadline = 0;
    if (delay && kernel_time_is_initialized()) {
        uint64_t now = scheduler.rt_bandwidth.last_account_ns;
        uint64_t target = UINT64_MAX - now < delay ? UINT64_MAX : now + delay;
        enum kernel_time_status status = kernel_time_deadline_from_monotonic(target, &deadline);
        if (status == KERNEL_TIME_STATUS_DEADLINE_PASSED) deadline = riscv_time_read() + 1U;
        else if (status != KERNEL_TIME_STATUS_OK) __builtin_trap();
    }
    /* 最早阻塞期限与 RT/时间片预算同为 tick 域；取最小者编入硬件事件。 */
    struct kernel_task *blocked = deadline_index_first();
    if (blocked != 0 && (!deadline || blocked->wakeup_deadline < deadline))
        deadline = blocked->wakeup_deadline;
    scheduler.armed_deadline = deadline;
    enum riscv_timer_status status = riscv_timer_set_scheduler_deadline(deadline);
    if (status != RISCV_TIMER_STATUS_OK && status != RISCV_TIMER_STATUS_NOT_STARTED)
        __builtin_trap();
}

enum kernel_scheduler_status scheduler_reschedule(int rotate_other, int voluntary)
{
    scheduler_account_runtime();
    struct kernel_task *current = scheduler.current;
    struct kernel_task *next = ready_best();
    int throttled = is_rt(current) && !kernel_rt_bandwidth_eligible(&scheduler.rt_bandwidth);
    int expired = current != &scheduler.idle && kernel_sched_policy_expired(&current->scheduling);
    int higher = next && (current == &scheduler.idle ||
                         next->scheduling.priority > current->scheduling.priority);
    int rotate = voluntary || expired ||
                 (rotate_other && current->scheduling.policy == KERNEL_SCHED_OTHER);
    if (expired) kernel_sched_policy_rotate(&current->scheduling);
    scheduler.need_resched = 0;
    if (!throttled && !higher && !(rotate && next)) {
        scheduler_rearm_timer();
        return KERNEL_SCHEDULER_STATUS_OK;
    }
    if (current != &scheduler.idle) {
        current->state = KERNEL_THREAD_STATE_READY;
        /* 高优先级抢占/节流保留同级队首；只有yield/时间片结束移到队尾。 */
        ready_enqueue(current, !rotate);
    } else scheduler.idle_context_saved = 1U;
    return scheduler_switch_current_away(current);
}

void kernel_scheduler_prepare_idle_return(void)
{
    if (scheduler.initialized != KERNEL_SCHEDULER_INITIALIZED ||
        scheduler.current != &scheduler.idle || !scheduler.need_resched) return;
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
    uintptr_t irq = riscv_interrupt_save();
    struct kernel_task *task = sched_target(caller, pid);
    if (task) *state = task->scheduling;
    riscv_interrupt_restore(irq);
    return task ? 0 : -KERNEL_ESRCH;
}

int kernel_task_sched_set(struct kernel_task *caller, int32_t pid,
                           int policy, int priority, int keep_policy)
{
    if (pid < 0) return -KERNEL_EINVAL;
    uintptr_t irq = riscv_interrupt_save();
    struct kernel_task *task = sched_target(caller, pid);
    if (!task) { riscv_interrupt_restore(irq); return -KERNEL_ESRCH; }
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
        scheduler.need_resched = 1;
        scheduler_rearm_timer();
    }
    riscv_interrupt_restore(irq);
    return result;
}

void kernel_scheduler_rt_bandwidth_get(int64_t *period_us, int64_t *runtime_us)
{
    uintptr_t irq = riscv_interrupt_save();
    if (scheduler.initialized != KERNEL_SCHEDULER_INITIALIZED) {
        *period_us = 1000000; *runtime_us = 950000;
    } else {
        *period_us = (int64_t)(scheduler.rt_bandwidth.period_ns / 1000U);
        *runtime_us = scheduler.rt_bandwidth.runtime_ns < 0 ? -1 :
                      scheduler.rt_bandwidth.runtime_ns / 1000;
    }
    riscv_interrupt_restore(irq);
}

int kernel_scheduler_rt_bandwidth_set(int runtime_field, int64_t value)
{
    uintptr_t irq = riscv_interrupt_save();
    if (scheduler.initialized != KERNEL_SCHEDULER_INITIALIZED) {
        riscv_interrupt_restore(irq); return -KERNEL_EAGAIN;
    }
    scheduler_account_runtime();
    int result = kernel_rt_bandwidth_set(&scheduler.rt_bandwidth, runtime_field, value,
        scheduler.rt_bandwidth.last_account_ns, 0);
    if (!result) {
        scheduler.need_resched = 1;
        scheduler_rearm_timer();
    }
    riscv_interrupt_restore(irq);
    return result;
}
