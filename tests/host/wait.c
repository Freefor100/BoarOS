#define _POSIX_C_SOURCE 200809L
#include <kernel/wait_internal.h>
#include <kernel/sched_runqueue.h>
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

/* Machine switching is the host seam; arbitration/cursors/runqueue are production. */
struct kernel_task {
    struct kernel_wait_record wait;
    struct kernel_wait_node node;
    struct kernel_sched_node ready_node;
    unsigned blocked, completions;
};
_Thread_local uintptr_t sync_test_irq;
static _Thread_local struct kernel_cpu cpu;
static _Thread_local struct kernel_task task;
static _Thread_local struct kernel_task *selected_task;
static struct kernel_sched_runqueue runnable;
static pthread_barrier_t committed, notified;
static struct kernel_wait_token shared_token;
static unsigned wake_after_switch;
static int switching;
static struct kernel_wait_queue closing;
static pthread_barrier_t close_entered, close_released;
static _Thread_local unsigned pause_close;
static _Thread_local struct kernel_wait_queue *finish_queues;
static _Thread_local unsigned finish_remaining;
static _Thread_local int segment_parking;
static pthread_barrier_t segment_committed, segment_woken;
static struct kernel_task *segment_tasks;
static unsigned segment_count, segment_notified;
static _Thread_local unsigned forbid_wait_lock;
static _Thread_local int observe_prepared_notify;
static unsigned prepared_notify_progress;
static pthread_barrier_t prepared_notify_start;
static struct kernel_wait_token prepared_notify_token;
static _Thread_local int prepared_during_signal_check;
static unsigned prepared_signal_progress;
static _Thread_local unsigned pause_retired;
static struct kernel_wait_node *retired_first;
static pthread_barrier_t retired_paused, retired_resume;
void __real_kernel_raw_lock_acquire(struct kernel_raw_lock *,struct kernel_raw_guard *);
void __wrap_kernel_raw_lock_acquire(struct kernel_raw_lock *lock,struct kernel_raw_guard *guard)
{
    assert(!(forbid_wait_lock && lock==&kernel_wait_domain));
    if (observe_prepared_notify && lock==&kernel_wait_domain)
        __atomic_store_n(&prepared_notify_progress,2,__ATOMIC_RELEASE);
    __real_kernel_raw_lock_acquire(lock,guard);
}
void __real_kernel_raw_lock_release(struct kernel_raw_guard *);
void __wrap_kernel_raw_lock_release(struct kernel_raw_guard *guard)
{
    if (segment_tasks && guard->lock == &kernel_wait_domain) {
        unsigned count = 0;
        for (unsigned i = 0; i < segment_count; i++) count += kernel_wait_record_phase(&segment_tasks[i].wait) == KERNEL_WAIT_NOTIFIED;
        assert(count >= segment_notified && count - segment_notified <= 16);
        segment_notified = count;
    }
    int pause = pause_close && guard->lock == &kernel_wait_domain && closing.closed;
    if (pause) pause_close = 0;
    int pause_batch = pause_retired && guard->lock == &kernel_wait_domain &&
        retired_first && !retired_first->references;
    if (pause_batch) pause_retired = 0;
    if (finish_queues && guard->lock == &kernel_wait_domain) {
        unsigned remaining = 0;
        for (unsigned i = 0; i < 33; i++) remaining += finish_queues[i].registrations;
        assert(remaining <= finish_remaining && finish_remaining - remaining <= 16);
        finish_remaining = remaining;
    }
    __real_kernel_raw_lock_release(guard);
    if (pause_batch) {
        int result = pthread_barrier_wait(&retired_paused);
        assert(!result || result == PTHREAD_BARRIER_SERIAL_THREAD);
        result = pthread_barrier_wait(&retired_resume);
        assert(!result || result == PTHREAD_BARRIER_SERIAL_THREAD);
    }
    if (pause) {
        int result = pthread_barrier_wait(&close_entered);
        assert(!result || result == PTHREAD_BARRIER_SERIAL_THREAD);
        result = pthread_barrier_wait(&close_released);
        assert(!result || result == PTHREAD_BARRIER_SERIAL_THREAD);
    }
}
void *sync_test_cpu(void) { return &cpu; }
static void initialize(unsigned id)
{
    kernel_cpu_initialize(&cpu, id, 0);
    sync_test_irq = 0;
    task = (struct kernel_task){0};
    kernel_wait_record_init(&task.wait);
    task.wait.on_cpu = 1;
    kernel_wait_node_init(&task.node, &task);
}
static void barrier(pthread_barrier_t *b)
{ int result = pthread_barrier_wait(b); assert(!result || result == PTHREAD_BARRIER_SERIAL_THREAD); }
struct kernel_wait_record *kernel_wait_record_of(struct kernel_task *t) { return &t->wait; }
struct kernel_wait_node *kernel_wait_default_node(struct kernel_task *t) { return &t->node; }
struct kernel_task *kernel_wait_current_task(void) { return selected_task ? selected_task : &task; }
int kernel_wait_backend_initialized(void) { return 1; }
int kernel_wait_backend_signal(struct kernel_task *t)
{
    (void)t;
    if (prepared_during_signal_check) {
        barrier(&prepared_notify_start);
        do { prepared_signal_progress=__atomic_load_n(&prepared_notify_progress,__ATOMIC_ACQUIRE); }
        while (prepared_signal_progress<2);
        /* Let the old implementation leave the domain rather than deadlock;
         * the test below still requires the actual wake to win. */
        return prepared_signal_progress==2;
    }
    return 0;
}
uint64_t kernel_wait_backend_time(void) { return 1; }
void kernel_wait_backend_commit(struct kernel_task *t, uint64_t deadline, int interruptible)
{ (void)deadline; (void)interruptible; assert(!t->blocked); t->blocked = 1; }
void kernel_wait_backend_notify(struct kernel_task *t, uint32_t reason)
{
    (void)reason;
    assert(t->blocked == 1);
    t->blocked = 0;
    t->completions++;
    /* COMMITTED通知与生产backend一样摘除默认登记，next也随之清空。 */
    kernel_wait_node_remove_locked(&t->node);
}
void kernel_wait_backend_ready(struct kernel_task *t)
{ assert(!t->ready_node.queued && !t->wait.on_cpu); kernel_sched_enqueue(&runnable, &t->ready_node, t, 0, 0); }
enum kernel_scheduler_status kernel_wait_backend_switch(struct kernel_task *t)
{
    if (segment_parking) {
        { KERNEL_RAW_SCOPE(guard, &kernel_wait_domain); kernel_wait_switch_finish_locked(t); }
        barrier(&segment_committed);
        barrier(&segment_woken);
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        assert(kernel_wait_record_phase(&t->wait) == KERNEL_WAIT_NOTIFIED && t->completions == 1 && t->ready_node.queued);
        kernel_sched_dequeue(&runnable, &t->ready_node);
        t->wait.on_cpu = 1;
        return KERNEL_SCHEDULER_STATUS_OK;
    }
    assert(switching && !sync_test_irq && !cpu.raw_locks && !cpu.preempt_depth);
    if (wake_after_switch) {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        kernel_wait_switch_finish_locked(t);
        assert(!t->ready_node.queued);
    }
    barrier(&committed);
    barrier(&notified);
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    assert(kernel_wait_record_phase(&t->wait) == KERNEL_WAIT_NOTIFIED && t->completions == 1);
    if (!wake_after_switch) {
        assert(t->wait.on_cpu && !t->ready_node.queued && t->wait.ready_pending);
        kernel_wait_switch_finish_locked(t);
    }
    assert(t->ready_node.queued && !t->wait.on_cpu);
    kernel_sched_dequeue(&runnable, &t->ready_node);
    t->wait.on_cpu = 1;
    /* The resumed owner already has the completed result; only finish needs
     * to return to the shared domain to detach its remaining registrations. */
    forbid_wait_lock = 1;
    return KERNEL_SCHEDULER_STATUS_OK;
}
void kernel_wait_backend_quiesce(void) { sched_yield(); }
static void *notify_worker(void *argument)
{
    initialize(10 + (unsigned)(uintptr_t)argument);
    barrier(&committed);
    enum kernel_wait_wake_reason reason = (unsigned)(uintptr_t)argument % 3;
    assert(kernel_wait_notify(&shared_token, reason) == KERNEL_SCHEDULER_STATUS_OK);
    assert(kernel_wait_notify(&shared_token, reason) == KERNEL_SCHEDULER_STATUS_OK);
    barrier(&notified);
    assert(!cpu.raw_locks && !cpu.preempt_depth);
    return 0;
}
static void *prepared_notify_worker(void *argument)
{
    (void)argument;
    initialize(30);
    barrier(&prepared_notify_start);
    __atomic_store_n(&prepared_notify_progress,1,__ATOMIC_RELEASE);
    observe_prepared_notify=1;
    assert(kernel_wait_notify(&prepared_notify_token,KERNEL_WAIT_WOKEN)==KERNEL_SCHEDULER_STATUS_OK);
    observe_prepared_notify=0;
    __atomic_store_n(&prepared_notify_progress,3,__ATOMIC_RELEASE);
    return 0;
}
static void prepared_notify_while_domain_busy(void)
{
    for (unsigned with_queue=0;with_queue<2;with_queue++) {
        struct kernel_wait_queue queue;
        kernel_wait_queue_init(&queue);
        prepared_notify_progress=0;
        assert(kernel_wait_prepare(with_queue?&queue:0,100,1,&prepared_notify_token)==KERNEL_SCHEDULER_STATUS_OK);
        assert(!pthread_barrier_init(&prepared_notify_start,0,2));
        pthread_t worker;
        assert(!pthread_create(&worker,0,prepared_notify_worker,0));
        unsigned progress;
        {
            KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
            barrier(&prepared_notify_start);
            do { progress=__atomic_load_n(&prepared_notify_progress,__ATOMIC_ACQUIRE); }
            while (progress<2);
        }
        assert(!pthread_join(worker,0));
        assert(!pthread_barrier_destroy(&prepared_notify_start));
        /* The notification must complete before the unrelated owner releases
         * the scheduler domain; a blocked lock attempt is detected explicitly. */
        assert(progress==3);
        enum kernel_wait_wake_reason reason;
        assert(kernel_wait_park(&prepared_notify_token,&reason)==KERNEL_SCHEDULER_STATUS_OK);
        assert(reason==KERNEL_WAIT_WOKEN && !task.blocked && !task.ready_node.queued);
        assert(kernel_wait_finish(&prepared_notify_token)==KERNEL_SCHEDULER_STATUS_OK);
        assert(!queue.registrations && !queue.borrows);
        assert(kernel_wait_queue_close(&queue)==KERNEL_SCHEDULER_STATUS_OK);
        assert(kernel_wait_queue_destroy(&queue)==KERNEL_SCHEDULER_STATUS_OK);
    }
    puts("wait: prepared notifications complete while another CPU owns the scheduler domain");
}
static void prepared_notify_during_park_check(void)
{
    struct kernel_wait_queue queue;
    kernel_wait_queue_init(&queue);
    prepared_notify_progress=prepared_signal_progress=0;
    assert(kernel_wait_prepare(&queue,100,1,&prepared_notify_token)==KERNEL_SCHEDULER_STATUS_OK);
    assert(!pthread_barrier_init(&prepared_notify_start,0,2));
    pthread_t worker;
    assert(!pthread_create(&worker,0,prepared_notify_worker,0));
    prepared_during_signal_check=1;
    enum kernel_wait_wake_reason reason;
    assert(kernel_wait_park(&prepared_notify_token,&reason)==KERNEL_SCHEDULER_STATUS_OK);
    prepared_during_signal_check=0;
    assert(!pthread_join(worker,0) && !pthread_barrier_destroy(&prepared_notify_start));
    assert(prepared_signal_progress==3 && reason==KERNEL_WAIT_WOKEN &&
           !task.blocked && !task.ready_node.queued);
    assert(kernel_wait_finish(&prepared_notify_token)==KERNEL_SCHEDULER_STATUS_OK);
    assert(!queue.registrations && !queue.borrows);
    assert(kernel_wait_queue_close(&queue)==KERNEL_SCHEDULER_STATUS_OK);
    assert(kernel_wait_queue_destroy(&queue)==KERNEL_SCHEDULER_STATUS_OK);
    puts("wait: notification wins during the interruptible park check without committing sleep");
}
static void arbitration(unsigned count)
{
    pthread_t workers[4];
    struct kernel_wait_queue queue;
    kernel_wait_queue_init(&queue);
    assert(kernel_wait_prepare(&queue, 100, 1, &shared_token) == KERNEL_SCHEDULER_STATUS_OK);

    assert(!pthread_barrier_init(&committed, 0, count + 1));
    assert(!pthread_barrier_init(&notified, 0, count + 1));
    for (unsigned i = 0; i < count; i++) assert(!pthread_create(&workers[i], 0, notify_worker, (void *)(uintptr_t)i));
    switching = 1;
    enum kernel_wait_wake_reason reason;
    assert(kernel_wait_park(&shared_token, &reason) == KERNEL_SCHEDULER_STATUS_OK);
    forbid_wait_lock = 0;
    switching = 0;
    for (unsigned i = 0; i < count; i++) assert(!pthread_join(workers[i], 0));
    assert(reason <= KERNEL_WAIT_SIGNALLED);
    struct kernel_wait_token old = shared_token;
    assert(kernel_wait_finish(&shared_token) == KERNEL_SCHEDULER_STATUS_OK);
    assert(!pthread_barrier_destroy(&committed) && !pthread_barrier_destroy(&notified));
    assert(kernel_wait_prepare(&queue, 0, 0, &shared_token) == KERNEL_SCHEDULER_STATUS_OK);
    assert(kernel_wait_notify(&old, KERNEL_WAIT_TIMEOUT) == KERNEL_SCHEDULER_STATUS_OK);
    assert(kernel_wait_record_phase(&task.wait) == KERNEL_WAIT_PREPARED);
    assert(kernel_wait_finish(&shared_token) == KERNEL_SCHEDULER_STATUS_OK);
    assert(kernel_wait_queue_close(&queue) == KERNEL_SCHEDULER_STATUS_OK);
    assert(kernel_wait_queue_destroy(&queue) == KERNEL_SCHEDULER_STATUS_OK);
    task.completions = 0;
}
static struct kernel_wait_queue callbacks, nested;
static struct kernel_wait_node nodes[64];
static pthread_barrier_t callback_entered, callback_release;
static unsigned callback_calls;
static void callback(struct kernel_wait_node *node, uint32_t reason)
{
    assert(reason == KERNEL_WAIT_WOKEN && cpu.preempt_depth && !cpu.raw_locks);
    assert(node->context == &callback_calls);
    callback_calls++;
    if (node == nodes) {
        barrier(&callback_entered);
        barrier(&callback_release);
        kernel_wait_queue_remove(node); /* self cancellation may only detach */
        assert(kernel_wait_queue_wake_all(&nested) == KERNEL_SCHEDULER_STATUS_OK);
    }
}
static void *wake_worker(void *argument)
{
    (void)argument;
    initialize(7);
    struct kernel_wait_cursor cursor={0};
    assert(kernel_wait_cursor_prepare(&callbacks,&cursor));
    kernel_wait_cursor_wake(&cursor,1);
    return 0;
}
static void callback_lifetime(void)
{
    kernel_wait_queue_init(&callbacks);
    kernel_wait_queue_init(&nested);
    for (unsigned i = 0; i < 64; i++) {
        kernel_wait_node_init_callback(&nodes[i], callback, &callback_calls);
        kernel_wait_queue_add(&callbacks, &nodes[i]);
    }
    assert(!pthread_barrier_init(&callback_entered, 0, 2));
    assert(!pthread_barrier_init(&callback_release, 0, 2));
    pthread_t worker;
    assert(!pthread_create(&worker, 0, wake_worker, 0));
    barrier(&callback_entered);
    /* Detach a whole successor chain while the first callback owns the cursor. */
    for (unsigned i = 0; i < 64; i++) kernel_wait_queue_remove(&nodes[i]);
    struct kernel_wait_cursor empty={0};
    forbid_wait_lock=1;
    assert(!kernel_wait_cursor_prepare(&callbacks,&empty) && !empty.queue);
    forbid_wait_lock=0;
    assert(callbacks.borrows);
    assert(kernel_wait_queue_close(&callbacks) == KERNEL_SCHEDULER_STATUS_OK);
    assert(kernel_wait_queue_destroy(&callbacks) == KERNEL_SCHEDULER_STATUS_BUSY);
    barrier(&callback_release);
    assert(!pthread_join(worker, 0));
    for (unsigned i = 0; i < 64; i++) {
        assert(kernel_wait_node_remove_sync(&nodes[i]) == KERNEL_SCHEDULER_STATUS_OK);
        assert(!nodes[i].borrow_owner && !nodes[i].references && !nodes[i].retired_next);
    }
    assert(callback_calls == 1 && !callbacks.borrows && !callbacks.registrations);
    assert(kernel_wait_queue_destroy(&callbacks) == KERNEL_SCHEDULER_STATUS_OK);
    assert(!pthread_barrier_destroy(&callback_entered) && !pthread_barrier_destroy(&callback_release));
}
struct retired_cursor_case { struct kernel_wait_cursor cursor; int advance; };
static void *finish_retired_cursor(void *argument)
{
    struct retired_cursor_case *test=argument;
    initialize(12);pause_retired=1;
    if (test->advance) {
        kernel_wait_cursor_advance(&test->cursor);
        assert(!test->cursor.node);
    }
    kernel_wait_cursor_end(&test->cursor);
    return 0;
}
static void retired_cursor_batches(void)
{
    for (int advance=0;advance<2;advance++) {
        struct kernel_wait_queue queue;
        struct kernel_wait_node entries[64];
        struct kernel_task owners[64]={0};
        struct retired_cursor_case test={.advance=advance};
        kernel_wait_queue_init(&queue);
        for (unsigned i=0;i<64;i++) {
            kernel_wait_node_init(&entries[i],&owners[i]);
            kernel_wait_queue_add(&queue,&entries[i]);
            if (advance && !i) kernel_wait_cursor_begin(&queue,&test.cursor);
        }
        if (!advance) kernel_wait_cursor_begin(&queue,&test.cursor);
        for (unsigned i=0;i<64;i++) kernel_wait_queue_remove(&entries[i]);
        assert(!kernel_wait_queue_close(&queue));
        retired_first=&entries[0];
        assert(!pthread_barrier_init(&retired_paused,0,2));
        assert(!pthread_barrier_init(&retired_resume,0,2));
        pthread_t worker;assert(!pthread_create(&worker,0,finish_retired_cursor,&test));
        barrier(&retired_paused);
        assert(kernel_wait_queue_destroy(&queue)==KERNEL_SCHEDULER_STATUS_BUSY);
        { KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
          assert(queue.borrows==49 && !queue.registrations);
          for (unsigned i=0;i<64;i++) {
              assert(entries[i].references==(i>=16));
              assert(owners[i].wait.borrows==(i>=16));
          }
          if (advance) assert(!test.cursor.node); }
        barrier(&retired_resume);assert(!pthread_join(worker,0));
        for (unsigned i=0;i<64;i++)
            assert(!entries[i].borrow_owner && !entries[i].references && !owners[i].wait.borrows);
        assert(!queue.borrows && !kernel_wait_queue_destroy(&queue));
        assert(!pthread_barrier_destroy(&retired_paused) && !pthread_barrier_destroy(&retired_resume));
        retired_first=0;
    }
    puts("wait: retired cursor end and sequence boundary retain queue and task owners across bounded batches");
}
static void *close_worker(void *argument)
{
    (void)argument;
    initialize(8);
    pause_close = 1;
    assert(kernel_wait_queue_close(&closing) == KERNEL_SCHEDULER_STATUS_OK);
    return 0;
}
static void close_lifetime(void)
{
    kernel_wait_queue_init(&closing);
    assert(!pthread_barrier_init(&close_entered, 0, 2));
    assert(!pthread_barrier_init(&close_released, 0, 2));
    pthread_t worker;
    assert(!pthread_create(&worker, 0, close_worker, 0));
    barrier(&close_entered);
    /* 已停止登记但close还要访问空队列，destroy不能提前归还owner。 */
    assert(kernel_wait_queue_destroy(&closing) == KERNEL_SCHEDULER_STATUS_BUSY);
    barrier(&close_released);
    assert(!pthread_join(worker, 0));
    assert(!closing.borrows && !closing.registrations);
    assert(kernel_wait_queue_destroy(&closing) == KERNEL_SCHEDULER_STATUS_OK);
    assert(!pthread_barrier_destroy(&close_entered) && !pthread_barrier_destroy(&close_released));
}
static struct kernel_wait_queue published_queue;
static pthread_barrier_t publish_entered,publish_completed;
static unsigned empty_notification;
static void *published_notification_worker(void *unused)
{
    (void)unused;initialize(11);
    if(empty_notification) {
        barrier(&publish_entered);
        struct kernel_wait_cursor cursor={0};
        forbid_wait_lock=1;
        assert(!kernel_wait_cursor_prepare(&published_queue,&cursor) && !cursor.queue);
        assert(!kernel_wait_queue_wake_one(&published_queue));
        assert(!kernel_wait_queue_wake_all(&published_queue));
        forbid_wait_lock=0;
        barrier(&publish_completed);
    } else {
        struct kernel_wait_token token={0};
        assert(!kernel_wait_prepare(&published_queue,0,0,&token));
        barrier(&publish_entered);barrier(&publish_completed);
        enum kernel_wait_wake_reason reason;
        assert(!kernel_wait_park(&token,&reason) && reason==KERNEL_WAIT_WOKEN);
        assert(!task.blocked && !kernel_wait_finish(&token));
    }
    return 0;
}
static void published_notifications(void)
{
    for(unsigned empty=0;empty<2;empty++) {
        empty_notification=empty;kernel_wait_queue_init(&published_queue);
        assert(!pthread_barrier_init(&publish_entered,0,2));
        assert(!pthread_barrier_init(&publish_completed,0,2));
        pthread_t worker;assert(!pthread_create(&worker,0,published_notification_worker,0));
        if(empty) {
            /* 无登记的通知不能等待另一CPU持有的全局调度锁。 */
            KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
            barrier(&publish_entered);barrier(&publish_completed);
        } else {
            barrier(&publish_entered);
            assert(__atomic_load_n(&published_queue.registrations,__ATOMIC_ACQUIRE)==1);
            struct kernel_wait_cursor cursor={0};
            assert(!kernel_wait_cursor_prepare(&published_queue,&cursor) && !cursor.queue);
            barrier(&publish_completed);
        }
        assert(!pthread_join(worker,0));
        assert(!published_queue.registrations && !published_queue.borrows);
        assert(!kernel_wait_queue_close(&published_queue) && !kernel_wait_queue_destroy(&published_queue));
        assert(!pthread_barrier_destroy(&publish_entered) && !pthread_barrier_destroy(&publish_completed));
    }
    puts("wait: published registrations notify before park; empty notification completes while another CPU owns wait domain");
}
static void early_and_multi(void)
{
    struct kernel_wait_queue queues[2];
    struct kernel_wait_node other;
    for (unsigned i = 0; i < 2; i++) kernel_wait_queue_init(&queues[i]);
    struct kernel_wait_token token;
    kernel_wait_node_init(&other, &task);
    assert(kernel_wait_prepare(&queues[0], 0, 0, &token) == KERNEL_SCHEDULER_STATUS_OK);
    assert(kernel_wait_node_bind(&queues[1], &other, &token) == KERNEL_SCHEDULER_STATUS_OK);
    assert(kernel_wait_queue_wake_one(&queues[1]) == KERNEL_SCHEDULER_STATUS_OK);
    enum kernel_wait_wake_reason reason;
    assert(kernel_wait_park(&token, &reason) == KERNEL_SCHEDULER_STATUS_OK);
    assert(reason == KERNEL_WAIT_WOKEN && !task.blocked && !task.ready_node.queued);
    assert(kernel_wait_finish(&token) == KERNEL_SCHEDULER_STATUS_OK);
    for (unsigned i = 0; i < 2; i++) {
        assert(kernel_wait_queue_close(&queues[i]) == KERNEL_SCHEDULER_STATUS_OK);
        assert(kernel_wait_queue_destroy(&queues[i]) == KERNEL_SCHEDULER_STATUS_OK);
    }
    int waited = 0;
    assert(KERNEL_WAIT_RECHECK(0, 0, 0, &reason, (++waited, 0)) == KERNEL_SCHEDULER_STATUS_OK);
    assert(waited == 1 && kernel_wait_record_phase(&task.wait) == KERNEL_WAIT_FINISHED);
    struct kernel_wait_cursor cursor;
    kernel_wait_queue_init(&queues[0]);
    kernel_wait_queue_init(&queues[1]);
    assert(kernel_wait_prepare(&queues[0], 0, 0, &token) == KERNEL_SCHEDULER_STATUS_OK);
    kernel_wait_cursor_begin(&queues[0], &cursor);
    assert(cursor.node == &task.node);
    kernel_wait_task_pin(&task);
    kernel_wait_cursor_advance(&cursor);
    assert(!cursor.node);
    kernel_wait_cursor_end(&cursor);
    { KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
      assert(kernel_wait_node_move_locked(&task.node, &queues[1]) == KERNEL_SCHEDULER_STATUS_OK); }
    assert(task.node.queue == &queues[1]);
    kernel_wait_task_unpin(&task);
    assert(kernel_wait_queue_wake_one(&queues[1]) == KERNEL_SCHEDULER_STATUS_OK);
    assert(kernel_wait_finish(&token) == KERNEL_SCHEDULER_STATUS_OK);
    kernel_wait_record_init(&task.wait);
    task.wait.on_cpu = 1;
    assert(kernel_wait_prepare(&queues[0], 0, 0, &token) == KERNEL_SCHEDULER_STATUS_OK);
    struct kernel_wait_token retired_task = token;
    assert(kernel_wait_finish(&token) == KERNEL_SCHEDULER_STATUS_OK);
    /* Same storage, a new task owner: pointer equality must not revive an old token. */
    kernel_wait_record_init(&task.wait);
    task.wait.on_cpu = 1;
    assert(kernel_wait_prepare(&queues[0], 0, 0, &token) == KERNEL_SCHEDULER_STATUS_OK);
    assert(kernel_wait_notify(&retired_task, KERNEL_WAIT_SIGNALLED) == KERNEL_SCHEDULER_STATUS_OK);
    assert(kernel_wait_record_phase(&task.wait) == KERNEL_WAIT_PREPARED);
    assert(kernel_wait_finish(&token) == KERNEL_SCHEDULER_STATUS_OK);
    kernel_wait_record_store(&task.wait,KERNEL_WAIT_GENERATION_MAX,KERNEL_WAIT_FINISHED,KERNEL_WAIT_WOKEN);
    assert(kernel_wait_prepare(0, 0, 0, &token) == KERNEL_SCHEDULER_STATUS_INVALID_STATE);
    assert(!task.wait.nodes && kernel_wait_record_phase(&task.wait) == KERNEL_WAIT_FINISHED);
    kernel_wait_record_store(&task.wait,0,KERNEL_WAIT_FINISHED,KERNEL_WAIT_WOKEN);
}
static void finish_many(void)
{
    struct kernel_wait_queue queues[33];
    struct kernel_wait_node nodes[32];
    struct kernel_wait_token token;
    for (unsigned i = 0; i < 33; i++) kernel_wait_queue_init(&queues[i]);
    assert(kernel_wait_prepare(&queues[0], 0, 0, &token) == KERNEL_SCHEDULER_STATUS_OK);
    for (unsigned i = 0; i < 32; i++) {
        kernel_wait_node_init(&nodes[i], &task);
        assert(kernel_wait_node_bind(&queues[i + 1], &nodes[i], &token) == KERNEL_SCHEDULER_STATUS_OK);
    }
    finish_queues = queues;
    finish_remaining = 33;
    assert(kernel_wait_finish(&token) == KERNEL_SCHEDULER_STATUS_OK);
    assert(!finish_remaining && !task.wait.nodes && !task.wait.borrows);
    finish_queues = 0;
    for (unsigned i = 0; i < 33; i++) {
        assert(kernel_wait_queue_close(&queues[i]) == KERNEL_SCHEDULER_STATUS_OK);
        assert(kernel_wait_queue_destroy(&queues[i]) == KERNEL_SCHEDULER_STATUS_OK);
    }
}
static void owner_failures(void)
{
    for (unsigned which = 0; which < 6; which++) {
        pid_t child = fork(); assert(child >= 0);
        if (!child) {
            struct kernel_wait_queue queue;
            struct kernel_wait_node node;
            kernel_wait_queue_init(&queue);
            kernel_wait_node_init_callback(&node, callback, 0);
            kernel_wait_queue_add(&queue, &node);
            if (!which) kernel_wait_queue_add(&queue, &node);
            else if (which == 1) { queue.registrations = 0; queue.closed = 1; kernel_wait_queue_destroy(&queue); }
            else if (which == 2) { node.previous = &node; kernel_wait_queue_wake_all(&queue); }
            else if (which == 3) {
                struct kernel_wait_token token;
                kernel_wait_prepare(0, 0, 0, &token);
                kernel_wait_prepare(0, 0, 0, &token);
            } else if (which == 4) {
                struct kernel_wait_token token = {(void *)1, 1};
                kernel_wait_finish(&token);
            } else {
                kernel_wait_queue_remove(&node);
                struct kernel_wait_token token;
                assert(kernel_wait_prepare(&queue, 0, 0, &token) == KERNEL_SCHEDULER_STATUS_OK);
                task.node.borrow_owner = 0;
                kernel_wait_queue_wake_all(&queue);
            }
            _exit(1);
        }
        int status; assert(waitpid(child, &status, 0) == child);
        assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGILL);
    }
}
struct segment_job { struct kernel_wait_queue *queue; unsigned cpu_id; };
static void *segment_worker(void *argument)
{
    struct segment_job *job = argument;
    struct kernel_wait_queue *queue = job->queue;
    initialize(job->cpu_id);
    segment_parking = 1;
    struct kernel_wait_token token;
    assert(kernel_wait_prepare(queue, 0, 0, &token) == KERNEL_SCHEDULER_STATUS_OK);
    enum kernel_wait_wake_reason reason;
    assert(kernel_wait_park(&token, &reason) == KERNEL_SCHEDULER_STATUS_OK && reason == KERNEL_WAIT_WOKEN);
    assert(kernel_wait_finish(&token) == KERNEL_SCHEDULER_STATUS_OK);
    assert(!task.node.references && !task.wait.borrows);
    segment_parking = 0;
    return 0;
}
static void committed_segments(void)
{
    for (unsigned count = 2; count <= 4; count *= 2) {
        struct kernel_wait_queue queue;
        pthread_t workers[4];
        struct segment_job jobs[4];
        kernel_wait_queue_init(&queue);
        assert(!pthread_barrier_init(&segment_committed, 0, count + 1));
        assert(!pthread_barrier_init(&segment_woken, 0, count + 1));
        for (unsigned i = 0; i < count; i++) {
            jobs[i] = (struct segment_job){&queue, 100 + i};
            assert(!pthread_create(&workers[i], 0, segment_worker, &jobs[i]));
        }
        barrier(&segment_committed);
        assert(queue.registrations == count);
        assert(kernel_wait_queue_wake_all(&queue) == KERNEL_SCHEDULER_STATUS_OK);
        /* 真实backend在notify时摘默认节点；不能在通知后读取被清空的next。 */
        {
            KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
            assert(!queue.registrations && runnable.first && runnable.last);
        }
        barrier(&segment_woken);
        for (unsigned i = 0; i < count; i++) assert(!pthread_join(workers[i], 0));
        assert(!runnable.first && !runnable.last && !queue.borrows);
        assert(kernel_wait_queue_close(&queue) == KERNEL_SCHEDULER_STATUS_OK);
        assert(kernel_wait_queue_destroy(&queue) == KERNEL_SCHEDULER_STATUS_OK);
        assert(!pthread_barrier_destroy(&segment_committed) && !pthread_barrier_destroy(&segment_woken));
    }
}

struct mixed_context {
    struct kernel_task *actors;
    struct kernel_wait_queue *queue;
    struct kernel_wait_node *extra;
    unsigned before, after, calls, extra_calls;
};
static void mixed_callback(struct kernel_wait_node *node, uint32_t reason)
{
    struct mixed_context *context = node->context;
    assert(reason == KERNEL_WAIT_WOKEN && cpu.preempt_depth && !cpu.raw_locks);
    for (unsigned i = 0; i < context->before; i++) assert(kernel_wait_record_phase(&context->actors[i].wait) == KERNEL_WAIT_NOTIFIED);
    for (unsigned i = context->before; i < context->after; i++) assert(kernel_wait_record_phase(&context->actors[i].wait) == KERNEL_WAIT_PREPARED);
    /* 回调新增的登记不能越过本次游标sequence；下一次通知才可执行。 */
    kernel_wait_queue_add(context->queue, context->extra);
    context->calls++;
}
static void appended_callback(struct kernel_wait_node *node, uint32_t reason)
{
    struct mixed_context *context = node->context;
    assert(reason == KERNEL_WAIT_WOKEN && cpu.preempt_depth && !cpu.raw_locks);
    context->extra_calls++;
}
static void task_segments(void)
{
    const unsigned counts[] = {2, 8, 16, 17, 33, 64};
    for (unsigned c = 0; c < sizeof(counts) / sizeof(*counts); c++) {
        for (unsigned mixed = 0; mixed < 2; mixed++) {
            unsigned count = counts[c];
            struct kernel_task actors[65] = {0};
            struct kernel_wait_token tokens[65] = {0};
            struct kernel_wait_queue queue;
            struct kernel_wait_node callback_node, extra_node;
            struct mixed_context context = {actors, &queue, &extra_node, count / 2, count, 0, 0};
            kernel_wait_queue_init(&queue);
            kernel_wait_node_init_callback(&callback_node, mixed_callback, &context);
            kernel_wait_node_init_callback(&extra_node, appended_callback, &context);
            for (unsigned i = 0; i < count; i++) {
                kernel_wait_record_init(&actors[i].wait);
                actors[i].wait.on_cpu = 1;
                kernel_wait_node_init(&actors[i].node, &actors[i]);
                if (mixed && i == count / 2) kernel_wait_queue_add(&queue, &callback_node);
                selected_task = &actors[i];
                assert(kernel_wait_prepare(&queue, 0, 0, &tokens[i]) == KERNEL_SCHEDULER_STATUS_OK);
            }
            selected_task = 0;
            segment_tasks = actors;
            segment_count = count;
            segment_notified = 0;
            if (!mixed) {
                assert(kernel_wait_queue_wake_one(&queue) == KERNEL_SCHEDULER_STATUS_OK);
                assert(kernel_wait_record_phase(&actors[0].wait) == KERNEL_WAIT_NOTIFIED);
                for (unsigned i = 1; i < count; i++) assert(kernel_wait_record_phase(&actors[i].wait) == KERNEL_WAIT_PREPARED);
            }
            assert(kernel_wait_queue_wake_all(&queue) == KERNEL_SCHEDULER_STATUS_OK);
            segment_tasks = 0;
            assert(context.calls == mixed);
            if (mixed) {
                assert(!context.extra_calls);
                kernel_wait_queue_remove(&callback_node);
                assert(kernel_wait_queue_wake_all(&queue) == KERNEL_SCHEDULER_STATUS_OK);
                assert(context.extra_calls == 1);
                kernel_wait_queue_remove(&extra_node);
            }
            for (unsigned i = 0; i < count; i++) {
                selected_task = &actors[i];
                assert(kernel_wait_record_phase(&actors[i].wait) == KERNEL_WAIT_NOTIFIED);
                assert(kernel_wait_finish(&tokens[i]) == KERNEL_SCHEDULER_STATUS_OK);
                assert(!actors[i].wait.borrows && !actors[i].node.references);
            }
            selected_task = 0;
            assert(!queue.registrations && !queue.borrows);
            assert(kernel_wait_queue_close(&queue) == KERNEL_SCHEDULER_STATUS_OK);
            assert(kernel_wait_queue_destroy(&queue) == KERNEL_SCHEDULER_STATUS_OK);
        }
    }
}

static struct kernel_wait_queue notification_queues[2];
static uint64_t callback_sequence;
static unsigned notification_calls;
static void sequence_callback(struct kernel_wait_node *node, uint32_t reason)
{
    (void)reason;
    uint64_t sequence = kernel_wait_notification_sequence();
    assert(sequence && cpu.preempt_depth && !cpu.raw_locks);
    if (!notification_calls++) callback_sequence = sequence;
    else assert(sequence == callback_sequence);
    if (node->context) assert(!kernel_wait_queue_wake_all(&notification_queues[1]));
}
static void notification_sequences(void)
{
    struct kernel_wait_node nodes[3];
    for (unsigned i = 0; i < 2; i++) kernel_wait_queue_init(&notification_queues[i]);
    for (unsigned i = 0; i < 3; i++) {
        kernel_wait_node_init_callback(&nodes[i], sequence_callback, i ? NULL : &nodes[0]);
        kernel_wait_queue_add(&notification_queues[i ? 1 : 0], &nodes[i]);
    }
    uint64_t previous = 0;
    for (unsigned i = 0; i < 2; i++) {
        notification_calls = 0;
        assert(!kernel_wait_queue_wake_all(&notification_queues[0]));
        assert(notification_calls == 3 && previous != callback_sequence);
        assert(!kernel_wait_notification_sequence());
        previous = callback_sequence;
    }
    for (unsigned i = 0; i < 3; i++) assert(!kernel_wait_node_remove_sync(&nodes[i]));
    for (unsigned i = 0; i < 2; i++) {
        assert(!notification_queues[i].borrows && !notification_queues[i].registrations);
        assert(!kernel_wait_queue_close(&notification_queues[i]));
        assert(!kernel_wait_queue_destroy(&notification_queues[i]));
    }
}
static struct kernel_wait_queue retiring;
static struct kernel_wait_node retiring_node;
static pthread_barrier_t retiring_entered,retiring_leave;
static void pollfree_callback(struct kernel_wait_node *node,uint32_t reason)
{
    assert(reason==KERNEL_WAIT_POLL_FREE);
    assert(node==&retiring_node && !kernel_cpu_current()->raw_locks);
    kernel_wait_queue_remove(node);
    barrier(&retiring_entered);barrier(&retiring_leave);
}
static void *pollfree_worker(void *unused)
{
    (void)unused;initialize(1);kernel_wait_queue_pollfree(&retiring);return 0;
}
static void pollfree_lifetime(void)
{
    kernel_wait_queue_init(&retiring);
    kernel_wait_node_init_callback(&retiring_node,pollfree_callback,0);
    kernel_wait_queue_add(&retiring,&retiring_node);
    /* A prior wake cursor must keep the queue alive even after POLLFREE
     * detached its registration and completed its own notification. */
    struct kernel_wait_cursor prior;kernel_wait_cursor_begin(&retiring,&prior);
    assert(!pthread_barrier_init(&retiring_entered,0,2));
    assert(!pthread_barrier_init(&retiring_leave,0,2));
    pthread_t worker;assert(!pthread_create(&worker,0,pollfree_worker,0));barrier(&retiring_entered);
    assert(retiring.closed && !retiring.registrations);
    assert(kernel_wait_queue_destroy(&retiring)==KERNEL_SCHEDULER_STATUS_BUSY);
    barrier(&retiring_leave);assert(!pthread_join(worker,0));
    assert(kernel_wait_queue_destroy(&retiring)==KERNEL_SCHEDULER_STATUS_BUSY);
    kernel_wait_cursor_wake(&prior,1);
    assert(!retiring.borrows && !retiring_node.references && !kernel_wait_queue_destroy(&retiring));
    assert(!pthread_barrier_destroy(&retiring_entered)&&!pthread_barrier_destroy(&retiring_leave));
    puts("wait: POLLFREE detaches callbacks and retains source through prior cursors passed");
}
static unsigned event_calls[2];
static void event_callback(struct kernel_wait_node *node,uint32_t reason)
{ assert(reason==KERNEL_WAIT_WOKEN);(*(unsigned *)node->context)++; }
static void event_filtering(void)
{
    struct kernel_wait_queue queue;kernel_wait_queue_init(&queue);
    struct kernel_task actors[3]={0};struct kernel_wait_token tokens[3];uint64_t masks[2]={1,2};
    for(unsigned i=0;i<3;i++) {
        kernel_wait_record_init(&actors[i].wait);actors[i].wait.on_cpu=1;kernel_wait_node_init(&actors[i].node,&actors[i]);
        selected_task=&actors[i];assert(!kernel_wait_prepare(&queue,0,0,&tokens[i]));actors[i].node.event_mask=&masks[i==1];
    }
    selected_task=0;struct kernel_wait_node callbacks[2];
    for(unsigned i=0;i<2;i++){kernel_wait_node_init_callback(&callbacks[i],event_callback,&event_calls[i]);callbacks[i].event_mask=&masks[i];kernel_wait_queue_add(&queue,&callbacks[i]);}
    struct kernel_wait_cursor cursor={0};
    {KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);assert(kernel_wait_cursor_prepare_event_locked(&queue,&cursor,1,&actors[0]));}
    kernel_wait_cursor_wake(&cursor,1);
    assert(kernel_wait_record_phase(&actors[0].wait)==KERNEL_WAIT_NOTIFIED && kernel_wait_record_phase(&actors[1].wait)==KERNEL_WAIT_PREPARED && kernel_wait_record_phase(&actors[2].wait)==KERNEL_WAIT_PREPARED);
    assert(event_calls[0]==1&&!event_calls[1]);
    {KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);assert(kernel_wait_cursor_prepare_event_locked(&queue,&cursor,1,0));}
    kernel_wait_cursor_wake(&cursor,1);assert(kernel_wait_record_phase(&actors[2].wait)==KERNEL_WAIT_NOTIFIED&&kernel_wait_record_phase(&actors[1].wait)==KERNEL_WAIT_PREPARED);
    /* A mask replacement wakes the whole source, independent of old interest. */
    masks[0]=masks[1]=0;assert(!kernel_wait_queue_wake_all(&queue));assert(kernel_wait_record_phase(&actors[1].wait)==KERNEL_WAIT_NOTIFIED);
    for(unsigned i=0;i<3;i++){selected_task=&actors[i];assert(!kernel_wait_finish(&tokens[i]));assert(!actors[i].node.event_mask);}
    selected_task=0;for(unsigned i=0;i<2;i++)assert(!kernel_wait_node_remove_sync(&callbacks[i]));
    assert(!kernel_wait_queue_close(&queue)&&!kernel_wait_queue_destroy(&queue));
    puts("wait: directed event filters, shared callbacks and mask-change broadcast passed");
}
int main(void)
{
    struct rlimit limit = {0}; assert(!setrlimit(RLIMIT_CORE, &limit));
    initialize(0);
    kernel_wait_domain_init();
    prepared_notify_while_domain_busy();
    prepared_notify_during_park_check();
    published_notifications();
    owner_failures();
    early_and_multi();
    finish_many();
    sync_test_irq = 1;
    arbitration(2);
    assert(sync_test_irq == 1);
    sync_test_irq = 0;
    arbitration(4);
    wake_after_switch = 1;
    arbitration(2);
    arbitration(4);
    callback_lifetime();
    retired_cursor_batches();
    close_lifetime();
    pollfree_lifetime();
    event_filtering();
    committed_segments();
    task_segments();
    notification_sequences();
    puts("wait: early wake, 2/4-thread first-winner arbitration, stale tokens, multi-queue, borrowed retired cursors and callback lifetime passed");
}
