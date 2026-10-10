#include "../../kernel/sched/private.h"
#include <arch/timer.h>
#include <kernel/time.h>
#include <assert.h>
#include <stdio.h>

struct kernel_scheduler scheduler;
struct kernel_raw_lock kernel_wait_domain;
_Thread_local uintptr_t sync_test_irq;
static _Thread_local struct kernel_cpu *active_cpu;
static uint64_t now;
static struct kernel_cpu cpus[2];
static struct kernel_task tasks[2], idles[2];
void *sync_test_cpu(void) { return active_cpu; }
uint64_t kernel_time_monotonic_ns(void) { return now; }
uint32_t kernel_time_is_initialized(void) { return 1; }
uint64_t riscv_time_read(void) { return now; }
enum riscv_timer_status riscv_timer_set_scheduler_deadline(uint64_t deadline)
{ (void)deadline; return RISCV_TIMER_STATUS_NOT_STARTED; }
enum kernel_time_status kernel_time_deadline_from_monotonic(uint64_t ns, uint64_t *ticks)
{ *ticks = ns; return KERNEL_TIME_STATUS_OK; }
struct kernel_task *deadline_index_first(void) { return 0; }

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
    assert(!kernel_scheduler_rt_bandwidth_set(0, 2000000));
    assert(cpus[0].rt_bandwidth.consumed_ns == UINT64_C(25000000));
    assert(cpus[1].rt_bandwidth.consumed_ns == UINT64_C(25000000));
    puts("scheduler per-CPU runtime accounting passed");
}

int main(void)
{
    independent_runtime();
    return 0;
}
