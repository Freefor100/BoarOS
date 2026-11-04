#include "../../kernel/sched/private.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>

struct kernel_scheduler scheduler;
struct kernel_raw_lock kernel_wait_domain __attribute__((weak));
_Thread_local uintptr_t sync_test_irq;
static _Thread_local struct kernel_cpu *active;
static struct kernel_cpu cpus[BOAROS_MAX_CPUS];
static struct kernel_task idles[BOAROS_MAX_CPUS], tasks[BOAROS_MAX_CPUS>4?BOAROS_MAX_CPUS:4];
static pthread_barrier_t gate;
static struct kernel_task *selected[BOAROS_MAX_CPUS];
static unsigned deliveries;
static struct kernel_cpu publishers[2];
static unsigned first_claimed;

void *sync_test_cpu(void) { return active; }
void *sync_test_current_task(void) { return active->current; }
uint64_t kernel_time_monotonic_ns(void) { return 1; }
#ifndef SMP_TEST_USE_LIFETIME
void scheduler_task_publish_locked(struct kernel_task *task) { (void)task; assert(0); }
#endif
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

static void reset_two_queues(void)
{
    scheduler=(struct kernel_scheduler){0};
    scheduler.rt_configuration=(struct kernel_rt_configuration){1000000000,950000000};
    for (unsigned i=0;i<2;i++) {
        kernel_cpu_initialize(&cpus[i],7+i*12,0); cpus[i].logical_id=i;
        cpus[i].state=KERNEL_CPU_ONLINE;
        idles[i]=(struct kernel_task){.state=KERNEL_THREAD_STATE_IDLE};
        idles[i].wait.on_cpu=1; cpus[i].current=&idles[i]; active=&cpus[i];
        scheduler_cpu_initialize(active,&idles[i],1);
    }
}

static void assert_ready_owner(struct kernel_task *task,struct kernel_cpu *owner)
{
    unsigned matches=0;
    for (struct kernel_cpu *cpu=scheduler.cpus;cpu;cpu=cpu->scheduler_next) {
        unsigned count=0;
        for (struct kernel_sched_node *node=cpu->runqueue.first;node;node=node->next) {
            count++;
            struct kernel_task *queued=node->owner;
            assert(node==&queued->ready_node && queued->ready_cpu==cpu && !queued->wait.on_cpu);
            matches+=queued==task;
        }
        assert(count==cpu->runqueue.count);
    }
    assert(matches==1 && task->ready_node.queued && task->ready_cpu==owner);
}

static void placement_blocker_cases(void)
{
    reset_two_queues();active=&cpus[0];
    { KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
      cpus[0].rt_bandwidth.runtime_ns=cpus[1].rt_bandwidth.runtime_ns=-1;
      setup_task(1,1,99);
      tasks[2]=(struct kernel_task){.accounted=1,.cpu=&cpus[1],
          .state=KERNEL_THREAD_STATE_RUNNING,.wait.on_cpu=1};
      tasks[2].scheduling.policy=KERNEL_SCHED_FIFO;tasks[2].scheduling.priority=10;cpus[1].current=&tasks[2];idles[1].wait.on_cpu=0;
      kernel_cpu_consume_schedule(&cpus[0]);kernel_cpu_consume_schedule(&cpus[1]);
      tasks[0]=(struct kernel_task){.accounted=1,.cpu_mask=3,.cpu=&cpus[0],
          .state=KERNEL_THREAD_STATE_READY};
      tasks[0].scheduling.policy=KERNEL_SCHED_FIFO;
      tasks[0].scheduling.priority=80;ready_enqueue_locked(&tasks[0],0);
      /* idle的CPU0即将运行pinned FIFO99，RT80必须抢占CPU1的FIFO10。 */
      assert_ready_owner(&tasks[0],&cpus[1]);
      assert(!kernel_cpu_schedule_requested(&cpus[0]) && kernel_cpu_schedule_requested(&cpus[1]));
      ready_remove_locked(&tasks[0]);ready_remove_locked(&tasks[1]); }
    puts("PASS placement: queued pinned FIFO99 cannot hide executable FIFO80 behind an idle last CPU");

    reset_two_queues();active=&cpus[0];
    { KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
      cpus[0].rt_bandwidth.consumed_ns=cpus[0].rt_bandwidth.runtime_ns;
      tasks[2]=(struct kernel_task){.accounted=1,.cpu=&cpus[1],
          .state=KERNEL_THREAD_STATE_RUNNING,.wait.on_cpu=1};
      cpus[1].current=&tasks[2];idles[1].wait.on_cpu=0;
      kernel_cpu_consume_schedule(&cpus[0]);kernel_cpu_consume_schedule(&cpus[1]);
      tasks[0]=(struct kernel_task){.accounted=1,.cpu_mask=3,.cpu=&cpus[0],
          .state=KERNEL_THREAD_STATE_READY};
      tasks[0].scheduling.policy=KERNEL_SCHED_FIFO;
      tasks[0].scheduling.priority=80;ready_enqueue_locked(&tasks[0],0);
      assert_ready_owner(&tasks[0],&cpus[1]);
      assert(!kernel_cpu_schedule_requested(&cpus[0]) && kernel_cpu_schedule_requested(&cpus[1]));
      ready_remove_locked(&tasks[0]);
      /* 所有核都无预算时仍有唯一排队owner，恢复预算后可取得执行资格。 */
      cpus[1].rt_bandwidth.consumed_ns=cpus[1].rt_bandwidth.runtime_ns;
      ready_enqueue_locked(&tasks[0],0);assert_ready_owner(&tasks[0],&cpus[0]);
      assert(!ready_best_locked());
      cpus[0].rt_bandwidth.consumed_ns=0;
      assert(ready_best_locked()==&tasks[0]);ready_remove_locked(&tasks[0]);
      /* 捐赠优先级的持锁者不受上述预算排除，仍选空闲的CPU0。 */
      cpus[0].rt_bandwidth.consumed_ns=cpus[0].rt_bandwidth.runtime_ns;
      cpus[1].rt_bandwidth.consumed_ns=0;
      tasks[0].scheduling.policy=KERNEL_SCHED_OTHER;tasks[0].pi_base_priority=1;
      ready_enqueue_locked(&tasks[0],0);assert_ready_owner(&tasks[0],&cpus[0]);
      assert(ready_best_locked()==&tasks[0]);ready_remove_locked(&tasks[0]); }
    puts("PASS placement: prefer usable RT budget; all-throttled fallback and PI exemption retain progress");
    reset_two_queues();
}

static void owned_queue_cases(void)
{
    reset_two_queues();active=&cpus[1];
    { KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
      for (unsigned i=0;i<2;i++) {
          tasks[i]=(struct kernel_task){.accounted=1,.cpu_mask=3,
              .state=KERNEL_THREAD_STATE_READY,.cpu=&cpus[0]};
          ready_enqueue_locked(&tasks[i],0);
      }
      assert_ready_owner(&tasks[0],&cpus[0]);assert_ready_owner(&tasks[1],&cpus[1]);
      struct kernel_sched_node *unrelated=cpus[1].runqueue.first;
      scheduler_pi_priority_locked(&tasks[0],0,80);
      assert_ready_owner(&tasks[0],&cpus[0]);
      assert(cpus[1].runqueue.first==unrelated && cpus[1].runqueue.count==1 &&
          !cpus[1].runqueue.boosted && cpus[0].runqueue.boosted==1);
      scheduler_pi_priority_locked(&tasks[0],80,0);
      kernel_cpu_consume_schedule(&cpus[0]);kernel_cpu_consume_schedule(&cpus[1]);
      /* 实际优先级相同但解除PI节流时仍通知真实owner。 */
      scheduler_pi_priority_locked(&tasks[0],0,80);
      assert(cpus[0].runqueue.boosted==1 && kernel_cpu_schedule_requested(&cpus[0]) &&
          !kernel_cpu_schedule_requested(&cpus[1]));
      scheduler_pi_priority_locked(&tasks[0],0,0);
      ready_remove_locked(&tasks[0]);ready_remove_locked(&tasks[1]);
      tasks[0].cpu=0;ready_enqueue_locked(&tasks[0],0);
      assert_ready_owner(&tasks[0],&cpus[1]);ready_remove_locked(&tasks[0]);
    }
    kernel_cpu_initialize(&publishers[0],100,0);active=&publishers[0];
    { KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
      ready_enqueue_locked(&tasks[0],0);
      assert_ready_owner(&tasks[0],&cpus[0]);ready_remove_locked(&tasks[0]); }
    active=&cpus[0];
    { KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
      for (unsigned i=0;i<2;i++) {
          tasks[2+i]=(struct kernel_task){.accounted=1,.state=KERNEL_THREAD_STATE_RUNNING,
              .cpu=&cpus[i],.wait.on_cpu=1};
          tasks[2+i].scheduling.priority=i?10:90;cpus[i].current=&tasks[2+i];
          kernel_cpu_consume_schedule(&cpus[i]);
      }
      setup_task(1,2,0);
      tasks[0]=(struct kernel_task){.accounted=1,.cpu_mask=3,.cpu=&cpus[0],
          .state=KERNEL_THREAD_STATE_READY};
      tasks[0].scheduling.priority=50;ready_enqueue_locked(&tasks[0],0);
      assert_ready_owner(&tasks[0],&cpus[1]);
      assert(kernel_cpu_schedule_requested(&cpus[1]) && !kernel_cpu_schedule_requested(&cpus[0]));
      ready_remove_locked(&tasks[0]);ready_remove_locked(&tasks[1]); }
    puts("PASS ready placement: load, last/caller/ID ties, sole priority owner and actual RT preemption target");

    reset_two_queues();active=&cpus[0];
    { KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
      setup_task(0,1,99);
      for (unsigned i=1;i<3;i++) {
          tasks[i]=(struct kernel_task){.accounted=1,.cpu_mask=3,.cpu=&cpus[0],
              .state=KERNEL_THREAD_STATE_READY};
          tasks[i].scheduling.priority=i==1?80:10;
          ready_enqueue_locked(&tasks[i],SCHED_READY_LOCAL);
          assert_ready_owner(&tasks[i],&cpus[0]);
      } }
    active=&cpus[1];
    { KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
      assert(ready_best_locked()==&tasks[1]);
      assert(tasks[1].ready_cpu==&cpus[0] && !tasks[1].wait.on_cpu);
      assert(scheduler_select_next_locked(active,active->current,1)==&tasks[1]);
      assert(!tasks[1].ready_cpu && tasks[1].wait.on_cpu && tasks[1].cpu==active);
      assert_ready_owner(&tasks[0],&cpus[0]);assert_ready_owner(&tasks[2],&cpus[0]); }
    puts("PASS idle pull: skip pinned head, select highest eligible priority, remove only the actual ready owner");
    reset_two_queues();
}

#if BOAROS_MAX_CPUS >= 4
static void many_claims(void)
{
    for (unsigned task_count = 1; task_count <= BOAROS_MAX_CPUS; task_count += BOAROS_MAX_CPUS-1) {
        scheduler = (struct kernel_scheduler){0};
        scheduler.rt_configuration.period_ns = 1000000000;
        scheduler.rt_configuration.runtime_ns = 950000000;
        for (unsigned i=0; i<BOAROS_MAX_CPUS; i++) {
            kernel_cpu_initialize(&cpus[i],7+i*19,0);cpus[i].logical_id=i;
            active=&cpus[i];idles[i]=(struct kernel_task){0};
            idles[i].wait.on_cpu=1;idles[i].state=KERNEL_THREAD_STATE_IDLE;
            cpus[i].current=&idles[i];scheduler_cpu_initialize(active,&idles[i],1);
            selected[i]=0;
        }
        active=&cpus[0];
        { KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
          for(unsigned i=0;i<task_count;i++)setup_task(i,(1U<<BOAROS_MAX_CPUS)-1,0); }
        assert(!pthread_barrier_init(&gate,0,BOAROS_MAX_CPUS));pthread_t threads[BOAROS_MAX_CPUS];
        for(unsigned i=0;i<BOAROS_MAX_CPUS;i++)assert(!pthread_create(&threads[i],0,claim,(void *)(uintptr_t)i));
        for(unsigned i=0;i<BOAROS_MAX_CPUS;i++)assert(!pthread_join(threads[i],0));
        for(unsigned t=0;t<task_count;t++) {
            unsigned owners=0;
            for(unsigned i=0;i<BOAROS_MAX_CPUS;i++) {
                owners+=selected[i]==&tasks[t];
                assert(!cpus[i].runqueue.first);
            }
            assert(owners==1 && tasks[t].wait.on_cpu && !tasks[t].ready_node.queued && !tasks[t].ready_cpu);
        }
        pthread_barrier_destroy(&gate);
    }
    puts("SMP simultaneous selectors claim one/all tasks exactly once and release their ready owners");
}
#endif

int main(void)
{
    kernel_raw_lock_init(&kernel_wait_domain, KERNEL_RAW_RANK_SCHEDULER);
    owned_queue_cases();
    placement_blocker_cases();
    scheduler=(struct kernel_scheduler){0};
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
    /* Two newly ready tasks last ran on the same now-idle CPU. The second
     * request must also wake the other idle CPU rather than duplicate it. */
    kernel_cpu_consume_schedule(&cpus[0]);
    kernel_cpu_consume_schedule(&cpus[1]);
    { KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
      for (unsigned i=0; i<2; i++) {
          tasks[i]=(struct kernel_task){.accounted=1,.cpu_mask=3,
              .state=KERNEL_THREAD_STATE_READY,.cpu=&cpus[0]};
          assert(!kernel_sched_policy_set(&tasks[i].scheduling,KERNEL_SCHED_OTHER,0,0));
          ready_enqueue_locked(&tasks[i],0);
      }
      assert(kernel_cpu_schedule_requested(&cpus[0]));
      assert(kernel_cpu_schedule_requested(&cpus[1]));
      ready_remove_locked(&tasks[0]); ready_remove_locked(&tasks[1]); }
    kernel_cpu_consume_schedule(&cpus[0]);
    kernel_cpu_consume_schedule(&cpus[1]);
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
    /* 本核owner和异核pull同时取得同一任务，唯一一个claim可成功。 */
    assert(!pthread_barrier_init(&gate, 0, 2));
    pthread_t threads[2];
    for (unsigned i = 0; i < 2; i++) assert(!pthread_create(&threads[i], 0, claim, (void *)(uintptr_t)i));
    for (unsigned i = 0; i < 2; i++) assert(!pthread_join(threads[i], 0));
    assert((selected[0] == &tasks[3]) + (selected[1] == &tasks[3]) == 1);
    assert(tasks[3].wait.on_cpu && !tasks[3].ready_node.queued);
    assert(!tasks[3].ready_cpu);
    assert(!cpus[0].runqueue.first && !cpus[1].runqueue.first);
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
    assert(!cpus[0].runqueue.first && !cpus[1].runqueue.first);
    assert(tasks[0].wait.on_cpu && tasks[1].wait.on_cpu && tasks[0].cpu != tasks[1].cpu);
    pthread_barrier_destroy(&gate);
#if BOAROS_MAX_CPUS >= 4
    many_claims();
#endif
    puts("SMP owned FIFO queues, idle pull and concurrent execution claim passed");
    return 0;
}
