#define _POSIX_C_SOURCE 200809L
#include <kernel/cpu.h>
#include <kernel/raw_lock.h>
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

_Thread_local uintptr_t sync_test_irq = 1;
static _Thread_local struct kernel_cpu cpu;
void *sync_test_cpu(void) { return &cpu; }
static struct kernel_raw_lock lock, inner;
static void initialize(unsigned id)
{ kernel_cpu_initialize(&cpu, id, 0); sync_test_irq = 1; }
#if BOAROS_MAX_CPUS > 1
static unsigned long count;
static pthread_barrier_t start;
static struct kernel_raw_rwlock views;
static unsigned view_entered, view_release, writer_owned, writer_release, late_attempted;
static unsigned late_entered, published;
static void await(unsigned *value, unsigned expected)
{ while (__atomic_load_n(value, __ATOMIC_ACQUIRE) != expected) sched_yield(); }
static void *view_reader(void *argument)
{
    unsigned id = (uintptr_t)argument;
    initialize(id);
    KERNEL_RAW_READ_SCOPE(guard, &views);
    assert(!sync_test_irq && cpu.preempt_depth && cpu.raw_locks == &guard.raw);
    __atomic_fetch_or(&view_entered, 1U << id, __ATOMIC_RELEASE);
    while (!(__atomic_load_n(&view_release, __ATOMIC_ACQUIRE) & (1U << id))) sched_yield();
    assert(!published);
    return NULL;
}
static void *view_writer(void *unused)
{
    (void)unused; initialize(2);
    KERNEL_RAW_WRITE_SCOPE(guard, &views);
    assert(!__atomic_load_n(&views.readers, __ATOMIC_RELAXED));
    published = 1;
    __atomic_store_n(&writer_owned, 1, __ATOMIC_RELEASE);
    await(&writer_release, 1);
    return NULL;
}
static void *view_late_reader(void *unused)
{
    (void)unused; initialize(3);
    __atomic_store_n(&late_attempted, 1, __ATOMIC_RELEASE);
    { KERNEL_RAW_READ_SCOPE(guard, &views);
      /* A queued writer closes admission even while old readers drain. */
      assert(published == 1 && __atomic_load_n(&writer_release, __ATOMIC_ACQUIRE));
      __atomic_store_n(&late_entered, 1, __ATOMIC_RELEASE); }
    assert(sync_test_irq && !cpu.preempt_depth && !cpu.raw_locks);
    return NULL;
}
static void shared_views(void)
{
    kernel_raw_rwlock_init(&views, KERNEL_RAW_RANK_MM);
    pthread_t readers[2], writer, late;
    for (unsigned id = 0; id < 2; id++)
        assert(!pthread_create(&readers[id], NULL, view_reader, (void *)(uintptr_t)id));
    await(&view_entered, 3);
    assert(!pthread_create(&writer, NULL, view_writer, NULL));
    await(&views.writer.word, 1);
    assert(!pthread_create(&late, NULL, view_late_reader, NULL));
    await(&late_attempted, 1);
    __atomic_store_n(&view_release, 1, __ATOMIC_RELEASE);
    assert(!pthread_join(readers[0], NULL));
    assert(!__atomic_load_n(&writer_owned, __ATOMIC_ACQUIRE));
    __atomic_store_n(&view_release, 3, __ATOMIC_RELEASE);
    assert(!pthread_join(readers[1], NULL));
    await(&writer_owned, 1);
    assert(!__atomic_load_n(&late_entered, __ATOMIC_ACQUIRE));
    __atomic_store_n(&writer_release, 1, __ATOMIC_RELEASE);
    assert(!pthread_join(writer, NULL) && !pthread_join(late, NULL));
    assert(!views.writer.word && !views.readers && late_entered);
    puts("Raw shared views: concurrent readers, writer drain and closed late-reader admission verified");
}

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
#endif
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
    case 10: kernel_raw_lock_release(&a); lock.word = 2; alarm(1);
             kernel_raw_lock_acquire(&lock, &b); break;
    case 11: case 12: {
        kernel_raw_lock_release(&a);
        struct kernel_raw_rwlock shared;
        struct kernel_raw_read_guard read = {0};
        kernel_raw_rwlock_init(&shared, KERNEL_RAW_RANK_MM);
        kernel_raw_rwlock_read_acquire(&shared, &read);
        if (which == 12) {
            struct kernel_raw_read_guard copied = read;
            kernel_raw_rwlock_read_release(&copied);
        }
        kernel_raw_rwlock_read_release(&read);
        kernel_raw_rwlock_read_release(&read);
        break;
    }
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
          kernel_preempt_disable();
          assert(cpu.preempt_depth == 2);
          kernel_preempt_enable();
          assert(cpu.preempt_depth == 1 && cpu.raw_locks);
          { KERNEL_RAW_SCOPE(b, &inner); assert(cpu.preempt_depth == 2); }
          assert(!sync_test_irq && cpu.preempt_depth == 1); }
        assert(sync_test_irq == enabled && !cpu.raw_locks && !cpu.preempt_depth);
    }
    unsigned fatal_cases = 0;
    for (unsigned which = 0; which < 13; which++) {
#if !BOAROS_LOCK_DEBUG
        if (which == 6 || (which == 0 && BOAROS_MAX_CPUS > 1)) continue;
#endif
        fatal_cases++;
        pid_t child = fork(); assert(child >= 0);
        if (!child) misuse(which);
        int status; assert(waitpid(child, &status, 0) == child);
        assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGILL);
    }
    kernel_raw_lock_init(&lock, KERNEL_RAW_RANK_HEAP);
#if BOAROS_MAX_CPUS > 1
    assert(pthread_barrier_init(&start, 0, 4) == 0);
    pthread_t threads[4];
    for (unsigned i = 0; i < 4; i++) assert(pthread_create(&threads[i], 0, worker, (void *)(uintptr_t)i) == 0);
    for (unsigned i = 0; i < 4; i++) assert(pthread_join(threads[i], 0) == 0);
    assert(count == 80000);
    assert(pthread_barrier_destroy(&start) == 0);
    shared_views();
#else
    struct kernel_raw_rwlock views;
    kernel_raw_rwlock_init(&views, KERNEL_RAW_RANK_MM);
    { KERNEL_RAW_READ_SCOPE(guard, &views);
      assert(!sync_test_irq && cpu.preempt_depth && cpu.raw_locks == &guard.raw); }
    assert(sync_test_irq && !cpu.preempt_depth && !cpu.raw_locks);
#endif
    printf("CPU/raw synchronization: cpus=%u debug=%u, IRQ/preemption restoration and %u fatal cases passed\n",
           BOAROS_MAX_CPUS, BOAROS_LOCK_DEBUG, fatal_cases);
}
