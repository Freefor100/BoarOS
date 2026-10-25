#include "../../kernel/sched/private.h"
#include "../../kernel/sched/futex_internal.h"
#include <kernel/time.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

_Thread_local uintptr_t sync_test_irq;
struct kernel_scheduler scheduler;
static struct kernel_cpu cpus[4];
static struct kernel_task idle[4], tasks[4];
static uint64_t now=100;
extern void scheduler_pi_priority_locked(struct kernel_task *,unsigned,unsigned) __attribute__((weak));
void *sync_test_cpu(void){return &cpus[0];}
void *sync_test_current_task(void){return cpus[0].current;}
void sync_test_current_task_set(void *task){cpus[0].current=task;}
uint64_t kernel_time_monotonic_ns(void){return now;}
uint32_t kernel_time_is_initialized(void){return 1;}
uint64_t riscv_time_read(void){return now;}
uint64_t kernel_time_ticks_to_ns(uint64_t n){return n;}
void scheduler_task_publish_locked(struct kernel_task *task){(void)task;__builtin_trap();}
static void setup(void)
{
    scheduler=(struct kernel_scheduler){0};memset(cpus,0,sizeof(cpus));
    memset(idle,0,sizeof(idle));memset(tasks,0,sizeof(tasks));
    kernel_wait_domain_init();
    scheduler.rt_configuration=(struct kernel_rt_configuration){1000000,950000};
    for(unsigned i=0;i<4;i++) {
        kernel_cpu_initialize(&cpus[i],i,0);cpus[i].logical_id=i;
        cpus[i].current=&idle[i];idle[i].idle=1;
        scheduler_cpu_initialize(&cpus[i],&idle[i],now);
    }
    for(unsigned i=0;i<4;i++) {
        tasks[i].accounted=1;tasks[i].state=KERNEL_THREAD_STATE_READY;
        tasks[i].cpu=&cpus[0];tasks[i].cpu_mask=15;tasks[i].arch.user_mode=1;
        tasks[i].scheduling.policy=KERNEL_SCHED_FIFO;
    }
    tasks[0].scheduling.priority=10;tasks[1].scheduling.priority=40;
    tasks[2].scheduling.priority=70;tasks[3].scheduling.policy=KERNEL_SCHED_OTHER;
}
static void ready_and_running(void)
{
    setup();KERNEL_RAW_SCOPE(g,&kernel_wait_domain);
    ready_enqueue_locked(&tasks[0],0);ready_enqueue_locked(&tasks[1],0);
    assert(ready_best_locked()==&tasks[1]);
    scheduler_pi_priority_locked(&tasks[0],10,80);
    assert(tasks[0].scheduling.priority==80 && ready_best_locked()==&tasks[0]);
    for(unsigned i=0;i<4;i++)assert(cpus[i].eligible.first->owner==&tasks[0]);
    scheduler_pi_priority_locked(&tasks[0],10,0);
    assert(tasks[0].scheduling.priority==10 && ready_best_locked()==&tasks[1]);
    ready_remove_locked(&tasks[0]);tasks[0].state=KERNEL_THREAD_STATE_RUNNING;
    tasks[0].cpu=&cpus[1];tasks[0].wait.on_cpu=1;cpus[1].current=&tasks[0];
    cpus[1].need_resched=0;
    scheduler_pi_priority_locked(&tasks[0],10,85);
    assert(tasks[0].scheduling.priority==85 && cpus[1].need_resched);
}
static void service_and_bandwidth(void)
{
    setup();KERNEL_RAW_SCOPE(g,&kernel_wait_domain);
    tasks[0].user_service=1;tasks[0].reservation_cpu=&cpus[1];
    kernel_sched_enqueue(&cpus[1].user_reservations,&tasks[0].reservation_node,&tasks[0],10,0);
    ready_enqueue_locked(&tasks[0],0);
    scheduler_pi_priority_locked(&tasks[0],10,80);
    assert(scheduler.rt_services.first->priority==80);
    assert(cpus[1].user_reservations.first->priority==80);
    scheduler_pi_priority_locked(&tasks[0],10,0);
    assert(scheduler.rt_services.first->priority==10 && cpus[1].user_reservations.first->priority==10);
    scheduler_user_reservation_release_locked(&tasks[0]);ready_remove_locked(&tasks[0]);
    tasks[0].user_service=0;
    ready_enqueue_locked(&tasks[0],0);ready_enqueue_locked(&tasks[1],0);ready_enqueue_locked(&tasks[3],0);
    cpus[0].rt_bandwidth.runtime_ns=0;
    assert(ready_best_locked()==&tasks[3]);
    scheduler_pi_priority_locked(&tasks[0],10,60);
    assert(ready_best_locked()==&tasks[0]);
    scheduler_pi_priority_locked(&tasks[1],70,0);
    assert(ready_best_locked()==&tasks[1]);
    scheduler_pi_priority_locked(&tasks[0],10,0);
    assert(ready_best_locked()==&tasks[3]);
}
static void equal_priority_order(void)
{
    setup();KERNEL_RAW_SCOPE(g,&kernel_wait_domain);
    ready_enqueue_locked(&tasks[2],0);ready_enqueue_locked(&tasks[0],0);
    scheduler_pi_priority_locked(&tasks[0],10,70);
    assert(tasks[2].ready_node.next==&tasks[0].ready_node);
    scheduler_pi_priority_locked(&tasks[0],70,0);
    assert(tasks[2].ready_node.next==&tasks[0].ready_node);
    scheduler_pi_priority_locked(&tasks[0],20,70);
    assert(tasks[2].ready_node.next==&tasks[0].ready_node);
    scheduler_pi_priority_locked(&tasks[0],20,0);
    assert(tasks[0].scheduling.priority==20);
}
static void donation_chain(void)
{
    setup();KERNEL_RAW_SCOPE(g,&kernel_wait_domain);
    /* 直接构造已登记关系，只隔离优先级传播；字/引用交接由PI syscall短例检查。 */
    struct kernel_futex_pi_state a={.owner=&tasks[0]},b={.owner=&tasks[1]};
    struct kernel_futex_pi_request middle={.task=&tasks[1],.state=&a,.queued=1,.order=1};
    struct kernel_futex_pi_request high={.task=&tasks[2],.state=&b,.queued=1,.order=2};
    struct kernel_futex_pi_request other={.task=&tasks[3],.state=&a,.queued=1,.order=3};
    tasks[0].pi_owned=&a;tasks[1].pi_owned=&b;
    tasks[1].pi_request=&middle;tasks[2].pi_request=&high;tasks[3].pi_request=&other;
    tasks[3].scheduling.policy=KERNEL_SCHED_FIFO;tasks[3].scheduling.priority=90;
    a.head=&other;a.tail=&middle;other.next=&middle;middle.previous=&other;
    b.head=b.tail=&high;
    ready_enqueue_locked(&tasks[0],0);
    kernel_futex_pi_recalculate_locked(&tasks[1],-1);
    assert(tasks[1].scheduling.priority==70 && tasks[0].scheduling.priority==90);
    assert(scheduler_base_priority(&tasks[0])==10 && scheduler_base_priority(&tasks[1])==40);
    kernel_futex_pi_recalculate_locked(&tasks[3],20);
    assert(a.head==&middle && tasks[0].scheduling.priority==70);
    kernel_futex_pi_recalculate_locked(&tasks[2],30);
    assert(tasks[1].scheduling.priority==40 && tasks[0].scheduling.priority==40);
    kernel_futex_pi_recalculate_locked(&tasks[0],90);
    assert(tasks[0].scheduling.priority==90 && !tasks[0].pi_base_priority);
    kernel_futex_pi_recalculate_locked(&tasks[2],95);
    assert(tasks[0].scheduling.priority==95 && scheduler_base_priority(&tasks[0])==90);
    kernel_futex_pi_recalculate_locked(&tasks[2],30);
    assert(tasks[0].scheduling.priority==90 && !tasks[0].pi_base_priority);
}
int main(void)
{
    assert(scheduler_pi_priority_locked && "PI effective-priority publication is missing");
    ready_and_running();service_and_bandwidth();equal_priority_order();donation_chain();
    puts("PI priority: ready/running, four eligibilities, service reservation, throttling, FIFO and donation chain passed");
}
