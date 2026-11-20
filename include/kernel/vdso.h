#ifndef BOAROS_KERNEL_VDSO_H
#define BOAROS_KERNEL_VDSO_H

#define KERNEL_VDSO_RESTORER_OFFSET 0x100
#define KERNEL_VDSO_CLOCK_SHIFT 32U

#ifndef __ASSEMBLER__
#include <stdint.h>

/* An epoch of the disciplined clock. All coefficients are constant until a
 * control operation replaces the epoch. Ordinary ticks never rewrite it. */
struct kernel_clock_epoch {
    uint64_t raw_ns, mono_ns;
    int64_t rate_q32, phase_ns, slew_ns;
    uint64_t phase_shift;
};

static inline uint64_t kernel_clock_mul_q32(uint64_t a, uint64_t b)
{
    return (uint64_t)(((unsigned __int128)a*b) >> 32);
}

/* A phase-locked loop drains 1/(2^shift) of the outstanding phase each
 * second. Exponentiation covers arbitrary idle intervals without ticking. */
static inline uint64_t kernel_clock_phase_second(uint64_t magnitude, uint64_t shift, uint64_t seconds)
{
    uint64_t factor=(UINT64_C(1)<<32)-(UINT64_C(1)<<(32-shift));
    while (seconds && magnitude) {
        if (seconds & 1) magnitude=kernel_clock_mul_q32(magnitude,factor);
        seconds>>=1;
        factor=kernel_clock_mul_q32(factor,factor);
    }
    return magnitude;
}

static inline int64_t kernel_clock_phase_q32(int64_t phase,uint64_t shift,uint64_t elapsed)
{
    if (!phase) return 0;
    uint64_t magnitude=(uint64_t)(phase<0?-phase:phase)<<32;
    uint64_t seconds=elapsed/1000000000, fraction=elapsed%1000000000;
    uint64_t start=kernel_clock_phase_second(magnitude,shift,seconds);
    if (start && fraction) {
        uint64_t end=kernel_clock_phase_second(magnitude,shift,seconds+1);
        uint64_t change=start-end;
        /* Divide before multiplying the integer part to avoid a 128-bit divide. */
        start-=change/1000000000*fraction+(change%1000000000)*fraction/1000000000;
    }
    return phase<0?-(int64_t)start:(int64_t)start;
}

static inline int64_t kernel_clock_phase_left(int64_t phase,uint64_t shift,uint64_t elapsed)
{
    return kernel_clock_phase_q32(phase,shift,elapsed)/INT64_C(4294967296);
}

static inline int64_t kernel_clock_slew_used(int64_t pending, uint64_t elapsed)
{
    /* 500 us correction per raw second. The representable correction in
     * one epoch exceeds what the counter can deliver before it wraps. */
    int64_t capacity = (int64_t)(elapsed / 2000);
    if (pending > capacity) return capacity;
    if (pending < -capacity) return -capacity;
    return pending;
}

static inline uint64_t kernel_clock_apply(const struct kernel_clock_epoch *epoch, uint64_t raw)
{
    uint64_t elapsed=raw-epoch->raw_ns;
    if (!epoch->rate_q32 && !epoch->phase_ns && !epoch->slew_ns)
        return epoch->mono_ns+elapsed;
    /* Round the sum once. Separately rounding negative frequency, phase
     * and single-shot corrections can make a 1 GHz clock run backward. */
    __int128 delta=(__int128)elapsed*(INT64_C(4294967296)+epoch->rate_q32);
    if (epoch->phase_ns)
        delta+=(__int128)epoch->phase_ns*INT64_C(4294967296)-
               kernel_clock_phase_q32(epoch->phase_ns,epoch->phase_shift,elapsed);
    if (epoch->slew_ns) {
        int64_t used=kernel_clock_slew_used(epoch->slew_ns,elapsed);
        delta+=(__int128)used*INT64_C(4294967296);
        if (used!=epoch->slew_ns) {
            int64_t fraction=(int64_t)(((elapsed%2000)<<32)/2000);
            delta+=epoch->slew_ns<0?-fraction:fraction;
        }
    }
    return epoch->mono_ns+(uint64_t)(delta>>32);
}

static inline void kernel_clock_load_epoch(struct kernel_clock_epoch *out,
                                            const struct kernel_clock_epoch *shared)
{
#define LOAD_CLOCK_FIELD(field) out->field = __atomic_load_n(&shared->field,__ATOMIC_RELAXED)
    LOAD_CLOCK_FIELD(raw_ns);
    LOAD_CLOCK_FIELD(mono_ns);
    LOAD_CLOCK_FIELD(rate_q32);
    LOAD_CLOCK_FIELD(phase_ns);
    LOAD_CLOCK_FIELD(slew_ns);
    LOAD_CLOCK_FIELD(phase_shift);
#undef LOAD_CLOCK_FIELD
}

/* Mutable clock offset and timezone are published under sequence. Readers
 * sample the counter inside that sequence; monotonic itself never steps. */
struct kernel_vdso_time_data {
    uint32_t sequence;
    int32_t minuteswest, dsttime;
    uint32_t reserved;
    uint64_t multiplier;
    /* Equal to the raw multiplier until an epoch requires calibration. */
    uint64_t fast_multiplier;
    uint64_t boot_realtime_ns;
    uint64_t coarse_monotonic_ns;
    uint64_t coarse_resolution_ns;
    struct kernel_clock_epoch epoch;
};

#if defined(__riscv) || defined(__loongarch__)
#define KERNEL_HAS_VDSO_ELF 1
struct physical_page_allocator;
/* One backing-owner reference per MM, independent of its mapped PTE. */
int kernel_time_vvar_acquire(struct physical_page_allocator *, uint64_t *);
void kernel_time_vvar_retain(uint64_t);
void kernel_time_vvar_release(struct physical_page_allocator *, uint64_t);
#else
#define KERNEL_HAS_VDSO_ELF 0
#endif
#endif

#endif
