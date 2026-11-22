#ifndef BOAROS_KERNEL_TIMEX_H
#define BOAROS_KERNEL_TIMEX_H
#include <stddef.h>
#include <stdint.h>

/* Native RV64 and LA64 Linux syscall layout (uapi/linux/timex.h). */
struct kernel_timex {
    uint32_t modes, pad0;
    int64_t offset, freq, maxerror, esterror;
    int32_t status, pad1;
    int64_t constant, precision, tolerance;
    struct { int64_t sec, subsec; } time;
    int64_t tick, ppsfreq, jitter;
    int32_t shift, pad2;
    int64_t stabil, jitcnt, calcnt, errcnt, stbcnt;
    int32_t tai, pad3[11];
};
_Static_assert(sizeof(struct kernel_timex) == 208 && offsetof(struct kernel_timex,time) == 72 &&
    offsetof(struct kernel_timex,tai) == 160, "Linux 64-bit timex layout");

enum {
    KADJ_OFFSET=1, KADJ_FREQUENCY=2, KADJ_MAXERROR=4, KADJ_ESTERROR=8,
    KADJ_STATUS=16, KADJ_TIMECONST=32, KADJ_TAI=128, KADJ_SETOFFSET=256,
    KADJ_MICRO=4096, KADJ_NANO=8192, KADJ_TICK=16384,
    KADJ_ADJTIME=32768, KADJ_SINGLESHOT=32769, KADJ_SS_READ=40961,
    KSTA_PLL=1, KSTA_PPSFREQ=2, KSTA_PPSTIME=4, KSTA_FLL=8, KSTA_INS=16,
    KSTA_DEL=32, KSTA_UNSYNC=64, KSTA_FREQHOLD=128, KSTA_NANO=8192, KSTA_MODE=16384,
};
static inline int kernel_timex_read_only(uint32_t modes)
{
    return !modes || ((modes & KADJ_SS_READ)==KADJ_SS_READ && !(modes & KADJ_SETOFFSET));
}

enum kernel_clock_change {
    KERNEL_CLOCK_UNCHANGED = 0,
    KERNEL_CLOCK_RATE = 1,
    KERNEL_CLOCK_STEP = 2,
    KERNEL_CLOCK_TAI_OFFSET = 4,
};
/* The scheduler holds the wait domain across mutation and wait-tree reindexing. */
int kernel_time_adjust(struct kernel_timex *, enum kernel_clock_change *);
int kernel_scheduler_adjust_time(struct kernel_timex *, enum kernel_clock_change *);
#endif
