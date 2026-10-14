/* 实际换栈由客户机验证；本测试直接使用生产资格索引及执行owner交接。 */
#define main controlled_smp_main
#include "smp_sched.c"
#undef main
#include <arch/timer.h>
#include <kernel/time.h>

extern void scheduler_user_service_set_locked(struct kernel_task *, int);
extern void scheduler_user_request_locked(struct kernel_task *);
extern int kernel_scheduler_user_service_pending(void);
struct kernel_task *process_find_identity(kernel_pid_t pid, enum kernel_pid_role role)
{ (void)role; for (unsigned i=0; i<4; i++) if (tasks[i].tid==pid) return &tasks[i]; return 0; }
struct kernel_io_context *kernel_io_context_current(void) { return &active->current->io_context; }
uint32_t kernel_time_is_initialized(void) { return 0; }
uint64_t riscv_time_read(void) { return 1; }
enum kernel_time_status kernel_time_deadline_from_monotonic(uint64_t ns,uint64_t *ticks)
{ (void)ns; (void)ticks; __builtin_trap(); }
enum riscv_timer_status riscv_timer_set_scheduler_deadline(uint64_t deadline)
{ (void)deadline; return RISCV_TIMER_STATUS_NOT_STARTED; }
enum kernel_scheduler_status validate_queues_locked(void) { __builtin_trap(); }
/* 唯一模拟的是机器换栈：生产select/finish决定owner及两侧资格。 */
enum kernel_scheduler_status scheduler_switch_claimed(struct kernel_task *old,struct kernel_task *next)
{
    assert(next==active->idle && old->wait.on_cpu && !old->ready_node.queued);
    unsigned other=active->logical_id^1;
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        kernel_wait_switch_finish_locked(old); active->switch_previous=0;
    }
    active=&cpus[other];
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        struct kernel_task *previous=active->current;
        assert(scheduler_select_next_locked(active,previous,1)==old);
        kernel_wait_switch_finish_locked(previous); active->switch_previous=0;
    }
    return KERNEL_SCHEDULER_STATUS_OK;
}
static void *late_service_notice(void *unused)
{
    (void)unused; active=&publishers[0]; sync_test_irq=1;
    int result=pthread_barrier_wait(&gate);
    assert(!result || result==PTHREAD_BARRIER_SERIAL_THREAD);
    { KERNEL_RAW_SCOPE(guard,&kernel_wait_domain); scheduler_user_request_locked(&tasks[0]); }
    return 0;
}

int main(void)
{
    assert(!controlled_smp_main());
    scheduler = (struct kernel_scheduler){0};
    scheduler.rt_configuration.period_ns = 1000000000;
    scheduler.rt_configuration.runtime_ns = 950000000;
    for (unsigned i = 0; i < 2; i++) {
        kernel_cpu_initialize(&cpus[i], i ? 19 : 7, 0);
        cpus[i].logical_id = i;
        cpus[i].state = KERNEL_CPU_ONLINE;
        active = &cpus[i];
        idles[i] = (struct kernel_task){.state = KERNEL_THREAD_STATE_IDLE};
        idles[i].wait.on_cpu = 1;
        cpus[i].current = &idles[i];
        scheduler_cpu_initialize(active, &idles[i], 1);
    }
    active = &cpus[0];
    tasks[0] = (struct kernel_task){.accounted = 1, .cpu_mask = 2,
        .arch.user_mode = 1, .tid_owned=1, .tid=7, .state = KERNEL_THREAD_STATE_READY};
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        scheduler_user_service_set_locked(&tasks[0], 1);
        ready_enqueue_locked(&tasks[0], 0);
        assert(ready_best_locked() == &tasks[0]);
        assert(tasks[0].cpu_mask == 2); /* 临时服务资格不能改写请求affinity。 */
    }
    uint64_t mask;
    assert(!kernel_task_affinity_get(&tasks[0],0,&mask) && mask==2);
    assert(kernel_task_affinity_set(&tasks[0],0,4)==-22);
    active = &cpus[1];
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        assert(!ready_best_locked());
        scheduler_user_service_set_locked(&tasks[0], 0);
        assert(ready_best_locked() == &tasks[0]);
        selected[1] = scheduler_select_next_locked(active, active->current, 1);
        assert(selected[1] == &tasks[0]);
        kernel_wait_switch_finish_locked(&idles[1]);
        cpus[1].switch_previous = 0;
        scheduler_user_request_locked(&tasks[0]);
    }
    assert(kernel_scheduler_user_service_pending());
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        scheduler_user_service_set_locked(&tasks[0], 1);
        tasks[0].state = KERNEL_THREAD_STATE_READY;
        tasks[0].wait.ready_pending = 1;
        selected[1] = scheduler_select_next_locked(active, &tasks[0], 1);
        assert(selected[1] == &idles[1]);
        assert(!tasks[0].ready_node.queued && tasks[0].wait.on_cpu);
    }
    active = &cpus[0];
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        assert(!ready_best_locked()); /* 旧栈尚未完成：另一核不能取得执行资格。 */
    }
    active = &cpus[1];
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        kernel_wait_switch_finish_locked(&tasks[0]);
        cpus[1].switch_previous = 0;
    }
    active = &cpus[0];
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        assert(ready_best_locked() == &tasks[0]);
        assert(!tasks[0].eligible_nodes[1].queued && tasks[0].cpu_mask == 2);
    }
    /* 通知在CPU0消费旧位之后到达；恢复资格不能清掉这个新发布。 */
    { KERNEL_RAW_SCOPE(guard,&kernel_wait_domain); scheduler_user_service_set_locked(&tasks[0],0); }
    active=&cpus[1];
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        assert(scheduler_select_next_locked(active,active->current,1)==&tasks[0]);
        kernel_wait_switch_finish_locked(&idles[1]); active->switch_previous=0;
    }
    kernel_scheduler_enter_user_service();
    assert(active==&cpus[0] && !kernel_scheduler_user_service_pending());
    assert(!pthread_barrier_init(&gate,0,2)); pthread_t notifier;
    assert(!pthread_create(&notifier,0,late_service_notice,0));
    int barrier=pthread_barrier_wait(&gate);
    assert(!barrier || barrier==PTHREAD_BARRIER_SERIAL_THREAD);
    assert(!pthread_join(notifier,0)); pthread_barrier_destroy(&gate);
    kernel_scheduler_leave_user_service();
    assert(active==&cpus[1] && kernel_scheduler_user_service_pending());
    kernel_scheduler_enter_user_service();
    assert(active==&cpus[0] && !kernel_scheduler_user_service_pending());
    assert(tasks[0].service_migrations==2 && tasks[0].cpu_mask==2);
    puts("user SMP service eligibility and old-stack execution owner passed");
    return 0;
}
