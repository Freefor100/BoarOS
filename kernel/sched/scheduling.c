#include "private.h"
#include <arch/timer.h>
#include <kernel/errno.h>
#include <kernel/time.h>

static int is_rt(const struct kernel_task *task)
{
    return task && !task->idle && task->scheduling.priority != 0;
}

#if BOAROS_USER_SMP
static void reserve_user_locked(struct kernel_task *task, struct kernel_cpu *cpu)
{
    if (!task->arch.user_mode || !is_rt(task) || task->reservation_node.queued) return;
    if (task->reservation_cpu || !task->wait.on_cpu) __builtin_trap();
    task->reservation_cpu=cpu;
    /* 仅资格借用：实际内核栈仍由on_cpu/switch_finish独占交接。 */
    kernel_sched_enqueue(&cpu->user_reservations,&task->reservation_node,task,
                         task->scheduling.priority,0);
}
static int user_reserved_out_locked(struct kernel_cpu *cpu, struct kernel_task *task)
{
    struct kernel_sched_node *node=kernel_sched_pick(&cpu->user_reservations,
        kernel_rt_bandwidth_eligible(&cpu->rt_bandwidth));
    return task->arch.user_mode && !task->user_service && node && node->owner!=task &&
           task->scheduling.priority<=(int)node->priority;
}

static void finish_service_sample_locked(struct kernel_task *task)
{
    if (!task->service_started_ticks) return;
    struct kernel_cpu *cpu=kernel_cpu_current();
    if (cpu->logical_id) __builtin_trap();
    cpu->service_samples++;
    cpu->service_elapsed_ticks += arch_time_read()-task->service_started_ticks;
    cpu->service_running_ns += task->service_running_ns;
    task->service_started_ticks=0;
    task->service_running_ns=0;
}
#endif

#if BOAROS_MAX_CPUS > 1
static uint32_t effective_cpu_mask(const struct kernel_task *task)
{
#if BOAROS_USER_SMP
    if (task->arch.user_mode && task->user_service) return 1;
#endif
#if BOAROS_USER_SMP
    if (task->reservation_node.queued) return 1U << task->reservation_cpu->logical_id;
#endif
    return task->cpu_mask ? task->cpu_mask : 1U;
}
#endif
#if BOAROS_USER_SMP
static uint32_t online_user_mask_locked(void)
{
    uint32_t mask=1;
    for (struct kernel_cpu *cpu=scheduler.cpus;cpu;cpu=cpu->scheduler_next)
        if (kernel_cpu_online(cpu)) mask |= 1U << cpu->logical_id;
    return mask;
}
#endif

void scheduler_user_reservation_release_locked(struct kernel_task *task)
{
#if BOAROS_USER_SMP
    struct kernel_cpu *caller=kernel_cpu_current();
    if (!caller->raw_locks || caller->raw_locks->lock!=&kernel_wait_domain || !task)
        __builtin_trap();
    if (!task->reservation_node.queued) {
        if (task->reservation_cpu) __builtin_trap();
        return;
    }
    struct kernel_cpu *cpu=task->reservation_cpu;
    if (!cpu) __builtin_trap();
    kernel_sched_dequeue(&cpu->user_reservations,&task->reservation_node);
    task->reservation_cpu=0;
    kernel_cpu_request_schedule(cpu);
#else
    (void)task;
#endif
}
int scheduler_user_yield_locked(struct kernel_task *task)
{
#if BOAROS_USER_SMP
    if (task->arch.user_mode && task->user_service) {
        scheduler_user_reservation_release_locked(task);
        task->user_yielded=1;
        return 1;
    }
#else
    (void)task;
#endif
    return 0;
}

void scheduler_user_service_set_locked(struct kernel_task *task, int service)
{
#if BOAROS_USER_SMP
    struct kernel_cpu *cpu = kernel_cpu_current();
    if (!cpu->raw_locks || cpu->raw_locks->lock != &kernel_wait_domain ||
        !task || !task->arch.user_mode) __builtin_trap();
    if (service && !task->user_service && task->wait.on_cpu)
        reserve_user_locked(task,task->cpu);
    int queued = task->ready_node.queued;
    if (queued) ready_remove_locked(task);
    task->user_service = service != 0;
    if (queued) ready_enqueue_locked(task, 0);
#else
    (void)task; (void)service;
#endif
}

void scheduler_user_request_locked(struct kernel_task *task)
{
#if BOAROS_USER_SMP
    struct kernel_cpu *cpu = kernel_cpu_current();
    if (!cpu->raw_locks || cpu->raw_locks->lock != &kernel_wait_domain) __builtin_trap();
    if (!task || !task->arch.user_mode || task->retirement) return;
    /* CPU1只读取这个发布位，完整信号/进程业务状态仍由CPU0处理。 */
    __atomic_store_n(&task->user_service_pending, 1, __ATOMIC_RELEASE);
    if (task->wait.on_cpu) kernel_cpu_request_schedule(task->cpu);
#else
    (void)task;
#endif
}

int kernel_scheduler_user_service_pending(void)
{
#if BOAROS_USER_SMP
    struct kernel_task *task = kernel_cpu_current()->current;
    return task && task->arch.user_mode &&
        __atomic_load_n(&task->user_service_pending, __ATOMIC_ACQUIRE);
#else
    return 0;
#endif
}

int kernel_scheduler_user_service_active(void)
{
#if BOAROS_USER_SMP
    struct kernel_cpu *cpu=kernel_cpu_current();
    return !cpu->logical_id && cpu->current && cpu->current->user_service;
#else
    return 1;
#endif
}

void kernel_scheduler_enter_user_service(void)
{
#if BOAROS_USER_SMP
    kernel_assert_can_block();
    struct kernel_io_context *io = kernel_io_context_current();
    if (io->locks || io->user_pins || io->backend_depth) __builtin_trap();
    uintptr_t irq = arch_interrupt_save();
    struct kernel_task *task = kernel_cpu_current()->current;
    if (!task || !task->arch.user_mode || !task->wait.on_cpu) __builtin_trap();
    if (!kernel_cpu_current()->logical_id && task->user_service && task->cpu_mask) {
        if (!task->service_started_ticks) {
            KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
            scheduler_account_runtime_locked();
            task->service_started_ticks=arch_time_read();
        }
        (void)__atomic_exchange_n(&task->user_service_pending,0,__ATOMIC_ACQ_REL);
        arch_interrupt_restore(irq);
        return;
    }
    uint64_t start=arch_time_read();
    unsigned remote=kernel_cpu_current()->logical_id;
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        scheduler_account_runtime_locked();
        if (!task->cpu_mask) task->cpu_mask=online_user_mask_locked();
        scheduler_user_service_set_locked(task, 1);
        task->service_started_ticks=arch_time_read();
        task->service_entries++;
        if (kernel_cpu_current()->logical_id) task->service_migrations++;
    }
    if (kernel_cpu_current()->logical_id &&
        scheduler_reschedule(0, 0, 0) != KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
    if (kernel_cpu_current()->logical_id || kernel_cpu_current()->current != task) __builtin_trap();
    if (remote) {
        kernel_cpu_current()->service_migrations++;
        kernel_cpu_current()->service_queue_ticks += arch_time_read()-start;
    }
    /* 先消费通知，再处理业务；之后到达的通知留给下一返回检查。 */
    (void)__atomic_exchange_n(&task->user_service_pending, 0, __ATOMIC_ACQ_REL);
    arch_interrupt_restore(irq);
#endif
}

void kernel_scheduler_leave_user_service(void)
{
#if BOAROS_USER_SMP
    uintptr_t irq = arch_interrupt_save();
    struct kernel_task *task = kernel_cpu_current()->current;
    if (!task || !task->arch.user_mode || kernel_cpu_current()->logical_id ||
        !task->user_service) __builtin_trap();
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        scheduler_account_runtime_locked();
        finish_service_sample_locked(task);
        scheduler_user_service_set_locked(task, 0);
    }
    /* 服务队列与用户资格是不同调度类；恢复时必须重查逻辑资格，
     * 即使没有新IPI，也不能让刚准备完的同级子任务越过FIFO owner。 */
    if (scheduler_reschedule(0,0,0) != KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
    if (kernel_cpu_current()->current != task) __builtin_trap();
    arch_interrupt_restore(irq);
#endif
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
#if BOAROS_USER_SMP
    if (cpu->current && cpu->current->arch.user_mode && cpu->current->user_service &&
        cpu->current->service_started_ticks) cpu->current->service_running_ns += delta;
#endif
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
#if BOAROS_USER_SMP
    struct kernel_cpu *cpu=kernel_cpu_current();
    int eligible=kernel_rt_bandwidth_eligible(&cpu->rt_bandwidth);
    if (!cpu->logical_id) {
        struct kernel_sched_node *service=kernel_sched_pick(&scheduler.user_services,eligible);
        if (service) return service->owner;
    }
#endif
    struct kernel_sched_node *node = kernel_sched_pick(
#if BOAROS_MAX_CPUS > 1
        &kernel_cpu_current()->eligible,
#else
        &scheduler.runqueue,
#endif
        kernel_rt_bandwidth_eligible(&kernel_cpu_current()->rt_bandwidth));
#if BOAROS_USER_SMP
    if (node && user_reserved_out_locked(cpu,node->owner)) {
        struct kernel_sched_node *reservation=kernel_sched_pick(&cpu->user_reservations,eligible);
        struct kernel_task *owner=reservation->owner;
        /* 保留者恢复到队首，不扫描同级任务；它尚在CPU0时本核可idle。 */
        return owner->ready_node.queued && !owner->user_service &&
            owner->eligible_nodes[cpu->logical_id].queued ? owner : 0;
    }
#endif
    return node ? node->owner : 0;
}
void ready_remove_locked(struct kernel_task *task)
{
#if BOAROS_USER_SMP
    if (task->service_node.queued)
        kernel_sched_dequeue(&scheduler.user_services,&task->service_node);
#endif
#if BOAROS_MAX_CPUS > 1
    for (struct kernel_cpu *cpu = scheduler.cpus; cpu; cpu = cpu->scheduler_next)
        if (task->eligible_nodes[cpu->logical_id].queued)
            kernel_sched_dequeue(&cpu->eligible, &task->eligible_nodes[cpu->logical_id]);
#endif
    kernel_sched_dequeue(&scheduler.runqueue, &task->ready_node);
}
void ready_enqueue_locked(struct kernel_task *task, int head)
{
#if BOAROS_COST_DIAGNOSTICS
    kernel_cost_ready(&task->cost);
#endif
    if (!task->accounted) scheduler_task_publish_locked(task);
    task->next = 0;
    kernel_sched_enqueue(&scheduler.runqueue, &task->ready_node, task,
                         (unsigned)task->scheduling.priority, head);
#if BOAROS_MAX_CPUS > 1
#if BOAROS_USER_SMP
    if (task->arch.user_mode && task->user_service)
        kernel_sched_enqueue(&scheduler.user_services,&task->service_node,task,
                             task->scheduling.priority,head);
#endif
    uint32_t mask = effective_cpu_mask(task);
    for (struct kernel_cpu *target = scheduler.cpus; target; target = target->scheduler_next) {
        if (!(mask & (1U << target->logical_id))) continue;
        kernel_sched_enqueue(&target->eligible, &task->eligible_nodes[target->logical_id],
                             task, (unsigned)task->scheduling.priority, head);
        if (!target->current || target->current->idle ||
#if BOAROS_USER_SMP
            (!target->logical_id && task->user_service && !target->current->user_service) ||
#endif
            task->scheduling.priority > target->current->scheduling.priority) {
            kernel_cpu_request_schedule(target);
        }
    }
#else
    struct kernel_cpu *target = task->cpu ? task->cpu : kernel_cpu_current();
    if (!target->current || target->current->idle ||
        task->scheduling.priority > target->current->scheduling.priority)
        kernel_cpu_request_schedule(target);
#endif
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
#if BOAROS_USER_SMP
    if (previous->state==KERNEL_THREAD_STATE_EXITED) {
        scheduler_user_reservation_release_locked(previous);
        finish_service_sample_locked(previous);
    }
#endif
    if (!next || next->wait.on_cpu || (next->idle && next->cpu != cpu)) __builtin_trap();
    if (!next->idle) {
        ready_remove_locked(next);
#if BOAROS_USER_SMP
        if (!next->user_service && next->reservation_cpu==cpu)
            scheduler_user_reservation_release_locked(next);
#endif
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
#if BOAROS_USER_SMP
    if (previous->state!=KERNEL_THREAD_STATE_RUNNING && previous->state!=KERNEL_THREAD_STATE_READY)
        scheduler_user_reservation_release_locked(previous);
#endif
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
#if BOAROS_MAX_CPUS > 1
        cpu->eligible.first && cpu->eligible.first->priority != 0);
#else
        scheduler.runqueue.first && scheduler.runqueue.first->priority != 0);
#endif
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
    struct kernel_task *blocked =
#if BOAROS_MAX_CPUS > 1
        cpu->logical_id ? 0 :
#endif
        deadline_index_first();
    if (blocked != 0 && (!deadline || blocked->wakeup_deadline < deadline))
        deadline = blocked->wakeup_deadline;
    cpu->armed_deadline = deadline;
    }
    enum arch_timer_status status = arch_timer_set_scheduler_deadline(deadline);
    if (status != ARCH_TIMER_STATUS_OK && status != ARCH_TIMER_STATUS_NOT_STARTED)
        __builtin_trap();
    arch_interrupt_restore(irq);
}
void kernel_scheduler_deadline_changed(void) { scheduler_rearm_timer(); }

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
#if BOAROS_USER_SMP
    if (current->user_yielded && !current->user_service) {
        voluntary=1;
        current->user_yielded=0;
    }
#endif
    int throttled = is_rt(current) && !kernel_rt_bandwidth_eligible(&cpu->rt_bandwidth);
    int expired = current != cpu->idle && kernel_sched_policy_expired(&current->scheduling);
#if BOAROS_USER_SMP
    /* 片尾先归还逻辑资格，再选择同级后继，不能用旧保留者过滤掉它。 */
    if (expired || throttled || voluntary) {
        scheduler_user_reservation_release_locked(current);
        if (current->user_service && expired) current->user_yielded=1;
    }
#endif
    struct kernel_task *next = ready_best_locked();
    int higher = next && (current == cpu->idle ||
                         next->scheduling.priority > current->scheduling.priority);
    int rotate = voluntary || expired ||
                 (rotate_other && current->scheduling.policy == KERNEL_SCHED_OTHER);
    /* 当前任务尚未入队；yield/片尾只可轮转同级，不能把低优先级当成替代者。 */
    int same_priority = next && next->scheduling.priority == current->scheduling.priority;
    int disallowed = 0;
#if BOAROS_MAX_CPUS > 1
    disallowed = !current->idle &&
        !(effective_cpu_mask(current) & (1U << cpu->logical_id));
#endif
#if BOAROS_USER_SMP
    int service_preempt=next && next->user_service && !current->user_service;
    if (current->user_service && next && !next->user_service) higher=0;
    disallowed |= !current->idle && user_reserved_out_locked(cpu,current);
    if (service_preempt && current->arch.user_mode && !rotate && !throttled)
        reserve_user_locked(current,cpu);
#else
    int service_preempt=0;
#endif
    if (expired) kernel_sched_policy_rotate(&current->scheduling);
    if (!disallowed && !service_preempt && !throttled && !higher && !(rotate && same_priority)) {
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

int kernel_task_affinity_get(struct kernel_task *caller, int32_t pid, uint64_t *mask)
{
    if (pid < 0) return -KERNEL_ESRCH;
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    struct kernel_task *task = sched_target(caller, pid);
    if (!task) return -KERNEL_ESRCH;
#if BOAROS_USER_SMP
    *mask = task->cpu_mask ? task->cpu_mask : online_user_mask_locked();
#else
    *mask = 1;
#endif
    return 0;
}

int kernel_task_affinity_set(struct kernel_task *caller, int32_t pid, uint64_t mask)
{
    if (pid < 0) return -KERNEL_ESRCH;
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    struct kernel_task *task = sched_target(caller, pid);
    if (!task) return -KERNEL_ESRCH;
    uint32_t available = 1;
#if BOAROS_USER_SMP
    available = online_user_mask_locked();
#endif
    uint32_t requested = (uint32_t)(mask & available);
    if (!requested) return -KERNEL_EINVAL;
#if BOAROS_USER_SMP
    if (task->cpu_mask != requested) {
        int queued = task->ready_node.queued;
        if (queued) ready_remove_locked(task);
        if (task->reservation_cpu && !(requested & (1U << task->reservation_cpu->logical_id)))
            scheduler_user_reservation_release_locked(task);
        task->cpu_mask = requested;
        if (queued) ready_enqueue_locked(task, 0);
        if (task->wait.on_cpu) kernel_cpu_request_schedule(task->cpu);
    }
#endif
    return 0;
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
#if BOAROS_USER_SMP
                struct kernel_cpu *reservation=task->reservation_cpu;
                if (reservation) scheduler_user_reservation_release_locked(task);
#endif
                task->scheduling = updated;
#if BOAROS_USER_SMP
                if (reservation && is_rt(task)) {
                    task->reservation_cpu=reservation;
                    kernel_sched_enqueue(&reservation->user_reservations,
                        &task->reservation_node,task,updated.priority,updated.priority<old_priority);
                }
#endif
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
#if BOAROS_MAX_CPUS > 1
            if (!cpu->logical_id || kernel_cpu_online(cpu))
#endif
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
#if BOAROS_MAX_CPUS > 1
                if (cpu->logical_id && !kernel_cpu_online(cpu)) continue;
#endif
                cpu->rt_bandwidth.period_ns = scheduler.rt_configuration.period_ns;
                cpu->rt_bandwidth.runtime_ns = scheduler.rt_configuration.runtime_ns;
                /* 改配置不赠送预算；只有周期长度变化才重建周期起点。 */
                if (old_period != scheduler.rt_configuration.period_ns)
                    cpu->rt_bandwidth.period_start_ns = now;
                kernel_cpu_request_schedule(cpu);
#if BOAROS_MAX_CPUS > 1
                if (cpu != kernel_cpu_current()) kernel_cpu_notify(cpu, 2U);
#endif
            }
        }
    }
    if (!result) scheduler_rearm_timer();
    arch_interrupt_restore(irq);
    return result;
}
