#include "../../kernel/sched/private.h"
#include <arch/timer.h>
#include <kernel/time.h>
#include <assert.h>
#include <stdio.h>

/* 时间/实际换栈是机器边界；决策、队列、策略与raw使用生产实现。 */
struct kernel_scheduler scheduler;
struct kernel_raw_lock kernel_wait_domain;
_Thread_local uintptr_t sync_test_irq;
static struct kernel_cpu cpu;
static struct kernel_task running, peer;
static unsigned switches;
static struct kernel_task *selected;
void *sync_test_cpu(void) { return &cpu; }
uint64_t kernel_time_monotonic_ns(void) { return 1; }
uint32_t kernel_time_is_initialized(void) { return 1; }
uint64_t riscv_time_read(void) { return 1; }
enum riscv_timer_status riscv_timer_set_scheduler_deadline(uint64_t deadline)
{ (void)deadline; return RISCV_TIMER_STATUS_NOT_STARTED; }
enum kernel_time_status kernel_time_deadline_from_monotonic(uint64_t ns, uint64_t *ticks)
{ *ticks = ns; return KERNEL_TIME_STATUS_OK; }
struct kernel_task *deadline_index_first(void) { return 0; }
enum kernel_scheduler_status scheduler_switch_current_away(struct kernel_task *previous)
{
    assert(previous == cpu.current && !cpu.raw_locks && !cpu.preempt_depth);
    switches++;
    selected = ready_best();
    return KERNEL_SCHEDULER_STATUS_OK;
}
static void setup(int policy, int priority, int next_priority)
{
    scheduler = (struct kernel_scheduler){0};
    running = (struct kernel_task){0};
    peer = (struct kernel_task){0};
    kernel_cpu_initialize(&cpu, 0, &running.io_context);
    kernel_raw_lock_init(&kernel_wait_domain, KERNEL_RAW_RANK_SCHEDULER);
    cpu.current = &running;
    running.cpu = &cpu;
    running.state = KERNEL_THREAD_STATE_RUNNING;
    running.wait.on_cpu = 1;
    assert(!kernel_sched_policy_set(&running.scheduling, policy, priority, 0));
    peer.scheduling.priority = next_priority;
    kernel_sched_enqueue(&scheduler.runqueue, &peer.ready_node, &peer,
                         (unsigned)next_priority, 0);
    kernel_rt_bandwidth_init(&scheduler.rt_bandwidth, 1);
    switches = 0;
    selected = 0;
    sync_test_irq = 0;
}
static void rotate_case(int policy, int next_priority, int expected)
{
    setup(policy, 50, next_priority);
    if (policy == KERNEL_SCHED_RR) running.scheduling.rr_remaining_ns = 0;
    assert(scheduler_reschedule(0, policy == KERNEL_SCHED_FIFO) == KERNEL_SCHEDULER_STATUS_OK);
    assert(switches == (unsigned)expected);
    assert(!running.ready_node.queued && running.wait.on_cpu);
    assert(running.wait.ready_pending == (unsigned)expected);
    if (expected) assert(selected == &peer);
    else assert(running.state == KERNEL_THREAD_STATE_RUNNING);
    if (policy == KERNEL_SCHED_RR) assert(running.scheduling.rr_remaining_ns == KERNEL_SCHED_RR_NS);
    assert(!cpu.raw_locks && !cpu.preempt_depth && !sync_test_irq);
}
int main(void)
{
    for (int policy = KERNEL_SCHED_FIFO; policy <= KERNEL_SCHED_RR; policy++) {
        rotate_case(policy, 10, 0);
        rotate_case(policy, 50, 1);
        rotate_case(policy, 60, 1);
    }
    setup(KERNEL_SCHED_FIFO, 50, 60);
    assert(scheduler_reschedule(0, 0) == KERNEL_SCHEDULER_STATUS_OK);
    assert(switches == 1 && selected == &peer && running.wait.ready_head == 1);
    setup(KERNEL_SCHED_FIFO, 50, 0);
    scheduler.rt_bandwidth.consumed_ns = (uint64_t)scheduler.rt_bandwidth.runtime_ns;
    assert(scheduler_reschedule(0, 0) == KERNEL_SCHEDULER_STATUS_OK);
    assert(switches == 1 && selected == &peer);
    setup(KERNEL_SCHED_OTHER, 0, 0);
    scheduler.idle.wait.on_cpu = 1;
    cpu.current = &scheduler.idle;
    assert(scheduler_reschedule(0, 0) == KERNEL_SCHEDULER_STATUS_OK);
    assert(switches == 1 && selected == &peer && scheduler.idle_context_saved);
    puts("scheduler handoff: FIFO/RR lower/equal/higher, throttling and idle passed");
}
