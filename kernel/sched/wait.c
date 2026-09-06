#include <arch/riscv/context.h>
#include <kernel/scheduler.h>

#include "private.h"

/*
 * Every BLOCKED task sits on the global blocked list.  wait_queue names
 * the event channel (NULL for wait4-style parent wakes) and
 * wakeup_deadline (raw time-counter ticks, 0 = none) adds a timeout.
 * Waking therefore walks one list instead of maintaining per-queue links;
 * the per-queue / deadline-ordered refinement is the documented growth
 * path once waiter counts make the O(blocked) walk measurable.
 */

void blocked_append(struct kernel_task *thread)
{
    thread->next = 0;
    if (scheduler.blocked_tail == 0) {
        scheduler.blocked_head = thread;
    } else {
        scheduler.blocked_tail->next = thread;
    }
    scheduler.blocked_tail = thread;
}

void blocked_unlink(struct kernel_task *thread)
{
    struct kernel_task *previous = 0;
    struct kernel_task *current = scheduler.blocked_head;

    while (current != 0 && current != thread) {
        previous = current;
        current = current->next;
    }
    if (current != thread) {
        return;
    }
    if (previous == 0) {
        scheduler.blocked_head = thread->next;
    } else {
        previous->next = thread->next;
    }
    if (scheduler.blocked_tail == thread) {
        scheduler.blocked_tail = previous;
    }
    thread->next = 0;
}

static void wake_task(struct kernel_task *thread, uint32_t reason)
{
    thread->wait_queue = 0;
    thread->wakeup_deadline = 0;
    thread->wake_reason = reason;
    thread->state = KERNEL_THREAD_STATE_READY;
    ready_append(thread);
}

void kernel_wait_queue_init(struct kernel_wait_queue *queue)
{
    if (queue != 0) {
        queue->initialized = KERNEL_WAIT_QUEUE_INITIALIZED;
    }
}

enum kernel_scheduler_status kernel_wait_queue_wake_one(
    struct kernel_wait_queue *queue)
{
    struct kernel_task *thread;

    if (queue == 0 || queue->initialized != KERNEL_WAIT_QUEUE_INITIALIZED) {
        return KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT;
    }
    if (scheduler.initialized != KERNEL_SCHEDULER_INITIALIZED) {
        return KERNEL_SCHEDULER_STATUS_NOT_INITIALIZED;
    }
    if (riscv_interrupt_is_enabled()) {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }

    for (thread = scheduler.blocked_head; thread != 0;
         thread = thread->next) {
        if (thread->wait_queue == queue) {
            blocked_unlink(thread);
            wake_task(thread, (uint32_t)KERNEL_WAIT_WOKEN);
            return KERNEL_SCHEDULER_STATUS_OK;
        }
    }
    return KERNEL_SCHEDULER_STATUS_OK;
}

enum kernel_scheduler_status kernel_scheduler_expire_deadlines(uint64_t now)
{
    struct kernel_task *previous = 0;
    struct kernel_task *thread;
    enum kernel_scheduler_status status;

    if (scheduler.initialized != KERNEL_SCHEDULER_INITIALIZED) {
        return KERNEL_SCHEDULER_STATUS_NOT_INITIALIZED;
    }
    if (riscv_interrupt_is_enabled()) {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }
    status = validate_queues();
    if (status != KERNEL_SCHEDULER_STATUS_OK) {
        return status;
    }

    thread = scheduler.blocked_head;
    while (thread != 0) {
        struct kernel_task *next = thread->next;

        if (thread->wakeup_deadline != 0U &&
            (int64_t)(now - thread->wakeup_deadline) >= 0) {
            if (previous == 0) {
                scheduler.blocked_head = next;
            } else {
                previous->next = next;
            }
            if (scheduler.blocked_tail == thread) {
                scheduler.blocked_tail = previous;
            }
            thread->next = 0;
            wake_task(thread, (uint32_t)KERNEL_WAIT_TIMEOUT);
        } else {
            previous = thread;
        }
        thread = next;
    }
    return KERNEL_SCHEDULER_STATUS_OK;
}

enum kernel_scheduler_status kernel_scheduler_block_current(
    struct kernel_wait_queue *queue,
    uint64_t deadline,
    enum kernel_wait_wake_reason *wake_reason)
{
    struct kernel_task *current;
    enum kernel_scheduler_status status;

    if (scheduler.initialized != KERNEL_SCHEDULER_INITIALIZED) {
        return KERNEL_SCHEDULER_STATUS_NOT_INITIALIZED;
    }
    if (wake_reason == 0 ||
        (queue != 0 && queue->initialized != KERNEL_WAIT_QUEUE_INITIALIZED)) {
        return KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT;
    }
    if (riscv_interrupt_is_enabled()) {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }
    status = validate_current();
    if (status != KERNEL_SCHEDULER_STATUS_OK) {
        return status;
    }
    current = scheduler.current;
    if (current == &scheduler.idle) {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }
    status = validate_queues();
    if (status != KERNEL_SCHEDULER_STATUS_OK) {
        return status;
    }

    current->wait_queue = queue;
    current->wakeup_deadline = deadline;
    current->wake_reason = (uint32_t)KERNEL_WAIT_WOKEN;
    current->state = KERNEL_THREAD_STATE_BLOCKED;
    blocked_append(current);
    status = scheduler_switch_current_away(current);
    if (status != KERNEL_SCHEDULER_STATUS_OK) {
        return status;
    }

    *wake_reason = (enum kernel_wait_wake_reason)current->wake_reason;
    return KERNEL_SCHEDULER_STATUS_OK;
}
