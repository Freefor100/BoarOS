/* Real timer state machine; the fixture supplies TIME/SBI/IRQ hardware only. */
#include <arch/riscv/timer.h>
#include <kernel/cpu.h>
#include <assert.h>
#include <stdio.h>
static struct kernel_cpu cpus[BOAROS_MAX_CPUS];
static unsigned current;
static uint64_t now, programmed[BOAROS_MAX_CPUS];
static unsigned set_calls;
static int set_error;
void *allocator_host_cpu_get(void) { return &cpus[current]; }
uint64_t riscv_time_read(void) { return now; }
long sbi_probe_extension(unsigned long ext) { assert(ext==0x54494d45);return 1; }
long sbi_set_timer(uint64_t deadline)
{ set_calls++;if (!set_error) programmed[current]=deadline;return set_error; }
void host_timer_enable(unsigned long flags) { assert(flags==(1UL<<5)); }
static void interrupt(uint64_t at,uint64_t expected_ticks,uint64_t expected_deadline)
{
    now=at;uint64_t ticks=UINT64_MAX;
    enum riscv_timer_status status=riscv_timer_handle_interrupt(&ticks);
    if (status) fprintf(stderr,"timer event at=%llu status=%u; current event=%llu programmed=%llu\n",
        (unsigned long long)at,status,(unsigned long long)cpus[current].timer_deadline,
        (unsigned long long)programmed[current]);
    assert(status==RISCV_TIMER_STATUS_OK && ticks==expected_ticks && programmed[current]==expected_deadline);
}
int main(void)
{
    for (unsigned i=0;i<BOAROS_MAX_CPUS;i++) cpus[i].initialized=KERNEL_CPU_INITIALIZED;
    now=1000;assert(!riscv_timer_start(100,1) && programmed[0]==1100);
    interrupt(1001,0,1100); /* STIP may outlive a completed timer reprogram. */
    assert(!riscv_timer_set_scheduler_deadline(1020));
    now=1021;assert(!riscv_timer_set_scheduler_deadline(0));
    interrupt(1021,0,1100); /* Cancelled event's pending interrupt. */
    assert(!riscv_timer_set_scheduler_deadline(1030));
    now=1031;assert(!riscv_timer_set_scheduler_deadline(1060));
    interrupt(1031,0,1060); /* Replaced event must retain its new deadline. */
    interrupt(1060,0,1100);
    interrupt(1100,1,1200);
    unsigned before=set_calls;
    assert(!riscv_timer_set_scheduler_deadline(1230));
    assert(!riscv_timer_set_scheduler_deadline(1250));
    assert(!riscv_timer_set_scheduler_deadline(1200)); /* Tied with periodic tick. */
    assert(set_calls==before && programmed[0]==1200 && cpus[0].timer_deadline==1200);
    set_error=-1;
    assert(riscv_timer_set_scheduler_deadline(1170)==RISCV_TIMER_STATUS_SBI_SET_FAILED);
    assert(set_calls==before+1 && programmed[0]==1200 && cpus[0].timer_deadline==1200);
    set_error=0;
    assert(!riscv_timer_set_scheduler_deadline(1170));
    assert(set_calls==before+2 && programmed[0]==1170 && cpus[0].timer_deadline==1170);
    assert(!riscv_timer_set_scheduler_deadline(1250));
    assert(set_calls==before+3 && programmed[0]==1200 && cpus[0].timer_deadline==1250);
    interrupt(1200,1,1250); /* The deferred event becomes the hardware deadline. */
    interrupt(1250,0,1300);
    interrupt(1401,2,1500);
    set_error=-1;now=1500;uint64_t ticks=0xa5;
    assert(riscv_timer_handle_interrupt(&ticks)==RISCV_TIMER_STATUS_SBI_SET_FAILED && ticks==0xa5);
    set_error=0;interrupt(1500,1,1600);
#if BOAROS_MAX_CPUS>1
    current=1;ticks=0xa5;
    assert(riscv_timer_handle_interrupt(&ticks)==RISCV_TIMER_STATUS_NOT_STARTED && ticks==0xa5);
    now=2000;assert(!riscv_timer_start(100,1));
    assert(!riscv_timer_set_scheduler_deadline(2050));
    current=0;interrupt(1550,0,1600);assert(programmed[1]==2050);
    current=1;interrupt(2050,0,2100);
#endif
    puts("PASS timer events: pending/cancelled/replaced IRQ, reused hardware deadline, periodic phase, zero false ticks, SBI failure and CPU ownership");
}
