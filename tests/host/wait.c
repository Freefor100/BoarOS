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
static struct kernel_sched_runqueue runnable;
static pthread_barrier_t committed, notified;
static struct kernel_wait_token shared_token;
static unsigned wake_after_switch;
static int switching;
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
struct kernel_task *kernel_wait_current_task(void) { return &task; }
int kernel_wait_backend_initialized(void) { return 1; }
int kernel_wait_backend_signal(struct kernel_task *t) { (void)t; return 0; }
uint64_t kernel_wait_backend_time(void) { return 1; }
void kernel_wait_backend_commit(struct kernel_task *t, uint64_t deadline, int interruptible)
{ (void)deadline; (void)interruptible; assert(!t->blocked); t->blocked = 1; }
void kernel_wait_backend_notify(struct kernel_task *t, uint32_t reason)
{ (void)reason; assert(t->blocked == 1); t->blocked = 0; t->completions++; }
void kernel_wait_backend_ready(struct kernel_task *t)
{ assert(!t->ready_node.queued && !t->wait.on_cpu); kernel_sched_enqueue(&runnable, &t->ready_node, t, 0, 0); }
enum kernel_scheduler_status kernel_wait_backend_switch(struct kernel_task *t)
{
    assert(switching && !cpu.raw_locks && !cpu.preempt_depth);
    if (wake_after_switch) {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        kernel_wait_switch_finish_locked(t);
        assert(!t->ready_node.queued);
    }
    barrier(&committed);
    barrier(&notified);
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    assert(t->wait.phase == KERNEL_WAIT_NOTIFIED && t->completions == 1);
    if (!wake_after_switch) {
        assert(t->wait.on_cpu && !t->ready_node.queued && t->wait.ready_pending);
        kernel_wait_switch_finish_locked(t);
    }
    assert(t->ready_node.queued && !t->wait.on_cpu);
    kernel_sched_dequeue(&runnable, &t->ready_node);
    t->wait.on_cpu = 1;
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
    switching = 0;
    for (unsigned i = 0; i < count; i++) assert(!pthread_join(workers[i], 0));
    assert(reason <= KERNEL_WAIT_SIGNALLED);
    struct kernel_wait_token old = shared_token;
    assert(kernel_wait_finish(&shared_token) == KERNEL_SCHEDULER_STATUS_OK);
    assert(!pthread_barrier_destroy(&committed) && !pthread_barrier_destroy(&notified));
    assert(kernel_wait_prepare(&queue, 0, 0, &shared_token) == KERNEL_SCHEDULER_STATUS_OK);
    assert(kernel_wait_notify(&old, KERNEL_WAIT_TIMEOUT) == KERNEL_SCHEDULER_STATUS_OK);
    assert(task.wait.phase == KERNEL_WAIT_PREPARED);
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
    assert(kernel_wait_queue_wake_all(&callbacks) == KERNEL_SCHEDULER_STATUS_OK);
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
    task.wait.generation = UINT64_MAX;
    assert(kernel_wait_prepare(0, 0, 0, &token) == KERNEL_SCHEDULER_STATUS_INVALID_STATE);
    assert(!task.wait.nodes && task.wait.phase == KERNEL_WAIT_FINISHED);
    task.wait.generation = 0;
}
int main(void)
{
    struct rlimit limit = {0}; assert(!setrlimit(RLIMIT_CORE, &limit));
    initialize(0);
    kernel_wait_domain_init();
    early_and_multi();
    arbitration(2);
    arbitration(4);
    wake_after_switch = 1;
    arbitration(2);
    arbitration(4);
    callback_lifetime();
    puts("wait: early wake, 2/4-thread first-winner arbitration, stale tokens, multi-queue, borrowed retired cursors and callback lifetime passed");
}
