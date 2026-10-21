#define _POSIX_C_SOURCE 200809L
#include <kernel/cost.h>
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static struct kernel_cost_cpu cpus[2];
static struct kernel_cost_task tasks[2];
static _Thread_local unsigned cpu_id;
static _Thread_local unsigned irq;
static uint64_t clocks[2]={1000,1100};
static unsigned watch_wait,at_wait,done,scan_armed,start_worker,during_scan;
static volatile void *aggregate;
uint64_t kernel_cost_clock(void) { return __atomic_load_n(&clocks[cpu_id],__ATOMIC_ACQUIRE); }
uint64_t kernel_cost_irq_save(void) { unsigned old=irq;irq=0;return old; }
void kernel_cost_irq_restore(uint64_t enabled) { if(enabled)irq=1; }
struct kernel_cost_cpu *kernel_cost_cpu_current(void) { return &cpus[cpu_id]; }
struct kernel_cost_task *kernel_cost_current(void) { return &tasks[cpu_id]; }
uint32_t __real___atomic_exchange_4(volatile void *,uint32_t,int);
uint32_t __wrap___atomic_exchange_4(volatile void *address,uint32_t value,int model)
{
    if(!aggregate)aggregate=address;
    return __real___atomic_exchange_4(address,value,model);
}
uint32_t __real___atomic_load_4(const volatile void *,int);
uint32_t __wrap___atomic_load_4(const volatile void *address,int model)
{
    uint32_t value=__real___atomic_load_4(address,model);
    /* cost.c alone uses out-of-line atomics: this is its actual locked poll,
     * after both the IRQ-entry publication and observer-wait publication. */
    if(address==aggregate && cpu_id==1 && value && __atomic_load_n(&watch_wait,__ATOMIC_ACQUIRE))
        __atomic_store_n(&at_wait,1,__ATOMIC_RELEASE);
    return value;
}
/* Stop the actual close scan around its first remote observation. Old code
 * exchanges empty pending first; corrected code first acquires waiting. */
static void publish_during_scan(void)
{
    __atomic_store_n(&scan_armed,0,__ATOMIC_RELEASE);
    __atomic_store_n(&start_worker,1,__ATOMIC_RELEASE);
    while(!__atomic_load_n(&at_wait,__ATOMIC_ACQUIRE))sched_yield();
}
uint64_t __real___atomic_exchange_8(volatile void *,uint64_t,int);
uint64_t __wrap___atomic_exchange_8(volatile void *address,uint64_t value,int model)
{
    uint64_t old=__real___atomic_exchange_8(address,value,model);
    if(cpu_id==0 && address==&cpus[1].irq_pending_start && __atomic_load_n(&scan_armed,__ATOMIC_ACQUIRE)) {
        assert(old==0);publish_during_scan();
    }
    return old;
}
uint64_t __real___atomic_load_8(const volatile void *,int);
uint64_t __wrap___atomic_load_8(const volatile void *address,int model)
{
    if(cpu_id==0 && address==&cpus[1].observer_wait_started && __atomic_load_n(&scan_armed,__ATOMIC_ACQUIRE))
        publish_during_scan();
    return __real___atomic_load_8(address,model);
}
static void *worker(void *unused)
{
    (void)unused;cpu_id=1;irq=0;
    if(during_scan)while(!__atomic_load_n(&start_worker,__ATOMIC_ACQUIRE))sched_yield();
    kernel_cost_irq_disabled(1300);
    __atomic_store_n(&clocks[1],2700,__ATOMIC_RELEASE);
    kernel_cost_irq_enabled(2700);
    __atomic_store_n(&done,1,__ATOMIC_RELEASE);return NULL;
}
int main(int argc,char **argv)
{
    assert(argc==2);unsigned scenario=(unsigned)atoi(argv[1]);assert(scenario<=3);
    unsigned previous=scenario&1;during_scan=scenario>=2;
    cpu_id=1;kernel_cost_register_cpu(&cpus[1]);
    kernel_cost_switch(&cpus[1].bootstrap,&tasks[1]);
    cpu_id=0;assert(!kernel_cost_begin(7,10000000,1,0));
    if(previous) {
        cpu_id=1;kernel_cost_irq_disabled(1100);
        clocks[1]=1200;kernel_cost_irq_enable_now();
        assert(cpus[1].return_pending==1);cpu_id=0;
    }
    uint64_t held=kernel_cost_lock();clocks[1]=during_scan?2100:1500;watch_wait=1;
    pthread_t thread;assert(!pthread_create(&thread,NULL,worker,NULL));
    if(during_scan)scan_armed=1;
    else while(!__atomic_load_n(&at_wait,__ATOMIC_ACQUIRE))sched_yield();
    clocks[0]=2000;assert(!kernel_cost_end(7,0));
    assert(cpus[1].lock_depth==1 && !irq && at_wait);
    uint64_t actual,observer;
    assert(!kernel_cost_read(0,COST_IRQ_OFF_TICKS,&actual));
    assert(!kernel_cost_read(2,COST_OBSERVER_TICKS,&observer));
    uint64_t expected=700+(previous?100:0);
    uint64_t expected_observer=during_scan?0:500;
    fprintf(stderr,"IRQ boundary scan=%u old_pending=%u: actual=%llu expected=%llu observer=%llu expected=%llu\n",
        during_scan,previous,(unsigned long long)actual,(unsigned long long)expected,(unsigned long long)observer,(unsigned long long)expected_observer);
    assert(actual==expected && observer==expected_observer);
    /* Open the next epoch while the same physical CPU is still blocked.
     * Only its [2500,2700] continuation belongs to that new window. */
    clocks[0]=2500;assert(!kernel_cost_begin(7,10000000,1,0));
    clocks[1]=2600;kernel_cost_unlock(held);
    while(!__atomic_load_n(&done,__ATOMIC_ACQUIRE))sched_yield();
    clocks[0]=3000;assert(!kernel_cost_end(7,0));
    assert(!kernel_cost_read(0,COST_IRQ_OFF_TICKS,&actual) && actual==200);
    assert(!pthread_join(thread,NULL));
    assert(!cpus[0].lock_depth && !cpus[1].lock_depth);
    char *output=malloc(kernel_cost_format_capacity());assert(output);
    assert(kernel_cost_format(output,kernel_cost_format_capacity())>0 && strstr(output,"inflight=0\n"));
    free(output);puts("PASS actual IRQ-off entry before aggregate wait: exact old/new epoch boundaries and owners returned");
}
