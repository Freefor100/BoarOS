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
        cpu->runqueue.boosted
#else
        scheduler.runqueue.boosted
#endif
#if BOAROS_USER_SMP
        || cpu->user_reservations.boosted || (!cpu->logical_id && scheduler.rt_services.boosted)
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
static int user_reserved_out_locked(struct kernel_cpu *cpu, const struct kernel_task *task)
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
    if (!kernel_raw_lock_owned(&kernel_wait_domain) || !task)
        __builtin_trap();
    if (!task->reservation_node.queued) {
        if (task->reservation_cpu) __builtin_trap();
        return;
    }
    KERNEL_RUN_SET(runs);
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
    if (!kernel_raw_lock_owned(&kernel_wait_domain) ||
        !task || !task->arch.user_mode) __builtin_trap();
    KERNEL_RUN_SET(runs);
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
    if (!kernel_raw_lock_owned(&kernel_wait_domain)) __builtin_trap();
    if (!task || !task->arch.user_mode || task->retirement) return;
    __atomic_fetch_or(&task->user_return_pending, KERNEL_USER_RETURN_CPU0, __ATOMIC_RELEASE);
    if (task->wait.on_cpu) kernel_cpu_request_schedule(__atomic_load_n(&task->cpu,__ATOMIC_ACQUIRE));
#else
    (void)task;
#endif
}

void scheduler_signal_request_locked(struct kernel_task *task)
{
#if BOAROS_USER_SMP
    struct kernel_cpu *cpu=kernel_cpu_current();
    if (!kernel_raw_lock_owned(&kernel_wait_domain)) __builtin_trap();
    if (!task || !task->arch.user_mode || task->retirement) return;
    __atomic_fetch_or(&task->user_return_pending, KERNEL_USER_RETURN_SIGNAL, __ATOMIC_RELEASE);
    if (task!=cpu->current && task->wait.on_cpu) kernel_cpu_request_schedule(__atomic_load_n(&task->cpu,__ATOMIC_ACQUIRE));
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
#if BOAROS_COST_DIAGNOSTICS
    idle->cost.wait_flags |= 128;
#endif
    cpu->scheduler_next = scheduler.cpus;
#if BOAROS_USER_SMP
    __atomic_store_n(&scheduler.cpus,cpu,__ATOMIC_RELEASE);
#else
    scheduler.cpus = cpu;
#endif
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
{
#if BOAROS_USER_SMP
    struct kernel_cpu *cpu=kernel_cpu_current();
    struct kernel_raw_guard guard __attribute__((cleanup(kernel_raw_scope_release)))={0};
    if (!kernel_raw_lock_owned(kernel_run_lock(cpu))) kernel_raw_lock_acquire(kernel_run_lock(cpu),&guard);
    account_cpu(cpu,kernel_time_monotonic_ns());
#else
    account_cpu(kernel_cpu_current(), kernel_time_monotonic_ns());
#endif
}

void scheduler_account_runtime(void)
{
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    scheduler_account_runtime_locked();
}

#if BOAROS_MAX_CPUS > 1
static struct kernel_sched_runqueue *ready_queue(struct kernel_task *task,
    struct kernel_cpu *cpu)
{
#if BOAROS_USER_SMP
    if (is_rt_service(task)) {
        if (cpu->logical_id) __builtin_trap();
        return &scheduler.rt_services;
    }
#else
    (void)task;
#endif
    return &cpu->runqueue;
}

static unsigned ready_load(const struct kernel_cpu *cpu)
{
    unsigned load=cpu->runqueue.count;
#if BOAROS_USER_SMP
    if (!cpu->logical_id) load+=scheduler.rt_services.count;
#endif
    return load + (cpu->current && !cpu->current->idle);
}

static int placement_preceded(const struct kernel_task *task,
    const struct kernel_task *other)
{
    if (!other || other->idle) return 0;
#if BOAROS_USER_SMP
    if (is_rt_service(task)!=is_rt_service(other)) return is_rt_service(other);
#endif
    return other->scheduling.priority>=task->scheduling.priority;
}

static int placement_class(const struct kernel_task *task, struct kernel_cpu *cpu,
    unsigned load)
{
    /* 无预算的idle核也不能执行普通RT；PI持锁者保留解除节流的资格。 */
    if (is_rt(task) && !task->pi_base_priority && !rt_eligible(cpu)) return 3;
#if BOAROS_USER_SMP
    /* 来源保留可能让空闲核暂不能执行这个用户；仍须给全被保留的affinity排队。 */
    if (user_reserved_out_locked(cpu,task)) return 3;
#endif
    /* idle只说明旧任务已离开；已排队的高优先级工作仍会先取得这个CPU。 */
    if (placement_preceded(task,cpu->current)) return 2;
    int eligible=rt_eligible(cpu) || task->pi_base_priority;
    struct kernel_sched_node *queued=kernel_sched_pick(&cpu->runqueue,eligible);
    if (queued && placement_preceded(task,queued->owner)) return 2;
#if BOAROS_USER_SMP
    if (!cpu->logical_id) {
        queued=kernel_sched_pick(&scheduler.rt_services,eligible);
        if (queued && placement_preceded(task,queued->owner)) return 2;
    }
#endif
    return load ? 1 : 0;
}

static int placement_tie(const struct kernel_task *task, struct kernel_cpu *candidate,
    struct kernel_cpu *previous)
{
    if ((candidate==task->cpu)!=(previous==task->cpu)) return candidate==task->cpu;
    struct kernel_cpu *caller=kernel_cpu_current();
    if ((candidate==caller)!=(previous==caller)) return candidate==caller;
    return candidate->logical_id<previous->logical_id;
}

static struct kernel_cpu *ready_place_locked(struct kernel_task *task, int flags)
{
    uint32_t mask=effective_cpu_mask(task);
    /* yield/片尾在实际换栈后回到原队列；资格改变时才重新放置。 */
    if ((flags&SCHED_READY_LOCAL) && task->cpu &&
        (mask&(1U<<task->cpu->logical_id))) return task->cpu;
    /* 唯一合法目标没有负载选择；沿最后执行者直接定位，仍保留RT排队规则。 */
    if (task->cpu && mask==(1U<<task->cpu->logical_id)) return task->cpu;
    struct kernel_cpu *caller=kernel_cpu_current();
    if (mask==(1U<<caller->logical_id)) return caller;
    struct kernel_cpu *best=0;
    unsigned best_load=0;
    int best_class=0;
    for (struct kernel_cpu *cpu=scheduler.cpus;cpu;cpu=cpu->scheduler_next) {
        if (!(mask&(1U<<cpu->logical_id))) continue;
        if (!(mask&(mask-1U))) return cpu;
        unsigned load=ready_load(cpu);
        int kind=placement_class(task,cpu,load);
        if (!best || kind<best_class || (kind==best_class &&
            (load<best_load || (load==best_load && placement_tie(task,cpu,best))))) {
            best=cpu; best_load=load; best_class=kind;
        }
    }
    if (!best) __builtin_trap();
    return best;
}

static int ready_allowed_on(struct kernel_cpu *cpu, struct kernel_task *task)
{
    if (!(effective_cpu_mask(task)&(1U<<cpu->logical_id))) return 0;
    if (is_rt(task) && !task->pi_base_priority && !rt_eligible(cpu)) return 0;
#if BOAROS_USER_SMP
    if (user_reserved_out_locked(cpu,task)) return 0;
#endif
    return 1;
}

static struct kernel_task *ready_pull_locked(struct kernel_cpu *cpu)
{
    struct kernel_task *best=0;
    for (struct kernel_cpu *other=scheduler.cpus;other;other=other->scheduler_next) {
        if (other==cpu || !other->pullable_ready) continue;
        for (struct kernel_sched_node *node=other->runqueue.first;node;node=node->next) {
            if (best && node->priority<=(unsigned)best->scheduling.priority) break;
            struct kernel_task *task=node->owner;
            if (task->wait.on_cpu || task->ready_cpu!=other) __builtin_trap();
            if (ready_allowed_on(cpu,task)) { best=task; break; }
        }
    }
    /* 只借用候选；claim在同一调度域内摘除原owner，再取得执行owner。 */
    return best;
}
#endif

struct kernel_task *ready_best_locked(void)
{
    KERNEL_RUN_SET(runs);
    struct kernel_cpu *cpu=kernel_cpu_current();
    int eligible=rt_eligible(cpu);
#if BOAROS_USER_SMP
    if (!cpu->logical_id) {
        struct kernel_sched_node *service=kernel_sched_pick(&scheduler.rt_services,eligible);
        if (service) return service->owner;
    }
#endif
    struct kernel_sched_node *node=kernel_sched_pick(
#if BOAROS_MAX_CPUS > 1
        &cpu->runqueue,
#else
        &scheduler.runqueue,
#endif
        eligible);
#if BOAROS_USER_SMP
    if (node && user_reserved_out_locked(cpu,node->owner)) {
        struct kernel_sched_node *reservation=kernel_sched_pick(&cpu->user_reservations,eligible);
        struct kernel_task *owner=reservation->owner;
        node=owner->ready_node.queued && !owner->user_service &&
            owner->ready_cpu==cpu ? &owner->ready_node : 0;
    }
#endif
#if BOAROS_MAX_CPUS > 1
    /* 正在运行者的yield只比较本核FIFO；空闲/阻塞后才取异核更高优先级工作。 */
    if (!cpu->current || cpu->current->idle ||
        (cpu->current->state!=KERNEL_THREAD_STATE_RUNNING &&
         cpu->current->state!=KERNEL_THREAD_STATE_READY)) {
        struct kernel_task *pulled=ready_pull_locked(cpu);
        if (pulled && (!node || pulled->scheduling.priority>(int)node->priority)) return pulled;
    }
#endif
    return node ? node->owner : 0;
}
#if BOAROS_USER_SMP
static void ready_remove_owned(struct kernel_task *task)
#else
#define ready_remove_owned ready_remove_locked
void ready_remove_locked(struct kernel_task *task)
#endif
{
#if BOAROS_MAX_CPUS > 1
    if (!task->ready_cpu || !task->ready_node.queued) __builtin_trap();
    struct kernel_cpu *owner=task->ready_cpu;
    struct kernel_sched_runqueue *queue=ready_queue(task,owner);
    if (queue==&owner->runqueue &&
        ((task->cpu_mask ? task->cpu_mask : 1U)&~(1U<<owner->logical_id))) {
        if (!owner->pullable_ready) __builtin_trap();
#if BOAROS_USER_SMP
        if (task->ready_node.priority) {
            if (!owner->pullable_rt) __builtin_trap();
            if (!--owner->pullable_rt)
                __atomic_fetch_and(&scheduler.pullable_rt_cpus,~(1U<<owner->logical_id),__ATOMIC_RELAXED);
        }
        __atomic_store_n(&owner->pullable_ready,owner->pullable_ready-1U,__ATOMIC_RELAXED);
#else
        owner->pullable_ready--;
#endif
    }
    dequeue_task(queue,&task->ready_node);
    task->ready_cpu=0;
#else
    dequeue_task(&scheduler.runqueue,&task->ready_node);
#endif
}
#if BOAROS_USER_SMP
void ready_remove_locked(struct kernel_task *task)
{ KERNEL_RUN_SET(runs); ready_remove_owned(task); }
#endif

static void ready_enqueue_at_locked(struct kernel_task *task, int flags
#if BOAROS_MAX_CPUS > 1
    ,struct kernel_cpu *target
#endif
    )
{
    if (task->wait.on_cpu) __builtin_trap();
#if BOAROS_COST_DIAGNOSTICS
    kernel_cost_ready(&task->cost);
#endif
    if (!task->accounted) scheduler_task_publish_locked(task);
    task->next=0;
#if BOAROS_MAX_CPUS > 1
    if (task->ready_cpu) __builtin_trap();
    if (!target) target=ready_place_locked(task,flags);
    task->ready_cpu=target;
    struct kernel_sched_runqueue *queue=ready_queue(task,target);
    enqueue_task(queue,&task->ready_node,task,flags&SCHED_READY_HEAD);
    /* Requested masks change only after dequeue. Unlike effective affinity,
     * these do not change when an outstanding RT reservation is released. */
    if (queue==&target->runqueue &&
        ((task->cpu_mask ? task->cpu_mask : 1U)&~(1U<<target->logical_id))) {
        if (target->pullable_ready==UINT32_MAX) __builtin_trap();
#if BOAROS_USER_SMP
        __atomic_store_n(&target->pullable_ready,target->pullable_ready+1U,__ATOMIC_RELAXED);
        if (task->ready_node.priority && !target->pullable_rt++)
            __atomic_fetch_or(&scheduler.pullable_rt_cpus,1U<<target->logical_id,__ATOMIC_RELAXED);
#else
        target->pullable_ready++;
#endif
    }
#else
    struct kernel_cpu *target=task->cpu?task->cpu:kernel_cpu_current();
    enqueue_task(&scheduler.runqueue,&task->ready_node,task,flags&SCHED_READY_HEAD);
#endif
    int preempt=!target->current || target->current->idle ||
        task->scheduling.priority>target->current->scheduling.priority;
#if BOAROS_USER_SMP
    preempt |= is_rt_service(task) && !is_rt_service(target->current);
#endif
    if (preempt) kernel_cpu_request_schedule(target);
}
void ready_enqueue_locked(struct kernel_task *task,int flags)
{
    KERNEL_RUN_SET(runs);
    ready_enqueue_at_locked(task,flags
#if BOAROS_MAX_CPUS > 1
        ,0
#endif
        );
}
void ready_remove(struct kernel_task *task)
{ KERNEL_RAW_SCOPE(guard, &kernel_wait_domain); ready_remove_locked(task); }
void ready_enqueue(struct kernel_task *task, int head)
{
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    KERNEL_RUN_SET(runs);
    if (task->wait.on_cpu) {
        if (task->wait.ready_pending) __builtin_trap();
        task->wait.ready_pending = 1;
        task->wait.ready_flags = head;
    } else ready_enqueue_locked(task, head);
}
void ready_append(struct kernel_task *task) { ready_enqueue(task, 0); }
static struct kernel_task *claim_next_locked(struct kernel_cpu *cpu,
    struct kernel_task *previous, struct kernel_task *next)
{
    if (!cpu || cpu != kernel_cpu_current() || !previous || cpu->current != previous ||
        cpu->switch_previous || !previous->wait.on_cpu
#if BOAROS_USER_SMP
        || (BOAROS_LOCK_DEBUG && !kernel_raw_lock_owned(kernel_run_lock(cpu)))
#else
        || !cpu->raw_locks || cpu->raw_locks->lock != &kernel_wait_domain
#endif
        ) __builtin_trap();
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
        ready_remove_owned(next);
#if BOAROS_USER_SMP
        if (!next->user_service && next->reservation_cpu==cpu)
            scheduler_user_reservation_release_locked(next);
#endif
#if BOAROS_USER_SMP
        __atomic_store_n(&next->state,KERNEL_THREAD_STATE_RUNNING,__ATOMIC_RELAXED);
#else
        next->state = KERNEL_THREAD_STATE_RUNNING;
#endif
    }
    /* 摘队列与取得执行owner是同一发布点，旧owner由新栈交接释放。 */
#if BOAROS_USER_SMP
    __atomic_store_n(&next->wait.on_cpu,1,__ATOMIC_RELAXED);
    __atomic_store_n(&next->cpu,cpu,__ATOMIC_RELEASE);
    __atomic_store_n(&cpu->current,next,__ATOMIC_RELEASE);
#else
    next->wait.on_cpu = 1;
    next->cpu = cpu;
    cpu->current = next;
#endif
    cpu->switch_previous = previous;
    cpu->rotate_other = 0;
    return next;
}

struct kernel_task *scheduler_select_next_locked(struct kernel_cpu *cpu,
    struct kernel_task *previous, int ready_allowed)
{
    KERNEL_RUN_SET(runs);
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
        (cpu->runqueue.first && cpu->runqueue.first->priority != 0)
#if BOAROS_USER_SMP
        || (!cpu->logical_id && scheduler.rt_services.first)
#endif
        );
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
#if BOAROS_USER_SMP
    uint64_t wait_deadline=cpu->logical_id?0:__atomic_load_n(&scheduler.wait_deadline,__ATOMIC_ACQUIRE);
    if (wait_deadline && (!deadline || wait_deadline < deadline)) deadline=wait_deadline;
#else
    struct kernel_task *blocked =
#if BOAROS_MAX_CPUS > 1
        cpu->logical_id ? 0 :
#endif
        deadline_index_first();
    if (blocked != 0 && (!deadline || blocked->wait.deadline < deadline))
        deadline = blocked->wait.deadline;
#endif
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
#if BOAROS_USER_SMP
        KERNEL_RAW_SCOPE(guard,kernel_run_lock(kernel_cpu_current()));
#else
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
#endif
        deadline = scheduler_deadline_locked(kernel_cpu_current());
    }
    scheduler_arm_timer(deadline);
    arch_interrupt_restore(irq);
}

void kernel_scheduler_deadline_changed(void) { scheduler_rearm_timer(); }

#if BOAROS_USER_SMP
int scheduler_local_task(const struct kernel_task *task,const struct kernel_cpu *cpu)
{
    return task->accounted && !task->scheduling.priority && !task->user_service &&
        !task->reservation_cpu && !task->rttime_ticks &&
        (effective_cpu_mask(task)&(1U<<cpu->logical_id));
}

void scheduler_ready_local_locked(struct kernel_task *task,struct kernel_cpu *cpu)
{
    ready_enqueue_at_locked(task,task->wait.ready_flags,cpu);
    /* 无绑定的任务优先复用上次执行核；该核忙时让一个空闲核尝试偷取。 */
    unsigned mask=effective_cpu_mask(task);
    if ((cpu->current!=cpu->idle || cpu->runqueue.count>1) && (mask&(mask-1U))) {
        for (struct kernel_cpu *other=__atomic_load_n(&scheduler.cpus,__ATOMIC_ACQUIRE);other;
             other=other->scheduler_next) {
            if (other!=cpu && (mask&(1U<<other->logical_id)) && kernel_cpu_online(other) &&
                __atomic_load_n(&other->current,__ATOMIC_ACQUIRE)==other->idle) {
                kernel_cpu_request_schedule(other);
                break;
            }
        }
    }
}

static int local_queue_eligible(struct kernel_cpu *cpu,struct kernel_task *current,int blocked)
{
    return (current->idle || (scheduler_local_task(current,cpu) && !current->user_slice_expired &&
        (blocked || current->state==KERNEL_THREAD_STATE_RUNNING))) && !cpu->user_reservations.first &&
        (cpu->logical_id || !scheduler.rt_services.first) &&
        (!cpu->runqueue.first || !cpu->runqueue.first->priority);
}

static int remote_rt_ready(const struct kernel_cpu *cpu)
{
    /* 调用者持目标核锁。RT入队和资格更新持共同控制及所有队列锁，
     * 因此在本次领取前不会悄悄出现可迁移的更高优先级任务。 */
    return (__atomic_load_n(&scheduler.pullable_rt_cpus,__ATOMIC_RELAXED)&~(1U<<cpu->logical_id))!=0;
}

/* 空闲核只借实际来源和目标两把队列锁。来源选中前旧栈必已归还；
 * 领取时一次完成摘除、CPU归属发布和执行权转交。RT由共同策略仲裁。 */
static __attribute__((noinline)) int try_pull_ordinary(struct kernel_cpu *cpu,
    struct kernel_task *current,int blocked,struct kernel_task **selected,uint64_t *deadline)
{
    for (struct kernel_cpu *source=__atomic_load_n(&scheduler.cpus,__ATOMIC_ACQUIRE);source;
         source=source->scheduler_next) {
        if (source==cpu || !__atomic_load_n(&source->pullable_ready,__ATOMIC_RELAXED)) continue;
        struct kernel_task *next=0;
        {
            struct kernel_raw_lock *one=kernel_run_lock(cpu),*two=kernel_run_lock(source);
            if (cpu->logical_id>source->logical_id) { one=two; two=kernel_run_lock(cpu); }
            KERNEL_RAW_SCOPE(first,one);
            KERNEL_RAW_SCOPE(second,two);
            if (!local_queue_eligible(cpu,current,blocked) || remote_rt_ready(cpu)) return -1;
            if (cpu->runqueue.first || (blocked && current->wait.ready_pending)) return 0;
            for (struct kernel_sched_node *node=source->runqueue.first;node;node=node->next) {
                if (node->priority) continue;
                if (!ready_allowed_on(cpu,node->owner)) continue;
                if (!scheduler_local_task(node->owner,cpu)) return -1;
                next=node->owner;
                break;
            }
            if (next) {
                account_cpu(cpu,kernel_time_monotonic_ns());
                if (current->idle) cpu->idle_context_saved=1;
                if (!blocked) (void)kernel_cpu_consume_schedule(cpu);
                next=claim_next_locked(cpu,current,next);
                if (!next) __builtin_trap();
                *deadline=scheduler_deadline_locked(cpu);
            }
        }
        if (next) {
            *selected=next;
            return 1;
        }
    }
    return 0;
}

int scheduler_select_local(int blocked,int rotate,int voluntary,struct kernel_task **selected,
                           uint64_t *deadline)
{
    struct kernel_cpu *cpu=kernel_cpu_current();
    struct kernel_task *current=cpu->current;
    *selected=0;
    if (cpu->preempt_depth) return 0;
    if (blocked || current->idle) {
        int pulled=try_pull_ordinary(cpu,current,blocked,selected,deadline);
        if (pulled) return pulled>0;
    }
    {
        KERNEL_RAW_SCOPE(guard,kernel_run_lock(cpu));
        if (!local_queue_eligible(cpu,current,blocked)) return 0;
        if ((blocked || current->idle) && remote_rt_ready(cpu)) return 0;
        struct kernel_sched_node *head=cpu->runqueue.first;
        account_cpu(cpu,kernel_time_monotonic_ns());
        if (blocked) {
            /* 唤醒在选择后继前到达时，当前栈仍归自己，无需切走再取回。 */
            if (current->wait.ready_pending && current->state==KERNEL_THREAD_STATE_READY) {
                current->wait.ready_pending=0;
                __atomic_store_n(&current->state,KERNEL_THREAD_STATE_RUNNING,__ATOMIC_RELAXED);
            } else *selected=claim_next_locked(cpu,current,head?head->owner:0);
        } else {
            (void)kernel_cpu_consume_schedule(cpu);
            rotate |= cpu->rotate_other;
            cpu->rotate_other=0;
            if (head && (current->idle || rotate || voluntary)) {
                if (current->idle) cpu->idle_context_saved=1;
                else {
                    if (current->wait.ready_pending) __builtin_trap();
                    __atomic_store_n(&current->state,KERNEL_THREAD_STATE_READY,__ATOMIC_RELAXED);
                    current->wait.ready_pending=1;
                    current->wait.ready_flags=SCHED_READY_LOCAL;
                }
                *selected=claim_next_locked(cpu,current,head->owner);
            }
        }
        *deadline=scheduler_deadline_locked(cpu);
    }
    /* 在实际切栈、页表和 FPU 交接前退掉本函数和双队列 guard 的栈帧。 */
    return 1;
}
#endif

enum kernel_scheduler_status scheduler_reschedule(int rotate_other, int voluntary, int check_queues)
{
    /* 调用者保持IRQ关闭；CPU借用在切换前结束，不跨睡眠或恢复点。 */
    struct kernel_cpu *cpu = kernel_cpu_current();
    uint64_t deadline;
#if BOAROS_USER_SMP
    struct kernel_task *old=cpu->current,*local;
    if ((!check_queues || !BOAROS_LOCK_DEBUG) && scheduler_select_local(0,rotate_other,voluntary,&local,&deadline)) {
        scheduler_arm_timer(deadline);
        return local?scheduler_switch_claimed(old,local):KERNEL_SCHEDULER_STATUS_OK;
    }
#endif
    if (cpu->preempt_depth) {
        {
            KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
            KERNEL_RUN_SET(runs);
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
    KERNEL_RUN_SET(runs);
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
#if BOAROS_USER_SMP
        kernel_run_set_release(&runs);
#endif
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
        current->wait.ready_flags = (!rotate &&
            !(disallowed && current->scheduling.policy==KERNEL_SCHED_OTHER)) ? SCHED_READY_HEAD : 0;
#if BOAROS_MAX_CPUS > 1
        if (!disallowed) current->wait.ready_flags |= SCHED_READY_LOCAL;
#endif
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
        KERNEL_RUN_SET(runs);
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
    if(!kernel_raw_lock_owned(&kernel_wait_domain) ||
       !task || base>99 || inherited>99)__builtin_trap();
    KERNEL_RUN_SET(runs);
    unsigned priority=base>inherited?base:inherited;
    unsigned saved=priority>base?base+1U:0;
    int old=task->scheduling.priority,move=priority!=(unsigned)old;
    int changed_boost=!!saved!=!!task->pi_base_priority;
    if(!move && saved==task->pi_base_priority)return;
    /* 未改变实际执行优先级时，owner交接不产生新的CPU记账边界。 */
    if(move && task->wait.on_cpu) {
        if(!task->cpu)__builtin_trap();
        if(task->cpu->current==task)account_cpu(task->cpu,kernel_time_monotonic_ns());
    }
    int queued=task->ready_node.queued;
#if BOAROS_MAX_CPUS > 1
    struct kernel_cpu *ready_owner=task->ready_cpu;
#endif
    if(move && queued)ready_remove_locked(task);
#if BOAROS_USER_SMP
    struct kernel_cpu *reservation=task->reservation_cpu;
    if(move && reservation)scheduler_user_reservation_release_locked(task);
#endif
    /* 只改变基础值而实际优先级相同时，保持同级FIFO位置。 */
    if(!move && changed_boost) {
        if (queued) boost_count_change(
#if BOAROS_MAX_CPUS > 1
            ready_queue(task,ready_owner),
#else
            &scheduler.runqueue,
#endif
            saved!=0);
#if BOAROS_USER_SMP
        if(reservation)boost_count_change(&reservation->user_reservations,saved!=0);
#endif
    }
    task->pi_base_priority=saved;
    __atomic_store_n(&task->scheduling.priority,(int)priority,__ATOMIC_RELAXED);
#if BOAROS_USER_SMP
    if(move && reservation && priority) {
        task->reservation_cpu=reservation;
        enqueue_task(&reservation->user_reservations,&task->reservation_node,task,priority<(unsigned)old);
    }
#endif
    if(move && queued)ready_enqueue_at_locked(task,priority<(unsigned)old
#if BOAROS_MAX_CPUS > 1
        ,ready_owner
#endif
        );
#if BOAROS_MAX_CPUS > 1
    if (changed_boost && queued) kernel_cpu_request_schedule(ready_owner);
#else
    if (changed_boost && queued) kernel_cpu_request_schedule(task->cpu?task->cpu:caller);
#endif
    if((move || changed_boost) && task->wait.on_cpu)
        kernel_cpu_request_schedule(task->cpu?task->cpu:caller);
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
            KERNEL_RUN_SET(runs);
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
                if(updated.policy!=task->scheduling.policy && task->wait.on_cpu) {
                    if(!task->cpu)__builtin_trap();
                    if(task->cpu->current==task)account_cpu(task->cpu,kernel_time_monotonic_ns());
                }
                if(task->pi_owned || task->pi_request)
                    kernel_futex_pi_recalculate_locked(task,updated.priority);
                else scheduler_pi_priority_locked(task,(unsigned)updated.priority,0);
                task->scheduling.policy=updated.policy;
                task->scheduling.reset_on_fork=updated.reset_on_fork;
                if(updated.policy==KERNEL_SCHED_RR && !task->scheduling.rr_remaining_ns)
                    task->scheduling.rr_remaining_ns=KERNEL_SCHED_RR_NS;
                struct kernel_cpu *target=task->cpu?task->cpu:kernel_cpu_current();
#if BOAROS_MAX_CPUS > 1
                if (task->ready_node.queued) target=task->ready_cpu;
#endif
                kernel_cpu_request_schedule(target);
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
        KERNEL_RUN_SET(runs);
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
