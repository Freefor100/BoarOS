#define _POSIX_C_SOURCE 200809L
#include <kernel/cost.h>
#include <kernel/errno.h>
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static struct kernel_cost_cpu cpus[2];
static struct kernel_cost_task tasks[2], idle;
static _Thread_local unsigned cpu_id;
static _Thread_local unsigned irq_enabled=1;
static _Thread_local struct kernel_cost_task *actor;
static atomic_uint phase;
static atomic_uint_fast64_t ticks=100;
uint64_t kernel_cost_clock(void) { return atomic_fetch_add(&ticks,1); }
uint64_t kernel_cost_irq_save(void) { unsigned old=irq_enabled;irq_enabled=0;return old; }
void kernel_cost_irq_restore(uint64_t enabled) { if (enabled) irq_enabled=1; }
struct kernel_cost_cpu *kernel_cost_cpu_current(void) { return &cpus[cpu_id]; }
struct kernel_cost_task *kernel_cost_current(void) { return actor; }
static void wait_for(unsigned n)
{ while (atomic_load_explicit(&phase,memory_order_acquire)!=n) sched_yield(); }
static void advance(unsigned n)
{ atomic_store_explicit(&phase,n,memory_order_release); }
static void *worker(void *unused)
{
    (void)unused;cpu_id=1;actor=&tasks[1];
    kernel_cost_register_cpu(&cpus[1]);
    wait_for(1);
    kernel_cost_join(actor);
    kernel_cost_switch(&cpus[1].bootstrap,actor);
    /* A foreign CPU's return must leave begin pending. */
    kernel_cost_boundary();assert(!kernel_cost_capture().epoch);
    advance(2);wait_for(3);
    for (unsigned i=0;i<6000;i++) kernel_cost_add(COST_OPERATIONS,1);
    struct kernel_cost_scope held=kernel_cost_enter(COST_OPERATION_TICKS);
    advance(4);wait_for(5);
    kernel_cost_leave(&held);
    /* The same task cost record follows execution onto another CPU. */
    kernel_cost_switch(actor,&tasks[0]);actor=&tasks[0];
    kernel_cost_add(COST_FILE_CALLS,7);
    advance(6);wait_for(7);
    kernel_cost_switch(actor,&cpus[1].bootstrap);actor=&cpus[1].bootstrap;
    advance(8);wait_for(9);
    kernel_cost_add(COST_OPERATIONS,100);
    advance(10);return NULL;
}
int main(void)
{
    actor=&tasks[0];pthread_t thread;
    assert(pthread_create(&thread,NULL,worker,NULL)==0);
    assert(kernel_cost_begin(9,10000000,0,1)==0);kernel_cost_join(actor);
    advance(1);wait_for(2);
    assert(kernel_cost_read(0,COST_OPERATIONS,&(uint64_t){0})==-KERNEL_EBUSY);
    kernel_cost_boundary();advance(3);
    for (unsigned i=0;i<6000;i++) kernel_cost_add(COST_OPERATIONS,1);
    wait_for(4);
    assert(kernel_cost_end(9,0)==-KERNEL_EBUSY);
    kernel_cost_switch(actor,&idle);actor=&idle;
    advance(5);wait_for(6);
    assert(kernel_cost_end(9,0)==0);
    uint64_t count;
    assert(kernel_cost_read(0,COST_OPERATIONS,&count)==0 && count==12000);
    assert(kernel_cost_read(0,COST_FILE_CALLS,&count)==0 && count==7);
    assert(cpus[0].lock_depth==0 && cpus[1].lock_depth==0 && irq_enabled);
    size_t capacity=kernel_cost_format_capacity();char *output=malloc(capacity);assert(output);
    assert(kernel_cost_format(output,capacity)>0);
    assert(strstr(output,"inflight=0\n") && strstr(output,"state=complete\n"));
    char task_bytes[40];snprintf(task_bytes,sizeof(task_bytes),"task_bytes=%zu\n",KERNEL_COST_TASK_BYTES);
    assert(strstr(output,task_bytes));
    printf("cost registry and CPU storage: %lu bytes; CPU state: %zu bytes\n",
        strtoul(strstr(output,"storage_bytes=")+14,NULL,10),sizeof(struct kernel_cost_cpu));
    free(output);
    advance(7);wait_for(8);
    assert(kernel_cost_begin(9,10000000,1,0)==0);
    uint64_t guard=kernel_cost_lock();advance(9);
    while (!__atomic_load_n(&cpus[1].observer_wait_started,__ATOMIC_ACQUIRE)) sched_yield();
    assert(kernel_cost_end(9,0)==0);
    kernel_cost_unlock(guard);wait_for(10);
    assert(kernel_cost_read(0,COST_OPERATIONS,&count)==0 && !count);
    assert(kernel_cost_read(2,COST_IRQ_OFF_TICKS,&count)==0 && count>0);
    assert(pthread_join(thread,NULL)==0);
    /* Epoch clipping keeps each physical CPU's IRQ interval independent. */
    irq_enabled=0;cpu_id=0;actor=&tasks[0];
    assert(kernel_cost_begin(9,10000000,1,0)==0);
    uint64_t start=kernel_cost_clock();kernel_cost_irq_disabled(start);
    cpu_id=1;actor=&tasks[1];kernel_cost_join(actor);
    uint64_t other=kernel_cost_clock();kernel_cost_irq_disabled(other);
    kernel_cost_irq_enabled(other+13);
    cpu_id=0;actor=&tasks[0];kernel_cost_irq_enabled(start+29);
    assert(kernel_cost_end(9,0)==0);
    assert(kernel_cost_read(0,COST_IRQ_OFF_TICKS,&count)==0 && count==42);
    assert(kernel_cost_rank(21)==6 && kernel_cost_rank(31)==7 && kernel_cost_rank(32)==8 && kernel_cost_rank(35)==9);
    assert(kernel_cost_lock_metric(kernel_cost_rank(35))==COST_LOCK35_ATTEMPTS);
    assert(kernel_cost_rank(45)==10 && kernel_cost_lock_metric(10)==COST_LOCK45_ATTEMPTS);
    puts("cost SMP: recursive IRQ observer, exact concurrent totals, owner boundaries, inflight close, migration and per-CPU IRQ passed");
}
