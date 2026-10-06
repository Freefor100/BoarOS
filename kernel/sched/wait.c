#include <arch/context.h>
#include <kernel/scheduler.h>
#include <kernel/socket.h>

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

/*
 * Tasks with a timeout enter an intrusive AVL keyed by (deadline, tid).
 * Insertion happens once per block, removal rides the single wake path
 * (scheduler_wake_task), and expiry pops only the earliest entries, so
 * tick cost tracks expirations instead of the blocked population.
 */

static int deadline_height(const struct kernel_task *task)
{
    return task ? task->deadline_height : 0;
}

static void deadline_update(struct kernel_task *task)
{
    int left = deadline_height(task->deadline_left);
    int right = deadline_height(task->deadline_right);
    task->deadline_height = (int8_t)(1 + (left > right ? left : right));
}

static struct kernel_task *deadline_rotate_left(struct kernel_task *root)
{
    struct kernel_task *next = root->deadline_right;
    root->deadline_right = next->deadline_left;
    next->deadline_left = root;
    deadline_update(root);
    deadline_update(next);
    return next;
}

static struct kernel_task *deadline_rotate_right(struct kernel_task *root)
{
    struct kernel_task *next = root->deadline_left;
    root->deadline_left = next->deadline_right;
    next->deadline_right = root;
    deadline_update(root);
    deadline_update(next);
    return next;
}

static struct kernel_task *deadline_balance(struct kernel_task *root)
{
    deadline_update(root);
    if (deadline_height(root->deadline_left) -
            deadline_height(root->deadline_right) > 1) {
        if (deadline_height(root->deadline_left->deadline_left) <
            deadline_height(root->deadline_left->deadline_right))
            root->deadline_left = deadline_rotate_left(root->deadline_left);
        return deadline_rotate_right(root);
    }
    if (deadline_height(root->deadline_right) -
            deadline_height(root->deadline_left) > 1) {
        if (deadline_height(root->deadline_right->deadline_right) <
            deadline_height(root->deadline_right->deadline_left))
            root->deadline_right = deadline_rotate_right(root->deadline_right);
        return deadline_rotate_left(root);
    }
    return root;
}

static int deadline_before(const struct kernel_task *a,
                           const struct kernel_task *b)
{
    return a->wakeup_deadline < b->wakeup_deadline ||
           (a->wakeup_deadline == b->wakeup_deadline && a->tid < b->tid);
}

static struct kernel_task *deadline_insert_node(struct kernel_task *root,
                                                struct kernel_task *task)
{
    if (root == 0) return task;
    if (deadline_before(task, root))
        root->deadline_left = deadline_insert_node(root->deadline_left, task);
    else
        root->deadline_right = deadline_insert_node(root->deadline_right, task);
    return deadline_balance(root);
}

static struct kernel_task *deadline_extract_min(struct kernel_task *root,
                                                struct kernel_task **minimum)
{
    if (root->deadline_left == 0) {
        *minimum = root;
        return root->deadline_right;
    }
    root->deadline_left = deadline_extract_min(root->deadline_left, minimum);
    return deadline_balance(root);
}

static struct kernel_task *deadline_remove_node(struct kernel_task *root,
                                                const struct kernel_task *task)
{
    if (root == 0) __builtin_trap();
    if (root != task) {
        if (deadline_before(task, root))
            root->deadline_left =
                deadline_remove_node(root->deadline_left, task);
        else
            root->deadline_right =
                deadline_remove_node(root->deadline_right, task);
        return deadline_balance(root);
    }
    if (root->deadline_left == 0) return root->deadline_right;
    if (root->deadline_right == 0) return root->deadline_left;
    struct kernel_task *next;
    struct kernel_task *right = deadline_extract_min(root->deadline_right, &next);
    next->deadline_left = root->deadline_left;
    next->deadline_right = right;
    return deadline_balance(next);
}

void deadline_index_insert(struct kernel_task *task)
{
    if (task == 0 || task->wakeup_deadline == 0U ||
        task->deadline_indexed) __builtin_trap();
    task->deadline_left = 0;
    task->deadline_right = 0;
    task->deadline_height = 1;
    task->deadline_indexed = 1U;
    scheduler.deadline_root =
        deadline_insert_node(scheduler.deadline_root, task);
}

void deadline_index_remove(struct kernel_task *task)
{
    if (task == 0 || !task->deadline_indexed) return;
    scheduler.deadline_root =
        deadline_remove_node(scheduler.deadline_root, task);
    task->deadline_left = 0;
    task->deadline_right = 0;
    task->deadline_height = 0;
    task->deadline_indexed = 0U;
}

struct kernel_task *deadline_index_first(void)
{
    struct kernel_task *task = scheduler.deadline_root;
    while (task != 0 && task->deadline_left != 0) task = task->deadline_left;
    return task;
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
#if BOAROS_COST_DIAGNOSTICS
    kernel_cost_wake(&thread->cost);
    if (reason == KERNEL_WAIT_TIMEOUT && thread->wakeup_deadline)
        kernel_cost_timeout(&thread->cost, thread->wakeup_deadline);
#endif
    scheduler_wait_requeue(thread, 0);
    deadline_index_remove(thread);
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
    if (arch_interrupt_is_enabled()) {
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
    if (arch_interrupt_is_enabled()) {
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
    COST_SCOPE(cost_deadline, DEADLINE_TICKS);
    COST_ADD(DEADLINE_PASSES, 1);
    struct kernel_task *thread;
    enum kernel_scheduler_status status;

    if (scheduler.initialized != KERNEL_SCHEDULER_INITIALIZED) {
        return KERNEL_SCHEDULER_STATUS_NOT_INITIALIZED;
    }
    if (arch_interrupt_is_enabled()) {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }
    status = validate_queues();
    if (status != KERNEL_SCHEDULER_STATUS_OK) {
        return status;
    }

    while ((thread = deadline_index_first()) != 0 &&
           (int64_t)(now - thread->wakeup_deadline) >= 0) {
        deadline_index_remove(thread);
        COST_ADD(DEADLINE_VISITS, 1);
        blocked_unlink(thread);
        scheduler_wake_task(thread, (uint32_t)KERNEL_WAIT_TIMEOUT);
    }
    kernel_socket_expire_timers();
    kernel_signal_timer_expire();
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
    if (arch_interrupt_is_enabled()) {
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
    if (deadline != 0U) deadline_index_insert(current);
    current->wake_reason = (uint32_t)KERNEL_WAIT_WOKEN;
    current->wait_interruptible = (uint32_t)interruptible;
#if BOAROS_COST_DIAGNOSTICS
    kernel_cost_block(&current->cost);
#endif
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
    if (task == 0 || arch_interrupt_is_enabled()) {
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
