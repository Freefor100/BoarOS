#include <kernel/wait_internal.h>
#include <kernel/irq.h>
#include <string.h>

struct kernel_raw_lock kernel_wait_domain;
/* 全局不回退代次也覆盖任务页复用，旧token不能命中同地址的新owner。 */
static uint64_t wait_generation;
void kernel_wait_domain_init(void)
{ kernel_raw_lock_init(&kernel_wait_domain, KERNEL_RAW_RANK_SCHEDULER); }
void kernel_wait_record_init(struct kernel_wait_record *record)
{ *record = (struct kernel_wait_record){0}; }
void kernel_wait_queue_init(struct kernel_wait_queue *queue)
{
    if (queue) *queue = (struct kernel_wait_queue){ .initialized = KERNEL_WAIT_QUEUE_INITIALIZED };
}
void kernel_wait_node_init(struct kernel_wait_node *node, struct kernel_task *task)
{ if (node) *node = (struct kernel_wait_node){ .task = task }; }
void kernel_wait_node_init_callback(struct kernel_wait_node *node,
    kernel_wait_callback_fn callback, void *context)
{ if (node) *node = (struct kernel_wait_node){ .callback = callback, .context = context }; }

static void queue_shape(const struct kernel_wait_queue *queue)
{
    if (queue->initialized != KERNEL_WAIT_QUEUE_INITIALIZED || queue->closed > 1 ||
        (!!queue->head != !!queue->tail) || (!!queue->registrations != !!queue->head) ||
        (queue->head && queue->head->previous) || (queue->tail && queue->tail->next))
        __builtin_trap();
}

static void borrow(struct kernel_wait_node *node)
{
    if (!node->borrow_owner || node->references == UINT32_MAX ||
        node->borrow_owner->borrows == UINT32_MAX) __builtin_trap();
    node->references++;
    node->borrow_owner->borrows++;
    if (node->task) {
        struct kernel_wait_record *record = kernel_wait_record_of(node->task);
        if (record->borrows == UINT32_MAX) __builtin_trap();
        record->borrows++;
    }
}
/* 退休后继的引用链迭代归还，每段最多16项，不能递归消耗任务栈。 */
static void put(struct kernel_wait_node *node)
{
    while (node) {
        struct kernel_wait_node *next = 0;
        {
            KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
            for (unsigned count = 0; node && count < 16; count++) {
                if (!node->references || !node->borrow_owner || !node->borrow_owner->borrows)
                    __builtin_trap();
                node->references--;
                node->borrow_owner->borrows--;
                if (node->task) {
                    struct kernel_wait_record *record = kernel_wait_record_of(node->task);
                    if (!record->borrows) __builtin_trap();
                    record->borrows--;
                }
                next = 0;
                if (!node->references && !node->queue) {
                    next = node->retired_next;
                    node->retired_next = 0;
                    node->borrow_owner = 0;
                }
                node = next;
            }
        }
    }
}
void kernel_wait_node_add_locked(struct kernel_wait_queue *queue, struct kernel_wait_node *node)
{
    if (queue) queue_shape(queue);
    if (!queue || queue->initialized != KERNEL_WAIT_QUEUE_INITIALIZED || queue->closed ||
        !node || node->queue || node->borrow_owner || queue->registrations == UINT32_MAX ||
        queue->sequence == UINT64_MAX) __builtin_trap();
    node->sequence = ++queue->sequence;
    node->queue = node->borrow_owner = queue;
    node->previous = queue->tail;
    node->next = 0;
    if (queue->tail) queue->tail->next = node;
    else queue->head = node;
    queue->tail = node;
    queue->registrations++;
}
void kernel_wait_node_remove_locked(struct kernel_wait_node *node)
{
    if (!node || !node->queue) return;
    struct kernel_wait_queue *queue = node->queue;
    queue_shape(queue);
    if (queue->initialized != KERNEL_WAIT_QUEUE_INITIALIZED || !queue->registrations ||
        (node->previous ? node->previous->next != node : queue->head != node) ||
        (node->next ? node->next->previous != node : queue->tail != node)) __builtin_trap();
    if (node->references && node->next) {
        borrow(node->next);
        node->retired_next = node->next;
    }
    if (node->previous) node->previous->next = node->next;
    else queue->head = node->next;
    if (node->next) node->next->previous = node->previous;
    else queue->tail = node->previous;
    queue->registrations--;
    node->queue = 0;
    node->previous = node->next = 0;
    if (!node->references) node->borrow_owner = 0;
}
void kernel_wait_queue_remove(struct kernel_wait_node *node)
{ KERNEL_RAW_SCOPE(guard, &kernel_wait_domain); kernel_wait_node_remove_locked(node); }
void kernel_wait_queue_add(struct kernel_wait_queue *queue, struct kernel_wait_node *node)
{
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    if (node && node->task) {
        struct kernel_wait_record *record = kernel_wait_record_of(node->task);
        if (record->phase != KERNEL_WAIT_FINISHED && !node->generation) {
            node->generation = record->generation;
            node->token_next = record->nodes;
            record->nodes = node;
        }
    }
    kernel_wait_node_add_locked(queue, node);
}
static int matches(const struct kernel_wait_token *token)
{
    return token && token->task && token->generation &&
        kernel_wait_record_of(token->task)->generation == token->generation &&
        kernel_wait_record_of(token->task)->phase != KERNEL_WAIT_FINISHED;
}
enum kernel_scheduler_status kernel_wait_node_bind(struct kernel_wait_queue *queue,
    struct kernel_wait_node *node, const struct kernel_wait_token *token)
{
    if (!queue || !node || !token || queue->initialized != KERNEL_WAIT_QUEUE_INITIALIZED)
        return KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT;
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    if (queue->closed || !matches(token)) return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    if (node->task != token->task || node->generation) __builtin_trap();
    struct kernel_wait_record *record = kernel_wait_record_of(token->task);
    node->generation = token->generation;
    node->token_next = record->nodes;
    record->nodes = node;
    kernel_wait_node_add_locked(queue, node);
    return KERNEL_SCHEDULER_STATUS_OK;
}
enum kernel_scheduler_status kernel_wait_prepare(struct kernel_wait_queue *queue,
    uint64_t deadline, int interruptible, struct kernel_wait_token *token)
{
    if (!token || (interruptible != 0 && interruptible != 1) ||
        (queue && queue->initialized != KERNEL_WAIT_QUEUE_INITIALIZED))
        return KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT;
    if (!kernel_wait_backend_initialized()) return KERNEL_SCHEDULER_STATUS_NOT_INITIALIZED;
    struct kernel_task *task = kernel_wait_current_task();
    if (!task) return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    struct kernel_wait_record *record = kernel_wait_record_of(task);
    if (record->phase != KERNEL_WAIT_FINISHED || record->nodes || record->borrows) __builtin_trap();
    if (record->generation == UINT64_MAX || wait_generation == UINT64_MAX || (queue && queue->closed))
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    record->generation = ++wait_generation;
    record->phase = KERNEL_WAIT_PREPARED;
    record->deadline = deadline;
    record->interruptible = interruptible;
    record->reason = KERNEL_WAIT_WOKEN;
    *token = (struct kernel_wait_token){ task, record->generation };
    if (queue) {
        struct kernel_wait_node *node = kernel_wait_default_node(task);
        if (node->generation || node->queue || node->references) __builtin_trap();
        if (node->task && node->task != task) __builtin_trap();
        node->task = task;
        node->generation = record->generation;
        record->nodes = node;
        kernel_wait_node_add_locked(queue, node);
    }
    return KERNEL_SCHEDULER_STATUS_OK;
}
int kernel_wait_notify_locked(const struct kernel_wait_token *token, uint32_t reason)
{
    if (!matches(token)) return 0;
    struct kernel_wait_record *record = kernel_wait_record_of(token->task);
    if (record->phase == KERNEL_WAIT_NOTIFIED) return 0;
    if (record->phase == KERNEL_WAIT_COMMITTED) {
        kernel_wait_backend_notify(token->task, reason);
        if (record->on_cpu) record->ready_pending = 1;
        else kernel_wait_backend_ready(token->task);
    }
    record->reason = reason;
    record->phase = KERNEL_WAIT_NOTIFIED;
    return 1;
}
enum kernel_scheduler_status kernel_wait_notify(const struct kernel_wait_token *token,
    enum kernel_wait_wake_reason reason)
{
    if (!token || !token->task || reason > KERNEL_WAIT_SIGNALLED)
        return KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT;
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    kernel_wait_notify_locked(token, reason);
    return KERNEL_SCHEDULER_STATUS_OK;
}
enum kernel_scheduler_status kernel_wait_park(const struct kernel_wait_token *token,
    enum kernel_wait_wake_reason *reason)
{
    kernel_assert_can_block();
    KERNEL_IRQ_SCOPE(park_irq);
    if (!token || !reason || !token->task) return KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT;
    if (token->task != kernel_wait_current_task()) __builtin_trap();
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        if (!matches(token)) return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        struct kernel_wait_record *record = kernel_wait_record_of(token->task);
        if (record->phase == KERNEL_WAIT_PREPARED) {
            if (record->interruptible && kernel_wait_backend_signal(token->task))
                kernel_wait_notify_locked(token, KERNEL_WAIT_SIGNALLED);
            else if (record->deadline && (int64_t)(kernel_wait_backend_time() - record->deadline) >= 0)
                kernel_wait_notify_locked(token, KERNEL_WAIT_TIMEOUT);
        }
        if (record->phase == KERNEL_WAIT_NOTIFIED) { *reason = record->reason; return KERNEL_SCHEDULER_STATUS_OK; }
        if (record->phase != KERNEL_WAIT_PREPARED) __builtin_trap();
        record->phase = KERNEL_WAIT_COMMITTED;
        kernel_wait_backend_commit(token->task, record->deadline, record->interruptible);
    }
    enum kernel_scheduler_status status = kernel_wait_backend_switch(token->task);
    if (status != KERNEL_SCHEDULER_STATUS_OK) return status;
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    struct kernel_wait_record *record = kernel_wait_record_of(token->task);
    if (record->phase != KERNEL_WAIT_NOTIFIED || !record->on_cpu) __builtin_trap();
    *reason = record->reason;
    return KERNEL_SCHEDULER_STATUS_OK;
}
enum kernel_scheduler_status kernel_wait_node_remove_sync(struct kernel_wait_node *node)
{
    if (!node) return KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT;
    kernel_assert_can_block();
    kernel_wait_queue_remove(node);
    for (;;) {
        {
            KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
            if (!node->references) return KERNEL_SCHEDULER_STATUS_OK;
        }
        kernel_wait_backend_quiesce();
    }
}
enum kernel_scheduler_status kernel_wait_finish(struct kernel_wait_token *token)
{
    kernel_assert_can_block();
    if (!token || !token->task) return KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT;
    if (token->task != kernel_wait_current_task()) __builtin_trap();
    for (;;) {
        struct kernel_wait_node *node;
        {
            KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
            if (!matches(token)) return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
            struct kernel_wait_record *record = kernel_wait_record_of(token->task);
            if (record->phase == KERNEL_WAIT_COMMITTED) __builtin_trap();
            node = record->nodes;
            if (!node && !record->borrows) {
                record->phase = KERNEL_WAIT_FINISHED;
                *token = (struct kernel_wait_token){0};
                return KERNEL_SCHEDULER_STATUS_OK;
            }
            if (node) {
                kernel_wait_node_remove_locked(node);
                if (!node->references) {
                    record->nodes = node->token_next;
                    node->generation = 0;
                    node->token_next = 0;
                    continue;
                }
            }
        }
        kernel_wait_backend_quiesce();
    }
}
void kernel_wait_switch_finish_locked(struct kernel_task *task)
{
    struct kernel_wait_record *record = kernel_wait_record_of(task);
    if (!record->on_cpu) __builtin_trap();
    record->on_cpu = 0;
    if (record->ready_pending) {
        record->ready_pending = 0;
        kernel_wait_backend_ready(task);
    }
}
static int cursor_begin(struct kernel_wait_queue *queue, struct kernel_wait_cursor *cursor,
    int skip_empty)
{
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    if (!queue || queue->initialized != KERNEL_WAIT_QUEUE_INITIALIZED || queue->borrows == UINT32_MAX)
        __builtin_trap();
    queue_shape(queue);
    /* 空通知在同一锁内完成判断，不建立无需跨解锁使用的游标借用。 */
    if (skip_empty && !queue->head) return 0;
    queue->borrows++;
    *cursor = (struct kernel_wait_cursor){queue, queue->head, queue->sequence};
    if (cursor->node) borrow(cursor->node);
    return 1;
}
void kernel_wait_cursor_begin(struct kernel_wait_queue *queue, struct kernel_wait_cursor *cursor)
{ (void)cursor_begin(queue, cursor, 0); }
void kernel_wait_cursor_advance(struct kernel_wait_cursor *cursor)
{
    struct kernel_wait_node *old = cursor->node;
    if (!old) return;
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        struct kernel_wait_node *next = old->queue ? old->next : old->retired_next;
        if (next && next->sequence <= cursor->sequence) borrow(next);
        else next = 0;
        cursor->node = next;
    }
    put(old);
}
void kernel_wait_cursor_end(struct kernel_wait_cursor *cursor)
{
    if (cursor->node) put(cursor->node);
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    if (!cursor->queue || !cursor->queue->borrows) __builtin_trap();
    cursor->queue->borrows--;
    *cursor = (struct kernel_wait_cursor){0};
}
void kernel_wait_task_pin(struct kernel_task *task)
{
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    struct kernel_wait_record *record = kernel_wait_record_of(task);
    if (record->borrows == UINT32_MAX) __builtin_trap();
    record->borrows++;
}
void kernel_wait_task_unpin(struct kernel_task *task)
{
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    struct kernel_wait_record *record = kernel_wait_record_of(task);
    if (!record->borrows) __builtin_trap();
    record->borrows--;
}
enum kernel_scheduler_status kernel_wait_node_move(struct kernel_wait_node *node, struct kernel_wait_queue *queue)
{
    if (!node || !node->task || !queue || queue->initialized != KERNEL_WAIT_QUEUE_INITIALIZED)
        return KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT;
    kernel_assert_can_block();
    kernel_wait_queue_remove(node);
    for (;;) {
        {
            KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
            struct kernel_wait_token token = {node->task, node->generation};
            if (queue->closed) return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
            if (!matches(&token) || kernel_wait_record_of(node->task)->phase == KERNEL_WAIT_NOTIFIED)
                return KERNEL_SCHEDULER_STATUS_EMPTY;
            if (!node->references) {
                kernel_wait_node_add_locked(queue, node);
                return KERNEL_SCHEDULER_STATUS_OK;
            }
        }
        kernel_wait_backend_quiesce();
    }
}
static enum kernel_scheduler_status wake(struct kernel_wait_queue *queue, int all)
{
    if (!queue || queue->initialized != KERNEL_WAIT_QUEUE_INITIALIZED)
        return KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT;
    if (!kernel_wait_backend_initialized()) return KERNEL_SCHEDULER_STATUS_NOT_INITIALIZED;
    if (arch_interrupt_is_enabled()) return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    struct kernel_wait_cursor cursor;
    if (!cursor_begin(queue, &cursor, 1)) return KERNEL_SCHEDULER_STATUS_OK;
    while (cursor.node) {
        struct kernel_wait_node *node = cursor.node;
        kernel_wait_callback_fn callback = 0;
        int notified = 0;
        {
            KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
            if (node->queue == queue) {
                callback = node->callback;
                if (!callback && node->task && node->generation) {
                    struct kernel_wait_token token = {node->task, node->generation};
                    notified = kernel_wait_notify_locked(&token, KERNEL_WAIT_WOKEN);
                }
            }
        }
        if (callback) {
            kernel_preempt_disable();
            callback(node, KERNEL_WAIT_WOKEN);
            kernel_preempt_enable();
            notified = 1;
        }
        if (notified && !all) break;
        kernel_wait_cursor_advance(&cursor);
    }
    kernel_wait_cursor_end(&cursor);
    return KERNEL_SCHEDULER_STATUS_OK;
}
enum kernel_scheduler_status kernel_wait_queue_wake_one(struct kernel_wait_queue *queue) { return wake(queue, 0); }
enum kernel_scheduler_status kernel_wait_queue_wake_all(struct kernel_wait_queue *queue) { return wake(queue, 1); }
enum kernel_scheduler_status kernel_wait_queue_close(struct kernel_wait_queue *queue)
{
    if (!queue || queue->initialized != KERNEL_WAIT_QUEUE_INITIALIZED)
        return KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT;
    KERNEL_IRQ_SCOPE(close_irq);
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        queue_shape(queue);
        if (queue->borrows == UINT32_MAX) __builtin_trap();
        /* close跨解锁继续通知，空队列也必须保留操作自身的借用。 */
        queue->borrows++;
        queue->closed = 1;
    }
    enum kernel_scheduler_status status = wake(queue, 1);
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        if (!queue->borrows) __builtin_trap();
        queue->borrows--;
    }
    return status;
}
enum kernel_scheduler_status kernel_wait_queue_destroy(struct kernel_wait_queue *queue)
{
    if (!queue || queue->initialized != KERNEL_WAIT_QUEUE_INITIALIZED)
        return KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT;
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    if (!queue->closed) return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    queue_shape(queue);
    if (queue->registrations || queue->borrows) return KERNEL_SCHEDULER_STATUS_BUSY;
    *queue = (struct kernel_wait_queue){0};
    return KERNEL_SCHEDULER_STATUS_OK;
}
