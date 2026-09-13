#include <arch/riscv/context.h>
#include <kernel/scheduler.h>

#include "private.h"

/*
 * Every BLOCKED task sits on the global blocked list.  wait_queue names
 * the event channel and
 * wakeup_deadline (raw time-counter ticks, 0 = none) adds a timeout.
 * Event wakeups use each queue's FIFO membership; timeout expiry alone
 * scans the global blocked list. Both memberships are removed in O(1).
 */

void blocked_append(struct kernel_task *thread)
{
    thread->blocked_previous = scheduler.blocked_tail;
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
    struct kernel_task *previous = thread->blocked_previous;
    if (previous == 0) {
        scheduler.blocked_head = thread->next;
    } else {
        previous->next = thread->next;
    }
    if (scheduler.blocked_tail == thread) {
        scheduler.blocked_tail = previous;
    }
    if (thread->next != 0) {
        thread->next->blocked_previous = previous;
    }
    thread->blocked_previous = 0;
    thread->next = 0;
}

void kernel_wait_node_init(struct kernel_wait_node *node,
                           struct kernel_task *task)
{
    if (node != 0) {
        node->task = task;
        node->callback = 0;
        node->context = 0;
        node->queue = 0;
        node->previous = 0;
        node->next = 0;
    }
}

void kernel_wait_node_init_callback(struct kernel_wait_node *node,
                                    kernel_wait_callback_fn callback,
                                    void *context)
{
    if (node != 0) {
        node->task = 0;
        node->callback = callback;
        node->context = context;
        node->queue = 0;
        node->previous = 0;
        node->next = 0;
    }
}

void kernel_wait_queue_add(struct kernel_wait_queue *queue,
                           struct kernel_wait_node *node)
{
    if (queue == 0 || node == 0) {
        return;
    }
    node->queue = queue;
    node->previous = queue->tail;
    node->next = 0;
    if (queue->tail != 0) {
        queue->tail->next = node;
    } else {
        queue->head = node;
    }
    queue->tail = node;
}

void kernel_wait_queue_remove(struct kernel_wait_node *node)
{
    struct kernel_wait_queue *queue;

    if (node == 0 || node->queue == 0) {
        return;
    }
    queue = node->queue;
    if (node->previous != 0) {
        node->previous->next = node->next;
    } else {
        queue->head = node->next;
    }
    if (node->next != 0) {
        node->next->previous = node->previous;
    } else {
        queue->tail = node->previous;
    }
    node->queue = 0;
    node->previous = 0;
    node->next = 0;
}

void scheduler_wait_requeue(struct kernel_task *task,
                            struct kernel_wait_queue *queue)
{
    if (task->wait_queue != 0) {
        kernel_wait_queue_remove(&task->default_wait_node);
    }
    task->wait_queue = queue;
    if (queue != 0) {
        task->default_wait_node.task = task;
        kernel_wait_queue_add(queue, &task->default_wait_node);
    }
}

/* STOPPED tasks park on their own list so the blocked-list invariant
 * (state == BLOCKED) stays intact; SIGCONT/SIGKILL move them back to
 * the ready queue. */
void stopped_append(struct kernel_task *thread)
{
    thread->next = 0;
    if (scheduler.stopped_tail == 0) {
        scheduler.stopped_head = thread;
    } else {
        scheduler.stopped_tail->next = thread;
    }
    scheduler.stopped_tail = thread;
}

void stopped_unlink(struct kernel_task *thread)
{
    struct kernel_task *previous = 0;
    struct kernel_task *current = scheduler.stopped_head;

    while (current != 0 && current != thread) {
        previous = current;
        current = current->next;
    }
    if (current != thread) {
        return;
    }
    if (previous == 0) {
        scheduler.stopped_head = thread->next;
    } else {
        previous->next = thread->next;
    }
    if (scheduler.stopped_tail == thread) {
        scheduler.stopped_tail = previous;
    }
    thread->next = 0;
}

void scheduler_wake_task(struct kernel_task *thread, uint32_t reason)
{
    scheduler_wait_requeue(thread, 0);
    thread->wakeup_deadline = 0;
    thread->wait_interruptible = 0U;
    thread->wake_reason = reason;
    thread->state = KERNEL_THREAD_STATE_READY;
    ready_append(thread);
}

void kernel_wait_queue_init(struct kernel_wait_queue *queue)
{
    if (queue != 0) {
        queue->initialized = KERNEL_WAIT_QUEUE_INITIALIZED;
        queue->head = 0;
        queue->tail = 0;
    }
}

enum kernel_scheduler_status kernel_wait_queue_wake_one(
    struct kernel_wait_queue *queue)
{
    struct kernel_wait_node *node;

    if (queue == 0 || queue->initialized != KERNEL_WAIT_QUEUE_INITIALIZED) {
        return KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT;
    }
    if (scheduler.initialized != KERNEL_SCHEDULER_INITIALIZED) {
        return KERNEL_SCHEDULER_STATUS_NOT_INITIALIZED;
    }
    if (riscv_interrupt_is_enabled()) {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }

    node = queue->head;
    while (node != 0) {
        struct kernel_wait_node *current_node = node;
        node = node->next;
        if (current_node->callback != 0) {
            current_node->callback(current_node, (uint32_t)KERNEL_WAIT_WOKEN);
            break;
        } else {
            struct kernel_task *thread = current_node->task;
            if (thread != 0 && thread->state == KERNEL_THREAD_STATE_BLOCKED) {
                blocked_unlink(thread);
                scheduler_wake_task(thread, (uint32_t)KERNEL_WAIT_WOKEN);
                break;
            }
        }
    }
    return KERNEL_SCHEDULER_STATUS_OK;
}

enum kernel_scheduler_status kernel_wait_queue_wake_all(
    struct kernel_wait_queue *queue)
{
    struct kernel_wait_node *node;

    if (queue == 0 || queue->initialized != KERNEL_WAIT_QUEUE_INITIALIZED) {
        return KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT;
    }
    if (scheduler.initialized != KERNEL_SCHEDULER_INITIALIZED) {
        return KERNEL_SCHEDULER_STATUS_NOT_INITIALIZED;
    }
    if (riscv_interrupt_is_enabled()) {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }
    node = queue->head;
    while (node != 0) {
        struct kernel_wait_node *current_node = node;
        node = node->next;
        if (current_node->callback != 0) {
            current_node->callback(current_node, (uint32_t)KERNEL_WAIT_WOKEN);
        } else {
            struct kernel_task *thread = current_node->task;
            if (thread != 0 && thread->state == KERNEL_THREAD_STATE_BLOCKED) {
                blocked_unlink(thread);
                scheduler_wake_task(thread, (uint32_t)KERNEL_WAIT_WOKEN);
            }
        }
    }
    return KERNEL_SCHEDULER_STATUS_OK;
}

enum kernel_scheduler_status kernel_scheduler_expire_deadlines(uint64_t now)
{
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
            blocked_unlink(thread);
            scheduler_wake_task(thread, (uint32_t)KERNEL_WAIT_TIMEOUT);
        }
        thread = next;
    }
    return KERNEL_SCHEDULER_STATUS_OK;
}

enum kernel_scheduler_status kernel_scheduler_block_current(
    struct kernel_wait_queue *queue,
    uint64_t deadline,
    int interruptible,
    enum kernel_wait_wake_reason *wake_reason)
{
    struct kernel_task *current;
    enum kernel_scheduler_status status;

    if (scheduler.initialized != KERNEL_SCHEDULER_INITIALIZED) {
        return KERNEL_SCHEDULER_STATUS_NOT_INITIALIZED;
    }
    if (wake_reason == 0 || (interruptible != 0 && interruptible != 1) ||
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

    if (interruptible && current->arch.user_mode &&
        (current->terminate_requested || kernel_signal_has_pending(current))) {
        *wake_reason = KERNEL_WAIT_SIGNALLED;
        return KERNEL_SCHEDULER_STATUS_OK;
    }
    scheduler_wait_requeue(current, queue);
    current->wakeup_deadline = deadline;
    current->wake_reason = (uint32_t)KERNEL_WAIT_WOKEN;
    current->wait_interruptible = (uint32_t)interruptible;
    current->state = KERNEL_THREAD_STATE_BLOCKED;
    blocked_append(current);
    status = scheduler_switch_current_away(current);
    if (status != KERNEL_SCHEDULER_STATUS_OK) {
        return status;
    }

    *wake_reason = (enum kernel_wait_wake_reason)current->wake_reason;
    return KERNEL_SCHEDULER_STATUS_OK;
}

enum kernel_scheduler_status kernel_scheduler_wake_signal(
    struct kernel_task *task)
{
    if (scheduler.initialized != KERNEL_SCHEDULER_INITIALIZED) {
        return KERNEL_SCHEDULER_STATUS_NOT_INITIALIZED;
    }
    if (task == 0 || riscv_interrupt_is_enabled()) {
        return KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT;
    }
    if (task->state != KERNEL_THREAD_STATE_BLOCKED ||
        task->wait_interruptible == 0U) {
        return KERNEL_SCHEDULER_STATUS_OK;
    }
    blocked_unlink(task);
    scheduler_wake_task(task, (uint32_t)KERNEL_WAIT_SIGNALLED);
    return KERNEL_SCHEDULER_STATUS_OK;
}
