#include <arch/context.h>
#include <arch/timer.h>
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

struct kernel_wait_record *kernel_wait_record_of(struct kernel_task *task) { return &task->wait; }
struct kernel_wait_node *kernel_wait_default_node(struct kernel_task *task) { return &task->default_wait_node; }
struct kernel_task *kernel_wait_current_task(void)
{
    struct kernel_task *task = kernel_cpu_current()->current;
    return task && !task->idle ? task : 0;
}
int kernel_wait_backend_initialized(void) { return scheduler.initialized == KERNEL_SCHEDULER_INITIALIZED; }
int kernel_wait_backend_signal(struct kernel_task *task)
{ return task->arch.user_mode && (task->terminate_requested || kernel_signal_has_pending(task)); }
uint64_t kernel_wait_backend_time(void) { return arch_time_read(); }
void kernel_wait_backend_commit(struct kernel_task *task, uint64_t deadline, int interruptible)
{
    task->wait.ready_head = 0;
    task->wait_queue = task->default_wait_node.queue;
    task->wakeup_deadline = deadline;
    task->wait_interruptible = interruptible;
    if (deadline) deadline_index_insert(task);
#if BOAROS_COST_DIAGNOSTICS
    kernel_cost_block(&task->cost);
#endif
    task->state = KERNEL_THREAD_STATE_BLOCKED;
    blocked_append(task);
}
void kernel_wait_backend_notify(struct kernel_task *task, uint32_t reason)
{
#if BOAROS_COST_DIAGNOSTICS
    kernel_cost_wake(&task->cost);
    if (reason == KERNEL_WAIT_TIMEOUT && task->wakeup_deadline)
        kernel_cost_timeout(&task->cost, task->wakeup_deadline);
#endif
    blocked_unlink(task);
    deadline_index_remove(task);
    kernel_wait_node_remove_locked(&task->default_wait_node);
    task->wait_queue = 0;
    task->wakeup_deadline = 0;
    task->wait_interruptible = 0;
    task->wake_reason = reason;
    task->state = KERNEL_THREAD_STATE_READY;
}
void kernel_wait_backend_ready(struct kernel_task *task) { ready_enqueue_locked(task, task->wait.ready_head); }
enum kernel_scheduler_status kernel_wait_backend_switch(struct kernel_task *task)
{ return scheduler_switch_current_away(task); }
void kernel_wait_backend_quiesce(void)
{
    uintptr_t irq = arch_interrupt_save();
    if (kernel_scheduler_yield_current() != KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
    arch_interrupt_restore(irq);
}
void scheduler_wake_task(struct kernel_task *task, uint32_t reason)
{
    struct kernel_wait_token token = { task, task->wait.generation };
    (void)kernel_wait_notify(&token, reason);
}

enum kernel_scheduler_status kernel_scheduler_expire_deadlines(uint64_t now)
{
    COST_SCOPE(cost_deadline, DEADLINE_TICKS);
    COST_ADD(DEADLINE_PASSES, 1);
    if (!kernel_wait_backend_initialized()) return KERNEL_SCHEDULER_STATUS_NOT_INITIALIZED;
    if (arch_interrupt_is_enabled()) return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    for (;;) {
        int done = 0;
        {
            KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
            for (unsigned n = 0; n < 16; n++) {
                struct kernel_task *task = deadline_index_first();
                if (!task || (int64_t)(now - task->wakeup_deadline) < 0) { done = 1; break; }
                struct kernel_wait_token token = {task, task->wait.generation};
                COST_ADD(DEADLINE_VISITS, 1);
                if (!kernel_wait_notify_locked(&token, KERNEL_WAIT_TIMEOUT)) __builtin_trap();
            }
        }
        if (done) break;
    }
    /* 协议和信号业务回调不能进入调度raw域。 */
    kernel_socket_expire_timers();
    kernel_signal_timer_expire();
    return KERNEL_SCHEDULER_STATUS_OK;
}
enum kernel_scheduler_status kernel_scheduler_block_current(struct kernel_wait_queue *queue,
    uint64_t deadline, int interruptible, enum kernel_wait_wake_reason *reason)
{
    kernel_assert_can_block();
    if (arch_interrupt_is_enabled()) return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    if (!reason) return KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT;
    struct kernel_wait_token token;
    enum kernel_scheduler_status status = kernel_wait_prepare(queue, deadline, interruptible, &token);
    if (status != KERNEL_SCHEDULER_STATUS_OK) return status;
    status = kernel_wait_park(&token, reason);
    enum kernel_scheduler_status finish = kernel_wait_finish(&token);
    return status == KERNEL_SCHEDULER_STATUS_OK ? finish : status;
}
enum kernel_scheduler_status kernel_scheduler_wake_signal(struct kernel_task *task)
{
    if (!kernel_wait_backend_initialized()) return KERNEL_SCHEDULER_STATUS_NOT_INITIALIZED;
    if (!task) return KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT;
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    if (task->wait.interruptible) {
        struct kernel_wait_token token = {task, task->wait.generation};
        kernel_wait_notify_locked(&token, KERNEL_WAIT_SIGNALLED);
    }
    return KERNEL_SCHEDULER_STATUS_OK;
}
