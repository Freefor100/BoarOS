/* Task-only cache exclusion: UP preemption and shared SMP ownership. */
#include <ext4_bcache.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/resource.h>
static _Thread_local struct kernel_cpu cpu={.initialized=KERNEL_CPU_INITIALIZED};
_Thread_local uintptr_t sync_test_irq=2;
void *sync_test_cpu(void) { return &cpu; }
void *sync_test_current_task(void) { return NULL; }
static unsigned raw_acquires;
void __real_kernel_raw_lock_acquire(struct kernel_raw_lock *,struct kernel_raw_guard *);
void __wrap_kernel_raw_lock_acquire(struct kernel_raw_lock *lock,struct kernel_raw_guard *guard)
{
    __atomic_fetch_add(&raw_acquires,1,__ATOMIC_RELAXED);
    __real_kernel_raw_lock_acquire(lock,guard);
}
static struct kernel_raw_lock object;
static void finish_early(void)
{
    uintptr_t old=sync_test_irq;
    KERNEL_TASK_LOCK_SCOPE(guard,&object);
#if BOAROS_MAX_CPUS==1 && !BOAROS_LOCK_DEBUG && !BOAROS_COST_DIAGNOSTICS
    assert(sync_test_irq==old);
#else
    (void)old;assert(!sync_test_irq);
#endif
}
#if BOAROS_MAX_CPUS>1
static unsigned shared;
static void *increment(void *unused)
{
    (void)unused;
    for (unsigned i=0;i<20000;i++) {
        KERNEL_TASK_LOCK_SCOPE(guard,&object);
        assert(!sync_test_irq);
        shared++;
    }
    assert(sync_test_irq==2 && !cpu.raw_locks && !cpu.preempt_depth);
    return NULL;
}
#endif
static void invalid_object(int recursive)
{
    pid_t child=fork();assert(child>=0);
    if (!child) {
        if (recursive) { KERNEL_TASK_LOCK_SCOPE(guard,&object);finish_early(); }
        else { struct kernel_raw_lock invalid={0};KERNEL_TASK_LOCK_SCOPE(guard,&invalid); }
        _exit(99);
    }
    int status;assert(waitpid(child,&status,0)==child && WIFSIGNALED(status) && WTERMSIG(status)==SIGILL);
}
int main(void)
{
    struct rlimit no_core={0,0};assert(!setrlimit(RLIMIT_CORE,&no_core));
    struct kernel_raw_lock outer;
    kernel_raw_lock_init(&outer,KERNEL_RAW_RANK_PAGE);
    kernel_raw_lock_init(&object,KERNEL_RAW_RANK_OBJECT);
    invalid_object(0);
#if BOAROS_MAX_CPUS==1 || BOAROS_LOCK_DEBUG
    invalid_object(1);
#endif
    raw_acquires=0;
    {
        KERNEL_TASK_LOCK_SCOPE(guard,&object);
        /* Default UP need not mask device interrupts for task-only cache state. */
#if BOAROS_MAX_CPUS==1 && !BOAROS_LOCK_DEBUG && !BOAROS_COST_DIAGNOSTICS
        /* No cross-CPU owner exists; default UP needs only its saved IRQ state. */
        assert(sync_test_irq==2 && !cpu.raw_locks && cpu.preempt_depth==1 && !raw_acquires);
#else
        assert(!sync_test_irq && cpu.raw_locks && cpu.preempt_depth==1 && raw_acquires==1);
#endif
        kernel_task_lock_release(&guard);
        assert(sync_test_irq==2 && !cpu.raw_locks && !cpu.preempt_depth);
        kernel_task_lock_acquire(&object,&guard);
    }
    assert(sync_test_irq==2 && !cpu.raw_locks && !cpu.preempt_depth);
    pid_t child=fork();assert(child>=0);
    if (!child) { KERNEL_TASK_LOCK_SCOPE(guard,&object);kernel_assert_can_block();_exit(99); }
    int status;assert(waitpid(child,&status,0)==child && WIFSIGNALED(status) && WTERMSIG(status)==SIGILL);
    {
        KERNEL_IRQ_LOCK_SCOPE(guard,&object);
        assert(!sync_test_irq && cpu.preempt_depth==1);
        kernel_irq_lock_release(&guard);
        assert(sync_test_irq==2 && !cpu.preempt_depth && !cpu.raw_locks);
        kernel_irq_lock_acquire(&object,&guard);
    }
    assert(sync_test_irq==2 && !cpu.preempt_depth && !cpu.raw_locks);
    finish_early();assert(sync_test_irq==2);
    sync_test_irq=0;finish_early();assert(!sync_test_irq);sync_test_irq=2;
    {
        KERNEL_RAW_SCOPE(guard,&outer);
        struct kernel_raw_guard *held=cpu.raw_locks;
        unsigned depth=cpu.preempt_depth;
        finish_early();
        assert(!sync_test_irq && cpu.raw_locks==held && cpu.preempt_depth==depth);
    }
    assert(sync_test_irq==2 && !cpu.raw_locks && !cpu.preempt_depth);
#if BOAROS_MAX_CPUS>1
    pthread_t a,b;assert(!pthread_create(&a,NULL,increment,NULL));assert(!pthread_create(&b,NULL,increment,NULL));
    assert(!pthread_join(a,NULL) && !pthread_join(b,NULL) && shared==40000);
#endif
    puts("bcache exclusion: IRQ preservation/nesting/early return/reacquire, cannot block and SMP shared updates passed");
}
