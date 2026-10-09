#define _POSIX_C_SOURCE 200809L
#include <kernel/cpu.h>
#include <kernel/raw_lock.h>
#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

_Thread_local uintptr_t sync_test_irq = 1;
static _Thread_local struct kernel_cpu cpu;
void *sync_test_cpu(void) { return &cpu; }
static struct kernel_raw_lock lock, inner;
static unsigned long count;
static pthread_barrier_t start;

static void initialize(unsigned id)
{ kernel_cpu_initialize(&cpu, id, 0); sync_test_irq = 1; }
static void *worker(void *argument)
{
    initialize((unsigned)(uintptr_t)argument);
    int status = pthread_barrier_wait(&start);
    assert(status == 0 || status == PTHREAD_BARRIER_SERIAL_THREAD);
    for (unsigned i = 0; i < 20000; i++) {
        KERNEL_RAW_SCOPE(guard, &lock);
        assert(!sync_test_irq && cpu.preempt_depth && cpu.raw_locks);
        count++;
    }
    assert(sync_test_irq && !cpu.preempt_depth && !cpu.raw_locks);
    return 0;
}
static void misuse(unsigned which)
{
    struct kernel_raw_guard a = {0}, b = {0};
    initialize(0);
    kernel_raw_lock_init(&lock, KERNEL_RAW_RANK_HEAP);
    kernel_raw_lock_init(&inner, KERNEL_RAW_RANK_PAGE);
    kernel_raw_lock_acquire(&lock, &a);
    switch (which) {
    case 0: kernel_raw_lock_acquire(&lock, &b); break;
    case 1: kernel_raw_lock_release(&a); kernel_raw_lock_release(&a); break;
    case 2: { struct kernel_cpu wrong; kernel_cpu_initialize(&wrong, 1, 0); a.cpu = &wrong;
              kernel_raw_lock_release(&a); break; }
    case 3: kernel_raw_lock_acquire(&inner, &b); kernel_raw_lock_release(&a); break;
    case 4: kernel_assert_can_block(); break;
    case 5: cpu.preempt_depth = 0; kernel_raw_lock_release(&a); break;
    case 6: kernel_raw_lock_release(&a); kernel_raw_lock_acquire(&inner, &a);
            kernel_raw_lock_acquire(&lock, &b); break;
    case 7: kernel_raw_lock_release(&a); cpu.preempt_depth = UINT32_MAX;
            kernel_preempt_disable(); break;
    case 8: kernel_raw_lock_release(&a); kernel_preempt_enable(); break;
    case 9: kernel_raw_lock_release(&a); kernel_preempt_disable(); kernel_assert_can_block(); break;
    default: assert(0);
    }
    _exit(1);
}
int main(void)
{
    struct rlimit limit = {0}; assert(setrlimit(RLIMIT_CORE, &limit) == 0);
    initialize(73);
    assert(kernel_cpu_current() == &cpu && cpu.hardware_id == 73);
    kernel_raw_lock_init(&lock, KERNEL_RAW_RANK_HEAP);
    kernel_raw_lock_init(&inner, KERNEL_RAW_RANK_PAGE);
    for (unsigned enabled = 0; enabled < 2; enabled++) {
        sync_test_irq = enabled;
        { KERNEL_RAW_SCOPE(a, &lock);
          { KERNEL_RAW_SCOPE(b, &inner); assert(cpu.preempt_depth == 2); }
          assert(!sync_test_irq && cpu.preempt_depth == 1); }
        assert(sync_test_irq == enabled && !cpu.raw_locks && !cpu.preempt_depth);
    }
    for (unsigned which = 0; which < 10; which++) {
        pid_t child = fork(); assert(child >= 0);
        if (!child) misuse(which);
        int status; assert(waitpid(child, &status, 0) == child);
        assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGILL);
    }
    kernel_raw_lock_init(&lock, KERNEL_RAW_RANK_HEAP);
    assert(pthread_barrier_init(&start, 0, 4) == 0);
    pthread_t threads[4];
    for (unsigned i = 0; i < 4; i++) assert(pthread_create(&threads[i], 0, worker, (void *)(uintptr_t)i) == 0);
    for (unsigned i = 0; i < 4; i++) assert(pthread_join(threads[i], 0) == 0);
    assert(count == 80000);
    assert(pthread_barrier_destroy(&start) == 0);
    puts("CPU/raw synchronization: mutual exclusion, visibility, IRQ restoration and 10 fatal cases passed");
}
