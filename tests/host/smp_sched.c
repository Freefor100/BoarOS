#include "../../kernel/sched/private.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>

struct kernel_scheduler scheduler;
struct kernel_raw_lock kernel_wait_domain __attribute__((weak));
_Thread_local uintptr_t sync_test_irq;
static _Thread_local struct kernel_cpu *active;
static struct kernel_cpu cpus[2];
static struct kernel_task idles[2], tasks[4];
static pthread_barrier_t gate;
static struct kernel_task *selected[2];
static unsigned deliveries;
static struct kernel_cpu publishers[2];
static unsigned first_claimed;

void *sync_test_cpu(void) { return active; }
void *sync_test_current_task(void) { return active->current; }
uint64_t kernel_time_monotonic_ns(void) { return 1; }
void scheduler_task_publish_locked(struct kernel_task *task) { (void)task; assert(0); }
void kernel_cpu_notify(struct kernel_cpu *target, unsigned events)
{
    assert(active->raw_locks && active->raw_locks->lock == &kernel_wait_domain);
    __atomic_fetch_or(&target->events, events, __ATOMIC_RELEASE);
    if (target != active) active->outbound |= 1U << target->logical_id;
}
void kernel_cpu_flush_notifications(void)
{
    assert(!active->raw_locks && !sync_test_irq);
    if (active->outbound) __atomic_fetch_add(&deliveries, 1, __ATOMIC_RELAXED);
    active->outbound = 0;
}

static void setup_task(unsigned i, unsigned mask, unsigned priority)
{
    tasks[i] = (struct kernel_task){.accounted = 1, .cpu_mask = mask,
        .state = KERNEL_THREAD_STATE_READY};
    assert(!kernel_sched_policy_set(&tasks[i].scheduling,
        priority ? KERNEL_SCHED_FIFO : KERNEL_SCHED_OTHER, priority, 0));
    ready_enqueue_locked(&tasks[i], 0);
}

static void *claim(void *argument)
{
    unsigned id = (uintptr_t)argument;
    active = &cpus[id];
    sync_test_irq = 1;
    int result = pthread_barrier_wait(&gate);
    assert(!result || result == PTHREAD_BARRIER_SERIAL_THREAD);
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    selected[id] = scheduler_select_next_locked(active, active->current, 1);
    return 0;
}

static void *publish_and_claim(void *argument)
{
    unsigned id = (uintptr_t)argument;
    active = id < 2 ? &cpus[id] : &publishers[id - 2];
    sync_test_irq = 1;
    int result = pthread_barrier_wait(&gate);
    assert(!result || result == PTHREAD_BARRIER_SERIAL_THREAD);
    if (id >= 2) {
        if (id == 3) while (!__atomic_load_n(&first_claimed, __ATOMIC_ACQUIRE)) {}
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        setup_task(id - 2, 3, 10);
        return 0;
    }
    for (;;) {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        if (ready_best_locked()) {
            selected[id] = scheduler_select_next_locked(active, active->current, 1);
            __atomic_store_n(&first_claimed, 1, __ATOMIC_RELEASE);
            return 0;
        }
    }
}

int main(void)
{
    kernel_raw_lock_init(&kernel_wait_domain, KERNEL_RAW_RANK_SCHEDULER);
    scheduler.rt_configuration.period_ns = 1000000000;
    scheduler.rt_configuration.runtime_ns = 950000000;
    for (unsigned i = 0; i < 2; i++) {
        kernel_cpu_initialize(&cpus[i], i ? 19 : 7, 0);
        cpus[i].logical_id = i;
        active = &cpus[i];
        idles[i].wait.on_cpu = 1;
        idles[i].state = KERNEL_THREAD_STATE_IDLE;
        cpus[i].current = &idles[i];
        scheduler_cpu_initialize(active, &idles[i], 1);
    }
    active = &cpus[0];
    { KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
      setup_task(0, 1, 99); setup_task(1, 2, 10); setup_task(2, 2, 10);
      assert(ready_best_locked() == &tasks[0]); }
    active = &cpus[1];
    { KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
      assert(ready_best_locked() == &tasks[1]);
      ready_remove_locked(&tasks[1]);
      assert(ready_best_locked() == &tasks[2]);
      ready_remove_locked(&tasks[2]); ready_remove_locked(&tasks[0]);
      setup_task(3, 3, 0); }
    /* 两CPU同时准备取得同一全局runnable owner，唯一一个claim可成功。 */
    assert(!pthread_barrier_init(&gate, 0, 2));
    pthread_t threads[2];
    for (unsigned i = 0; i < 2; i++) assert(!pthread_create(&threads[i], 0, claim, (void *)(uintptr_t)i));
    for (unsigned i = 0; i < 2; i++) assert(!pthread_join(threads[i], 0));
    assert((selected[0] == &tasks[3]) + (selected[1] == &tasks[3]) == 1);
    assert(tasks[3].wait.on_cpu && !tasks[3].ready_node.queued);
    assert(!tasks[3].eligible_nodes[0].queued && !tasks[3].eligible_nodes[1].queued);
    assert(!cpus[0].eligible.first && !cpus[1].eligible.first && !scheduler.runqueue.first);
    unsigned owner = tasks[3].cpu->logical_id;
    assert(cpus[owner].switch_previous == &idles[owner] && idles[owner].wait.on_cpu);
    assert(deliveries);
    pthread_barrier_destroy(&gate);
    { KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
      for (unsigned i = 0; i < 4; i++) {
          tasks[i].wait.deadline = 55;
          tasks[i].tid = 0;
          deadline_index_insert(&tasks[i]);
      }
      for (unsigned i = 0; i < 4; i++) deadline_index_remove(&tasks[i]);
      assert(!scheduler.deadline_root); }
    /* 额外两个线程是发布/通知actor；执行资格仍只有两个注册CPU。 */
    scheduler = (struct kernel_scheduler){0};
    scheduler.rt_configuration.period_ns = 1000000000;
    scheduler.rt_configuration.runtime_ns = 950000000;
    for (unsigned i = 0; i < 2; i++) {
        kernel_cpu_initialize(&cpus[i], i ? 19 : 7, 0);
        cpus[i].logical_id = i;
        active = &cpus[i];
        idles[i] = (struct kernel_task){0};
        idles[i].wait.on_cpu = 1;
        cpus[i].current = &idles[i];
        scheduler_cpu_initialize(active, &idles[i], 1);
        kernel_cpu_initialize(&publishers[i], 100 + i, 0);
    }
    assert(!pthread_barrier_init(&gate, 0, 4));
    pthread_t actors[4];
    for (unsigned i = 0; i < 4; i++) assert(!pthread_create(&actors[i], 0, publish_and_claim, (void *)(uintptr_t)i));
    for (unsigned i = 0; i < 4; i++) assert(!pthread_join(actors[i], 0));
    assert(selected[0] != selected[1] && selected[0] && selected[1]);
    assert(!scheduler.runqueue.first && !cpus[0].eligible.first && !cpus[1].eligible.first);
    assert(tasks[0].wait.on_cpu && tasks[1].wait.on_cpu && tasks[0].cpu != tasks[1].cpu);
    pthread_barrier_destroy(&gate);
    puts("SMP eligibility FIFO, global membership and concurrent execution claim passed");
    return 0;
}
