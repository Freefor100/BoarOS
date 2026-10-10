#include "../../kernel/sched/private.h"
#include <arch/timer.h>
#include <kernel/time.h>
#include <assert.h>
#include <stdio.h>

/* 时间/实际换栈是机器边界；决策、队列、策略与raw使用生产实现。 */
struct kernel_raw_lock kernel_wait_domain;
_Thread_local uintptr_t sync_test_irq;
static struct kernel_cpu cpu;
static struct kernel_task running __attribute__((aligned(BOAROS_PAGE_SIZE)));
static struct kernel_task peer __attribute__((aligned(BOAROS_PAGE_SIZE)));
static unsigned char peer_stack[KERNEL_STACK_BYTES] __attribute__((aligned(KERNEL_STACK_BYTES)));
static unsigned char running_stack[KERNEL_STACK_BYTES] __attribute__((aligned(KERNEL_STACK_BYTES)));
static unsigned lock_calls;
void __real_kernel_raw_lock_acquire(struct kernel_raw_lock *, struct kernel_raw_guard *);
void __wrap_kernel_raw_lock_acquire(struct kernel_raw_lock *lock, struct kernel_raw_guard *guard)
{
    lock_calls++;
    __real_kernel_raw_lock_acquire(lock, guard);
}
uint64_t riscv_sv39_current_satp(void) { return 0; }
static unsigned switches;
static struct kernel_task *selected;
void *sync_test_cpu(void) { return &cpu; }
void *sync_test_current_task(void) { return cpu.current; }
void sync_test_current_task_set(void *task) { cpu.current = task; }
uintptr_t sync_test_stack(void) { return cpu.current->stack_high - 16; }
uint64_t kernel_time_monotonic_ns(void) { return 1; }
uint32_t kernel_time_is_initialized(void) { return 1; }
uint64_t riscv_time_read(void) { return 1; }
enum riscv_timer_status riscv_timer_set_scheduler_deadline(uint64_t deadline)
{ (void)deadline; return RISCV_TIMER_STATUS_NOT_STARTED; }
enum kernel_time_status kernel_time_deadline_from_monotonic(uint64_t ns, uint64_t *ticks)
{ *ticks = ns; return KERNEL_TIME_STATUS_OK; }
struct kernel_task *deadline_index_first(void) { return 0; }
enum kernel_scheduler_status __wrap_scheduler_switch_claimed(struct kernel_task *previous, struct kernel_task *next)
{
    assert(previous != cpu.current && next == cpu.current &&
           !cpu.raw_locks && !cpu.preempt_depth);
    switches++;
    selected = next;
    return KERNEL_SCHEDULER_STATUS_OK;
}
static void setup(int policy, int priority, int next_priority)
{
    scheduler = (struct kernel_scheduler){.initialized = KERNEL_SCHEDULER_INITIALIZED};
    running = (struct kernel_task){0};
    peer = (struct kernel_task){0};
    kernel_cpu_initialize(&cpu, 0, &running.io_context);
    kernel_raw_lock_init(&kernel_wait_domain, KERNEL_RAW_RANK_SCHEDULER);
    cpu.current = &running;
    running.cpu = &cpu;
    running.state = KERNEL_THREAD_STATE_RUNNING;
    running.wait.on_cpu = 1;
    assert(!kernel_sched_policy_set(&running.scheduling, policy, priority, 0));
    running.magic = KERNEL_THREAD_MAGIC;
    running.physical_address = 12288;
    running.stack_physical_address = 16384;
    running.stack_high = (uintptr_t)running_stack + sizeof(running_stack);
    running.stack_low = (uintptr_t)running_stack + KERNEL_STACK_GUARD_BYTES;
    *(uint64_t *)(running.stack_low - sizeof(uint64_t)) = KERNEL_STACK_CANARY;
    running.context.tp = (uintptr_t)&running;
    running.context.sp = running.stack_high;
    running.arch.kernel_sp = running.stack_high;
    peer.magic = KERNEL_THREAD_MAGIC;
    peer.physical_address = 4096;
    peer.stack_physical_address = 8192;
    peer.stack_high = (uintptr_t)peer_stack + sizeof(peer_stack);
    peer.stack_low = (uintptr_t)peer_stack + KERNEL_STACK_GUARD_BYTES;
    *(uint64_t *)(peer.stack_low - sizeof(uint64_t)) = KERNEL_STACK_CANARY;
    peer.context.tp = (uintptr_t)&peer;
    peer.context.sp = peer.stack_high;
    peer.arch.kernel_sp = peer.stack_high;
    peer.scheduling.priority = next_priority;
    peer.state = KERNEL_THREAD_STATE_READY;
    kernel_sched_enqueue(&scheduler.runqueue, &peer.ready_node, &peer,
                         (unsigned)next_priority, 0);
    struct kernel_rt_bandwidth initial_budget;
    kernel_rt_bandwidth_init(&initial_budget, 1);
    scheduler.rt_configuration = (struct kernel_rt_configuration){
        .period_ns = initial_budget.period_ns, .runtime_ns = initial_budget.runtime_ns };
    scheduler_cpu_initialize(&cpu, &scheduler.boot_idle, 1);
    switches = 0;
    selected = 0;
    sync_test_irq = 0;
}
static void rotate_case(int policy, int next_priority, int expected)
{
    setup(policy, 50, next_priority);
    if (policy == KERNEL_SCHED_RR) running.scheduling.rr_remaining_ns = 0;
    assert(scheduler_reschedule(0, policy == KERNEL_SCHED_FIFO, 1) == KERNEL_SCHEDULER_STATUS_OK);
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
    assert(scheduler_reschedule(0, 0, 1) == KERNEL_SCHEDULER_STATUS_OK);
    assert(switches == 1 && selected == &peer && running.wait.ready_head == 1);
    setup(KERNEL_SCHED_FIFO, 50, 0);
    cpu.rt_bandwidth.consumed_ns = (uint64_t)cpu.rt_bandwidth.runtime_ns;
    assert(scheduler_reschedule(0, 0, 1) == KERNEL_SCHEDULER_STATUS_OK);
    assert(switches == 1 && selected == &peer);
    setup(KERNEL_SCHED_OTHER, 0, 0);
    scheduler.boot_idle.wait.on_cpu = 1;
    cpu.current = &scheduler.boot_idle;
    assert(scheduler_reschedule(0, 0, 1) == KERNEL_SCHEDULER_STATUS_OK);
    assert(switches == 1 && selected == &peer && cpu.idle_context_saved);
    /* A no-switch yield validates and accounts under one shared-domain
     * acquisition, followed by separate hardware-deadline preparation. */
    setup(KERNEL_SCHED_FIFO, 50, 10);
    lock_calls = 0;
    assert(kernel_scheduler_yield_current() == KERNEL_SCHEDULER_STATUS_OK);
    assert(lock_calls <= 2 && !switches);
    /* Reject a candidate that would otherwise preempt: neither deferred nor
     * ordinary validation failure may consume an existing request or owner. */
    for (unsigned deferred = 0; deferred < 2; deferred++) {
        setup(KERNEL_SCHED_FIFO, 50, 60);
        peer.magic = 0;
        kernel_cpu_request_schedule(&cpu);
        cpu.rotate_other = 1;
        if (deferred) kernel_preempt_disable();
        assert(scheduler_reschedule(0, 0, 1) == KERNEL_SCHEDULER_STATUS_INVALID_STATE);
        assert(!switches && !cpu.switch_previous && !peer.wait.on_cpu);
        assert(cpu.current == &running && running.wait.on_cpu && peer.ready_node.queued);
        assert(kernel_cpu_schedule_requested(&cpu) && cpu.rotate_other == 1);
        assert(!cpu.raw_locks && cpu.preempt_depth == deferred && !sync_test_irq);
        if (deferred) kernel_preempt_enable();
    }
    puts("scheduler handoff: FIFO/RR lower/equal/higher, throttling and idle passed");
}
