#include "../../kernel/sched/private.h"
#include <arch/timer.h>
#include <kernel/time.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>

struct kernel_scheduler scheduler;
_Thread_local uintptr_t sync_test_irq;
static _Thread_local struct kernel_cpu *active_cpu;
static uint64_t now;
static _Thread_local int check_policy_domain;
static struct kernel_cpu cpus[4];
static struct kernel_task tasks[4], idles[4];
static pthread_barrier_t start;
static struct kernel_task *selected[4];
void *sync_test_cpu(void) { return active_cpu; }
uint64_t kernel_time_monotonic_ns(void) { return now; }
void __real_kernel_sched_policy_charge(struct kernel_sched_policy *, uint64_t);
void __wrap_kernel_sched_policy_charge(struct kernel_sched_policy *policy, uint64_t delta)
{
    assert(active_cpu->raw_locks && active_cpu->raw_locks->lock == &kernel_wait_domain);
    __real_kernel_sched_policy_charge(policy, delta);
}
int __real_kernel_sched_policy_set(struct kernel_sched_policy *, int, int, int);
int __wrap_kernel_sched_policy_set(struct kernel_sched_policy *policy,
                                   int kind, int priority, int keep)
{
    if (check_policy_domain)
        assert(active_cpu->raw_locks && active_cpu->raw_locks->lock == &kernel_wait_domain);
    return __real_kernel_sched_policy_set(policy, kind, priority, keep);
}
struct kernel_task *process_find_identity(kernel_pid_t number, enum kernel_pid_role role)
{ (void)number; (void)role; __builtin_trap(); }
uint32_t kernel_time_is_initialized(void) { return 1; }
uint64_t riscv_time_read(void) { return now; }
enum riscv_timer_status riscv_timer_set_scheduler_deadline(uint64_t deadline)
{ (void)deadline; return RISCV_TIMER_STATUS_NOT_STARTED; }
enum kernel_time_status kernel_time_deadline_from_monotonic(uint64_t ns, uint64_t *ticks)
{ *ticks = ns; return KERNEL_TIME_STATUS_OK; }
struct kernel_task *deadline_index_first(void) { return 0; }
struct kernel_wait_record *kernel_wait_record_of(struct kernel_task *task)
{ return &task->wait; }
void kernel_wait_backend_ready(struct kernel_task *task)
{ ready_enqueue_locked(task, task->wait.ready_head); }

static void independent_runtime(void)
{
    scheduler = (struct kernel_scheduler){0};
    kernel_raw_lock_init(&kernel_wait_domain, KERNEL_RAW_RANK_SCHEDULER);
    struct kernel_rt_bandwidth initial_budget;
    kernel_rt_bandwidth_init(&initial_budget, 0);
    scheduler.rt_configuration = (struct kernel_rt_configuration){
        .period_ns = initial_budget.period_ns, .runtime_ns = initial_budget.runtime_ns };
    for (unsigned i = 0; i < 2; i++) {
        kernel_cpu_initialize(&cpus[i], 7 + i * 6, &tasks[i].io_context);
        tasks[i] = (struct kernel_task){0};
        tasks[i].cpu = &cpus[i];
        tasks[i].state = KERNEL_THREAD_STATE_RUNNING;
        cpus[i].current = &tasks[i];
        active_cpu = &cpus[i];
        scheduler_cpu_initialize(&cpus[i], &idles[i], 0);
        assert(!kernel_sched_policy_set(&tasks[i].scheduling, KERNEL_SCHED_RR, 50, 0));
    }
    active_cpu = &cpus[0];
    now = UINT64_C(10000000);
    scheduler_account_runtime();
    active_cpu = &cpus[1];
    now = UINT64_C(20000000);
    scheduler_account_runtime();
    assert(tasks[0].scheduling.rr_remaining_ns == KERNEL_SCHED_RR_NS - UINT64_C(10000000));
    /* 两个任务都从0开始运行，不能把另一CPU的结算点当成自己的起点。 */
    assert(tasks[1].scheduling.rr_remaining_ns == KERNEL_SCHED_RR_NS - UINT64_C(20000000));
    active_cpu = &cpus[0];
    now = UINT64_C(25000000);
    scheduler_account_runtime();
    assert(tasks[0].scheduling.rr_remaining_ns == KERNEL_SCHED_RR_NS - UINT64_C(25000000));
    scheduler.initialized = KERNEL_SCHEDULER_INITIALIZED;
    assert(!kernel_scheduler_rt_bandwidth_set(1, 10000));
    assert(!kernel_rt_bandwidth_eligible(&cpus[0].rt_bandwidth));
    assert(!kernel_rt_bandwidth_eligible(&cpus[1].rt_bandwidth));
    assert(cpus[0].rt_bandwidth.consumed_ns == UINT64_C(25000000));
    assert(cpus[1].rt_bandwidth.consumed_ns == UINT64_C(25000000));
    tasks[0].accounted = tasks[0].arch.user_mode = tasks[0].tid_owned = 1;
    check_policy_domain = 1;
    assert(!kernel_task_sched_set(&tasks[0], 0, KERNEL_SCHED_FIFO, 70, 0));
    check_policy_domain = 0;
    struct kernel_sched_policy result;
    assert(!kernel_task_sched_get(&tasks[0], 0, &result));
    assert(result.policy == KERNEL_SCHED_FIFO && result.priority == 70);
    assert(!kernel_scheduler_rt_bandwidth_set(0, 2000000));
    assert(cpus[0].rt_bandwidth.consumed_ns == UINT64_C(25000000));
    assert(cpus[1].rt_bandwidth.consumed_ns == UINT64_C(25000000));
    puts("scheduler per-CPU runtime accounting passed");
}

static void complete_machine_handoff(struct kernel_cpu *cpu)
{
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    struct kernel_task *previous = cpu->switch_previous;
    assert(previous && previous->wait.on_cpu && cpu->current->wait.on_cpu);
    cpu->switch_previous = 0;
    kernel_wait_switch_finish_locked(previous);
}

static void *claim_one(void *argument)
{
    unsigned id = (unsigned)(uintptr_t)argument;
    active_cpu = &cpus[id];
    sync_test_irq = 1;
    int barrier = pthread_barrier_wait(&start);
    assert(!barrier || barrier == PTHREAD_BARRIER_SERIAL_THREAD);
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        selected[id] = scheduler_select_next_locked(active_cpu, active_cpu->current, 1);
        if (selected[id]) {
            assert(selected[id] == &tasks[0] && !tasks[0].ready_node.queued);
            assert(tasks[0].cpu == active_cpu && tasks[0].wait.on_cpu);
            /* 尚未换栈，旧idle必须仍有执行owner。 */
            assert(idles[id].wait.on_cpu && active_cpu->switch_previous == &idles[id]);
        }
    }
    if (selected[id]) complete_machine_handoff(active_cpu);
    assert(sync_test_irq && !active_cpu->raw_locks && !active_cpu->preempt_depth);
    return 0;
}

static void concurrent_claim(unsigned count)
{
    scheduler = (struct kernel_scheduler){0};
    now = 0;
    kernel_raw_lock_init(&kernel_wait_domain, KERNEL_RAW_RANK_SCHEDULER);
    scheduler.rt_configuration = (struct kernel_rt_configuration){UINT64_C(1000000000), INT64_C(950000000)};
    for (unsigned i = 0; i < count; i++) {
        idles[i] = tasks[i] = (struct kernel_task){0};
        kernel_cpu_initialize(&cpus[i], 7 + i * 6, &idles[i].io_context);
        active_cpu = &cpus[i];
        scheduler_cpu_initialize(active_cpu, &idles[i], 0);
        active_cpu->current = &idles[i];
        idles[i].wait.on_cpu = 1;
        idles[i].state = KERNEL_THREAD_STATE_IDLE;
        selected[i] = 0;
    }
    tasks[0].state = KERNEL_THREAD_STATE_READY;
    ready_append(&tasks[0]);
    assert(!pthread_barrier_init(&start, 0, count));
    pthread_t threads[4];
    for (unsigned i = 0; i < count; i++) assert(!pthread_create(&threads[i], 0, claim_one, (void *)(uintptr_t)i));
    unsigned claims = 0;
    for (unsigned i = 0; i < count; i++) {
        assert(!pthread_join(threads[i], 0));
        claims += selected[i] != 0;
    }
    assert(claims == 1 && !scheduler.runqueue.first && tasks[0].wait.on_cpu);
    active_cpu = tasks[0].cpu;
    sync_test_irq = 1;
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        tasks[0].state = KERNEL_THREAD_STATE_EXITED;
        assert(scheduler_select_next_locked(active_cpu, &tasks[0], 0) == active_cpu->idle);
        assert(tasks[0].wait.on_cpu);
    }
    complete_machine_handoff(active_cpu);
    assert(!tasks[0].wait.on_cpu);
    for (unsigned i = 0; i < count; i++)
        assert(cpus[i].current == &idles[i] && idles[i].wait.on_cpu && !cpus[i].switch_previous);
    assert(!pthread_barrier_destroy(&start));
}

int main(void)
{
    independent_runtime();
    for (unsigned count = 2; count <= 4; count += 2)
        for (unsigned iteration = 0; iteration < 64; iteration++) concurrent_claim(count);
    puts("scheduler 2/4-CPU atomic selection and execution-owner handoff passed");
    return 0;
}
