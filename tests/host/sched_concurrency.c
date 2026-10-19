#include "../../kernel/sched/private.h"
#include <arch/timer.h>
#include <kernel/time.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <sched.h>

struct kernel_scheduler scheduler;
_Thread_local uintptr_t sync_test_irq;
static _Thread_local struct kernel_cpu *active_cpu;
static uint64_t now;
static _Thread_local uint64_t accounting_sample_time;
static _Thread_local int check_policy_domain;
static struct kernel_cpu cpus[4];
static struct kernel_task tasks[4], idles[4];
static pthread_barrier_t start;
static struct kernel_task *selected[4];
void *sync_test_cpu(void) { return active_cpu; }
void *sync_test_current_task(void) { return active_cpu->current; }
static _Thread_local unsigned group_lock_calls;
static _Thread_local unsigned cleanup_domain_entry, complete_disposal_at;
static void complete_held_disposal(void);
static unsigned accounting_progress_check,accounting_lock_attempt,accounting_finished,accounting_go;
void signal_owner_cases(unsigned *lock_calls);
void __real_kernel_raw_lock_acquire(struct kernel_raw_lock *, struct kernel_raw_guard *);
void __wrap_kernel_raw_lock_acquire(struct kernel_raw_lock *lock, struct kernel_raw_guard *guard)
{
    group_lock_calls++;
    if (__atomic_load_n(&accounting_progress_check,__ATOMIC_ACQUIRE) &&
        active_cpu==&cpus[1] && lock==&kernel_wait_domain)
        __atomic_store_n(&accounting_lock_attempt,1,__ATOMIC_RELEASE);
    if (complete_disposal_at && ++cleanup_domain_entry == complete_disposal_at) {
        assert(lock == &kernel_wait_domain && !active_cpu->raw_locks);
        assert(scheduler.cleanup_queue.registrations == (complete_disposal_at > 2));
        complete_disposal_at = 0;
        complete_held_disposal();
    }
    __real_kernel_raw_lock_acquire(lock, guard);
}
/* STOPPED state removal is covered by real guest signals; this READY-only
 * group cost model must never take that separate state transition. */
void stopped_unlink_locked(struct kernel_task *task) { (void)task; __builtin_trap(); }
uint64_t kernel_time_monotonic_ns(void)
{ return accounting_sample_time=__atomic_load_n(&now,__ATOMIC_RELAXED); }
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
uint64_t riscv_time_read(void)
{ return accounting_sample_time=__atomic_load_n(&now,__ATOMIC_RELAXED); }
uint64_t kernel_time_ticks_to_ns(uint64_t ticks) { return ticks; }
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
static unsigned cleanup_parks;
static int allow_cleanup_park;
int kernel_wait_backend_signal(struct kernel_task *task) { (void)task; return 0; }
uint64_t kernel_wait_backend_time(void) { return now; }
void kernel_wait_backend_commit(struct kernel_task *task, uint64_t deadline, int interruptible)
{
    (void)deadline; (void)interruptible;
    assert(task == scheduler.cleanup_task && allow_cleanup_park);
    cleanup_parks++;
}
enum kernel_scheduler_status kernel_wait_backend_switch(struct kernel_task *task)
{
    /* The real prepare/commit/finish path is linked here. Actual old/new stack
     * handoff has its separate host and guest tests; this shim supplies a wake. */
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    struct kernel_wait_token token = {task, task->wait.generation};
    assert(kernel_wait_notify_locked(&token, KERNEL_WAIT_WOKEN));
    return KERNEL_SCHEDULER_STATUS_OK;
}

static unsigned disposals, notifications;
static struct kernel_task *disposal_target;
static int hold_disposal;
static unsigned held_collectors;
static pthread_t collectors[3];
static pthread_barrier_t disposal_entered, disposal_continue;
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
    if (hold_disposal) {
        pthread_barrier_wait(&disposal_entered);
        pthread_barrier_wait(&disposal_continue);
    }
    __atomic_fetch_add(&disposals, 1, __ATOMIC_RELAXED);
    return PHYSICAL_PAGE_STATUS_OK;
}
void scheduler_task_disposal_notify(void)
{
    assert(!active_cpu->raw_locks);
    __atomic_fetch_add(&notifications, 1, __ATOMIC_RELAXED);
    if (scheduler.cleanup_queue.initialized == KERNEL_WAIT_QUEUE_INITIALIZED)
        assert(kernel_wait_queue_wake_all(&scheduler.cleanup_queue) == KERNEL_SCHEDULER_STATUS_OK);
}
static void *collect_held_metadata(void *argument)
{
    active_cpu=&cpus[(uintptr_t)argument];
    scheduler_task_collect();
    return 0;
}
static void complete_held_disposal(void)
{
    pthread_barrier_wait(&disposal_continue);
    for (unsigned i = 0; i < held_collectors; i++) assert(!pthread_join(collectors[i], 0));
    hold_disposal = 0;
}
static void shutdown_during_disposal(unsigned threads, unsigned completion_window)
{
    scheduler=(struct kernel_scheduler){0};
    kernel_raw_lock_init(&kernel_wait_domain,KERNEL_RAW_RANK_SCHEDULER);
    for(unsigned i=0;i<threads;i++) {
        kernel_cpu_initialize(&cpus[i],i,0);
        cpus[i].current=&idles[i];
    }
    active_cpu=&cpus[0];disposal_target=0;disposals=notifications=0;
    scheduler.cleanup_task = active_cpu->current;
    scheduler.cleanup_task->wait.on_cpu = 1;
    kernel_wait_queue_init(&scheduler.cleanup_queue);
    for(unsigned i=0;i<threads-1;i++) {
        tasks[i]=(struct kernel_task){.physical_address=(i+1)*4096,
                                     .stack_physical_address=KERNEL_THREAD_NO_PAGE};
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        scheduler_task_unlink_locked(&tasks[i]);
    }
    /* Unlink stops lookup, but the cleanup/retirement owner is still alive. */
    assert(kernel_scheduler_stop_users() == 1);
    for (unsigned i = 0; i < threads - 1; i++) {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        scheduler_task_retire_locked(&tasks[i]);
    }
    pthread_barrier_init(&disposal_entered,0,threads);
    pthread_barrier_init(&disposal_continue,0,threads);
    hold_disposal=1;
    held_collectors = threads - 1;
    for(unsigned i=1;i<threads;i++)assert(!pthread_create(&collectors[i-1],0,collect_held_metadata,(void *)(uintptr_t)i));
    pthread_barrier_wait(&disposal_entered);
    /* Each real collector has claimed a record and reached its page service,
     * but that service has not completed. Empty indices are not quiescence. */
    assert(kernel_scheduler_stop_users()==1);
    assert(!disposals && !notifications);
    /* Force last ack before the first check, before prepare, after prepare
     * before recheck, or between recheck and park. Only real registration and
     * domain entry count control the handshake; no random sleeps are used. */
    cleanup_domain_entry = 0;
    complete_disposal_at = completion_window;
    if (!completion_window) complete_held_disposal();
    sync_test_irq = 0;
    allow_cleanup_park = 0;
    cleanup_parks = 0;
    kernel_scheduler_wait_cleanup(0);
    assert(!complete_disposal_at && disposals==threads-1 && notifications>=1);
    assert(!scheduler.retired_pending && !scheduler.retired_tasks);
    assert(!cleanup_parks && !scheduler.cleanup_queue.registrations &&
           !scheduler.cleanup_queue.borrows);
    /* Retry retains its timed-wait contract even while progress is latched. */
    allow_cleanup_park = 1;
    kernel_scheduler_wait_cleanup(1);
    assert(cleanup_parks == 1);
    assert(kernel_scheduler_stop_users()==0);
    /* A progress notification is not a shutdown verdict. An indexed user
     * owner still makes the next stop/wait cycle pending, even with no retired
     * pages; retry deadlines likewise retain the existing timed wait. */
    tasks[0] = (struct kernel_task){.physical_address = 4096,
                                  .stack_physical_address = KERNEL_THREAD_NO_PAGE,
                                  .arch.user_mode = 1,
                                  .state = KERNEL_THREAD_STATE_ZOMBIE};
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        scheduler_task_publish_locked(&tasks[0]);
    }
    assert(kernel_scheduler_stop_users()==1);
    allow_cleanup_park = 1;
    kernel_scheduler_wait_cleanup(0);
    kernel_scheduler_wait_cleanup(1);
    assert(cleanup_parks == 3 && !scheduler.cleanup_queue.registrations &&
           !scheduler.cleanup_queue.borrows);
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        scheduler_task_unlink_locked(&tasks[0]);
        scheduler_task_retire_locked(&tasks[0]);
    }
    scheduler_task_collect();
    assert(!scheduler.task_count && !scheduler.all_tasks &&
           !scheduler.retired_pending && !scheduler.retired_tasks);
    pthread_barrier_destroy(&disposal_entered);pthread_barrier_destroy(&disposal_continue);
    puts("shutdown waits for completed metadata page disposal");
}
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
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        scheduler_task_retire_locked(&tasks[0]);
    }
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
    group_lock_calls = 0;
    scheduler_task_put(borrow);
    /* Last put, claiming disposal and publishing completed page return stay
     * bounded. The final domain entry acknowledges the real service owner. */
    assert(group_lock_calls <= 3);
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

static void parent_callback(struct kernel_wait_node *node, uint32_t reason)
{
    struct kernel_task *parent = node->context;
    assert(reason == KERNEL_WAIT_WOKEN && parent->storage_borrows == 1);
    assert(parent->child_exit_queue.borrows && !active_cpu->raw_locks);
    kernel_wait_queue_remove(node);
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
    /* Empty and direct task notification stay within their protecting domain;
     * they must not create a cursor/storage borrow that survives the unlock. */
    group_lock_calls = 0;
    wake_waiting_parent(child);
    assert(group_lock_calls <= 1 && waiter->wait.phase == KERNEL_WAIT_NOTIFIED);
    assert(!tasks[0].storage_borrows && !tasks[0].child_exit_queue.borrows);
    assert(kernel_wait_finish(&parent_wait) == KERNEL_SCHEDULER_STATUS_OK);
    group_lock_calls = 0;
    wake_waiting_parent(child);
    assert(group_lock_calls <= 1);
    assert(kernel_wait_prepare(&tasks[0].child_exit_queue, 0, 0, &parent_wait) == KERNEL_SCHEDULER_STATUS_OK);
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
    struct kernel_wait_node callback_node;
    kernel_wait_node_init_callback(&callback_node, parent_callback, &tasks[0]);
    kernel_wait_queue_add(&tasks[0].child_exit_queue, &callback_node);
    wake_waiting_parent(child);
    assert(!tasks[0].storage_borrows && !tasks[0].child_exit_queue.borrows);
    assert(!tasks[0].child_exit_queue.registrations && !callback_node.references);
    puts("wait-event claim release hands off to registered waiter and reaper");
}

static void group_exit_work(void)
{
    static struct kernel_task unrelated[64];
    for (unsigned members = 1; members <= 4; members *= 2) {
        scheduler = (struct kernel_scheduler){0};
        kernel_cpu_initialize(&cpus[0], 7, 0);
        active_cpu = &cpus[0];
        sync_test_irq = 0;
        kernel_raw_lock_init(&kernel_wait_domain, KERNEL_RAW_RANK_SCHEDULER);
        struct kernel_task *leader = &tasks[0];
        for (unsigned i = 0; i < members; i++) {
            tasks[i] = (struct kernel_task){.state = KERNEL_THREAD_STATE_READY, .magic = KERNEL_THREAD_MAGIC, .usage={.user_ns = (i+1)*10000000U, .kernel_ns = 2*(i+1)*10000000U}};
            tasks[i].group_leader = leader;
            tasks[i].group_next = &tasks[(i + 1) % members];
            tasks[i].group_previous = &tasks[(i + members - 1) % members];
            kernel_wait_queue_init(&tasks[i].group_wait_queue);
        }
        leader->group_members = members;
        {
            KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
            for (unsigned i = 0; i < members; i++) scheduler_task_publish_locked(&tasks[i]);
            for (unsigned i = 0; i < 64; i++) {
                unrelated[i] = (struct kernel_task){0};
                scheduler_task_publish_locked(&unrelated[i]);
            }
        }
        uint64_t user, kernel, child_user, child_kernel;
        leader->child_usage.user_ns=70000000U;leader->child_usage.kernel_ns=90000000U;
        group_lock_calls=0;
        kernel_task_cpu_ticks(&tasks[members-1],&user,&kernel,&child_user,&child_kernel);
        assert(user==members*(members+1)/2 && kernel==members*(members+1));
        assert(child_user==7 && child_kernel==9 && group_lock_calls<=32*members);
        group_lock_calls = 0;
        process_group_request_exit(&tasks[members - 1], KERNEL_THREAD_EXIT_SYSCALL, 42, 0);
        assert(leader->group_exiting && leader->completion.status == 42);
        /* Work may grow with members, but must not borrow64 unrelated tasks.
         * This is an upper bound, not an exact private lock-call count. */
        assert(group_lock_calls <= 32 * members);
        for (unsigned i = 0; i < members; i++) {
            assert(tasks[i].terminate_requested && !tasks[i].storage_borrows);
            assert(!tasks[i].group_wait_queue.borrows);
        }
        for (unsigned i = 0; i < 64; i++)
            assert(!unrelated[i].storage_borrows && !unrelated[i].terminate_requested);
        {
            KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
            for (unsigned i = 0; i < members; i++) scheduler_task_unlink_locked(&tasks[i]);
            for (unsigned i = 0; i < 64; i++) scheduler_task_unlink_locked(&unrelated[i]);
        }
        assert(!scheduler.task_count && !scheduler.all_tasks);
    }
    puts("group exit work is bounded by1/2/4 members, independent of unrelated tasks");
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
        cpus[i].logical_id = i;
        cpus[i].state = KERNEL_CPU_ONLINE;
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
    tasks[0].cpu_mask = 3;
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
        cpus[i].logical_id = i;
        cpus[i].state = KERNEL_CPU_ONLINE;
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
        cpus[i].logical_id = i;
        cpus[i].state = KERNEL_CPU_ONLINE;
        active_cpu = &cpus[i];
        scheduler_cpu_initialize(active_cpu, &idles[i], 0);
        active_cpu->current = &idles[i];
        idles[i].wait.on_cpu = 1;
        idles[i].state = KERNEL_THREAD_STATE_IDLE;
        selected[i] = 0;
    }
    tasks[0].state = KERNEL_THREAD_STATE_READY;
    tasks[0].cpu_mask = 3;
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

static void cpu_time_without_ticks(void)
{
    scheduler = (struct kernel_scheduler){.initialized=KERNEL_SCHEDULER_INITIALIZED};
    tasks[0] = (struct kernel_task){.magic=KERNEL_THREAD_MAGIC,.arch.user_mode=1,.group_members=1};
    tasks[0].group_leader=&tasks[0];
    kernel_cpu_initialize(&cpus[0],0,0);active_cpu=&cpus[0];cpus[0].current=&tasks[0];
    kernel_rt_bandwidth_init(&cpus[0].rt_bandwidth,0);
    scheduler.cpus=&cpus[0];kernel_raw_lock_init(&kernel_wait_domain,KERNEL_RAW_RANK_SCHEDULER);
    now=0;
    struct kernel_cpu_account_scope boot;
    kernel_cpu_account_trap_begin(&boot,0,1);kernel_cpu_account_trap_end(&boot);
    now=20000000;
    uint64_t user=0,system=0,children_user=0,children_system=0;
    kernel_task_cpu_ticks(&tasks[0],&user,&system,&children_user,&children_system);
    assert(user==0 && system==2 && !children_user && !children_system);
    kernel_cpu_account_user_return();
    now+=700;
    struct kernel_cpu_account_scope irq,nested;
    kernel_cpu_account_trap_begin(&irq,1,1);now+=500;
    kernel_cpu_account_trap_begin(&nested,0,1);now+=100;
    kernel_cpu_account_trap_end(&nested);now+=500;
    kernel_cpu_account_trap_end(&irq);now+=200;
    kernel_cpu_account_user_return();now+=300;
    uint64_t u,k;
    { KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);scheduler_task_cpu_time_locked(&tasks[0],&u,&k); }
    assert(u==1000 && k==20000200);
    struct kernel_cpu_account_scope syscall;
    kernel_cpu_account_trap_begin(&syscall,1,0);now+=30;
    tasks[1]=(struct kernel_task){.magic=KERNEL_THREAD_MAGIC};
    cpus[0].current=&tasks[1];scheduler_cpu_time_switch(&tasks[0],&tasks[1]);now+=1000;
    { KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
      scheduler_task_cpu_time_locked(&tasks[0],&u,&k);assert(u==1000 && k==20000230);
      scheduler_task_cpu_time_locked(&tasks[1],&u,&k);assert(!u && k==1000); }
    now=0;
    puts("CPU elapsed user/kernel time, nested IRQ exclusion and exact execution-owner transfer passed");
}

static void *account_on_other_cpu(void *unused)
{
    (void)unused;active_cpu=&cpus[1];sync_test_irq=0;
    while (!__atomic_load_n(&accounting_go,__ATOMIC_ACQUIRE)) sched_yield();
    for (unsigned i=0;i<10000;i++) {
        struct kernel_cpu_account_scope call;
        __atomic_add_fetch(&now,1,__ATOMIC_RELAXED);
        kernel_cpu_account_trap_begin(&call,1,0);
        kernel_cpu_account_trap_end(&call);
        __atomic_add_fetch(&now,1,__ATOMIC_RELAXED);
        kernel_cpu_account_user_return();
    }
    __atomic_store_n(&accounting_finished,1,__ATOMIC_RELEASE);
    return 0;
}
static void accounting_progress_without_scheduler_lock(void)
{
    scheduler=(struct kernel_scheduler){.initialized=KERNEL_SCHEDULER_INITIALIZED};now=1000;
    kernel_raw_lock_init(&kernel_wait_domain,KERNEL_RAW_RANK_SCHEDULER);
    for (unsigned i=0;i<2;i++) {
        kernel_cpu_initialize(&cpus[i],i,0);active_cpu=&cpus[i];sync_test_irq=0;
        tasks[i]=(struct kernel_task){.magic=KERNEL_THREAD_MAGIC,.arch.user_mode=1,.group_members=1};
        tasks[i].group_leader=&tasks[i];cpus[i].current=&tasks[i];
        cpus[i].scheduler_next=i ? &cpus[0] : 0;
        kernel_cpu_account_user_return();
    }
    scheduler.cpus=&cpus[1];active_cpu=&cpus[0];
    accounting_lock_attempt=accounting_finished=accounting_go=0;
    __atomic_store_n(&accounting_progress_check,1,__ATOMIC_RELEASE);
    pthread_t writer;int progressed;
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        assert(!pthread_create(&writer,0,account_on_other_cpu,0));
        __atomic_store_n(&accounting_go,1,__ATOMIC_RELEASE);
        do {
            uint64_t user,system;
            scheduler_task_cpu_time_locked(&tasks[1],&user,&system);
            assert(user+system==accounting_sample_time-1000);
        } while (!__atomic_load_n(&accounting_finished,__ATOMIC_ACQUIRE) &&
                 !__atomic_load_n(&accounting_lock_attempt,__ATOMIC_ACQUIRE));
        progressed=__atomic_load_n(&accounting_finished,__ATOMIC_ACQUIRE);
    }
    assert(!pthread_join(writer,0));
    __atomic_store_n(&accounting_progress_check,0,__ATOMIC_RELEASE);
    assert(progressed && !accounting_lock_attempt);
    assert(!cpus[1].preempt_depth && !cpus[1].raw_locks);
    puts("CPU accounting: 20000 remote mode transitions and coherent snapshots while scheduler domain is held");
}

static void resource_snapshot(void)
{
    scheduler=(struct kernel_scheduler){0};kernel_cpu_initialize(&cpus[0],0,0);
    active_cpu=&cpus[0];kernel_raw_lock_init(&kernel_wait_domain,KERNEL_RAW_RANK_SCHEDULER);
    tasks[0]=(struct kernel_task){.magic=KERNEL_THREAD_MAGIC,.group_members=1};
    tasks[0].group_leader=&tasks[0];
    tasks[0].usage=(struct kernel_task_usage){.user_ns=1000000,.minor_faults=3,.read_bytes=1024,.voluntary=1};
    tasks[0].dead_usage=(struct kernel_task_usage){.user_ns=2000000,.minor_faults=5,.maxrss_pages=7,.write_bytes=4096};
    tasks[0].child_usage=(struct kernel_task_usage){.user_ns=4000000,.minor_faults=11,.maxrss_pages=3};
    struct kernel_task_usage u={0};struct kernel_linux_rusage r;
    scheduler_usage_snapshot(&tasks[0],1,&u);assert(u.user_ns==1000000 && u.minor_faults==3 && u.maxrss_pages==7);
    scheduler_usage_snapshot(&tasks[0],0,&u);assert(u.user_ns==3000000 && u.minor_faults==8);
    scheduler_usage_format(&u,&r);assert(r.ru_utime.tv_usec==3000 && r.ru_minflt==8 &&
        r.ru_maxrss==7*(BOAROS_PAGE_SIZE/1024) && r.ru_inblock==2 && r.ru_oublock==8 && r.ru_nvcsw==1);
    scheduler_usage_snapshot(&tasks[0],-1,&u);assert(u.user_ns==4000000 && u.minor_faults==11 && u.maxrss_pages==3);
    scheduler_usage_snapshot(&tasks[0],-2,&u);assert(u.user_ns==7000000 && u.minor_faults==19 && u.maxrss_pages==7);
    puts("resource SELF/THREAD/CHILDREN snapshots preserve units, dead-thread separation and RSS maximum");
}

int main(void)
{
    accounting_progress_without_scheduler_lock();
    cpu_time_without_ticks();
    resource_snapshot();
    for (unsigned threads = 2; threads <= 4; threads += 2) {
        shutdown_during_disposal(threads, 0);
        for (unsigned window = 2; window <= 4; window++)
            shutdown_during_disposal(threads, window);
    }
    lifetime_generation();
    retired_cursor_chain();
    event_claim_handoff();
    signal_owner_cases(&group_lock_calls);
    group_exit_work();
    concurrent_retirement(2);
    concurrent_retirement(4);
    concurrent_exit_claim(2);
    concurrent_exit_claim(4);
    policy_selection_race();
    concurrent_group_retirement(2);
    concurrent_group_retirement(4);
    independent_runtime();
    for (unsigned iteration = 0; iteration < 128; iteration++) concurrent_claim(2);
    puts("scheduler 2-CPU execution claims and 2/4-actor metadata lifetime passed");
    return 0;
}
