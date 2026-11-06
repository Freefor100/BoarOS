#include <kernel/wait_internal.h>
#include <kernel/irq.h>
#include <string.h>

struct kernel_raw_lock kernel_wait_domain __attribute__((section(".bss.scheduler.wait")));
#if BOAROS_MAX_CPUS > 1
_Static_assert(__atomic_always_lock_free(8, 0), "wait states require inline 64-bit atomics");
#endif
/* 全局不回退代次也覆盖任务页复用，旧token不能命中同地址的新owner。 */
static uint64_t wait_generation;
void kernel_wait_domain_init(void)
{ kernel_raw_lock_init(&kernel_wait_domain, KERNEL_RAW_RANK_SCHEDULER); }
void kernel_wait_record_init(struct kernel_wait_record *record)
{ *record = (struct kernel_wait_record){0}; }
void kernel_wait_queue_init(struct kernel_wait_queue *queue)
{
    if (!queue) return;
    *queue=(struct kernel_wait_queue){.initialized=KERNEL_WAIT_QUEUE_INITIALIZED};
    kernel_raw_lock_init(&queue->lock,KERNEL_RAW_RANK_WAIT_QUEUE);
}
void kernel_wait_node_init(struct kernel_wait_node *node, struct kernel_task *task)
{ if (node) *node = (struct kernel_wait_node){ .task = task }; }
void kernel_wait_node_init_callback(struct kernel_wait_node *node,
    kernel_wait_callback_fn callback, void *context)
{ if (node) *node = (struct kernel_wait_node){ .callback = callback, .context = context }; }

static void queue_shape(const struct kernel_wait_queue *queue)
{
    if (queue->initialized != KERNEL_WAIT_QUEUE_INITIALIZED || queue->closed > 1 ||
        (!!queue->head != !!queue->tail) ||
        (!!__atomic_load_n(&queue->registrations,__ATOMIC_RELAXED) != !!queue->head) ||
        (queue->head && queue->head->previous) || (queue->tail && queue->tail->next))
        __builtin_trap();
}

static inline void publish_references(struct kernel_wait_node *node,uint32_t references)
{
#if BOAROS_MAX_CPUS > 1
    __atomic_store_n(&node->references,references,__ATOMIC_RELEASE);
#else
    node->references=references;
#endif
}
static inline void publish_queue(struct kernel_wait_node *node,struct kernel_wait_queue *queue)
{
#if BOAROS_MAX_CPUS > 1
    __atomic_store_n(&node->queue,queue,__ATOMIC_RELEASE);
#else
    node->queue=queue;
#endif
}
static void record_get(struct kernel_wait_record *record)
{
#if BOAROS_MAX_CPUS > 1
    if (__atomic_fetch_add(&record->borrows,1,__ATOMIC_ACQ_REL)==UINT32_MAX) __builtin_trap();
#else
    if (record->borrows==UINT32_MAX) __builtin_trap();
    record->borrows++;
#endif
}
static void record_put(struct kernel_wait_record *record)
{
#if BOAROS_MAX_CPUS > 1
    if (!__atomic_fetch_sub(&record->borrows,1,__ATOMIC_RELEASE)) __builtin_trap();
#else
    if (!record->borrows) __builtin_trap();
    record->borrows--;
#endif
}
static void borrow(struct kernel_wait_node *node)
{
    uint32_t references=kernel_wait_node_references(node);
    if (!node->borrow_owner || references==UINT32_MAX ||
        node->borrow_owner->borrows==UINT32_MAX) __builtin_trap();
    node->borrow_owner->borrows++;
    if (node->task) record_get(kernel_wait_record_of(node->task));
    publish_references(node,references+1);
}
/* 原队列保护内最多归还16项；剩余后继继续由游标借用保活。 */
static struct kernel_wait_node *put_batch_locked(struct kernel_wait_node *node)
{
    for (unsigned count=0; node && count<16; count++) {
        uint32_t references=kernel_wait_node_references(node);
        struct kernel_wait_queue *queue=node->borrow_owner;
        if (!references || !queue || !queue->borrows) __builtin_trap();
        queue->borrows--;
        struct kernel_wait_node *next=0;
        if (references==1 && !node->queue) {
            next=node->retired_next;
            node->retired_next=0;
            node->borrow_owner=0;
        }
        if (node->task) record_put(kernel_wait_record_of(node->task));
        /* Last access: finish/remove_sync may now release the node owner. */
        publish_references(node,references-1);
        node=next;
    }
    return node;
}
/* 退休链迭代归还，不能递归消耗任务栈。 */
static void put(struct kernel_wait_node *node)
{
    while (node) {
        /* A cursor borrow keeps this original queue alive through retirement. */
        struct kernel_wait_queue *queue=node->borrow_owner;
        if (!queue) __builtin_trap();
        KERNEL_IRQ_LOCK_SCOPE(guard,&queue->lock);
        node=put_batch_locked(node);
    }
}
void kernel_wait_node_add_locked(struct kernel_wait_queue *queue, struct kernel_wait_node *node)
{
    if (queue) queue_shape(queue);
    uint32_t registrations=queue?__atomic_load_n(&queue->registrations,__ATOMIC_RELAXED):0;
    if (!queue || queue->initialized != KERNEL_WAIT_QUEUE_INITIALIZED || queue->closed ||
        !node || node->queue || node->borrow_owner || registrations == UINT32_MAX ||
        queue->sequence == UINT64_MAX) __builtin_trap();
    node->sequence = ++queue->sequence;
    node->borrow_owner=queue;
    publish_queue(node,queue);
    node->previous = queue->tail;
    node->next = 0;
    if (queue->tail) queue->tail->next = node;
    else queue->head = node;
    queue->tail = node;
    __atomic_store_n(&queue->registrations,registrations+1,__ATOMIC_RELEASE);
}
void kernel_wait_node_remove_locked(struct kernel_wait_node *node)
{
    if (!node || !node->queue) return;
    struct kernel_wait_queue *queue = node->queue;
    queue_shape(queue);
    uint32_t registrations=__atomic_load_n(&queue->registrations,__ATOMIC_RELAXED);
    if (queue->initialized != KERNEL_WAIT_QUEUE_INITIALIZED || !registrations ||
        (node->previous ? node->previous->next != node : queue->head != node) ||
        (node->next ? node->next->previous != node : queue->tail != node)) __builtin_trap();
    if (kernel_wait_node_references(node) && node->next) {
        borrow(node->next);
        node->retired_next = node->next;
    }
    if (node->previous) node->previous->next = node->next;
    else queue->head = node->next;
    if (node->next) node->next->previous = node->previous;
    else queue->tail = node->previous;
    node->previous=node->next=0;
    if (!kernel_wait_node_references(node)) node->borrow_owner=0;
    publish_queue(node,0);
    __atomic_store_n(&queue->registrations,registrations-1,__ATOMIC_RELEASE);
}
void kernel_wait_queue_remove(struct kernel_wait_node *node)
{
    if (!node) return;
    /* Caller owns the source, or serializes source detach (epoll POLLFREE).
     * Never capture borrow_owner here: a detached source may already be gone. */
    for (;;) {
        struct kernel_wait_queue *queue=__atomic_load_n(&node->queue,__ATOMIC_ACQUIRE);
        if (!queue) return;
        KERNEL_IRQ_LOCK_SCOPE(guard,&queue->lock);
        if (node->queue!=queue) continue;
        kernel_wait_node_remove_locked(node);
        return;
    }
}
void kernel_wait_queue_add(struct kernel_wait_queue *queue, struct kernel_wait_node *node)
{
    KERNEL_IRQ_LOCK_SCOPE(guard,&queue->lock);
    if (node && node->task) {
        struct kernel_wait_record *record=kernel_wait_record_of(node->task);
        if (kernel_wait_record_phase(record)!=KERNEL_WAIT_FINISHED && !node->generation) {
            if (node->task!=kernel_wait_current_task()) __builtin_trap();
            node->generation=kernel_wait_record_generation(record);
            node->token_next=record->nodes;
            record->nodes=node;
        }
    }
    kernel_wait_node_add_locked(queue,node);
}
static int matches(const struct kernel_wait_token *token)
{
    return token && token->task && token->generation &&
        kernel_wait_record_generation(kernel_wait_record_of(token->task)) == token->generation &&
        kernel_wait_record_phase(kernel_wait_record_of(token->task)) != KERNEL_WAIT_FINISHED;
}
enum kernel_scheduler_status kernel_wait_node_bind_locked(struct kernel_wait_queue *queue,
    struct kernel_wait_node *node, const struct kernel_wait_token *token)
{
    if (queue->closed || !matches(token)) return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    if (node->task != token->task || node->generation) __builtin_trap();
    struct kernel_wait_record *record = kernel_wait_record_of(token->task);
    node->generation = token->generation;
    node->token_next = record->nodes;
    record->nodes = node;
    kernel_wait_node_add_locked(queue, node);
    return KERNEL_SCHEDULER_STATUS_OK;
}
enum kernel_scheduler_status kernel_wait_node_bind(struct kernel_wait_queue *queue,
    struct kernel_wait_node *node, const struct kernel_wait_token *token)
{
    if (!queue || !node || !token || queue->initialized!=KERNEL_WAIT_QUEUE_INITIALIZED)
        return KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT;
    KERNEL_IRQ_LOCK_SCOPE(guard,&queue->lock);
    return kernel_wait_node_bind_locked(queue,node,token);
}
static enum kernel_scheduler_status prepare_owner(struct kernel_wait_queue *queue,
    uint64_t deadline, int interruptible, struct kernel_wait_token *token)
{
    struct kernel_task *task=kernel_wait_current_task();
    if (!task) return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    struct kernel_wait_record *record=kernel_wait_record_of(task);
    if (kernel_wait_record_phase(record)!=KERNEL_WAIT_FINISHED || record->nodes ||
        kernel_wait_record_borrows(record)) __builtin_trap();
    if (kernel_wait_record_generation(record)==KERNEL_WAIT_GENERATION_MAX ||
        (queue && queue->closed)) return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    uint64_t generation;
#if BOAROS_MAX_CPUS > 1
    generation=__atomic_load_n(&wait_generation,__ATOMIC_RELAXED);
    do {
        if (generation==KERNEL_WAIT_GENERATION_MAX) return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    } while (!__atomic_compare_exchange_n(&wait_generation,&generation,generation+1,0,
                __ATOMIC_RELAXED,__ATOMIC_RELAXED));
    generation++;
#else
    if (wait_generation==KERNEL_WAIT_GENERATION_MAX) return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    generation=++wait_generation;
#endif
    record->deadline=deadline;record->interruptible=interruptible;
    kernel_wait_record_store(record,generation,KERNEL_WAIT_PREPARED,KERNEL_WAIT_WOKEN);
    *token=(struct kernel_wait_token){task,generation};
    if (queue) {
        struct kernel_wait_node *node=kernel_wait_default_node(task);
        if (node->generation || node->queue || kernel_wait_node_references(node) ||
            (node->task && node->task!=task)) __builtin_trap();
        node->task=task;node->generation=generation;record->nodes=node;
        kernel_wait_node_add_locked(queue,node);
    }
    return KERNEL_SCHEDULER_STATUS_OK;
}
enum kernel_scheduler_status kernel_wait_prepare(struct kernel_wait_queue *queue,
    uint64_t deadline,int interruptible,struct kernel_wait_token *token)
{
    if (!token || (interruptible!=0 && interruptible!=1) ||
        (queue && queue->initialized!=KERNEL_WAIT_QUEUE_INITIALIZED))
        return KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT;
    if (!kernel_wait_backend_initialized()) return KERNEL_SCHEDULER_STATUS_NOT_INITIALIZED;
    if (queue) {
        KERNEL_IRQ_LOCK_SCOPE(guard,&queue->lock);
        return prepare_owner(queue,deadline,interruptible,token);
    }
    KERNEL_IRQ_SCOPE(irq);
    return prepare_owner(0,deadline,interruptible,token);
}
#if BOAROS_USER_SMP
static int notify_locked(const struct kernel_wait_token *token,uint32_t reason)
#else
int kernel_wait_notify_locked(const struct kernel_wait_token *token,uint32_t reason)
#endif
{
    if (!token || !token->task || !token->generation) return 0;
    struct kernel_wait_record *record=kernel_wait_record_of(token->task);
#if BOAROS_MAX_CPUS > 1
    uint64_t state=kernel_wait_record_load(record);
    if ((state>>5)!=token->generation) return 0;
    unsigned phase=(unsigned)(state&3U);
#else
    if (!matches(token)) return 0;
    unsigned phase=record->phase;
#endif
    if (phase==KERNEL_WAIT_FINISHED || phase==KERNEL_WAIT_NOTIFIED) return 0;
#if BOAROS_MAX_CPUS > 1
    if (phase==KERNEL_WAIT_PREPARED) {
        uint64_t notified=(state&~UINT64_C(15))|((uint64_t)reason<<2)|KERNEL_WAIT_NOTIFIED;
        if (!__atomic_compare_exchange_n(&record->state,&state,notified,0,
                __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) {
            /* The owner can finish or start its next wait without this domain. */
            if ((state>>5)==token->generation && (state&3U)==KERNEL_WAIT_COMMITTED)
                __builtin_trap();
            return 0;
        }
        COST_ADD(WAIT_NOTIFICATIONS,1);
        return 1;
    }
#endif
    if (phase==KERNEL_WAIT_COMMITTED) {
        kernel_wait_backend_notify(token->task,reason);
        if (record->on_cpu) record->ready_pending=1;
        else kernel_wait_backend_ready(token->task);
    }
    COST_ADD(WAIT_NOTIFICATIONS,1);
    kernel_wait_record_transition(record,token->generation,KERNEL_WAIT_NOTIFIED,reason);
    return 1;
}
#if BOAROS_USER_SMP
int kernel_wait_notify_locked(const struct kernel_wait_token *token,uint32_t reason)
{
    if (token && token->task && kernel_wait_record_phase(kernel_wait_record_of(token->task))==KERNEL_WAIT_COMMITTED) {
        struct kernel_raw_guard run __attribute__((cleanup(kernel_raw_scope_release)))={0};
        int local=kernel_wait_backend_lock_local(token,&run,1);
        if (local>=0) return local ? notify_locked(token,reason) : 0;
        KERNEL_RUN_SET(runs);
        return notify_locked(token,reason);
    }
    return notify_locked(token,reason);
}
#endif
static int notify_token(const struct kernel_wait_token *token,uint32_t reason)
{
#if BOAROS_MAX_CPUS > 1
    uint64_t state=kernel_wait_record_load(kernel_wait_record_of(token->task));
    if ((state>>5)!=token->generation || (state&3U)==KERNEL_WAIT_FINISHED ||
        (state&3U)==KERNEL_WAIT_NOTIFIED) return 0;
    if ((state&3U)==KERNEL_WAIT_PREPARED) {
        uint64_t notified=(state&~UINT64_C(15))|((uint64_t)reason<<2)|KERNEL_WAIT_NOTIFIED;
        if (__atomic_compare_exchange_n(&kernel_wait_record_of(token->task)->state,&state,notified,0,
                __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) {
            COST_ADD(WAIT_NOTIFICATIONS,1);
            return 1;
        }
        if ((state>>5)!=token->generation || (state&3U)==KERNEL_WAIT_FINISHED ||
            (state&3U)==KERNEL_WAIT_NOTIFIED) return 0;
    }
#if BOAROS_USER_SMP
    struct kernel_raw_guard guard __attribute__((cleanup(kernel_raw_scope_release)))={0};
    if ((state&3U)==KERNEL_WAIT_COMMITTED) {
        int result=kernel_wait_backend_lock_local(token,&guard,0);
        if (result>0) return notify_locked(token,reason);
        if (!result) return 0;
    }
    kernel_raw_lock_acquire(&kernel_wait_domain,&guard);
    return kernel_wait_notify_locked(token,reason);
#endif
#endif
#if !BOAROS_USER_SMP
    KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
    return kernel_wait_notify_locked(token,reason);
#endif
}
enum kernel_scheduler_status kernel_wait_notify(const struct kernel_wait_token *token,
    enum kernel_wait_wake_reason reason)
{
    if (!token || !token->task || reason>KERNEL_WAIT_SIGNALLED)
        return KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT;
    (void)notify_token(token,reason);
    return KERNEL_SCHEDULER_STATUS_OK;
}
enum kernel_scheduler_status kernel_wait_park(const struct kernel_wait_token *token,
    enum kernel_wait_wake_reason *reason)
{
    kernel_assert_can_block();
    KERNEL_IRQ_SCOPE(park_irq);
    if (!token || !reason || !token->task) return KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT;
    if (token->task != kernel_wait_current_task()) __builtin_trap();
#if BOAROS_MAX_CPUS > 1
    struct kernel_wait_record *record_fast = kernel_wait_record_of(token->task);
    uint64_t state_fast = kernel_wait_record_load(record_fast);
    if ((state_fast >> 5) == token->generation && (state_fast & 3U) == KERNEL_WAIT_NOTIFIED) {
        *reason = (enum kernel_wait_wake_reason)((state_fast >> 2) & 3U);
        return KERNEL_SCHEDULER_STATUS_OK;
    }
    enum kernel_scheduler_status status;
#endif
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        if (!matches(token)) return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        struct kernel_wait_record *record = kernel_wait_record_of(token->task);
        if (kernel_wait_record_phase(record) == KERNEL_WAIT_PREPARED) {
            if (record->interruptible && kernel_wait_backend_signal(token->task))
                kernel_wait_notify_locked(token, KERNEL_WAIT_SIGNALLED);
            else if (record->deadline && (int64_t)(kernel_wait_backend_time() - record->deadline) >= 0)
                kernel_wait_notify_locked(token, KERNEL_WAIT_TIMEOUT);
        }
        unsigned phase=kernel_wait_record_phase(record);
        if (phase == KERNEL_WAIT_NOTIFIED) {
            *reason = kernel_wait_record_reason(record); return KERNEL_SCHEDULER_STATUS_OK;
        }
        if (phase != KERNEL_WAIT_PREPARED) __builtin_trap();
#if BOAROS_MAX_CPUS > 1
        /* A PREPARED notifier may win after the phase check above. Never use
         * a later NOTIFIED snapshot as an expected value for committing sleep. */
        uint64_t expected=kernel_wait_state(token->generation,KERNEL_WAIT_PREPARED,KERNEL_WAIT_WOKEN) |
            ((uint64_t)record->interruptible<<4);
        uint64_t committed=(expected&~UINT64_C(3))|KERNEL_WAIT_COMMITTED;
        if (!__atomic_compare_exchange_n(&record->state,&expected,committed,0,
                __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) {
            if ((expected >> 5) != token->generation || (expected & 3U) != KERNEL_WAIT_NOTIFIED)
                __builtin_trap();
            *reason=(enum kernel_wait_wake_reason)((expected >> 2) & 3U);
            return KERNEL_SCHEDULER_STATUS_OK;
        }
#else
        record->phase = KERNEL_WAIT_COMMITTED;
#endif
        kernel_wait_backend_commit(token->task, record->deadline, record->interruptible);
#if BOAROS_MAX_CPUS > 1
        status = kernel_wait_backend_switch_locked(token->task, &guard);
#endif
    }
#if BOAROS_MAX_CPUS == 1
    enum kernel_scheduler_status status = kernel_wait_backend_switch(token->task);
#endif
    if (status != KERNEL_SCHEDULER_STATUS_OK) return status;
    /* 调度交接已把CPU交还当前owner。NOTIFIED结果到本owner的finish前
     * 保持不变；IRQ仍关闭，无需为读取完成原因再次取得共享调度保护。 */
    struct kernel_wait_record *record = kernel_wait_record_of(token->task);
    if (kernel_wait_record_phase(record) != KERNEL_WAIT_NOTIFIED || !record->on_cpu) __builtin_trap();
    *reason = kernel_wait_record_reason(record);
    return KERNEL_SCHEDULER_STATUS_OK;
}
enum kernel_scheduler_status kernel_wait_node_remove_sync(struct kernel_wait_node *node)
{
    if (!node) return KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT;
    kernel_assert_can_block();
    kernel_wait_queue_remove(node);
    while (kernel_wait_node_references(node)) kernel_wait_backend_quiesce();
    return KERNEL_SCHEDULER_STATUS_OK;
}
enum kernel_scheduler_status kernel_wait_finish(struct kernel_wait_token *token)
{
    kernel_assert_can_block();
    if (!token || !token->task) return KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT;
    if (token->task!=kernel_wait_current_task()) __builtin_trap();
    KERNEL_IRQ_SCOPE(irq);
    struct kernel_wait_record *record=kernel_wait_record_of(token->task);
    if (!matches(token)) return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    if (kernel_wait_record_phase(record)==KERNEL_WAIT_COMMITTED) __builtin_trap();
    while (record->nodes) {
        struct kernel_wait_node *node=record->nodes;
        kernel_wait_queue_remove(node);
        if (kernel_wait_node_references(node)) { kernel_wait_backend_quiesce();continue; }
        record->nodes=node->token_next;
        node->generation=0;node->event_mask=0;node->token_next=0;
    }
    while (kernel_wait_record_borrows(record)) kernel_wait_backend_quiesce();
    record->deadline=0;record->interruptible=0;
    kernel_wait_record_transition(record,token->generation,KERNEL_WAIT_FINISHED,KERNEL_WAIT_WOKEN);
    *token=(struct kernel_wait_token){0};
    return KERNEL_SCHEDULER_STATUS_OK;
}
void kernel_wait_switch_finish_locked(struct kernel_task *task)
{
    struct kernel_wait_record *record = kernel_wait_record_of(task);
    if (!record->on_cpu) __builtin_trap();
#if BOAROS_USER_SMP
    __atomic_store_n(&record->on_cpu,0,__ATOMIC_RELEASE);
#else
    record->on_cpu = 0;
#endif
    if (record->ready_pending) {
        record->ready_pending = 0;
        kernel_wait_backend_ready(task);
    }
}
static int event_accepts(const struct kernel_wait_node *node,uint64_t event,struct kernel_task *recipient)
{
    if(!event || !node->event_mask)return 1;
    return (__atomic_load_n(node->event_mask,__ATOMIC_ACQUIRE) & event) && (!node->task || !recipient || node->task==recipient);
}
static int cursor_prepare_queue_locked(struct kernel_wait_queue *queue,
    struct kernel_wait_cursor *cursor,int allow_empty,uint64_t event,struct kernel_task *recipient)
{
    if (!queue || queue->initialized!=KERNEL_WAIT_QUEUE_INITIALIZED || queue->borrows==UINT32_MAX)
        __builtin_trap();
    queue_shape(queue);
    if (!allow_empty && !queue->head) return 0;
    queue->borrows++;
    *cursor=(struct kernel_wait_cursor){.queue=queue,.node=queue->head,
        .sequence=queue->sequence,.event=event,.recipient=recipient};
    if (cursor->node) borrow(cursor->node);
    return 1;
}
int kernel_wait_cursor_prepare_event(struct kernel_wait_queue *queue,
    struct kernel_wait_cursor *cursor,uint64_t event,struct kernel_task *recipient)
{
    if (!queue || queue->initialized!=KERNEL_WAIT_QUEUE_INITIALIZED) __builtin_trap();
    if (!kernel_wait_queue_registrations(queue)) return 0;
    KERNEL_IRQ_LOCK_SCOPE(guard,&queue->lock);
    return cursor_prepare_queue_locked(queue,cursor,0,event,recipient);
}
int kernel_wait_cursor_prepare(struct kernel_wait_queue *queue,struct kernel_wait_cursor *cursor)
{ return kernel_wait_cursor_prepare_event(queue,cursor,0,0); }
void kernel_wait_cursor_begin(struct kernel_wait_queue *queue,struct kernel_wait_cursor *cursor)
{
    KERNEL_IRQ_LOCK_SCOPE(guard,&queue->lock);
    (void)cursor_prepare_queue_locked(queue,cursor,1,0,0);
}
void kernel_wait_cursor_advance(struct kernel_wait_cursor *cursor)
{
    struct kernel_wait_node *old = cursor->node;
    if (!old) return;
    {
        KERNEL_IRQ_LOCK_SCOPE(guard, &cursor->queue->lock);
        struct kernel_wait_node *next = old->queue ? old->next : old->retired_next;
        if (next && next->sequence <= cursor->sequence) borrow(next);
        else next = 0;
        cursor->node = next;
        /* 前后节点的借用交接属于同一保护，不必解锁后重新取得。 */
        old = put_batch_locked(old);
    }
    put(old);
}
void kernel_wait_cursor_end(struct kernel_wait_cursor *cursor)
{
    struct kernel_wait_node *remaining;
    {
        KERNEL_IRQ_LOCK_SCOPE(guard, &cursor->queue->lock);
        remaining = put_batch_locked(cursor->node);
        if (!remaining) {
            if (!cursor->queue || !cursor->queue->borrows) __builtin_trap();
            cursor->queue->borrows--;
            *cursor = (struct kernel_wait_cursor){0};
            return;
        }
    }
    put(remaining);
    KERNEL_IRQ_LOCK_SCOPE(guard, &cursor->queue->lock);
    if (!cursor->queue || !cursor->queue->borrows) __builtin_trap();
    cursor->queue->borrows--;
    *cursor = (struct kernel_wait_cursor){0};
}
void kernel_wait_task_pin(struct kernel_task *task)
{ record_get(kernel_wait_record_of(task)); }
void kernel_wait_task_unpin(struct kernel_task *task)
{ record_put(kernel_wait_record_of(task)); }
uint64_t kernel_wait_notification_sequence(void)
{
    struct kernel_cpu *cpu = kernel_cpu_current();
    if (!cpu->wait_notification_depth) return 0;
    unsigned logical = 0;
#if BOAROS_MAX_CPUS > 1
    logical = cpu->logical_id;
#endif
    return (cpu->wait_notification_sequence << 8) | (logical + 1U);
}
enum kernel_scheduler_status kernel_wait_node_move_locked(struct kernel_wait_node *node, struct kernel_wait_queue *queue)
{
    if (!node || !node->task || !queue || queue->initialized != KERNEL_WAIT_QUEUE_INITIALIZED)
        return KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT;
    struct kernel_wait_token token = {node->task, node->generation};
    if (queue->closed) return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    if (!node->queue || !matches(&token)) return KERNEL_SCHEDULER_STATUS_EMPTY;
    /* 调用者先归还游标node借用，另借task覆盖重查与移动；不把旧队列的
     * retired_next/borrow_owner带进新队列，也不拆开可见的登记位置。 */
    if (node->references || node->retired_next) __builtin_trap();
    if (node->queue == queue) return KERNEL_SCHEDULER_STATUS_OK;
    kernel_wait_node_remove_locked(node);
    kernel_wait_node_add_locked(queue, node);
    return KERNEL_SCHEDULER_STATUS_OK;
}
static void cursor_wake_reason(struct kernel_wait_cursor *cursor,int all,uint32_t reason)
{
    struct kernel_cpu *cpu=kernel_cpu_current();
    if (!cpu->wait_notification_depth++) {
        if (cpu->wait_notification_sequence==(UINT64_MAX>>8)) __builtin_trap();
        cpu->wait_notification_sequence++;
    }
    while (cursor->node) {
        struct kernel_wait_node *node=cursor->node;
        kernel_wait_callback_fn callback=0;
        struct kernel_wait_token token={0};
        {
            KERNEL_IRQ_LOCK_SCOPE(guard,&cursor->queue->lock);
            if (node->queue==cursor->queue && event_accepts(node,cursor->event,cursor->recipient)) {
                callback=node->callback;
                if (!callback && node->task && node->generation)
                    token=(struct kernel_wait_token){node->task,node->generation};
            }
        }
        int notified=0;
        /* Cursor/node borrows cover both the token and callback after unlock. */
        if (token.task) notified=notify_token(&token,KERNEL_WAIT_WOKEN);
        if (callback) {
            kernel_preempt_disable();callback(node,reason);kernel_preempt_enable();
            notified=1;
        }
        if (notified && !all) break;
        kernel_wait_cursor_advance(cursor);
    }
    kernel_wait_cursor_end(cursor);
    cpu->wait_notification_depth--;
}
void kernel_wait_cursor_wake(struct kernel_wait_cursor *cursor, int all)
{ cursor_wake_reason(cursor,all,KERNEL_WAIT_WOKEN); }
void kernel_wait_queue_pollfree(struct kernel_wait_queue *queue)
{
    struct kernel_wait_cursor cursor={0};
    KERNEL_IRQ_SCOPE(irq);
    {
        KERNEL_IRQ_LOCK_SCOPE(guard,&queue->lock);
        if (queue->initialized!=KERNEL_WAIT_QUEUE_INITIALIZED || queue->closed) __builtin_trap();
        queue->closed=1;
        (void)cursor_prepare_queue_locked(queue,&cursor,1,0,0);
    }
    cursor_wake_reason(&cursor,1,KERNEL_WAIT_POLL_FREE);
}
static enum kernel_scheduler_status wake(struct kernel_wait_queue *queue, int all)
{
    if (!queue || queue->initialized != KERNEL_WAIT_QUEUE_INITIALIZED)
        return KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT;
    if (!kernel_wait_backend_initialized()) return KERNEL_SCHEDULER_STATUS_NOT_INITIALIZED;
    if (arch_interrupt_is_enabled()) return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    struct kernel_wait_cursor cursor;
    if (kernel_wait_cursor_prepare(queue, &cursor)) kernel_wait_cursor_wake(&cursor, all);
    return KERNEL_SCHEDULER_STATUS_OK;
}
enum kernel_scheduler_status kernel_wait_queue_wake_one(struct kernel_wait_queue *queue) { return wake(queue, 0); }
enum kernel_scheduler_status kernel_wait_queue_wake_all(struct kernel_wait_queue *queue) { return wake(queue, 1); }
enum kernel_scheduler_status kernel_wait_queue_close(struct kernel_wait_queue *queue)
{
    if (!queue || queue->initialized!=KERNEL_WAIT_QUEUE_INITIALIZED)
        return KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT;
    KERNEL_IRQ_SCOPE(irq);
    struct kernel_wait_cursor cursor={0};
    {
        KERNEL_IRQ_LOCK_SCOPE(guard,&queue->lock);
        queue_shape(queue);queue->closed=1;
        (void)cursor_prepare_queue_locked(queue,&cursor,1,0,0);
    }
    cursor_wake_reason(&cursor,1,KERNEL_WAIT_WOKEN);
    return KERNEL_SCHEDULER_STATUS_OK;
}
enum kernel_scheduler_status kernel_wait_queue_destroy(struct kernel_wait_queue *queue)
{
    if (!queue || queue->initialized!=KERNEL_WAIT_QUEUE_INITIALIZED)
        return KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT;
    KERNEL_IRQ_LOCK_SCOPE(guard,&queue->lock);
    if (!queue->closed) return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    queue_shape(queue);
    if (kernel_wait_queue_registrations(queue) || queue->borrows) return KERNEL_SCHEDULER_STATUS_BUSY;
    /* Keep the embedded lock intact until its guard has actually released it. */
    queue->initialized=0;queue->closed=0;queue->sequence=0;
    return KERNEL_SCHEDULER_STATUS_OK;
}
