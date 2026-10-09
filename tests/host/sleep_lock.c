#define _POSIX_C_SOURCE 200809L
#include "../../kernel/sched/private.h"
struct host_task { struct kernel_task task; unsigned *dispatch; unsigned id; };
#define host_of(task_) ((struct host_task *)(task_))
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

_Thread_local uintptr_t sync_test_irq = 1;
static _Thread_local struct kernel_cpu cpu;
static _Thread_local struct host_task host_current;
#define current host_current.task
static _Thread_local unsigned ready;
static struct kernel_rwlock lock;
static unsigned order[64], count;
static unsigned entered[64], leave[64], parked[64], granted[64];
static pthread_barrier_t segment_entered, segment_released;
static unsigned segment_enabled, finish_pause;
static pthread_barrier_t finish_entered, finish_released;
void *sync_test_cpu(void) { return &cpu; }
struct kernel_task *kernel_task_current(void) { return &current; }
static void initialize(unsigned id)
{
    kernel_cpu_initialize(&cpu, id, &current.io_context);
    current = (struct kernel_task){0};
    host_current.id = id;
    current.wait.on_cpu = 1;
    kernel_wait_node_init(&current.default_wait_node, &current);
    sync_test_irq = 1;
}
struct kernel_wait_record *kernel_wait_record_of(struct kernel_task *task) { return &task->wait; }
struct kernel_wait_node *kernel_wait_default_node(struct kernel_task *task) { return &task->default_wait_node; }
struct kernel_task *kernel_wait_current_task(void) { return &current; }
int kernel_wait_backend_initialized(void) { return 1; }
int kernel_wait_backend_signal(struct kernel_task *task) { (void)task; return 0; }
uint64_t kernel_wait_backend_time(void) { return 1; }
void kernel_wait_backend_commit(struct kernel_task *task, uint64_t deadline, int interruptible)
{ (void)deadline; (void)interruptible; task->state = KERNEL_THREAD_STATE_BLOCKED; __atomic_fetch_add(&parked[host_of(task)->id - 1], 1, __ATOMIC_RELEASE); }
void kernel_wait_backend_notify(struct kernel_task *task, uint32_t reason)
{ task->wake_reason = reason; task->state = KERNEL_THREAD_STATE_READY; }
void kernel_wait_backend_ready(struct kernel_task *task)
{ __atomic_store_n(&granted[host_of(task)->id - 1], 1, __ATOMIC_RELEASE); __atomic_store_n(host_of(task)->dispatch, 1, __ATOMIC_RELEASE); }
enum kernel_scheduler_status kernel_wait_backend_switch(struct kernel_task *task)
{
    { KERNEL_RAW_SCOPE(guard, &kernel_wait_domain); kernel_wait_switch_finish_locked(task); }
    while (!__atomic_load_n(&ready, __ATOMIC_ACQUIRE)) sched_yield();
    { KERNEL_RAW_SCOPE(guard, &kernel_wait_domain); task->wait.on_cpu = 1; ready = 0; }
    return KERNEL_SCHEDULER_STATUS_OK;
}
void kernel_wait_backend_quiesce(void) { sched_yield(); }
void __real_kernel_raw_lock_acquire(struct kernel_raw_lock *, struct kernel_raw_guard *);
void __wrap_kernel_raw_lock_acquire(struct kernel_raw_lock *raw, struct kernel_raw_guard *guard)
{
    if (raw == &lock.metadata && host_current.id == 1 && current.wait.phase == KERNEL_WAIT_FINISHED &&
        __atomic_load_n(&parked[0], __ATOMIC_ACQUIRE) == 1 &&
        __atomic_exchange_n(&finish_pause, 0, __ATOMIC_ACQ_REL)) {
        int result = pthread_barrier_wait(&finish_entered);
        assert(!result || result == PTHREAD_BARRIER_SERIAL_THREAD);
        result = pthread_barrier_wait(&finish_released);
        assert(!result || result == PTHREAD_BARRIER_SERIAL_THREAD);
    }
    __real_kernel_raw_lock_acquire(raw, guard);
}
void __real_kernel_raw_lock_release(struct kernel_raw_guard *);
void __wrap_kernel_raw_lock_release(struct kernel_raw_guard *guard)
{
    int pause = segment_enabled && guard->lock == &lock.metadata &&
        lock.handoff_owner && lock.readers == 16 && host_current.id == 2;
    __real_kernel_raw_lock_release(guard);
    if (pause) {
        int result = pthread_barrier_wait(&segment_entered);
        assert(!result || result == PTHREAD_BARRIER_SERIAL_THREAD);
        result = pthread_barrier_wait(&segment_released);
        assert(!result || result == PTHREAD_BARRIER_SERIAL_THREAD);
    }
}
static void *worker(void *argument)
{
    unsigned id = (unsigned)(uintptr_t)argument;
    initialize(id + 1);
    host_current.dispatch = &ready;
    struct kernel_lock_guard guard = {0};
    if (id == 1) kernel_rwlock_write(&lock, &guard);
    else kernel_rwlock_read(&lock, &guard);
    assert(sync_test_irq && !cpu.raw_locks && !cpu.preempt_depth);
    { KERNEL_RAW_SCOPE(raw, &lock.metadata); order[count++] = id; }
    __atomic_store_n(&entered[id], 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&leave[id], __ATOMIC_ACQUIRE)) sched_yield();
    kernel_lock_release(&guard);
    assert(!current.io_context.locks && current.wait.phase == KERNEL_WAIT_FINISHED);
    return 0;
}
static void queued(unsigned expected)
{
    for (;;) {
        unsigned actual;
        { KERNEL_RAW_SCOPE(guard, &kernel_wait_domain); actual = lock.waiters.registrations; }
        if (actual == expected) return;
        sched_yield();
    }
}
static void fifo(unsigned waiters)
{
    pthread_t threads[33];
    struct kernel_lock_guard owner = {0};
    kernel_rwlock_init(&lock, 10, 1);
    kernel_rwlock_write(&lock, &owner);
    count = 0;
    finish_pause = waiters == 1;
    if (finish_pause) {
        assert(!pthread_barrier_init(&finish_entered, 0, 2));
        assert(!pthread_barrier_init(&finish_released, 0, 2));
    }
    segment_enabled = waiters == 32;
    if (segment_enabled) {
        assert(!pthread_barrier_init(&segment_entered, 0, 2));
        assert(!pthread_barrier_init(&segment_released, 0, 2));
        entered[32] = leave[32] = parked[32] = granted[32] = 0;
    }
    for (unsigned i = 0; i < waiters; i++) {
        entered[i] = leave[i] = parked[i] = 0;
        assert(!pthread_create(&threads[i], 0, worker, (void *)(uintptr_t)i));
        queued(i + 1); /* deterministic registration order */
        if (!i) {
            while (!__atomic_load_n(&parked[0], __ATOMIC_ACQUIRE)) sched_yield();
            uintptr_t irq = arch_interrupt_save();
            assert(kernel_wait_queue_wake_all(&lock.waiters) == KERNEL_SCHEDULER_STATUS_OK);
            arch_interrupt_restore(irq);
            if (waiters == 1) {
                int result = pthread_barrier_wait(&finish_entered);
                assert(!result || result == PTHREAD_BARRIER_SERIAL_THREAD);
                kernel_lock_release(&owner);
                result = pthread_barrier_wait(&finish_released);
                assert(!result || result == PTHREAD_BARRIER_SERIAL_THREAD);
            } else while (__atomic_load_n(&parked[0], __ATOMIC_ACQUIRE) < 2) sched_yield();
            if (waiters > 1) assert(!__atomic_load_n(&entered[0], __ATOMIC_ACQUIRE));
        }
    }
    if (owner.lock) kernel_lock_release(&owner);
    while (!__atomic_load_n(&entered[0], __ATOMIC_ACQUIRE)) sched_yield();
    if (waiters > 1) {
        assert(!__atomic_load_n(&entered[1], __ATOMIC_ACQUIRE));
        for (unsigned i = 2; i < waiters; i++) assert(!__atomic_load_n(&entered[i], __ATOMIC_ACQUIRE));
    }
    __atomic_store_n(&leave[0], 1, __ATOMIC_RELEASE);
    assert(!pthread_join(threads[0], 0));
    if (waiters > 1) {
        while (!__atomic_load_n(&entered[1], __ATOMIC_ACQUIRE)) sched_yield();
        for (unsigned i = 2; i < waiters; i++) assert(!__atomic_load_n(&entered[i], __ATOMIC_ACQUIRE));
        __atomic_store_n(&leave[1], 1, __ATOMIC_RELEASE);
        if (segment_enabled) {
            int result = pthread_barrier_wait(&segment_entered);
            assert(!result || result == PTHREAD_BARRIER_SERIAL_THREAD);
            assert(!pthread_create(&threads[32], 0, worker, (void *)(uintptr_t)32));
            while (!__atomic_load_n(&parked[32], __ATOMIC_ACQUIRE)) sched_yield();
            assert(!__atomic_load_n(&entered[32], __ATOMIC_ACQUIRE));
            result = pthread_barrier_wait(&segment_released);
            assert(!result || result == PTHREAD_BARRIER_SERIAL_THREAD);
        }
        assert(!pthread_join(threads[1], 0));
        if (segment_enabled) assert(!__atomic_load_n(&granted[32], __ATOMIC_ACQUIRE));
    }
    for (unsigned i = 2; i < waiters; i++) {
        while (!__atomic_load_n(&entered[i], __ATOMIC_ACQUIRE)) sched_yield();
        __atomic_store_n(&leave[i], 1, __ATOMIC_RELEASE);
    }
    for (unsigned i = 2; i < waiters; i++) assert(!pthread_join(threads[i], 0));
    if (segment_enabled) {
        while (!__atomic_load_n(&entered[32], __ATOMIC_ACQUIRE)) sched_yield();
        __atomic_store_n(&leave[32], 1, __ATOMIC_RELEASE);
        assert(!pthread_join(threads[32], 0));
        assert(!pthread_barrier_destroy(&segment_entered) && !pthread_barrier_destroy(&segment_released));
    }
    assert(count == waiters + (waiters == 32) && order[0] == 0 && (waiters == 1 || order[1] == 1));
    assert(!lock.writer && !lock.readers && !lock.pending_head && !lock.pending_tail);
    assert(!lock.waiters.registrations && !lock.waiters.borrows);
    if (waiters == 1) assert(!pthread_barrier_destroy(&finish_entered) && !pthread_barrier_destroy(&finish_released));
}
static void fatal_cases(void)
{
    struct rlimit limit = {0}; assert(!setrlimit(RLIMIT_CORE, &limit));
    for (unsigned which = 0; which < 5; which++) {
        pid_t child = fork(); assert(child >= 0);
        if (!child) {
            struct kernel_lock_guard a = {0}, b = {0};
            struct kernel_rwlock other;
            kernel_rwlock_init(&lock, 10, 1);
            kernel_rwlock_init(&other, 9, 1);
            kernel_rwlock_write(&lock, &a);
            if (!which) kernel_rwlock_read(&lock, &b);
            else if (which == 1) kernel_rwlock_write(&other, &b);
            else if (which == 2) { kernel_lock_release(&a); kernel_lock_release(&a); }
            else if (which == 3) { a.owner = 0; kernel_lock_release(&a); }
            else { kernel_preempt_disable(); kernel_rwlock_write(&other, &b); }
            _exit(1);
        }
        int status; assert(waitpid(child, &status, 0) == child);
        assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGILL);
    }
}
int main(void)
{
    initialize(0);
    kernel_wait_domain_init();
    fatal_cases();
    for (unsigned irq = 0; irq < 2; irq++) {
        sync_test_irq = irq;
        struct kernel_lock_guard guard = {0};
        kernel_rwlock_init(&lock, 10, 1);
        assert(kernel_rwlock_try_read(&lock, &guard));
        kernel_lock_release(&guard);
        assert(sync_test_irq == irq && !cpu.raw_locks && !cpu.preempt_depth);
    }
    sync_test_irq = 1;
    fifo(1); fifo(8); fifo(32);
    puts("sleep lock: deterministic FIFO writer boundary, reserved grants and 1/8/32 waiters passed");
}
