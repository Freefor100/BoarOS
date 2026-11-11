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
    __asm__ volatile("rdtime %0" : "=r"(result));
#else
    __asm__ volatile("rdtime.d %0, $zero" : "=r"(result));
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

/* Kernel clock IDs: REALTIME=0, MONOTONIC=1 and the two COARSE clocks=5/6. */
static inline int read_clock(int clock, uint64_t *ns)
{
    if (clock != 0 && clock != 1 && clock != 5 && clock != 6) return 0;
    uint64_t mult = __atomic_load_n(&__boaros_vvar.multiplier, __ATOMIC_ACQUIRE);
    if (!mult) return 0;
    if (clock == 5 || clock == 6)
        *ns = __atomic_load_n(&__boaros_vvar.coarse_monotonic_ns, __ATOMIC_RELAXED);
    else
        *ns = (uint64_t)(((unsigned __int128)counter() * mult) >> KERNEL_VDSO_CLOCK_SHIFT);
    if (clock == 0 || clock == 5) *ns += __boaros_vvar.boot_realtime_ns;
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
    if (clock != 0 && clock != 1 && clock != 5 && clock != 6)
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
    if (tz) *tz = (struct vdso_timezone){0, 0};
    return 0;
}
