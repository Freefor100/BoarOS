#include "../../kernel/sched/private.h"
#include <arch/timer.h>
#include <kernel/time.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* 时间/实际换栈是机器边界；决策、队列、策略与raw使用生产实现。 */
_Thread_local uintptr_t sync_test_irq;
static struct kernel_cpu cpu;
static struct kernel_task running __attribute__((aligned(BOAROS_PAGE_SIZE)));
static struct kernel_task peer __attribute__((aligned(BOAROS_PAGE_SIZE)));
static unsigned char peer_stack[KERNEL_STACK_BYTES] __attribute__((aligned(KERNEL_STACK_BYTES)));
static unsigned char running_stack[KERNEL_STACK_BYTES] __attribute__((aligned(KERNEL_STACK_BYTES)));
static unsigned lock_calls, cpu_queries;
static uint64_t timer_request;
void __real_kernel_raw_lock_acquire(struct kernel_raw_lock *, struct kernel_raw_guard *);
void __wrap_kernel_raw_lock_acquire(struct kernel_raw_lock *lock, struct kernel_raw_guard *guard)
{
    lock_calls++;
    __real_kernel_raw_lock_acquire(lock, guard);
}
uint64_t riscv_sv39_current_satp(void) { return 0; }
static unsigned stack_releases, metadata_owned, stack_owned;
static unsigned char metadata_arena[3 * BOAROS_PAGE_SIZE] __attribute__((aligned(BOAROS_PAGE_SIZE)));
static unsigned char stack_arena[6 * BOAROS_PAGE_SIZE] __attribute__((aligned(KERNEL_STACK_BYTES)));
enum physical_page_status physical_page_allocate(struct physical_page_allocator *a, uint64_t *address)
{ (void)a; assert(!metadata_owned); metadata_owned = 1; *address = 10 * BOAROS_PAGE_SIZE; return PHYSICAL_PAGE_STATUS_OK; }
enum physical_page_status physical_page_allocate_order(struct physical_page_allocator *a, uint32_t order, uint64_t *address)
{ (void)a; assert(metadata_owned && !stack_owned && order == KERNEL_STACK_ORDER); stack_owned = 1; *address = 12 * BOAROS_PAGE_SIZE; return PHYSICAL_PAGE_STATUS_OK; }
enum physical_page_status physical_page_resolve(const struct physical_page_allocator *a, uint64_t address, void **pointer)
{
    (void)a;
    if (address == 10 * BOAROS_PAGE_SIZE) *pointer = metadata_arena + BOAROS_PAGE_SIZE;
    else { assert(address == 12 * BOAROS_PAGE_SIZE); *pointer = stack_arena + 2 * BOAROS_PAGE_SIZE; }
    return PHYSICAL_PAGE_STATUS_OK;
}
enum physical_page_status physical_page_release(struct physical_page_allocator *a, uint64_t address)
{ (void)a; assert(address == 10 * BOAROS_PAGE_SIZE && metadata_owned && !stack_owned && !cpu.raw_locks); metadata_owned = 0; return PHYSICAL_PAGE_STATUS_OK; }
enum riscv_sv39_status riscv_sv39_kernel_window_map(struct physical_page_allocator *a, uint64_t va, uint64_t pa)
{ (void)a; (void)va; (void)pa; __builtin_trap(); }

void kernel_heap_release(struct kernel_heap *heap, void *pointer)
{ (void)heap; (void)pointer; __builtin_trap(); /* This private construction has no PID heap owner. */ }

int riscv_sv39_kernel_window_active(void) { return 0; }
enum riscv_sv39_status riscv_sv39_kernel_window_map_pages(
    struct physical_page_allocator *a, uint64_t va, uint64_t pa, unsigned pages)
{ (void)a; (void)va; (void)pa; (void)pages; __builtin_trap(); }
enum riscv_sv39_status riscv_sv39_kernel_window_unmap_pages(
    struct physical_page_allocator *a, uint64_t va, unsigned pages)
{ (void)a; (void)va; (void)pages; __builtin_trap(); }
enum riscv_sv39_status riscv_sv39_kernel_window_unmap(
    struct physical_page_allocator *allocator, uint64_t address)
{ (void)allocator; (void)address; __builtin_trap(); }
enum physical_page_status physical_page_release_order(
    struct physical_page_allocator *allocator, uint64_t address, uint32_t order)
{
    (void)allocator;
    assert(order == KERNEL_STACK_ORDER);
    if (address == 12 * BOAROS_PAGE_SIZE) { assert(stack_owned && metadata_owned); stack_owned = 0; }
    else assert(address == peer.stack_physical_address);
    assert(!cpu.raw_locks && !peer.wait.on_cpu);
    stack_releases++;
    return PHYSICAL_PAGE_STATUS_OK;
}

static unsigned switches;
static struct kernel_task *selected;
void *sync_test_cpu(void) { cpu_queries++; return &cpu; }
void *sync_test_current_task(void) { return cpu.current; }
void sync_test_current_task_set(void *task) { cpu.current = task; }
uintptr_t sync_test_stack(void) { return cpu.current->stack_high - 16; }
uint64_t kernel_time_monotonic_ns(void) { return 1; }
uint32_t kernel_time_is_initialized(void) { return 1; }
enum kernel_mm_status kernel_mm_resolve_user_fault(struct kernel_mm *mm,
                                                   uint64_t address, uint32_t access)
{ (void)mm; (void)address; (void)access; __builtin_trap(); /* Rejected kernel caller cannot enter MM. */ }
uint64_t riscv_time_read(void) { return 1; }
enum riscv_timer_status riscv_timer_set_scheduler_deadline(uint64_t deadline)
{ assert(!cpu.raw_locks);timer_request=deadline;return RISCV_TIMER_STATUS_NOT_STARTED; }
enum kernel_time_status kernel_time_deadline_from_monotonic(uint64_t ns, uint64_t *ticks)
{ *ticks = ns; return KERNEL_TIME_STATUS_OK; }
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
    running.physical_address = 3 * BOAROS_PAGE_SIZE;
    running.stack_physical_address = 4 * BOAROS_PAGE_SIZE;
    running.stack_high = (uintptr_t)running_stack + sizeof(running_stack);
    running.stack_low = (uintptr_t)running_stack + KERNEL_STACK_GUARD_BYTES;
    *(uint64_t *)(running.stack_low - sizeof(uint64_t)) = KERNEL_STACK_CANARY;
    running.context.tp = (uintptr_t)&running;
    running.context.sp = running.stack_high;
    running.arch.kernel_sp = running.stack_high;
    peer.magic = KERNEL_THREAD_MAGIC;
    peer.physical_address = BOAROS_PAGE_SIZE;
    peer.stack_physical_address = 2 * BOAROS_PAGE_SIZE;
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

static void private_storage_case(void)
{
    memset(metadata_arena, 0xa7, sizeof(metadata_arena));
    memset(stack_arena, 0xa7, sizeof(stack_arena));
    struct kernel_task *created;
    assert(allocate_task_storage(&created) == KERNEL_SCHEDULER_STATUS_OK);
    assert(created == (void *)(metadata_arena + BOAROS_PAGE_SIZE));
    assert(created->cpu == &cpu && !created->accounted && !created->tid_owned);
    for (unsigned i = 0; i < sizeof(metadata_arena); i++)
        if (i < BOAROS_PAGE_SIZE || i >= 2 * BOAROS_PAGE_SIZE) assert(metadata_arena[i] == 0xa7);
    for (unsigned i = 0; i < sizeof(stack_arena); i++) {
        if (i >= 2 * BOAROS_PAGE_SIZE && i < 4 * BOAROS_PAGE_SIZE) {
            if (i < 2 * BOAROS_PAGE_SIZE + KERNEL_STACK_GUARD_BYTES - sizeof(uint64_t) ||
                i >= 2 * BOAROS_PAGE_SIZE + KERNEL_STACK_GUARD_BYTES)
                assert(stack_arena[i] == KERNEL_STACK_FILL);
        } else assert(stack_arena[i] == 0xa7);
    }
    assert(*(uint64_t *)(created->stack_low - sizeof(uint64_t)) == KERNEL_STACK_CANARY);
    assert(release_task_storage(created, KERNEL_SCHEDULER_STATUS_OK) == KERNEL_SCHEDULER_STATUS_OK);
    assert(!metadata_owned && !stack_owned && !scheduler.retired_pending);
}

static void clear_owned_page_case(void)
{
    static unsigned char arena[3 * BOAROS_PAGE_SIZE] __attribute__((aligned(BOAROS_PAGE_SIZE)));
    memset(arena, 0xa7, sizeof(arena));
    clear_page(arena + BOAROS_PAGE_SIZE);
    for (unsigned i = 0; i < sizeof(arena); i++)
        assert(arena[i] == (i >= BOAROS_PAGE_SIZE && i < 2 * BOAROS_PAGE_SIZE ? 0 : 0xa7));
}

static void stack_prefix_case(unsigned prefix, int canary_bad)
{
    setup(KERNEL_SCHED_OTHER, 0, 0);
    const unsigned bytes = KERNEL_STACK_BYTES - KERNEL_STACK_GUARD_BYTES;
    assert(prefix <= bytes);
    unsigned char *stack = (void *)peer.stack_low;
    memset(stack, KERNEL_STACK_FILL, bytes);
    if (prefix < bytes) stack[prefix] ^= 0x80;
    if (canary_bad) *(uint64_t *)(peer.stack_low - sizeof(uint64_t)) ^= 1;
    scheduler.stack_statistics.minimum_free_bytes = bytes;
    stack_releases = 0;
    assert(release_task_stack(&peer) == (canary_bad
        ? KERNEL_SCHEDULER_STATUS_STACK_CORRUPT : KERNEL_SCHEDULER_STATUS_OK));
    if (canary_bad) { assert(!stack_releases && !scheduler.stack_statistics.stacks_released); return; }
    assert(stack_releases == 1 && scheduler.stack_statistics.stacks_released == 1);
    assert(scheduler.stack_statistics.maximum_used_bytes == bytes - prefix);
    assert(scheduler.stack_statistics.minimum_free_bytes == prefix);
    assert(peer.stack_physical_address == KERNEL_THREAD_NO_PAGE && !peer.stack_low);
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
    /* 校验、记账、选择和期限计算共用一个调度域，硬件重装在锁外。 */
    setup(KERNEL_SCHED_FIFO, 50, 10);
    cpu_queries = 0;
    assert(validate_current() == KERNEL_SCHEDULER_STATUS_OK);
    assert(cpu_queries <= 2);
    cpu_queries = 0;
    assert(kernel_scheduler_resolve_current_user_fault(0, 0) == KERNEL_MM_STATUS_STATE);
    /* The rejected kernel caller must still validate the real current owner;
     * stable CPU/current checks need no repeated lookup for each field. */
    assert(cpu_queries <= 1);
    lock_calls = cpu_queries = 0;
    assert(kernel_scheduler_yield_current() == KERNEL_SCHEDULER_STATUS_OK);
    assert(lock_calls == 1 && !switches);
    fprintf(stderr, "no-switch yield CPU queries=%u\n", cpu_queries);
    assert(cpu_queries <= 20);
    lock_calls = cpu_queries = 0;
    assert(kernel_scheduler_on_tick(1) == KERNEL_SCHEDULER_STATUS_OK);
    assert(lock_calls == 1 && !switches);
    /* CPU identity is stable through the IRQ-off decision and timer update.
     * A generous work bound protects against repeated volatile TP lookups. */
    fprintf(stderr, "no-switch tick CPU queries=%u\n", cpu_queries);
    assert(cpu_queries <= 20);
    /* 检查送给实际timer边界的请求，不保留另一个生产deadline镜像供测试读。 */
    setup(KERNEL_SCHED_OTHER,0,0);
    { KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
      ready_remove_locked(&peer);peer.wait.deadline=73;deadline_index_insert(&peer); }
    scheduler_rearm_timer();assert(timer_request==73);
    { KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);deadline_index_remove(&peer);peer.wait.deadline=0; }
    scheduler_rearm_timer();assert(!timer_request);
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
    clear_owned_page_case();
    private_storage_case();
    const unsigned bytes = KERNEL_STACK_BYTES - KERNEL_STACK_GUARD_BYTES;
    for (unsigned prefix = 0; prefix <= bytes; prefix++) stack_prefix_case(prefix, 0);
    stack_prefix_case(0, 1); stack_prefix_case(bytes, 1);
    puts("scheduler handoff: FIFO/RR lower/equal/higher, exact stack prefix and canary passed");
}
