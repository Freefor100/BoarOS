#define _POSIX_C_SOURCE 200809L
#include "../../kernel/sched/private.h"
struct host_task { struct kernel_task task; unsigned *dispatch; unsigned id; };
#define host_of(task_) ((struct host_task *)(task_))
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include <kernel/time.h>

_Thread_local uintptr_t sync_test_irq = 1;
_Thread_local unsigned sync_test_irq_restores;
static _Thread_local unsigned segment_restores;
static _Thread_local struct kernel_cpu cpu;
static void publish_task(struct kernel_cpu *target,struct kernel_task *task) { target->current = task; }
static _Thread_local struct host_task host_current;
static _Thread_local struct kernel_io_context bootstrap_io;
static _Thread_local unsigned publishing_next;
static _Thread_local unsigned notification_flushes;
#define current host_current.task
static _Thread_local unsigned ready;
static struct kernel_rwlock lock;
static unsigned order[64], count;
static unsigned entered[64], leave[64], parked[64], granted[64];
static pthread_barrier_t segment_entered, segment_released;
static unsigned segment_enabled, finish_pause;
static unsigned handoff_irq = 1;
static pthread_barrier_t finish_entered, finish_released;
static unsigned spin_mode,spin_clock_entered,spin_clock_resume,spin_metadata_seen;
static unsigned spin_long_expected,spin_long_seen,spin_clock_finish;
static _Thread_local unsigned spin_clock_reads;
static uintptr_t spin_irq;
static struct kernel_cpu spin_cpus[2];
uint64_t riscv_time_read(void)
{
    if(!spin_mode || host_current.id!=1)return 0;
    spin_clock_reads++;
    assert(sync_test_irq==spin_irq && !cpu.raw_locks && !cpu.preempt_depth);
    if(spin_clock_reads==1) {
        __atomic_store_n(&spin_clock_entered,1,__ATOMIC_RELEASE);
        while(!__atomic_load_n(&spin_clock_resume,__ATOMIC_ACQUIRE))sched_yield();
    }
    if(spin_mode==4 && spin_clock_reads>2) {
        if(spin_clock_reads==4) {
            assert(spin_long_expected);
            __atomic_store_n(&spin_long_seen,1,__ATOMIC_RELEASE);
            while(!__atomic_load_n(&spin_clock_finish,__ATOMIC_ACQUIRE))sched_yield();
        }
        return 100000;
    }
    if(spin_mode==5 && spin_clock_reads>2)return 1000001;
    return spin_mode==2 && spin_clock_reads>2?UINT64_C(1000000000):0;
}
void *sync_test_cpu(void) { return &cpu; }
void kernel_cpu_flush_notifications(void)
{
    assert(!cpu.raw_locks);
    notification_flushes++;
    cpu.outbound=0;
}
struct kernel_task *kernel_task_current(void) { return publishing_next ? NULL : &current; }
void *sync_test_current_task(void) { return &current; }
static void initialize(unsigned id)
{
    kernel_cpu_initialize(&cpu, id, &bootstrap_io);
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
{ (void)reason; task->state = KERNEL_THREAD_STATE_READY; }
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
    if(spin_mode==1 && raw==&lock.metadata && host_current.id==1 &&
       kernel_wait_record_phase(&current.wait)==KERNEL_WAIT_FINISHED && spin_clock_reads)
        __atomic_store_n(&spin_metadata_seen,1,__ATOMIC_RELEASE);
    if (segment_enabled && raw == &lock.metadata && lock.handoff_owner &&
        lock.readers == 16 && host_current.id == 2) {
        assert((handoff_irq ? sync_test_irq_restores > segment_restores :
            sync_test_irq_restores == segment_restores) && !cpu.raw_locks && !cpu.preempt_depth);
        int result = pthread_barrier_wait(&segment_entered);
        assert(!result || result == PTHREAD_BARRIER_SERIAL_THREAD);
        result = pthread_barrier_wait(&segment_released);
        assert(!result || result == PTHREAD_BARRIER_SERIAL_THREAD);
    }
    if (raw == &lock.metadata && host_current.id == 1 && kernel_wait_record_phase(&current.wait) == KERNEL_WAIT_FINISHED &&
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
    if(spin_mode==4 && spin_long_expected && guard->lock==&lock.metadata &&
       host_current.id==0 && lock.writer && lock.writer!=&current.io_context && !lock.pending_head)
        __atomic_store_n(&spin_clock_finish,1,__ATOMIC_RELEASE);
    if(spin_mode==1 && guard->lock==&lock.metadata && host_current.id==0 &&
       lock.writer && lock.writer!=&current.io_context && !lock.pending_head) {
        __atomic_store_n(&spin_clock_resume,1,__ATOMIC_RELEASE);
        while(!__atomic_load_n(&spin_metadata_seen,__ATOMIC_ACQUIRE))sched_yield();
    }
    if (segment_enabled && guard->lock == &lock.metadata &&
        lock.handoff_owner && lock.readers == 16 && host_current.id == 2)
        segment_restores = sync_test_irq_restores;
    __real_kernel_raw_lock_release(guard);
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
    if (id == 1) sync_test_irq = handoff_irq;
    kernel_lock_release(&guard);
    assert(sync_test_irq == (id == 1 ? handoff_irq : 1));
    assert(!current.io_context.locks && kernel_wait_record_phase(&current.wait) == KERNEL_WAIT_FINISHED);
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
static unsigned try_mode, try_entered, try_leave;
static void *try_owner(void *unused)
{
    (void)unused;
    initialize(1);
    struct kernel_lock_guard guard = {0};
    if (try_mode) kernel_rwlock_write(&lock, &guard);
    else kernel_rwlock_read(&lock, &guard);
    __atomic_store_n(&try_entered, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&try_leave, __ATOMIC_ACQUIRE)) sched_yield();
    kernel_lock_release(&guard);
    return 0;
}
static void try_write_busy(void)
{
    for (unsigned irq = 0; irq < 2; irq++) for (try_mode = 0; try_mode < 2; try_mode++) {
        kernel_rwlock_init(&lock, 10, 1);
        try_entered = try_leave = 0;
        pthread_t owner;
        assert(!pthread_create(&owner, 0, try_owner, 0));
        while (!__atomic_load_n(&try_entered, __ATOMIC_ACQUIRE)) sched_yield();
        sync_test_irq = irq;
        struct kernel_lock_guard guard = {0}, saved = guard;
        assert(!kernel_rwlock_try_write(&lock, &guard));
        assert(!memcmp(&guard, &saved, sizeof(guard)) && !current.io_context.locks);
        assert(sync_test_irq == irq && !cpu.raw_locks && !cpu.preempt_depth);
        assert(!lock.waiters.registrations && !lock.pending_head && !lock.handoff_owner);
        __atomic_store_n(&try_leave, 1, __ATOMIC_RELEASE);
        assert(!pthread_join(owner, 0));
        assert(kernel_rwlock_try_write(&lock, &guard));
        assert(lock.writer == &current.io_context && !lock.readers);
        kernel_lock_release(&guard);
        assert(sync_test_irq == irq && !current.io_context.locks);
    }
}
static void *spin_waiter(void *unused)
{
    (void)unused;initialize(1);host_current.dispatch=&ready;spin_clock_reads=0;
    sync_test_irq=spin_irq;
    struct kernel_lock_guard guard={0};kernel_rwlock_write(&lock,&guard);
    assert(sync_test_irq==spin_irq && guard.owner==&current.io_context && guard.write &&
           kernel_wait_record_phase(&current.wait)==KERNEL_WAIT_FINISHED && !cpu.raw_locks && !cpu.preempt_depth);
    assert(spin_mode==1?!parked[0]:parked[0]==1);
    __atomic_store_n(&entered[0],1,__ATOMIC_RELEASE);
    kernel_lock_release(&guard);
    assert(sync_test_irq==spin_irq && !current.io_context.locks);
    return NULL;
}
static void queued_spin_case(unsigned mode,unsigned parallel,uintptr_t irq)
{
    struct kernel_lock_guard owner={0};pthread_t thread;
    kernel_rwlock_init(&lock,10,1);kernel_rwlock_write(&lock,&owner);
    memset(spin_cpus,0,sizeof(spin_cpus));spin_cpus[0].state=KERNEL_CPU_ONLINE;
    spin_cpus[1].state=KERNEL_CPU_ONLINE;spin_cpus[1].logical_id=1;
    spin_cpus[0].scheduler_next=parallel?&spin_cpus[1]:NULL;
    scheduler.cpus=spin_cpus;spin_mode=mode;spin_irq=irq;
    spin_clock_entered=spin_clock_resume=spin_metadata_seen=0;
    entered[0]=parked[0]=granted[0]=0;
    assert(!pthread_create(&thread,NULL,spin_waiter,NULL));queued(1);
    if(parallel) {
        while(!__atomic_load_n(&spin_clock_entered,__ATOMIC_ACQUIRE) &&
              !__atomic_load_n(&parked[0],__ATOMIC_ACQUIRE))sched_yield();
        assert(__atomic_load_n(&spin_clock_entered,__ATOMIC_ACQUIRE));
        if(mode==2)__atomic_store_n(&spin_clock_resume,1,__ATOMIC_RELEASE);
    }
    if(mode==2 || !parallel)
        while(!__atomic_load_n(&parked[0],__ATOMIC_ACQUIRE))sched_yield();
    kernel_lock_release(&owner);assert(!pthread_join(thread,NULL));
    assert(entered[0] && !lock.writer && !lock.readers && !lock.pending_head &&
           !lock.waiters.registrations && !lock.waiters.borrows &&
           (!parallel?!spin_clock_entered:mode!=1 || spin_metadata_seen));
    scheduler.cpus=NULL;spin_mode=0;
}
static void queued_spin_fifo(void)
{
    struct kernel_lock_guard owner={0};pthread_t threads[3];
    kernel_rwlock_init(&lock,10,1);kernel_rwlock_write(&lock,&owner);
    memset(spin_cpus,0,sizeof(spin_cpus));spin_cpus[0].state=spin_cpus[1].state=KERNEL_CPU_ONLINE;
    spin_cpus[1].logical_id=1;spin_cpus[0].scheduler_next=&spin_cpus[1];scheduler.cpus=spin_cpus;
    spin_mode=3;spin_irq=1;spin_clock_entered=spin_clock_resume=spin_metadata_seen=0;count=0;
    for(unsigned i=0;i<3;i++) {
        entered[i]=leave[i]=parked[i]=granted[i]=0;
        assert(!pthread_create(&threads[i],NULL,worker,(void *)(uintptr_t)i));queued(i+1);
        if(!i)while(!__atomic_load_n(&spin_clock_entered,__ATOMIC_ACQUIRE))sched_yield();
    }
    __atomic_store_n(&spin_clock_resume,1,__ATOMIC_RELEASE);kernel_lock_release(&owner);
    for(unsigned i=0;i<3;i++) {
        while(!__atomic_load_n(&entered[i],__ATOMIC_ACQUIRE))sched_yield();
        if(i<2)assert(!__atomic_load_n(&entered[i+1],__ATOMIC_ACQUIRE));
        __atomic_store_n(&leave[i],1,__ATOMIC_RELEASE);assert(!pthread_join(threads[i],NULL));
    }
    assert(count==3 && order[0]==0 && order[1]==1 && order[2]==2 &&
           !lock.writer && !lock.readers && !lock.pending_head && !lock.pending_tail &&
           !lock.waiters.registrations && !lock.waiters.borrows);
    scheduler.cpus=NULL;spin_mode=0;
}
enum spin_owner_case { SPIN_REMOTE_RUNNING,SPIN_REMOTE_BLOCKED,SPIN_SAME_CPU,
    SPIN_NOT_HEAD,SPIN_READERS,SPIN_REMOTE_READY };
static void *spin_budget_waiter(void *unused)
{
    (void)unused;initialize(1);cpu.logical_id=1;host_current.dispatch=&ready;spin_clock_reads=0;
    sync_test_irq=spin_irq;
    struct kernel_lock_guard guard={0};kernel_rwlock_write(&lock,&guard);
    assert(sync_test_irq==spin_irq && guard.owner==&current.io_context && guard.write &&
           kernel_wait_record_phase(&current.wait)==KERNEL_WAIT_FINISHED && !cpu.raw_locks && !cpu.preempt_depth &&
           (spin_long_expected?!parked[0]:parked[0]==1));
    __atomic_store_n(&entered[0],1,__ATOMIC_RELEASE);kernel_lock_release(&guard);
    assert(sync_test_irq==spin_irq && !current.io_context.locks);return NULL;
}
static void queued_spin_owner_case(enum spin_owner_case kind,unsigned expire,uintptr_t irq)
{
    struct kernel_lock_guard owner={0};pthread_t thread,prior;
    kernel_rwlock_init(&lock,10,1);
    if(kind==SPIN_READERS)kernel_rwlock_read(&lock,&owner);
    else kernel_rwlock_write(&lock,&owner);
    memset(spin_cpus,0,sizeof(spin_cpus));spin_cpus[0].state=spin_cpus[1].state=KERNEL_CPU_ONLINE;
    spin_cpus[1].logical_id=1;spin_cpus[0].scheduler_next=&spin_cpus[1];publish_task(&spin_cpus[0],&current);
    current.state=kind==SPIN_REMOTE_BLOCKED?KERNEL_THREAD_STATE_BLOCKED:
        kind==SPIN_REMOTE_READY?KERNEL_THREAD_STATE_READY:KERNEL_THREAD_STATE_RUNNING;
    current.wait.on_cpu=kind!=SPIN_REMOTE_READY;
    if(kind==SPIN_SAME_CPU) {publish_task(&spin_cpus[0],NULL);publish_task(&spin_cpus[1],&current);}
    scheduler.cpus=spin_cpus;spin_mode=expire?5:4;spin_irq=irq;
    spin_long_expected=kind==SPIN_REMOTE_RUNNING && !expire;
    spin_clock_entered=spin_clock_resume=spin_long_seen=spin_clock_finish=0;
    for(unsigned i=0;i<2;i++)entered[i]=leave[i]=parked[i]=granted[i]=0;
    if(kind==SPIN_NOT_HEAD) {
        assert(!pthread_create(&prior,NULL,worker,(void *)(uintptr_t)1));queued(1);
    }
    assert(!pthread_create(&thread,NULL,spin_budget_waiter,NULL));queued(kind==SPIN_NOT_HEAD?2:1);
    if(kind==SPIN_NOT_HEAD) {
        while(!__atomic_load_n(&parked[0],__ATOMIC_ACQUIRE) &&
              !__atomic_load_n(&spin_clock_entered,__ATOMIC_ACQUIRE))sched_yield();
        assert(parked[0] && !spin_clock_entered);
    } else {
        while(!__atomic_load_n(&spin_clock_entered,__ATOMIC_ACQUIRE))sched_yield();
        __atomic_store_n(&spin_clock_resume,1,__ATOMIC_RELEASE);
    }
    if(spin_long_expected) {
        while(!__atomic_load_n(&spin_long_seen,__ATOMIC_ACQUIRE) &&
              !__atomic_load_n(&parked[0],__ATOMIC_ACQUIRE))sched_yield();
        assert(spin_long_seen && !parked[0]);
    } else while(!__atomic_load_n(&parked[0],__ATOMIC_ACQUIRE))sched_yield();
    kernel_lock_release(&owner);
    if(kind==SPIN_NOT_HEAD) {
        while(!__atomic_load_n(&entered[1],__ATOMIC_ACQUIRE))sched_yield();
        assert(!__atomic_load_n(&entered[0],__ATOMIC_ACQUIRE));
        __atomic_store_n(&leave[1],1,__ATOMIC_RELEASE);assert(!pthread_join(prior,NULL));
    }
    assert(!pthread_join(thread,NULL));
    assert(entered[0] && !lock.writer && !lock.readers && !lock.pending_head &&
           !lock.pending_tail && !lock.waiters.registrations && !lock.waiters.borrows);
    current.state=KERNEL_THREAD_STATE_RUNNING;current.wait.on_cpu=1;
    scheduler.cpus=NULL;spin_mode=spin_long_expected=0;
}
int main(void)
{
    initialize(0);
    /* CPU publication can name the next task while this stack and tp still
     * belong to the previous owner. Its I/O qualifications stay with it. */
    struct kernel_task next = {0};
    publish_task(&cpu,&next); publishing_next = 1;
    assert(kernel_io_context_current() == &current.io_context && sync_test_irq == 1);
    publishing_next = 0; publish_task(&cpu,&current);
    struct kernel_raw_lock outer, inner;
    struct kernel_raw_guard first={0}, second={0};
    kernel_raw_lock_init(&outer,KERNEL_RAW_RANK_OBJECT);
    kernel_raw_lock_init(&inner,KERNEL_RAW_RANK_SCHEDULER);
    notification_flushes=0;
    kernel_raw_lock_acquire(&outer,&first);kernel_raw_lock_release(&first);
    assert(!notification_flushes);
    kernel_raw_lock_acquire(&outer,&first);
    cpu.outbound=2;
    kernel_raw_lock_acquire(&inner,&second);kernel_raw_lock_release(&second);
    assert(!notification_flushes && cpu.outbound==2);
    kernel_raw_lock_release(&first);
    assert(notification_flushes==1 && !cpu.outbound && sync_test_irq==1);
    kernel_wait_domain_init();
    fatal_cases();
    try_write_busy();
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
    handoff_irq = 0; fifo(32);
    assert(!kernel_time_init(1000000000U,0));
    for(uintptr_t irq=0;irq<2;irq++) {
        queued_spin_case(1,1,irq);queued_spin_case(2,1,irq);queued_spin_case(2,0,irq);
    }
    queued_spin_fifo();
    for(uintptr_t irq=0;irq<2;irq++) {
        for(unsigned kind=SPIN_REMOTE_RUNNING;kind<=SPIN_REMOTE_READY;kind++)
            queued_spin_owner_case(kind,0,irq);
        queued_spin_owner_case(SPIN_REMOTE_RUNNING,1,irq);
    }
    puts("sleep lock: queued writers park without spinning; FIFO head behind a remote running writer receives extended budget; blocked/ready/same-CPU/readers retain base; all owners return");
    puts("sleep lock: queued grant avoids park/MM switch; 50us expiry and real single-CPU inventory retain sleeping path; IRQ, final notifier borrow and owners preserved");
    puts("sleep lock: deterministic FIFO writer boundary, reserved grants and 1/8/32 waiters passed");
}
