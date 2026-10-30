#include "../../kernel/sched/private.h"
#include <assert.h>
#include <stdio.h>
struct kernel_scheduler scheduler;
struct kernel_raw_lock kernel_wait_domain;
_Thread_local uintptr_t sync_test_irq = 1;
static struct kernel_cpu cpus[2], *active;
static struct kernel_task idles[2];
static uint64_t ticks;
void *sync_test_cpu(void) { return active; }
uint64_t kernel_cost_clock(void) { return ++ticks; }
uint64_t kernel_cost_irq_save(void) { return arch_interrupt_save(); }
void kernel_cost_irq_restore(uint64_t saved) { arch_interrupt_restore(saved); }
struct kernel_cost_cpu *kernel_cost_cpu_current(void) { return &active->cost; }
struct kernel_cost_task *kernel_cost_current(void) { return &active->current->cost; }
void kernel_cpu_flush_notifications(void) { assert(!active->outbound); }
int main(void)
{
    scheduler.rt_configuration.period_ns=1000000000;
    scheduler.rt_configuration.runtime_ns=950000000;
    kernel_raw_lock_init(&kernel_wait_domain,KERNEL_RAW_RANK_SCHEDULER);
    for(unsigned i=0;i<2;i++) {
        active=&cpus[i];kernel_cpu_initialize(active,i,0);active->logical_id=i;
        active->current=&idles[i];
        scheduler_cpu_initialize(active,&idles[i],1);
        kernel_cost_register_cpu(&active->cost);
        active->cost.running=&idles[i].cost;
    }
    active=&cpus[0];
    assert(!kernel_cost_begin(1,10000000,0,0));ticks+=100;
    assert(!kernel_cost_end(1,0));
    uint64_t idle,run;
    assert(!kernel_cost_read(1,COST_IDLE_TICKS,&idle) && idle>0);
    assert(!kernel_cost_read(1,COST_RUN_TICKS,&run) && run==0);
    puts("COST idle: boot and remote idle intervals enter idle_ticks, not background run_ticks");
}
