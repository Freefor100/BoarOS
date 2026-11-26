#include "../../kernel/sched/private.h"
#include <arch/timer.h>
#include <kernel/mm_backend.h>
#include <kernel/signal.h>
#include <kernel/time.h>
#include <kernel/ptrace.h>
#include <kernel/namespace.h>
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
static uint64_t now;
static struct kernel_wait_cursor join_cursor;
static struct kernel_wait_queue *join_borrow_queue;
static unsigned join_borrow_returned;
/* This scheduler-only fixture creates neither event FDs nor dispositions. */
uint64_t kernel_timer_fd_next;
/* These scheduler actors have no tracer or namespace owners. Check that
 * boundary explicitly when the production lifetime path reaches it. */
int kernel_ptrace_switch_finish_locked(struct kernel_task *task,struct kernel_ptrace_notice *notice)
{ (void)notice;assert(!task->ptrace);return 0; }
void kernel_ptrace_switch_notify(struct kernel_ptrace_notice *notice)
{ assert(!notice->tracer && !notice->child && !notice->waiters.queue); }
void kernel_ptrace_dispose(struct kernel_task *task)
{ assert(!task->ptrace); }
void kernel_namespace_put(struct kernel_namespace *ns)
{ assert(!ns); }
int kernel_signal_has_pending(const struct kernel_task *task)
{ (void)task;__builtin_trap(); /* These kernel actors have no signal state. */ }
enum kernel_signal_status kernel_signal_release_table(struct kernel_task *task)
{ (void)task;__builtin_trap(); }
void __real_kernel_raw_lock_acquire(struct kernel_raw_lock *, struct kernel_raw_guard *);
void __wrap_kernel_raw_lock_acquire(struct kernel_raw_lock *lock, struct kernel_raw_guard *guard)
{
    lock_calls++;
    __real_kernel_raw_lock_acquire(lock, guard);
}
void __wrap_kernel_wait_backend_quiesce(void)
{
    struct kernel_wait_queue *queue=join_borrow_queue;
    assert(queue && queue->borrows==1 && join_cursor.queue==queue && !join_cursor.node);
    assert(!cpu.raw_locks && !cpu.preempt_depth);
    join_borrow_queue=0;
    /* The machine scheduling seam lets the remote cursor finish; join must
     * retain its queue owner until this real borrow has been returned. */
    kernel_wait_cursor_end(&join_cursor);
    assert(!queue->borrows && !join_cursor.queue);
    join_borrow_returned++;
}
uint64_t riscv_sv39_current_satp(void) { return 0; }
static unsigned stack_releases, metadata_owned, stack_owned;
static unsigned char metadata_arena[3 * BOAROS_PAGE_SIZE] __attribute__((aligned(BOAROS_PAGE_SIZE)));
static unsigned char stack_arena[6 * BOAROS_PAGE_SIZE] __attribute__((aligned(KERNEL_STACK_BYTES)));
enum physical_page_status physical_page_allocate(struct physical_page_allocator *a, uint64_t *address)
{ (void)a; assert(!metadata_owned); metadata_owned = 1; *address = 10 * BOAROS_PAGE_SIZE; return PHYSICAL_PAGE_STATUS_OK; }
enum physical_page_status physical_page_allocate_order(struct physical_page_allocator *a, uint32_t order, uint64_t *address)
{ (void)a; assert(metadata_owned && !stack_owned && order == KERNEL_STACK_ORDER); stack_owned = 1; *address = 12 * BOAROS_PAGE_SIZE; return PHYSICAL_PAGE_STATUS_OK; }
enum physical_page_status physical_page_map_owned(const struct physical_page_allocator *a, uint64_t address, void **pointer)
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

static unsigned switches, park_handoff;
static struct kernel_task *selected;
void *sync_test_cpu(void) { cpu_queries++; return &cpu; }
void *sync_test_current_task(void) { return cpu.current; }
void sync_test_current_task_set(void *task) { cpu.current = task; }
uintptr_t sync_test_stack(void) { return cpu.current->stack_high - 16; }
uint64_t kernel_time_monotonic_ns(void) { return 1; }
uint64_t kernel_time_raw_ns(void) { return now; }
uint64_t kernel_time_ticks_to_ns(uint64_t ticks) { return ticks; }
uint32_t kernel_time_is_initialized(void) { return 1; }
enum kernel_mm_status kernel_mm_resolve_owned_user_fault(struct kernel_mm *mm, struct kernel_task *task,
                                                   uint64_t address, uint32_t access)
{ (void)mm; (void)task; (void)address; (void)access; __builtin_trap(); /* Rejected kernel caller cannot enter MM. */ }
uint64_t riscv_time_read(void) { return now; }
enum riscv_timer_status riscv_timer_set_scheduler_deadline(uint64_t deadline)
{ assert(!cpu.raw_locks);timer_request=deadline;return RISCV_TIMER_STATUS_NOT_STARTED; }
enum kernel_time_status kernel_time_deadline_from_monotonic(uint64_t ns, uint64_t *ticks)
{ *ticks = ns; return KERNEL_TIME_STATUS_OK; }
enum kernel_time_status kernel_time_deadline_from_raw(uint64_t ns,uint64_t *ticks)
{ return kernel_time_deadline_from_monotonic(ns,ticks); }
enum kernel_time_status kernel_time_deadline_from_realtime(uint64_t ns,uint64_t *ticks)
{ return kernel_time_deadline_from_monotonic(ns,ticks); }

enum kernel_scheduler_status __wrap_scheduler_switch_claimed(struct kernel_task *previous, struct kernel_task *next)
{
    assert(previous != cpu.current && next == cpu.current &&
           !cpu.raw_locks && !cpu.preempt_depth);
    switches++;
    selected = next;
    if (park_handoff) {
        assert(previous==&running && next==&peer);
        assert(running.state==KERNEL_THREAD_STATE_BLOCKED && running.wait.on_cpu);
        struct kernel_wait_token token={&running,kernel_wait_record_generation(&running.wait)};
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        assert(kernel_wait_notify_locked(&token,KERNEL_WAIT_WOKEN));
        assert(running.wait.ready_pending && !running.ready_node.queued);
        kernel_wait_switch_finish_locked(&running);cpu.switch_previous=0;
        peer.state=KERNEL_THREAD_STATE_BLOCKED;
        assert(scheduler_select_next_locked(&cpu,&peer,1)==&running);
        kernel_wait_switch_finish_locked(&peer);cpu.switch_previous=0;
        assert(running.wait.on_cpu && !running.ready_node.queued);
    }
    return KERNEL_SCHEDULER_STATUS_OK;
}
enum kernel_mm_status kernel_mm_activate_for_task(const struct kernel_mm *mm,uint64_t context)
{ (void)mm;(void)context;assert(park_handoff && !cpu.raw_locks);return KERNEL_MM_STATUS_OK; }
void riscv_fpu_switch(struct riscv_fpu_state *previous,struct riscv_fpu_state *next)
{
    assert(park_handoff && previous==&running.fpu && next==&peer.fpu);
}
void riscv_context_switch(struct riscv_switch_context *previous,
    const struct riscv_switch_context *next)
{
    assert(previous==&running.context && next==&peer.context);
    /* Runtime accounting has its own real domain hold after the MM/FPU seam. */
    assert(lock_calls==3);
    assert(!__wrap_scheduler_switch_claimed(&running,&peer));
}
static void setup(int policy, int priority, int next_priority)
{
    now = 1;
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

static void fair_slice_case(uint32_t slice)
{
    setup(KERNEL_SCHED_OTHER,0,0);
    running.scheduling.fair_slice_ns=slice;
    running.scheduling.fair_slice_used=slice-1;
    assert(!scheduler_reschedule(0,0,1) && !switches && timer_request==2);
    running.scheduling.fair_slice_used++;
    assert(!scheduler_reschedule(0,0,1) && switches==1 && selected==&peer);
    assert(!running.scheduling.fair_slice_used);
}

static void ready_wait_case(int nice, int policy, unsigned queued, int should_preempt)
{
    setup(KERNEL_SCHED_OTHER, 0, 0);
    kernel_sched_dequeue(&scheduler.runqueue, &peer.ready_node);
    running.arch.user_mode = peer.arch.user_mode = 1;
    peer.accounted = 1;
    peer.scheduling.nice = nice;
    peer.scheduling.policy = policy;
    peer.ready_node.fair.key = 6000000;
    peer.fair_queue = &scheduler.runqueue;
    ready_append(&peer);
    static struct kernel_task competitors[9];
    for (unsigned i=0;i<queued;i++) {
        struct kernel_task *competitor=&competitors[i];
        *competitor=(struct kernel_task){.accounted=1,.arch.user_mode=1,
            .state=KERNEL_THREAD_STATE_READY,.fair_queue=&scheduler.runqueue};
        competitor->ready_node.fair.key=7000000+i;
        ready_append(competitor);
    }

    assert(!scheduler_reschedule(0,0,0) && !switches);
    assert(timer_request == (should_preempt ? 2000001 : 10000001));
    now = 2000001;
    running.usage.user_ns = 2000000;
    assert(!scheduler_reschedule(0,0,0));
    assert(switches == (unsigned)should_preempt);
    if (should_preempt) assert(selected == &peer);
}

static void fair_peer_wake_case(int nice, unsigned competitors, uint32_t slice)
{
    setup(KERNEL_SCHED_OTHER,0,0);
    kernel_sched_dequeue(&scheduler.runqueue,&peer.ready_node);
    running.arch.user_mode=peer.arch.user_mode=1;
    running.ready_node.fair.key=2*KERNEL_SCHED_WAKE_NS;
    running.scheduling.fair_slice_ns=slice;
    peer.accounted=1;
    peer.scheduling.nice=nice;
    ready_append(&peer);
    static struct kernel_task queued[9];
    for (unsigned i=0;i<competitors;i++) {
        queued[i]=(struct kernel_task){.accounted=1,.arch.user_mode=1,
            .state=KERNEL_THREAD_STATE_READY,.fair_queue=&scheduler.runqueue};
        queued[i].ready_node.fair.key=3*KERNEL_SCHED_WAKE_NS+i;
        ready_append(&queued[i]);
    }

    assert(!scheduler_reschedule(0,0,0));
    if (!nice && !competitors) {
        assert(!switches && cpu.current==&running);
        assert(timer_request==1+(slice ? slice : 2*KERNEL_SCHED_WAKE_NS));
        uint64_t elapsed=slice ? slice : KERNEL_SCHED_WAKE_NS;
        now=1+elapsed;
        running.usage.user_ns=elapsed;
        assert(!scheduler_reschedule(0,0,0));
    }
    assert(switches==1 && selected==&peer);
}

static void deferred_slice_case(void)
{
    for (unsigned rr=0;rr<2;rr++) {
        setup(rr?KERNEL_SCHED_RR:KERNEL_SCHED_OTHER,rr?50:0,rr?50:0);
        running.scheduling.fair_slice_used=100000000;
        running.scheduling.rr_remaining_ns=0;
        kernel_preempt_disable();
        for (unsigned irq=0;irq<4;irq++) {
            assert(!scheduler_reschedule(0,0,1));
            assert(!timer_request && !switches && cpu.current==&running);
            assert(running.scheduling.fair_slice_used==100000000 &&
                   !running.scheduling.rr_remaining_ns && kernel_cpu_schedule_requested(&cpu));
        }
        /* An unrelated wait deadline must still fire while a task-only
         * qualification delays the actual context switch. */
        { KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
          ready_remove_locked(&peer);peer.wait.deadline=73;deadline_index_insert(&peer); }
        assert(!scheduler_reschedule(0,0,0) && timer_request==73 && !switches);
        { KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
          deadline_index_remove(&peer);peer.wait.deadline=0;ready_enqueue_locked(&peer,0); }
        kernel_preempt_enable();
        assert(!scheduler_reschedule(1,0,1) && switches==1 && selected==&peer);
    }
    puts("deferred expired fair/RR slices preserve external deadlines without immediate-IRQ livelock");
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
static void join_cursor_lifetime_case(void)
{
    setup(KERNEL_SCHED_FIFO,50,10);
    struct kernel_thread_join join={0};
    kernel_wait_queue_init(&join.waiters);
    kernel_wait_cursor_begin(&join.waiters,&join_cursor);
    assert(!join.task && !join_cursor.node && join.waiters.borrows==1);
    join_borrow_returned=0;
    join_borrow_queue=&join.waiters;
    kernel_thread_join(&join);
    assert(join_borrow_returned==1 && !join_borrow_queue && !join_cursor.queue);
    assert(!join.waiters.borrows && !switches && !cpu.raw_locks && !cpu.preempt_depth);
    assert(!kernel_wait_queue_close(&join.waiters) && !kernel_wait_queue_destroy(&join.waiters));
    puts("scheduler handoff: join waits for the detached remote cursor before returning its queue owner");
}
int main(int argc,char **argv)
{
    deferred_slice_case();
    fair_slice_case(100000);fair_slice_case(30000000);fair_slice_case(100000000);
    ready_wait_case(0,KERNEL_SCHED_OTHER,1,1);
    ready_wait_case(0,KERNEL_SCHED_OTHER,8,1);
    ready_wait_case(0,KERNEL_SCHED_OTHER,9,0);
    ready_wait_case(5,KERNEL_SCHED_OTHER,0,0);
    ready_wait_case(0,KERNEL_SCHED_BATCH,0,0);
    puts("ready wait: short same-nice queues preempt earlier; crowded queues and BATCH keep ordinary timeslices");
    fair_peer_wake_case(0,0,0);
    fair_peer_wake_case(0,0,100000);
    fair_peer_wake_case(-1,0,0);
    fair_peer_wake_case(0,9,0);
    puts("wake preemption: short equal-weight execution, sub-ms slice, higher weight and a crowded queue");
    setup(KERNEL_SCHED_OTHER,0,0);
    running.scheduling.fair_slice_ns=100000;running.scheduling.fair_slice_used=5000000;
    scheduler_rearm_timer();assert(timer_request==2);
    assert(!scheduler_reschedule(0,0,1) && switches==1 && selected==&peer);
    puts("fair slice: short/long request rotation and overdue shortened deadline passed");
    if(argc==2&&!strcmp(argv[1],"--slice-only"))return 0;
    setup(KERNEL_SCHED_OTHER,0,0);
    struct kernel_wait_token token;
    enum kernel_wait_wake_reason reason;
    assert(!kernel_wait_prepare(0,0,0,&token));
    lock_calls=0;park_handoff=1;
    assert(!kernel_wait_park(&token,&reason) && reason==KERNEL_WAIT_WOKEN);
    park_handoff=0;
    assert(!kernel_wait_finish(&token));
    puts("UP park preserves commit/select/accounting; early wake waits for actual old-stack handoff");
    for (int policy = KERNEL_SCHED_FIFO; policy <= KERNEL_SCHED_RR; policy++) {
        rotate_case(policy, 10, 0);
        rotate_case(policy, 50, 1);
        rotate_case(policy, 60, 1);
    }
    setup(KERNEL_SCHED_FIFO, 50, 60);
    assert(scheduler_reschedule(0, 0, 1) == KERNEL_SCHEDULER_STATUS_OK);
    assert(switches == 1 && selected == &peer && running.wait.ready_flags == 1);
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
#if BOAROS_LOCK_DEBUG
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
#endif
    clear_owned_page_case();
    private_storage_case();
    join_cursor_lifetime_case();
    const unsigned bytes = KERNEL_STACK_BYTES - KERNEL_STACK_GUARD_BYTES;
    for (unsigned prefix = 0; prefix <= bytes; prefix++) stack_prefix_case(prefix, 0);
    stack_prefix_case(0, 1); stack_prefix_case(bytes, 1);
    puts("scheduler handoff: FIFO/RR lower/equal/higher, exact stack prefix and canary passed");
}
