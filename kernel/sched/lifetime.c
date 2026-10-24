#include "private.h"
#include <kernel/credentials.h>

static void domain_owned(void)
{
    struct kernel_raw_guard *guard = kernel_cpu_current()->raw_locks;
    if (!guard || guard->lock != &kernel_wait_domain) __builtin_trap();
}

void scheduler_task_get_locked(struct kernel_task *task)
{
    domain_owned();
    if (!task || task->retirement > 2 || task->storage_borrows == UINT32_MAX)
        __builtin_trap();
    task->storage_borrows++;
}

struct kernel_task *scheduler_task_lookup_locked(kernel_pid_t number,
    enum kernel_pid_role role, uint64_t generation)
{
    domain_owned();
    if ((unsigned)role >= KERNEL_PID_ROLES) return 0;
    struct kernel_pid *id = kernel_pid_find(&scheduler.identities, number);
    if (!id || (generation && id->generation != generation)) return 0;
    struct kernel_task *task = id->members[role] ? id->members[role]->task : 0;
    if (!task || !task->accounted || task->retirement) return 0;
    scheduler_task_get_locked(task);
    return task;
}

struct kernel_task *scheduler_task_lookup(kernel_pid_t number,
    enum kernel_pid_role role, uint64_t generation)
{
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    return scheduler_task_lookup_locked(number, role, generation);
}

struct kernel_task *kernel_task_find_by_tid(kernel_pid_t tid)
{
    struct kernel_task *task;
    int visible;
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        task = scheduler_task_lookup_locked(tid, KERNEL_PID_TID, 0U);
        /* 退出线程的 TID 仍供清理者迁移 child_creator；进程身份则需
         * 保留到最后成员离开及父进程 wait，不能按信号可投递性过滤。 */
        visible = task && (task->state != KERNEL_THREAD_STATE_EXITED ||
                           task->identities[KERNEL_PID_TGID].identity);
    }
    if (task && !visible) scheduler_task_put(task);
    return visible ? task : 0;
}

static void queue_disposal_locked(struct kernel_task *task)
{
    if (task->retirement != 2 || task->storage_borrows) return;
    if (task->wait.on_cpu || task->wait.borrows || task->wait.nodes ||
        task->wait.phase != KERNEL_WAIT_FINISHED) __builtin_trap();
    task->retirement = 3;
    task->retired_next = scheduler.retired_tasks;
    scheduler.retired_tasks = task;
}

static struct kernel_task *put_locked(struct kernel_task *task)
{
    if (!task->storage_borrows || task->retirement > 2) __builtin_trap();
    if (--task->storage_borrows || !task->retirement) return 0;
    struct kernel_task *next = task->all_next;
    task->all_next = 0;
    queue_disposal_locked(task);
    return next;
}

static void disposal_progress(void)
{
    /* Resources and old stack are already gone. A normal trusted caller can
     * return metadata now; callback/preemption scopes leave the real queue owner. */
    struct kernel_cpu *cpu = kernel_cpu_current();
    if (cpu->raw_locks) __builtin_trap();
    if (!cpu->preempt_depth
#if BOAROS_MAX_CPUS > 1
        && cpu->logical_id == 0 && (!cpu->current || !cpu->current->controlled)
#endif
    ) scheduler_task_collect();
    else scheduler_task_disposal_notify();
}

void scheduler_task_put(struct kernel_task *task)
{
    if (!task) __builtin_trap();
    int notify = 0;
    do {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        /* At most16 retired cursor links per raw interval. The next link
         * remains owned between segments; disposal cannot consume it. */
        for (unsigned count = 0; task && count < 16; count++) {
            struct kernel_task *old = task;
            task = put_locked(old);
            notify |= old->retirement == 3;
        }
    } while (task);
    if (notify) disposal_progress();
}

void scheduler_task_publish_locked(struct kernel_task *task)
{
    domain_owned();
    if (task->accounted || task->retirement || task->all_next || task->all_previous)
        __builtin_trap();
    process_identity_commit_locked(task);
    task->all_next = scheduler.all_tasks;
    if (task->all_next) task->all_next->all_previous = task;
    scheduler.all_tasks = task;
    task->accounted = 1;
    if (scheduler.task_count == UINT64_MAX) __builtin_trap();
    scheduler.task_count++;
}

void scheduler_task_unlink_locked(struct kernel_task *task)
{
    domain_owned();
    if (task->retirement) __builtin_trap();
    if (task->accounted) {
        if (task->all_previous && task->all_previous->all_next != task) __builtin_trap();
        if (task->all_next && task->all_next->all_previous != task) __builtin_trap();
        if (task->all_previous) task->all_previous->all_next = task->all_next;
        else {
            if (scheduler.all_tasks != task) __builtin_trap();
            scheduler.all_tasks = task->all_next;
        }
        if (task->all_next) task->all_next->all_previous = task->all_previous;
        if (task->storage_borrows && task->all_next)
            scheduler_task_get_locked(task->all_next);
        else task->all_next = 0;
        task->all_previous = 0;
        task->accounted = 0;
        if (!scheduler.task_count) __builtin_trap();
        scheduler.task_count--;
    }
    else if (task->all_next || task->all_previous) __builtin_trap();
    /* 摘索引即把存储owner交给清理者；retire只改变可释放资格，不能在
     * 两个阶段间把真实owner漏出关机计数。 */
    if (scheduler.retired_pending == UINT64_MAX) __builtin_trap();
    scheduler.retired_pending++;
#if BOAROS_USER_SMP
    if (task->reservation_cpu || task->reservation_node.queued || task->service_node.queued)
        __builtin_trap();
#endif
    task->retirement = 1;
}

void scheduler_task_retire_locked(struct kernel_task *task)
{
    /* The trusted reaper/rollback has finished real resource cleanup. */
    domain_owned();
    if (task->retirement != 1 || !scheduler.retired_pending ||
        task->stack_physical_address != KERNEL_THREAD_NO_PAGE ||
        task->wait.on_cpu || task->wait.borrows || task->wait.nodes || task->wait.phase != KERNEL_WAIT_FINISHED)
        __builtin_trap();
    task->retirement = 2;
    queue_disposal_locked(task);
}

void scheduler_task_retire(struct kernel_task *task)
{
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    scheduler_task_retire_locked(task);
}

void scheduler_task_collect(void)
{
    kernel_assert_service_context();
    int notify = 0, completed = 0;
#if BOAROS_MAX_CPUS > 1
    int completed_controlled = 0;
#endif
    for (;;) {
        struct kernel_task *task;
        {
            KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
            /* 摘队列只转移给本次collector；页服务尚未完成时仍有owner。
             * 上一次实际归还结束后才发布计数，并与下一次摘取合并。 */
            if (completed) {
#if BOAROS_MAX_CPUS > 1
                if (completed_controlled) {
                    if (!scheduler.controlled_owners) __builtin_trap();
                    scheduler.controlled_owners--;
                    completed_controlled = 0;
                }
#endif
                if (!scheduler.retired_pending) __builtin_trap();
                scheduler.retired_pending--;
                completed = 0;
                notify = scheduler.shutdown_waiting && !scheduler.retired_pending;
                if (notify) scheduler.shutdown_progress = 1;
            }
            task = scheduler.retired_tasks;
            if (!task) break;
            scheduler.retired_tasks = task->retired_next;
            if (!scheduler.retired_pending) __builtin_trap();
            if (task->retirement != 3 || task->storage_borrows || task->wait.on_cpu)
                __builtin_trap();
#if BOAROS_MAX_CPUS > 1
            completed_controlled = task->controlled == 2;
#endif
        }
        /* No rank40 lock may be held while the allocator consumes the page. */
        kernel_credentials_put(task->credentials);
        task->credentials = 0;
        kernel_exec_security_put(task->exec_security);
        task->exec_security = 0;
        (void)physical_page_release(scheduler.allocator, task->physical_address);
        completed = 1;
    }
    /* PID1 teardown may be waiting for a borrowed retired record even after
     * user indices are empty. Ordinary inline disposal needs no worker turn. */
    if (notify && kernel_cpu_current()->current != scheduler.cleanup_task)
        scheduler_task_disposal_notify();
}

void scheduler_task_cursor_begin(struct scheduler_task_cursor *cursor)
{
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    cursor->task = scheduler.all_tasks;
    if (cursor->task) scheduler_task_get_locked(cursor->task);
}

void scheduler_task_cursor_next(struct scheduler_task_cursor *cursor)
{
    int notify;
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        struct kernel_task *old = cursor->task;
        if (!old) __builtin_trap();
        cursor->task = old->all_next;
        if (cursor->task) scheduler_task_get_locked(cursor->task);
        struct kernel_task *link_owner = put_locked(old);
        if (link_owner && put_locked(link_owner)) __builtin_trap(); /* next was pinned above */
        notify = old->retirement == 3;
    }
    if (notify) disposal_progress();
}

void scheduler_task_cursor_end(struct scheduler_task_cursor *cursor)
{
    if (cursor->task) scheduler_task_put(cursor->task);
    cursor->task = 0;
}

void kernel_task_lookup_put(struct kernel_task *task) { scheduler_task_put(task); }

void exited_append_locked(struct kernel_task *thread)
{
    if (thread->exit_queued || thread->next || thread->exit_previous) __builtin_trap();
    thread->exit_previous = scheduler.exited_tail;
    if (scheduler.exited_tail) scheduler.exited_tail->next = thread;
    else scheduler.exited_head = thread;
    scheduler.exited_tail = thread;
    thread->exit_queued = 1;
}

void exited_remove_locked(struct kernel_task *thread)
{
    if (!thread->exit_queued) __builtin_trap();
    if (thread->exit_previous) thread->exit_previous->next = thread->next;
    else {
        if (scheduler.exited_head != thread) __builtin_trap();
        scheduler.exited_head = thread->next;
    }
    if (thread->next) thread->next->exit_previous = thread->exit_previous;
    else scheduler.exited_tail = thread->exit_previous;
    thread->next = thread->exit_previous = 0;
    thread->exit_queued = 0;
}

enum kernel_scheduler_status scheduler_task_claim_exit_locked(struct kernel_task *task)
{
    domain_owned();
    if (!task || task->state != KERNEL_THREAD_STATE_EXITED || !task->exit_queued)
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    if (task->wait.on_cpu || task->wait.borrows || task->reaping)
        return KERNEL_SCHEDULER_STATUS_BUSY;
    task->reaping = 1;
    return KERNEL_SCHEDULER_STATUS_OK;
}

enum kernel_scheduler_status scheduler_join_claim_locked(struct kernel_thread_join *join,
                                                           struct kernel_task **owner)
{
    domain_owned();
    *owner = 0;
    if (!join || !join->task) return KERNEL_SCHEDULER_STATUS_EMPTY;
    struct kernel_task *task = join->task;
    if (task->state != KERNEL_THREAD_STATE_EXITED) return KERNEL_SCHEDULER_STATUS_BUSY;
    enum kernel_scheduler_status status = scheduler_task_claim_exit_locked(task);
    if (status != KERNEL_SCHEDULER_STATUS_OK) return status;
    if (task->arch.user_mode || task->io_context.locks ||
        task->io_context.backend_depth || task->io_buffer || task->socket_call) __builtin_trap();
    exited_remove_locked(task);
    join->task = 0;
    task->join = 0;
    *owner = task;
    return KERNEL_SCHEDULER_STATUS_OK;
}

int scheduler_task_descends_from(struct kernel_task *task,
                                const struct kernel_task *root, uint64_t identity)
{
    SCHEDULER_TASK_BORROW(owner, 0);
    uint32_t depth = 0;
    while (task) {
        struct kernel_task *next_owner;
        {
            KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
            for (unsigned n = 0; task && n < 16; n++) {
                if (++depth > KERNEL_PID_LIMIT) __builtin_trap();
                if ((root && task == root) || (identity && process_identity_generation(task) == identity))
                    return 1;
                struct kernel_task *leader = task->group_leader;
                task = leader ? leader->parent : task->parent;
            }
            next_owner = task;
            if (next_owner) scheduler_task_get_locked(next_owner);
        }
        if (owner) scheduler_task_put(owner);
        owner = next_owner;
    }
    return 0;
}

void scheduler_task_claim_release_locked(struct kernel_task *task)
{
    domain_owned();
    if (!task || !task->reaping) __builtin_trap();
    task->reaping = 0;
}

int scheduler_cleanup_ready_locked(void)
{
    domain_owned();
    struct kernel_task *task = scheduler.exited_head;
    /* Borrowed retirement is pending ownership, not executable work. A
     * temporary event/old-stack owner must run before this head can be reaped. */
    return scheduler.retired_tasks || (task && !task->reaping &&
        !task->wait.on_cpu && !task->wait.borrows);
}

void scheduler_task_wait_claim_release(struct kernel_task *task)
{
    struct kernel_wait_cursor parent_cursor, cleanup_cursor;
    struct kernel_task *parent = 0;
    int wake_parent = 0, wake_cleanup = 0;
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        scheduler_task_claim_release_locked(task);
        if (task->parent) {
            parent = task->parent;
            wake_parent = kernel_wait_cursor_prepare_locked(&parent->child_exit_queue, &parent_cursor, 1);
            if (wake_parent) scheduler_task_get_locked(parent);
        }
        if (task->exit_queued)
            wake_cleanup = kernel_wait_cursor_prepare_locked(&scheduler.cleanup_queue, &cleanup_cursor, 1);
    }
    /* Failed user copies leave the event selectable. Queue/cursor ownership
     * spans notification; callbacks and the parent's final put stay outside. */
    if (wake_parent) {
        kernel_wait_cursor_wake(&parent_cursor, 1);
        scheduler_task_put(parent);
    }
    if (wake_cleanup) kernel_wait_cursor_wake(&cleanup_cursor, 1);
}

void scheduler_group_cursor_begin(struct kernel_task *task, struct scheduler_group_cursor *cursor)
{
    *cursor = (struct scheduler_group_cursor){0};
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    struct kernel_task *leader = task ? task->group_leader : 0;
    if (!leader || leader->retirement || leader->group_leader != leader) return;
    scheduler_task_get_locked(leader);
    cursor->leader = cursor->task = leader;
    cursor->epoch = leader->group_epoch;
}
void scheduler_group_cursor_next(struct scheduler_group_cursor *cursor)
{
    struct kernel_task *old = cursor->member_owned ? cursor->task : 0;
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        struct kernel_task *leader = cursor->leader;
        if (!leader || !cursor->task) __builtin_trap();
        cursor->member_owned = 0;
        if (leader->retirement || leader->group_leader != leader) cursor->task = 0;
        else if (leader->group_epoch != cursor->epoch) {
            cursor->epoch = leader->group_epoch;
            cursor->task = leader;
            cursor->steps = 0;
        } else {
            if (++cursor->steps > KERNEL_PID_LIMIT) __builtin_trap();
            struct kernel_task *next = cursor->task->group_next;
            if (!next || next->group_previous != cursor->task) __builtin_trap();
            cursor->task = next == leader ? 0 : next;
            if (cursor->task) {
                scheduler_task_get_locked(cursor->task);
                cursor->member_owned = 1;
            }
        }
    }
    if (old) scheduler_task_put(old);
}
void scheduler_group_cursor_end(struct scheduler_group_cursor *cursor)
{
    if (cursor->member_owned) scheduler_task_put(cursor->task);
    if (cursor->leader) scheduler_task_put(cursor->leader);
    *cursor = (struct scheduler_group_cursor){0};
}
