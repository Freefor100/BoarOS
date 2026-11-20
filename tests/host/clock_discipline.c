/* Drive the real timekeeper from a deterministic 1 GHz counter. */
#include <arch/timer.h>
#include <kernel/time.h>
#include <kernel/timex.h>
#include <kernel/errno.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static uint64_t counter;
uint64_t arch_time_read(void) { return counter; }
static enum kernel_clock_change change;
static int adjust(struct kernel_timex *tx) { return kernel_time_adjust(tx,&change); }

int main(void)
{
    const uint64_t second=1000000000;
    assert(!kernel_time_init(second,100*second));
    struct kernel_timex tx={0};
    assert(adjust(&tx)==5 && tx.maxerror==16000000 && tx.time.sec==100);
    tx=(struct kernel_timex){.modes=KADJ_STATUS|KADJ_FREQUENCY|KADJ_MAXERROR|KADJ_TICK,
        .status=KSTA_PLL,.freq=32768000,.tick=11000,.maxerror=7};
    assert(adjust(&tx)==0 && change==KERNEL_CLOCK_RATE);
    counter=2*second;
    uint64_t mono=kernel_time_monotonic_ns();
    assert(mono>=2200999998 && mono<=2201000000);
    assert(kernel_time_raw_ns()==2*second);
    assert(kernel_time_realtime_ns()==mono+100*second);
    tx=(struct kernel_timex){0};
    assert(!adjust(&tx) && tx.maxerror==1007 && change==KERNEL_CLOCK_UNCHANGED);
    tx=(struct kernel_timex){.modes=KADJ_FREQUENCY|KADJ_TICK|KADJ_NANO|KADJ_TIMECONST|
        KADJ_OFFSET|KADJ_STATUS, .status=KSTA_PLL|KSTA_FREQHOLD,.tick=10000,.offset=16000000};
    assert(!adjust(&tx) && tx.offset==16000000 && kernel_time_monotonic_ns()==mono);
    tx=(struct kernel_timex){.modes=KADJ_SINGLESHOT,.offset=-1250};
    assert(!adjust(&tx) && tx.offset==0);
    counter+=second;
    assert(kernel_time_monotonic_ns()==mono+1003500000);
    tx=(struct kernel_timex){.modes=KADJ_SS_READ};
    assert(!adjust(&tx) && tx.offset==-750);
    tx=(struct kernel_timex){0};
    assert(!adjust(&tx) && tx.offset==12000000);
    uint64_t deadline,target=kernel_time_monotonic_ns()+2*second;
    assert(kernel_time_deadline_from_monotonic(target,&deadline)==KERNEL_TIME_STATUS_OK);
    counter=deadline-1;
    assert(kernel_time_monotonic_ns()<target);
    counter=deadline;
    assert(kernel_time_monotonic_ns()>=target);
    tx=(struct kernel_timex){.modes=KADJ_SS_READ};
    assert(!adjust(&tx) && tx.offset==0);
    uint64_t wall=kernel_time_realtime_ns();
    mono=kernel_time_monotonic_ns();
    tx=(struct kernel_timex){.modes=KADJ_SETOFFSET,.time={-1,750000}};
    assert(adjust(&tx)==5 && change==KERNEL_CLOCK_STEP);
    assert(tx.time.sec==(int64_t)(wall/second) && tx.time.subsec==(int64_t)(wall%second));
    assert(kernel_time_realtime_ns()==wall-250000000 && kernel_time_monotonic_ns()==mono);
    tx=(struct kernel_timex){.modes=KADJ_SINGLESHOT,.offset=INT64_MIN};
    assert(adjust(&tx)==5 && tx.offset==0);
    counter+=second;
    tx=(struct kernel_timex){.modes=KADJ_SS_READ};
    assert(adjust(&tx)==5 && tx.offset==INT64_MIN+500);
    tx=(struct kernel_timex){.modes=KADJ_SINGLESHOT,.offset=INT64_MAX};
    assert(adjust(&tx)==5 && tx.offset==INT64_MIN+500);
    counter+=second;
    tx=(struct kernel_timex){.modes=KADJ_SS_READ};
    assert(adjust(&tx)==5 && tx.offset==INT64_MAX-500);
    tx=(struct kernel_timex){.modes=KADJ_FREQUENCY,.freq=INT64_MAX};
    assert(adjust(&tx)==-KERNEL_EINVAL);
    tx=(struct kernel_timex){.modes=KADJ_SINGLESHOT,.offset=-1000000};
    assert(adjust(&tx)==5);
    tx=(struct kernel_timex){.modes=KADJ_OFFSET|KADJ_STATUS|KADJ_TIMECONST|
        KADJ_NANO|KADJ_FREQUENCY|KADJ_TICK,.status=KSTA_PLL|KSTA_FREQHOLD,
        .offset=-500000000,.freq=-32768000,.tick=9000};
    assert(!adjust(&tx));
    mono=kernel_time_monotonic_ns();
    for (unsigned i=0;i<50000;i++) {
        counter++;
        uint64_t next=kernel_time_monotonic_ns();
        assert(next>=mono);
        mono=next;
    }
    puts("clock discipline host PASS");
}
