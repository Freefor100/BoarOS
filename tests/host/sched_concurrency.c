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
int kernel_wait_backend_initialized(void) { return 1; }
struct kernel_task *kernel_wait_current_task(void) { return active_cpu->current; }
struct kernel_wait_node *kernel_wait_default_node(struct kernel_task *task)
{ return &task->default_wait_node; }
void kernel_wait_backend_notify(struct kernel_task *task, uint32_t reason)
{ (void)task; (void)reason; }
void kernel_wait_backend_quiesce(void) { __builtin_trap(); }

static unsigned disposals, notifications;
static struct kernel_task *disposal_target;
enum physical_page_status physical_page_release(struct physical_page_allocator *a, uint64_t address)
{
    (void)a;
    assert(!active_cpu->raw_locks);
    if (disposal_target) assert(address == disposal_target->physical_address && !disposal_target->storage_borrows);
    else {
        assert(address >= 4096 && address <= 3*4096);
        struct kernel_task *task = &tasks[address/4096 - 1];
        assert(!task->storage_borrows && task->retirement == 3);
    }
    disposals++;
    return PHYSICAL_PAGE_STATUS_OK;
}
void scheduler_task_disposal_notify(void)
{ assert(!active_cpu->raw_locks); __atomic_fetch_add(&notifications, 1, __ATOMIC_RELAXED); }
static void lifetime_generation(void)
{
    scheduler = (struct kernel_scheduler){0};
    tasks[0] = tasks[1] = (struct kernel_task){0};
    kernel_cpu_initialize(&cpus[0], 7, &tasks[0].io_context);
    active_cpu = &cpus[0];
    kernel_raw_lock_init(&kernel_wait_domain, KERNEL_RAW_RANK_SCHEDULER);
    assert(kernel_pid_allocator_init(&scheduler.pid_allocator, scheduler.pid_bitmap, 1) == KERNEL_PID_STATUS_OK);
    scheduler.identities.numbers = &scheduler.pid_allocator;
    scheduler.identities.next_generation = 1;
    struct kernel_pid id = {0}, replacement = {0};
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        assert(kernel_pid_reserve(&scheduler.identities, &id) == KERNEL_PID_STATUS_OK);
        kernel_pid_attach(&tasks[0].identities[KERNEL_PID_TID], &id, KERNEL_PID_TID, &tasks[0]);
        kernel_pid_put(&id);
        assert(!scheduler_task_lookup_locked(id.number, KERNEL_PID_TID, 0));
        scheduler_task_publish_locked(&tasks[0]);
    }
    uint64_t generation = id.generation;
    struct kernel_task *borrow = scheduler_task_lookup(1, KERNEL_PID_TID, generation);
    assert(borrow == &tasks[0] && borrow->storage_borrows == 1);
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        process_identity_release_locked(&tasks[0]);
        scheduler_task_unlink_locked(&tasks[0]);
    }
    assert(!scheduler_task_lookup(1, KERNEL_PID_TID, generation));
    tasks[0].stack_physical_address = KERNEL_THREAD_NO_PAGE;
    tasks[0].physical_address = 4096;
    tasks[0].reaping = 1; /* selected wait event still owns its completion */
    scheduler_task_retire(&tasks[0]);
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        scheduler_task_claim_release_locked(&tasks[0]);
    }
    disposal_target = &tasks[0];
    disposals = notifications = 0;
    scheduler_task_collect();
    assert(!disposals && scheduler.retired_pending == 1);
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        assert(kernel_pid_reserve(&scheduler.identities, &replacement) == KERNEL_PID_STATUS_OK);
        kernel_pid_attach(&tasks[1].identities[KERNEL_PID_TID], &replacement, KERNEL_PID_TID, &tasks[1]);
        kernel_pid_put(&replacement);
        scheduler_task_publish_locked(&tasks[1]);
    }
    assert(!scheduler_task_lookup(1, KERNEL_PID_TID, generation));
    struct kernel_task *fresh = scheduler_task_lookup(1, KERNEL_PID_TID, replacement.generation);
    assert(fresh == &tasks[1]);
    scheduler_task_put(fresh);
    scheduler_task_put(borrow);
    assert(notifications == 0 && disposals == 1);
    scheduler_task_collect();
    assert(disposals == 1 && !scheduler.retired_pending && !scheduler.retired_tasks);
    puts("scheduler reserved identity, lookup borrow, retirement and generation passed");
}

static void retired_cursor_chain(void)
{
    scheduler = (struct kernel_scheduler){0};
    kernel_cpu_initialize(&cpus[0], 7, 0);
    active_cpu = &cpus[0];
    kernel_raw_lock_init(&kernel_wait_domain, KERNEL_RAW_RANK_SCHEDULER);
    for (unsigned i = 0; i < 3; i++) {
        tasks[i] = (struct kernel_task){.physical_address = (i+1)*4096,
                                     .stack_physical_address = KERNEL_THREAD_NO_PAGE};
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        scheduler_task_publish_locked(&tasks[i]);
    }
    struct scheduler_task_cursor cursor;
    scheduler_task_cursor_begin(&cursor);
    assert(cursor.task == &tasks[2]);
    for (unsigned i = 3; i-- > 0;) {
        {
            KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
            scheduler_task_unlink_locked(&tasks[i]);
        }
        scheduler_task_retire(&tasks[i]);
    }
    disposal_target = 0;
    disposals = 0;
    scheduler_task_collect();
    assert(!disposals && scheduler.retired_pending == 3);
    for (unsigned i = 3; i-- > 0;) {
        assert(cursor.task == &tasks[i]);
        scheduler_task_cursor_next(&cursor);
    }
    assert(!cursor.task);
    scheduler_task_collect();
    assert(disposals == 3 && !scheduler.retired_pending && !scheduler.task_count);
}

static void event_claim_handoff(void)
{
    scheduler = (struct kernel_scheduler){0};
    tasks[0] = tasks[1] = tasks[2] = (struct kernel_task){0};
    kernel_cpu_initialize(&cpus[0], 7, 0);
    active_cpu = &cpus[0];
    sync_test_irq = 0;
    kernel_raw_lock_init(&kernel_wait_domain, KERNEL_RAW_RANK_SCHEDULER);
    kernel_wait_queue_init(&scheduler.cleanup_queue);
    kernel_wait_queue_init(&tasks[0].child_exit_queue);
    struct kernel_task *child = &tasks[1], *waiter = &tasks[2];
    child->parent = &tasks[0];
    child->reaping = 1; /* first wait4 owns an unconsumed STOP/CONT report */
    active_cpu->current = waiter;
    struct kernel_wait_token parent_wait, cleanup_wait;
    assert(kernel_wait_prepare(&tasks[0].child_exit_queue, 0, 0, &parent_wait) == KERNEL_SCHEDULER_STATUS_OK);
    assert(waiter->wait.phase == KERNEL_WAIT_PREPARED && tasks[0].child_exit_queue.registrations == 1);
    /* While the first copy sleeps, exit is published and the worker registers. */
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        child->state = KERNEL_THREAD_STATE_EXITED;
        exited_append_locked(child);
        assert(!scheduler_cleanup_ready_locked());
        assert(scheduler_task_claim_exit_locked(child) == KERNEL_SCHEDULER_STATUS_BUSY);
    }
    active_cpu->current = &idles[0];
    idles[0] = (struct kernel_task){0};
    assert(kernel_wait_prepare(&scheduler.cleanup_queue, 0, 0, &cleanup_wait) == KERNEL_SCHEDULER_STATUS_OK);
    /* EFAULT releases only the claim; both actual registrations are notified. */
    scheduler_task_wait_claim_release(child);
    assert(waiter->wait.phase == KERNEL_WAIT_NOTIFIED && idles[0].wait.phase == KERNEL_WAIT_NOTIFIED);
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        assert(scheduler_cleanup_ready_locked());
        assert(scheduler_task_claim_exit_locked(child) == KERNEL_SCHEDULER_STATUS_OK);
        exited_remove_locked(child);
        scheduler_task_claim_release_locked(child);
    }
    assert(kernel_wait_finish(&cleanup_wait) == KERNEL_SCHEDULER_STATUS_OK);
    active_cpu->current = waiter;
    assert(kernel_wait_finish(&parent_wait) == KERNEL_SCHEDULER_STATUS_OK);
    assert(!scheduler.cleanup_queue.registrations && !tasks[0].child_exit_queue.registrations);
    assert(!waiter->wait.borrows && !tasks[0].storage_borrows);
    puts("wait-event claim release hands off to registered waiter and reaper");
}

static struct kernel_task *lifetime_borrow[4];
static uint64_t lookup_generation;
static void *lookup_actor(void *argument)
{
    unsigned id = (unsigned)(uintptr_t)argument;
    active_cpu = &cpus[id];
    sync_test_irq = 1;
    lifetime_borrow[id] = scheduler_task_lookup(1, KERNEL_PID_TID, lookup_generation);
    assert(lifetime_borrow[id] == &tasks[0]);
    pthread_barrier_wait(&start); /* every reader actually owns its borrow */
    pthread_barrier_wait(&start); /* retirement has stopped discovery */
    assert(!scheduler_task_lookup(1, KERNEL_PID_TID, lookup_generation));
    scheduler_task_put(lifetime_borrow[id]);
    return 0;
}
static void concurrent_retirement(unsigned count)
{
    scheduler = (struct kernel_scheduler){0};
    tasks[0] = (struct kernel_task){0};
    for (unsigned i = 0; i <= count; i++) {
        struct kernel_cpu *cpu = i == count ? &cpus[0] : &cpus[i];
        if (i == count) break;
        kernel_cpu_initialize(cpu, 7 + i * 6, 0);
    }
    active_cpu = &cpus[0];
    kernel_raw_lock_init(&kernel_wait_domain, KERNEL_RAW_RANK_SCHEDULER);
    assert(kernel_pid_allocator_init(&scheduler.pid_allocator, scheduler.pid_bitmap, 1) == KERNEL_PID_STATUS_OK);
    scheduler.identities.numbers = &scheduler.pid_allocator;
    scheduler.identities.next_generation = 1;
    struct kernel_pid id = {0};
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        assert(kernel_pid_reserve(&scheduler.identities, &id) == KERNEL_PID_STATUS_OK);
        kernel_pid_attach(&tasks[0].identities[KERNEL_PID_TID], &id, KERNEL_PID_TID, &tasks[0]);
        kernel_pid_put(&id);
        scheduler_task_publish_locked(&tasks[0]);
    }
    lookup_generation = id.generation;
    pthread_t threads[4];
    assert(!pthread_barrier_init(&start, 0, count + 1));
    for (unsigned i = 0; i < count; i++) assert(!pthread_create(&threads[i], 0, lookup_actor, (void *)(uintptr_t)i));
    /* Main uses a separate CPU record: no two host actors share an owner chain. */
    struct kernel_cpu retiring_cpu;
    kernel_cpu_initialize(&retiring_cpu, 99, 0);
    active_cpu = &retiring_cpu;
    pthread_barrier_wait(&start);
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        assert(tasks[0].storage_borrows == count);
        process_identity_release_locked(&tasks[0]);
        scheduler_task_unlink_locked(&tasks[0]);
    }
    tasks[0].stack_physical_address = KERNEL_THREAD_NO_PAGE;
    tasks[0].physical_address = 4096;
    disposal_target = &tasks[0];
    disposals = 0;
    scheduler_task_retire(&tasks[0]);
    scheduler_task_collect();
    assert(!disposals);
    pthread_barrier_wait(&start);
    for (unsigned i = 0; i < count; i++) assert(!pthread_join(threads[i], 0));
    scheduler_task_collect();
    assert(disposals == 1 && !scheduler.retired_pending && !scheduler.all_tasks);
    assert(!pthread_barrier_destroy(&start));
}

static struct kernel_thread_join exit_join;
static unsigned exit_claims[4];
static void *exit_actor(void *argument)
{
    unsigned id = (unsigned)(uintptr_t)argument;
    active_cpu = &cpus[id];
    sync_test_irq = 1;
    pthread_barrier_wait(&start);
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    struct kernel_task *owner;
    enum kernel_scheduler_status status = id & 1
        ? scheduler_join_claim_locked(&exit_join, &owner)
        : scheduler_task_claim_exit_locked(&tasks[0]);
    exit_claims[id] = status == KERNEL_SCHEDULER_STATUS_OK;
    return 0;
}
static void concurrent_exit_claim(unsigned count)
{
    scheduler = (struct kernel_scheduler){0};
    tasks[0] = (struct kernel_task){0};
    exit_join = (struct kernel_thread_join){.task = &tasks[0]};
    for (unsigned i = 0; i < count; i++) kernel_cpu_initialize(&cpus[i], 7 + 6*i, 0);
    active_cpu = &cpus[0];
    kernel_raw_lock_init(&kernel_wait_domain, KERNEL_RAW_RANK_SCHEDULER);
    tasks[0].state = KERNEL_THREAD_STATE_EXITED;
    tasks[0].join = &exit_join;
    tasks[0].wait.on_cpu = 1;
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        exited_append_locked(&tasks[0]);
        assert(scheduler_task_claim_exit_locked(&tasks[0]) == KERNEL_SCHEDULER_STATUS_BUSY);
        struct kernel_task *owner;
        assert(scheduler_join_claim_locked(&exit_join, &owner) == KERNEL_SCHEDULER_STATUS_BUSY && !owner);
        tasks[0].wait.on_cpu = 0; /* actual B2 finish tested separately */
    }
    assert(!pthread_barrier_init(&start, 0, count));
    pthread_t threads[4];
    for (unsigned i = 0; i < count; i++) assert(!pthread_create(&threads[i], 0, exit_actor, (void *)(uintptr_t)i));
    unsigned claims = 0;
    for (unsigned i = 0; i < count; i++) { assert(!pthread_join(threads[i], 0)); claims += exit_claims[i]; }
    assert(claims == 1 && tasks[0].reaping);
    assert(!pthread_barrier_destroy(&start));
}

static void complete_machine_handoff(struct kernel_cpu *);
static void *policy_selection_actor(void *argument)
{
    unsigned id = (unsigned)(uintptr_t)argument;
    active_cpu = &cpus[id];
    sync_test_irq = 1;
    pthread_barrier_wait(&start);
    if (!id) {
        check_policy_domain = 1;
        assert(!kernel_task_sched_set(&tasks[0], 1, KERNEL_SCHED_FIFO, 70, 0));
        check_policy_domain = 0;
    } else {
        {
            KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
            scheduler_account_runtime_locked();
            assert(scheduler_select_next_locked(active_cpu, active_cpu->current, 1) == &tasks[0]);
        }
        complete_machine_handoff(active_cpu);
    }
    return 0;
}
static void policy_selection_race(void)
{
    scheduler = (struct kernel_scheduler){0};
    tasks[0] = idles[0] = idles[1] = (struct kernel_task){0};
    kernel_raw_lock_init(&kernel_wait_domain, KERNEL_RAW_RANK_SCHEDULER);
    scheduler.initialized = KERNEL_SCHEDULER_INITIALIZED;
    scheduler.rt_configuration = (struct kernel_rt_configuration){UINT64_C(1000000000), INT64_C(950000000)};
    for (unsigned i = 0; i < 2; i++) {
        kernel_cpu_initialize(&cpus[i], 7 + 6*i, &idles[i].io_context);
        active_cpu = &cpus[i];
        scheduler_cpu_initialize(active_cpu, &idles[i], 0);
        active_cpu->current = &idles[i];
        idles[i].wait.on_cpu = 1;
        idles[i].state = KERNEL_THREAD_STATE_IDLE;
    }
    active_cpu = &cpus[0];
    now = 1;
    assert(kernel_pid_allocator_init(&scheduler.pid_allocator, scheduler.pid_bitmap, 1) == KERNEL_PID_STATUS_OK);
    scheduler.identities.numbers = &scheduler.pid_allocator;
    scheduler.identities.next_generation = 1;
    struct kernel_pid identity = {0};
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        assert(kernel_pid_reserve(&scheduler.identities, &identity) == KERNEL_PID_STATUS_OK);
        kernel_pid_attach(&tasks[0].identities[KERNEL_PID_TID], &identity, KERNEL_PID_TID, &tasks[0]);
        kernel_pid_put(&identity);
        process_identity_sync_locked(&tasks[0]);
    }
    tasks[0].arch.user_mode = 1;
    tasks[0].state = KERNEL_THREAD_STATE_READY;
    assert(!kernel_sched_policy_set(&tasks[0].scheduling, KERNEL_SCHED_FIFO, 50, 0));
    ready_append(&tasks[0]);
    assert(!pthread_barrier_init(&start, 0, 2));
    pthread_t threads[2];
    for (unsigned i = 0; i < 2; i++) assert(!pthread_create(&threads[i], 0, policy_selection_actor, (void *)(uintptr_t)i));
    for (unsigned i = 0; i < 2; i++) assert(!pthread_join(threads[i], 0));
    assert(tasks[0].scheduling.priority == 70 && tasks[0].wait.on_cpu &&
           tasks[0].cpu == &cpus[1] && !tasks[0].ready_node.queued);
    assert(!pthread_barrier_destroy(&start));
}

static unsigned group_actor_count;
static void *group_cursor_actor(void *argument)
{
    unsigned id = (unsigned)(uintptr_t)argument;
    active_cpu = &cpus[id];
    sync_test_irq = 1;
    struct scheduler_group_cursor cursor = {0};
    if (id + 1 != group_actor_count) {
        scheduler_group_cursor_begin(&tasks[0], &cursor);
        assert(cursor.task == &tasks[0]);
        scheduler_group_cursor_next(&cursor);
        assert(cursor.task == &tasks[1]);
    }
    pthread_barrier_wait(&start);
    if (id + 1 == group_actor_count) {
        {
            KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
            assert(tasks[1].storage_borrows == group_actor_count - 1);
            tasks[0].group_next = &tasks[2];
            tasks[2].group_previous = &tasks[0];
            tasks[0].group_epoch++;
            tasks[0].group_members--;
            tasks[1].group_leader = tasks[1].group_next = tasks[1].group_previous = &tasks[1];
            scheduler_task_unlink_locked(&tasks[1]);
        }
        scheduler_task_retire(&tasks[1]);
        scheduler_task_collect();
        assert(!disposals);
    }
    pthread_barrier_wait(&start);
    if (id + 1 != group_actor_count) {
        scheduler_group_cursor_next(&cursor);
        assert(cursor.task == &tasks[0]); /* protected epoch restart */
        scheduler_group_cursor_next(&cursor);
        assert(cursor.task == &tasks[2]);
        scheduler_group_cursor_next(&cursor);
        assert(!cursor.task);
        scheduler_group_cursor_end(&cursor);
    }
    return 0;
}
static void concurrent_group_retirement(unsigned count)
{
    scheduler = (struct kernel_scheduler){0};
    for (unsigned i = 0; i < count; i++) kernel_cpu_initialize(&cpus[i], 7 + i*6, 0);
    active_cpu = &cpus[0];
    kernel_raw_lock_init(&kernel_wait_domain, KERNEL_RAW_RANK_SCHEDULER);
    for (unsigned i = 0; i < 3; i++) {
        tasks[i] = (struct kernel_task){.physical_address = (i+1)*4096,
                                      .stack_physical_address = KERNEL_THREAD_NO_PAGE};
        tasks[i].group_leader = &tasks[0];
        tasks[i].group_next = &tasks[(i+1)%3];
        tasks[i].group_previous = &tasks[(i+2)%3];
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        scheduler_task_publish_locked(&tasks[i]);
    }
    tasks[0].group_members = 3;
    group_actor_count = count;
    disposals = 0;
    disposal_target = &tasks[1];
    assert(!pthread_barrier_init(&start, 0, count));
    pthread_t threads[4];
    for (unsigned i = 0; i < count; i++) assert(!pthread_create(&threads[i], 0, group_cursor_actor, (void *)(uintptr_t)i));
    for (unsigned i = 0; i < count; i++) assert(!pthread_join(threads[i], 0));
    scheduler_task_collect();
    assert(disposals == 1 && !tasks[0].storage_borrows && !tasks[2].storage_borrows && !scheduler.retired_pending);
    assert(!pthread_barrier_destroy(&start));
}

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
    lifetime_generation();
    retired_cursor_chain();
    event_claim_handoff();
    concurrent_retirement(2);
    concurrent_retirement(4);
    concurrent_exit_claim(2);
    concurrent_exit_claim(4);
    policy_selection_race();
    concurrent_group_retirement(2);
    concurrent_group_retirement(4);
    independent_runtime();
    for (unsigned count = 2; count <= 4; count += 2)
        for (unsigned iteration = 0; iteration < 64; iteration++) concurrent_claim(count);
    puts("scheduler 2/4-CPU atomic selection and execution-owner handoff passed");
    return 0;
}
