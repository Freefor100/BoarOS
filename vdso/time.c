#include <kernel/vdso.h>

struct vdso_timespec { int64_t seconds, nanoseconds; };
struct vdso_timeval { int64_t seconds, microseconds; };
struct vdso_timezone { int32_t minuteswest, dsttime; };

extern const struct kernel_vdso_time_data __boaros_vvar
    __attribute__((visibility("hidden")));

static inline uint64_t counter(void)
{
    uint64_t result;
#ifdef __riscv
    __asm__ volatile("fence rw,i\n\trdtime %0\n\tfence i,r" : "=r"(result) : : "memory");
#else
    __asm__ volatile("dbar 0\n\trdtime.d %0, $zero\n\tdbar 0" : "=r"(result) : : "memory");
#endif
    return result;
}

static inline int syscall2(long number, long a, long b)
{
    register long arg0 __asm__("a0") = a;
    register long arg1 __asm__("a1") = b;
    register long nr __asm__("a7") = number;
#ifdef __riscv
    __asm__ volatile("ecall" : "+r"(arg0) : "r"(arg1), "r"(nr) : "memory");
#else
    __asm__ volatile("syscall 0" : "+r"(arg0) : "r"(arg1), "r"(nr) :
        "$t0", "$t1", "$t2", "$t3", "$t4", "$t5", "$t6", "$t7", "$t8", "memory");
#endif
    return (int)arg0;
}

/* Keep the original read path leaf-sized until clock control actually
 * installs an adjustment. A changing sequence retries the whole sample. */
static __attribute__((noinline)) uint64_t read_calibrated(int clock)
{
    for (;;) {
        uint32_t seq=__atomic_load_n(&__boaros_vvar.sequence,__ATOMIC_ACQUIRE);
        if (seq&1) continue;
        struct kernel_clock_epoch epoch;
        kernel_clock_load_epoch(&epoch,&__boaros_vvar.epoch);
        uint64_t mult=__atomic_load_n(&__boaros_vvar.multiplier,__ATOMIC_RELAXED);
        uint64_t wall=clock==0?__atomic_load_n(&__boaros_vvar.boot_realtime_ns,__ATOMIC_RELAXED):0;
        uint64_t sample=kernel_clock_mul_q32(counter(),mult);
        if (seq!=__atomic_load_n(&__boaros_vvar.sequence,__ATOMIC_RELAXED)) continue;
        return kernel_clock_apply(&epoch,sample)+wall;
    }
}

static inline __attribute__((always_inline)) int read_clock(int clock, uint64_t *ns)
{
    if (clock != 0 && clock != 1 && clock != 4 && clock != 5 && clock != 6 && clock != 7) return 0;
    for (;;) {
        uint32_t seq=__atomic_load_n(&__boaros_vvar.sequence,__ATOMIC_ACQUIRE);
        if (seq&1) continue;
        uint64_t mult=__atomic_load_n(&__boaros_vvar.fast_multiplier,__ATOMIC_RELAXED);
        if (__builtin_expect(!mult,0)) {
            mult=__atomic_load_n(&__boaros_vvar.multiplier,__ATOMIC_RELAXED);
            if (!mult) return 0;
            if (clock!=4 && clock!=5 && clock!=6) {
                *ns=read_calibrated(clock);
                return 1;
            }
        }
        uint64_t wall=(clock==0||clock==5)?
            __atomic_load_n(&__boaros_vvar.boot_realtime_ns,__ATOMIC_RELAXED):0;
        uint64_t value;
        if (clock==5||clock==6) {
            value=__atomic_load_n(&__boaros_vvar.coarse_monotonic_ns,__ATOMIC_RELAXED);
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
        } else {
            value=kernel_clock_mul_q32(counter(),mult);
        }
        if (seq!=__atomic_load_n(&__boaros_vvar.sequence,__ATOMIC_RELAXED)) continue;
        value+=wall;
        *ns=value;break;
    }
    return 1;
}

int __vdso_clock_gettime(int clock, struct vdso_timespec *ts)
{
    uint64_t ns;
    if (!ts || !read_clock(clock, &ns)) return syscall2(113, clock, (long)ts);
    ts->seconds = ns / UINT64_C(1000000000);
    ts->nanoseconds = ns % UINT64_C(1000000000);
    return 0;
}

int __vdso_clock_getres(int clock, struct vdso_timespec *ts)
{
    if (clock != 0 && clock != 1 && clock != 4 && clock != 5 && clock != 6 && clock != 7)
        return syscall2(114, clock, (long)ts);
    if (ts) {
        if (!__atomic_load_n(&__boaros_vvar.multiplier, __ATOMIC_ACQUIRE))
            return syscall2(114, clock, (long)ts);
        ts->seconds = 0;
        ts->nanoseconds = clock == 5 || clock == 6 ?
            __boaros_vvar.coarse_resolution_ns : 1;
    }
    return 0;
}

int __vdso_gettimeofday(struct vdso_timeval *tv, struct vdso_timezone *tz)
{
    uint64_t ns;
    if (tv) {
        if (!read_clock(0, &ns)) return syscall2(169, (long)tv, (long)tz);
        tv->seconds = ns / UINT64_C(1000000000);
        tv->microseconds = ns % UINT64_C(1000000000) / 1000U;
    }
    if (tz) for (;;) {
        uint32_t seq=__atomic_load_n(&__boaros_vvar.sequence,__ATOMIC_ACQUIRE);
        if (seq&1) continue;
        struct vdso_timezone value={
            __atomic_load_n(&__boaros_vvar.minuteswest,__ATOMIC_RELAXED),
            __atomic_load_n(&__boaros_vvar.dsttime,__ATOMIC_RELAXED)};
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (seq!=__atomic_load_n(&__boaros_vvar.sequence,__ATOMIC_RELAXED)) continue;
        *tz=value;break;
    }
    return 0;
}
