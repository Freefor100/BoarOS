#include <kernel/credentials.h>
#include <kernel/posix_timer.h>
#include "private.h"
#include <kernel/event_fd.h>
#include <arch/timer.h>
#include <kernel/errno.h>
#include <kernel/time.h>
#include <kernel/tick.h>

static int is_rt(const struct kernel_task *task)
{
    return task && !task->idle && task->scheduling.priority != 0;
}

static void enqueue_task(struct kernel_sched_runqueue *queue,struct kernel_sched_node *node,
    struct kernel_task *task,int head)
{
    kernel_sched_enqueue(queue,node,task,(unsigned)task->scheduling.priority,head);
    if(task->pi_base_priority)queue->boosted++;
}
static void dequeue_task(struct kernel_sched_runqueue *queue,struct kernel_sched_node *node)
{
    struct kernel_task *task=node->owner;
    if(task->pi_base_priority){if(!queue->boosted)__builtin_trap();queue->boosted--;}
    kernel_sched_dequeue(queue,node);
}
static int rt_eligible(struct kernel_cpu *cpu)
{
    if(kernel_rt_bandwidth_eligible(&cpu->rt_bandwidth))return 1;
    /* 已继承优先级的持锁者须能结束临界区；按本核就绪队列解除节流。 */
    return (cpu->current && cpu->current->pi_base_priority &&
            (cpu->current->state==KERNEL_THREAD_STATE_RUNNING || cpu->current->state==KERNEL_THREAD_STATE_READY)) ||
#if BOAROS_MAX_CPUS > 1
        cpu->eligible.boosted
#else
        scheduler.runqueue.boosted
#endif
#if BOAROS_USER_SMP
        || cpu->user_reservations.boosted
#endif
        ;
}

#if BOAROS_USER_SMP
static int is_rt_service(const struct kernel_task *task)
{
    return task && task->arch.user_mode && task->user_service && is_rt(task);
}

static void reserve_user_locked(struct kernel_task *task, struct kernel_cpu *cpu)
{
    if (!task->arch.user_mode || !is_rt(task) || task->reservation_node.queued) return;
    if (task->reservation_cpu || !task->wait.on_cpu) __builtin_trap();
    task->reservation_cpu=cpu;
    /* 仅资格借用：实际内核栈仍由on_cpu/switch_finish独占交接。 */
    enqueue_task(&cpu->user_reservations,&task->reservation_node,task,0);
}
static int user_reserved_out_locked(struct kernel_cpu *cpu, struct kernel_task *task)
{
    struct kernel_sched_node *node=kernel_sched_pick(&cpu->user_reservations,
        rt_eligible(cpu));
    return task->arch.user_mode && !task->user_service && node && node->owner!=task &&
           task->scheduling.priority<=(int)node->priority;
}

#if BOAROS_COST_DIAGNOSTICS
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
    dequeue_task(&cpu->user_reservations,&task->reservation_node);
    task->reservation_cpu=0;
    kernel_cpu_request_schedule(cpu);
#else
    (void)task;
#endif
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
    __atomic_fetch_or(&task->user_return_pending, KERNEL_USER_RETURN_CPU0, __ATOMIC_RELEASE);
    if (task->wait.on_cpu) kernel_cpu_request_schedule(task->cpu);
#else
    (void)task;
#endif
}

void scheduler_signal_request_locked(struct kernel_task *task)
{
#if BOAROS_USER_SMP
    struct kernel_cpu *cpu=kernel_cpu_current();
    if (!cpu->raw_locks || cpu->raw_locks->lock!=&kernel_wait_domain) __builtin_trap();
    if (!task || !task->arch.user_mode || task->retirement) return;
    __atomic_fetch_or(&task->user_return_pending, KERNEL_USER_RETURN_SIGNAL, __ATOMIC_RELEASE);
    if (task!=cpu->current && task->wait.on_cpu) kernel_cpu_request_schedule(task->cpu);
#else
    (void)task;
#endif
}

int kernel_scheduler_user_return_pending(void)
{
#if BOAROS_USER_SMP
    struct kernel_task *task=kernel_cpu_current()->current;
    return task && task->arch.user_mode &&
        __atomic_load_n(&task->user_return_pending,__ATOMIC_ACQUIRE);
#else
    return 0;
#endif
}

int kernel_scheduler_take_signal_work(void)
{
#if BOAROS_USER_SMP
    struct kernel_task *task=kernel_cpu_current()->current;
    if (!task || !(__atomic_load_n(&task->user_return_pending,__ATOMIC_ACQUIRE) & KERNEL_USER_RETURN_SIGNAL)) return 0;
    /* 先取走本批通知；消费期间的新发布留给下一次返回检查。 */
    return (__atomic_fetch_and(&task->user_return_pending,~KERNEL_USER_RETURN_SIGNAL,__ATOMIC_ACQ_REL) & KERNEL_USER_RETURN_SIGNAL)!=0;
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

#if BOAROS_USER_SMP
static void enter_user_service(void)
{
    kernel_assert_can_block();
    struct kernel_io_context *io = kernel_io_context_current();
    if (io->locks || io->user_pins || io->cache_writes || io->backend_depth ||
        kernel_cpu_current()->raw_locks) __builtin_trap();
    uintptr_t irq = arch_interrupt_save();
    struct kernel_task *task = kernel_cpu_current()->current;
    if (!task || !task->arch.user_mode || !task->wait.on_cpu) __builtin_trap();
    if (!kernel_cpu_current()->logical_id && task->user_service && task->cpu_mask) {
#if BOAROS_COST_DIAGNOSTICS
        if (!task->service_started_ticks) {
            KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
            scheduler_account_runtime_locked();
            task->service_started_ticks=arch_time_read();
        }
#endif
        arch_interrupt_restore(irq);
        return;
    }
#if BOAROS_COST_DIAGNOSTICS
    uint64_t start=arch_time_read();
    unsigned remote=kernel_cpu_current()->logical_id;
#endif
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        scheduler_account_runtime_locked();
        if (!task->cpu_mask) task->cpu_mask=online_user_mask_locked();
        scheduler_user_service_set_locked(task, 1);
#if BOAROS_COST_DIAGNOSTICS
        task->service_started_ticks=arch_time_read();
#endif
    }
    if (kernel_cpu_current()->logical_id &&
        scheduler_reschedule(0, 0, 0) != KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
    if (kernel_cpu_current()->logical_id || kernel_cpu_current()->current != task) __builtin_trap();
#if BOAROS_COST_DIAGNOSTICS
    if (remote) {
        kernel_cpu_current()->service_migrations++;
        kernel_cpu_current()->service_queue_ticks += arch_time_read()-start;
    }
#endif
    arch_interrupt_restore(irq);
}

#endif

void kernel_scheduler_enter_user_service(void)
{
    COST_SCOPE(cost,SERVICE_ENTER);
#if BOAROS_USER_SMP
    enter_user_service();
#endif
}

void kernel_scheduler_leave_user_service(void)
{
    COST_SCOPE(cost,SERVICE_RETURN);
#if BOAROS_USER_SMP
    uintptr_t irq = arch_interrupt_save();
    struct kernel_task *task = kernel_cpu_current()->current;
    if (!task || !task->arch.user_mode || kernel_cpu_current()->logical_id ||
        !task->user_service) __builtin_trap();
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        scheduler_account_runtime_locked();
#if BOAROS_COST_DIAGNOSTICS
        finish_service_sample_locked(task);
#endif
        scheduler_user_service_set_locked(task, 0);
    }
    /* 服务队列与用户资格是不同调度类；恢复时必须重查逻辑资格，
     * 即使没有新IPI，也不能让刚准备完的同级子任务越过FIFO owner。 */
    if (scheduler_reschedule(0,0,0) != KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
    if (kernel_cpu_current()->current != task) __builtin_trap();
    arch_interrupt_restore(irq);
#endif
}

#if BOAROS_USER_SMP
int kernel_service_scope_try_local(struct kernel_service_scope *scope)
{
    uintptr_t irq=arch_interrupt_save();
    struct kernel_cpu *cpu=kernel_cpu_current();
    if (cpu->logical_id) { arch_interrupt_restore(irq); return 0; }
    struct kernel_task *task=cpu->current;
    if (task && task->arch.user_mode && !task->user_service) {
        /* 核对CPU与发布资格之间不能抢占；这里只改调度资格，不等业务锁。 */
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        scheduler_account_runtime_locked();
        scheduler_user_service_set_locked(task,1);
#if BOAROS_COST_DIAGNOSTICS
        task->service_started_ticks=arch_time_read();
#endif
        scope->entered=1;
    }
    arch_interrupt_restore(irq);
    return 1;
}
void kernel_service_scope_begin(struct kernel_service_scope *scope)
{
    struct kernel_task *task = kernel_cpu_current()->current;
    if (task && task->arch.user_mode && !kernel_scheduler_user_service_active()) {
        kernel_scheduler_enter_user_service();
        scope->entered = 1;
    }
}
void kernel_service_scope_end(struct kernel_service_scope *scope)
{
    if (scope->entered) kernel_scheduler_leave_user_service();
}
void kernel_scheduler_ack_user_service(void)
{
    struct kernel_task *task = kernel_task_current();
    if (!kernel_scheduler_user_service_active()) __builtin_trap();
    (void)__atomic_fetch_and(&task->user_return_pending, ~KERNEL_USER_RETURN_CPU0, __ATOMIC_ACQ_REL);
}
#endif

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
    uint64_t cost_irq=kernel_cost_lock();
    if (cpu->cost.running) kernel_cost_account(cpu->cost.running);
    kernel_cost_unlock(cost_irq);
#endif
    if (now < cpu->rt_bandwidth.last_account_ns) __builtin_trap();
    uint64_t delta = now - cpu->rt_bandwidth.last_account_ns;
#if BOAROS_USER_SMP && BOAROS_COST_DIAGNOSTICS
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
    int eligible=rt_eligible(cpu);
    if (!cpu->logical_id) {
        struct kernel_sched_node *service=kernel_sched_pick(&scheduler.rt_services,eligible);
        if (service) return service->owner;
    }
#endif
    struct kernel_sched_node *node = kernel_sched_pick(
#if BOAROS_MAX_CPUS > 1
        &kernel_cpu_current()->eligible,
#else
        &scheduler.runqueue,
#endif
        rt_eligible(kernel_cpu_current()));
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
        dequeue_task(&scheduler.rt_services,&task->service_node);
#endif
#if BOAROS_MAX_CPUS > 1
    for (struct kernel_cpu *cpu = scheduler.cpus; cpu; cpu = cpu->scheduler_next)
        if (task->eligible_nodes[cpu->logical_id].queued)
            dequeue_task(&cpu->eligible, &task->eligible_nodes[cpu->logical_id]);
#endif
    dequeue_task(&scheduler.runqueue, &task->ready_node);
}
void ready_enqueue_locked(struct kernel_task *task, int head)
{
#if BOAROS_COST_DIAGNOSTICS
    kernel_cost_ready(&task->cost);
#endif
    if (!task->accounted) scheduler_task_publish_locked(task);
    task->next = 0;
    enqueue_task(&scheduler.runqueue, &task->ready_node, task,head);
#if BOAROS_MAX_CPUS > 1
#if BOAROS_USER_SMP
    /* 普通服务与CPU0 worker按OTHER轮转，否则忙查询可饿死其依赖的I/O。 */
    if (is_rt_service(task))
        enqueue_task(&scheduler.rt_services,&task->service_node,task,head);
#endif
    uint32_t mask = effective_cpu_mask(task);
    for (struct kernel_cpu *target = scheduler.cpus; target; target = target->scheduler_next) {
        if (!(mask & (1U << target->logical_id))) continue;
        enqueue_task(&target->eligible, &task->eligible_nodes[target->logical_id],task,head);
        if (!target->current || target->current->idle ||
#if BOAROS_USER_SMP
            (!target->logical_id && is_rt_service(task) && !is_rt_service(target->current)) ||
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
    /* 等待通知或 SIGCONT 可在切离前把状态改回 READY。只有确实
     * 带着睡眠/停止状态离开 CPU 才结束连续 RT；迁移和节流仍是 READY。
     * DUE 只在计数非零后发布；按历史计数判断，退出 RT 或取消限额后也要清理。 */
    if ((previous->state == KERNEL_THREAD_STATE_BLOCKED ||
         previous->state == KERNEL_THREAD_STATE_STOPPED) && previous->rttime_ticks) {
        previous->rttime_ticks = 0;
        if (__atomic_fetch_and(&previous->resource_flags,
                ~KERNEL_RESOURCE_RTTIME_DUE,__ATOMIC_RELAXED) & KERNEL_RESOURCE_RTTIME_DUE)
            scheduler_cpu_timer_update_locked(previous,1);
    }
#if BOAROS_USER_SMP
    if (previous->state==KERNEL_THREAD_STATE_EXITED) {
        scheduler_user_reservation_release_locked(previous);
#if BOAROS_COST_DIAGNOSTICS
        finish_service_sample_locked(previous);
#endif
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

uint64_t scheduler_deadline_locked(struct kernel_cpu *cpu)
{
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
    uint64_t deadline = 0;
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
    if (blocked != 0 && (!deadline || blocked->wait.deadline < deadline))
        deadline = blocked->wait.deadline;
    uint64_t event=0;
#if BOAROS_MAX_CPUS > 1
    if (!cpu->logical_id)
#endif
        event=kernel_timer_fd_deadline();
    if (event && (!deadline || event<deadline)) deadline=event;
    event=kernel_posix_timer_deadline();
    if(event&&(!deadline||event<deadline))deadline=event;
    return deadline;
}

void scheduler_arm_timer(uint64_t deadline)
{
    enum arch_timer_status status = arch_timer_set_scheduler_deadline(deadline);
    if (status != ARCH_TIMER_STATUS_OK && status != ARCH_TIMER_STATUS_NOT_STARTED) __builtin_trap();
}

void scheduler_rearm_timer(void)
{
    uintptr_t irq = arch_interrupt_save();
    uint64_t deadline;
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        deadline = scheduler_deadline_locked(kernel_cpu_current());
    }
    scheduler_arm_timer(deadline);
    arch_interrupt_restore(irq);
}

void kernel_scheduler_deadline_changed(void) { scheduler_rearm_timer(); }

enum kernel_scheduler_status scheduler_reschedule(int rotate_other, int voluntary, int check_queues)
{
    /* 调用者保持IRQ关闭；CPU借用在切换前结束，不跨睡眠或恢复点。 */
    struct kernel_cpu *cpu = kernel_cpu_current();
    uint64_t deadline;
    if (cpu->preempt_depth) {
        {
            KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
            if (check_queues) {
                enum kernel_scheduler_status status = validate_queues_locked();
                if (status != KERNEL_SCHEDULER_STATUS_OK) return status;
            }
            scheduler_account_runtime_locked();
            deadline = scheduler_deadline_locked(cpu);
        }
        if (voluntary) __builtin_trap();
        kernel_cpu_request_schedule(cpu);
        cpu->rotate_other |= rotate_other != 0;
        scheduler_arm_timer(deadline);
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
    if (current->user_slice_expired && !current->user_service) {
        voluntary=1;
        current->user_slice_expired=0;
    }
#endif
    int throttled = is_rt(current) && !rt_eligible(cpu);
    int expired = current != cpu->idle && kernel_sched_policy_expired(&current->scheduling);
#if BOAROS_USER_SMP
    /* 片尾先归还逻辑资格，再选择同级后继，不能用旧保留者过滤掉它。 */
    if (expired || throttled || voluntary) {
        scheduler_user_reservation_release_locked(current);
        if (current->user_service && expired) current->user_slice_expired=1;
    }
#endif
    struct kernel_task *next = ready_best_locked();
    int higher = next && (current == cpu->idle ||
                         next->scheduling.priority > current->scheduling.priority);
    int rotate = voluntary || expired ||
                 (rotate_other && !is_rt(current));
    /* 当前任务尚未入队；yield/片尾只可轮转同级，不能把低优先级当成替代者。 */
    int same_priority = next && next->scheduling.priority == current->scheduling.priority;
    int disallowed = 0;
#if BOAROS_MAX_CPUS > 1
    disallowed = !current->idle &&
        !(effective_cpu_mask(current) & (1U << cpu->logical_id));
#endif
#if BOAROS_USER_SMP
    int service_preempt=is_rt_service(next) && !is_rt_service(current);
    if (is_rt_service(current) && next && !is_rt_service(next)) higher=0;
    disallowed |= !current->idle && user_reserved_out_locked(cpu,current);
    if (service_preempt && current->arch.user_mode && !rotate && !throttled)
        reserve_user_locked(current,cpu);
#else
    int service_preempt=0;
#endif
    if (expired) kernel_sched_policy_rotate(&current->scheduling);
    if (!disallowed && !service_preempt && !throttled && !higher && !(rotate && same_priority)) {
        deadline = scheduler_deadline_locked(cpu);
        kernel_raw_lock_release(&guard);
        scheduler_arm_timer(deadline);
        return KERNEL_SCHEDULER_STATUS_OK;
    }
    if (current != cpu->idle) {
        current->state = KERNEL_THREAD_STATE_READY;
        /* 高优先级抢占/节流保留同级队首；普通迁移在目标核排队，
         * 不能让反复进入CPU0的短查询越过已就绪的I/O执行者。 */
        if (current->wait.ready_pending) __builtin_trap();
        current->wait.ready_pending = 1;
        current->wait.ready_head = !rotate &&
            !(disallowed && current->scheduling.policy==KERNEL_SCHED_OTHER);
    } else cpu->idle_context_saved = 1U;
    selected = claim_next_locked(cpu, current, next);
    deadline = scheduler_deadline_locked(cpu);
    }
    if (!selected) __builtin_trap();
    scheduler_arm_timer(deadline);
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

static int sched_owner_locked(const struct kernel_task *caller,const struct kernel_task *target)
{
    const struct kernel_credentials *from=kernel_credentials_task_locked(caller),
        *to=kernel_credentials_task_locked(target);
    return from->euid==to->uid || from->euid==to->euid ||
        kernel_credentials_capable(from,KERNEL_CAP_SYS_NICE);
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
    if(!sched_owner_locked(caller,task))return -KERNEL_EPERM;
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
        if (task) { *state = task->scheduling; state->priority=scheduler_base_priority(task); result = 0; }
    }
    return result;
}

static void boost_count_change(struct kernel_sched_runqueue *queue,int add)
{
    if(add)queue->boosted++;
    else {if(!queue->boosted)__builtin_trap();queue->boosted--;}
}
void scheduler_pi_priority_locked(struct kernel_task *task,unsigned base,unsigned inherited)
{
    struct kernel_cpu *caller=kernel_cpu_current();
    if(!caller->raw_locks || caller->raw_locks->lock!=&kernel_wait_domain ||
       !task || base>99 || inherited>99)__builtin_trap();
    if(task->wait.on_cpu) {
        if(!task->cpu)__builtin_trap();
        /* 旧栈的on_cpu借用可晚于current的交接；此时不再按旧策略计时。 */
        if(task->cpu->current==task)account_cpu(task->cpu,kernel_time_monotonic_ns());
    }
    unsigned priority=base>inherited?base:inherited;
    unsigned saved=priority>base?base+1U:0;
    int old=task->scheduling.priority,move=priority!=(unsigned)old;
    int changed_boost=!!saved!=!!task->pi_base_priority;
    int queued=task->ready_node.queued;
    if(move && queued)ready_remove_locked(task);
#if BOAROS_USER_SMP
    struct kernel_cpu *reservation=task->reservation_cpu;
    if(move && reservation)scheduler_user_reservation_release_locked(task);
#endif
    /* 只改变基础值而实际优先级相同时，保持同级FIFO位置。 */
    if(!move && changed_boost) {
        if(queued)boost_count_change(&scheduler.runqueue,saved!=0);
#if BOAROS_MAX_CPUS > 1
        for(struct kernel_cpu *cpu=scheduler.cpus;cpu;cpu=cpu->scheduler_next)
            if(task->eligible_nodes[cpu->logical_id].queued)
                boost_count_change(&cpu->eligible,saved!=0);
#endif
#if BOAROS_USER_SMP
        if(task->service_node.queued)boost_count_change(&scheduler.rt_services,saved!=0);
        if(reservation)boost_count_change(&reservation->user_reservations,saved!=0);
#endif
    }
    task->pi_base_priority=saved;
    task->scheduling.priority=(int)priority;
#if BOAROS_USER_SMP
    if(move && reservation && priority) {
        task->reservation_cpu=reservation;
        enqueue_task(&reservation->user_reservations,&task->reservation_node,task,priority<(unsigned)old);
    }
#endif
    if(move && queued)ready_enqueue_locked(task,priority<(unsigned)old);
    if(move || changed_boost)kernel_cpu_request_schedule(task->cpu?task->cpu:caller);
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
            struct kernel_sched_policy updated = task->scheduling;
            updated.priority=(int)scheduler_base_priority(task);
            result = kernel_sched_policy_set(&updated, policy, priority, keep_policy);
            if (!result && !kernel_credentials_capable(kernel_credentials_task_locked(caller),KERNEL_CAP_SYS_NICE)) {
                uint64_t rtprio=task->limits->values[KERNEL_RLIMIT_RTPRIO].current;
                if(!sched_owner_locked(caller,task) ||
                    (updated.priority &&
                     ((updated.policy!=task->scheduling.policy && !rtprio) ||
                      ((unsigned)updated.priority>scheduler_base_priority(task) &&
                       (uint64_t)updated.priority>rtprio))) ||
                    (task->scheduling.policy==5 && updated.policy!=5) ||
                    (task->scheduling.reset_on_fork && !updated.reset_on_fork))result=-KERNEL_EPERM;
            }
            if (!result) {
                if(task->pi_owned || task->pi_request)
                    kernel_futex_pi_recalculate_locked(task,updated.priority);
                else scheduler_pi_priority_locked(task,(unsigned)updated.priority,0);
                task->scheduling.policy=updated.policy;
                task->scheduling.reset_on_fork=updated.reset_on_fork;
                if(updated.policy==KERNEL_SCHED_RR && !task->scheduling.rr_remaining_ns)
                    task->scheduling.rr_remaining_ns=KERNEL_SCHED_RR_NS;
                kernel_cpu_request_schedule(task->cpu ? task->cpu : kernel_cpu_current());
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

/* Linux 的 RT watchdog 每个全局 tick 至多记一次；跨核迁移不会把
 * 同一 tick 变成两份预算，普通 trap 与未启用限制的任务不进入索引。 */
void scheduler_rt_watchdog_locked(struct kernel_task *task,uint64_t stamp)
{
    if (!task || !task->arch.user_mode || !task->scheduling.priority || !task->limits) return;
    const struct kernel_rlimit64 *limit=&task->limits->values[KERNEL_RLIMIT_RTTIME];
    uint64_t soft=limit->current,hard=limit->maximum;
    if (soft==KERNEL_RLIM_INFINITY) return;
    if (task->rttime_stamp!=stamp) {
        if (task->rttime_ticks!=UINT64_MAX) task->rttime_ticks++;
        task->rttime_stamp=stamp;
    }
    uint64_t usec_per_tick=UINT64_C(1000000)/KERNEL_TICKS_PER_SECOND;
    uint64_t earliest=soft<hard?soft:hard;
    uint64_t threshold=earliest/usec_per_tick+(earliest%usec_per_tick!=0);
    if (task->rttime_ticks>threshold &&
        !(__atomic_fetch_or(&task->resource_flags,KERNEL_RESOURCE_RTTIME_DUE,
                            __ATOMIC_RELAXED)&KERNEL_RESOURCE_RTTIME_DUE))
        scheduler_cpu_timer_update_locked(task,1);
}

unsigned scheduler_rt_limit_check_locked(struct kernel_task *task)
{
    if (!(__atomic_fetch_and(&task->resource_flags,~KERNEL_RESOURCE_RTTIME_DUE,
                            __ATOMIC_RELAXED)&KERNEL_RESOURCE_RTTIME_DUE)) return 0;
    struct kernel_rlimit64 *limit=&task->limits->values[KERNEL_RLIMIT_RTTIME];
    uint64_t soft=limit->current,hard=limit->maximum;
    unsigned fired=0;
    if (soft!=KERNEL_RLIM_INFINITY) {
        uint64_t usec_per_tick=UINT64_C(1000000)/KERNEL_TICKS_PER_SECOND;
        uint64_t ticks=task->rttime_ticks;
        uint64_t usec=ticks>UINT64_MAX/usec_per_tick?UINT64_MAX:ticks*usec_per_tick;
        if (hard!=KERNEL_RLIM_INFINITY && usec>=hard) fired=8U;
        else if (usec>=soft) {
            __atomic_store_n(&limit->current,soft+UINT64_C(1000000),__ATOMIC_RELAXED);
            fired=4U;
        }
    }
    scheduler_cpu_timer_update_locked(task,1);
    return fired;
}
