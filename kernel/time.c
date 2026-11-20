#include <kernel/cost.h>
#include <arch/timer.h>
#include <kernel/time.h>
#include <kernel/timex.h>
#include <kernel/errno.h>
#include <kernel/tick.h>
#include <kernel/vdso.h>

#include <stdint.h>

#define KERNEL_TIME_NS_PER_SECOND UINT64_C(1000000000)
#define KERNEL_TIME_MULTIPLIER_SHIFT 32U

/*
 * Nanoseconds are derived from the arch time-counter through a fixed-point
 * multiplier `m = ceil(1e9 * 2^32 / frequency)` computed once at init, so
 * the hot path is one 64x64->128 multiply-high per read instead of a
 * division. The absolute coefficient error is below 2^-32; relative
 * error depends on frequency. Deadlines also round up in the tick domain.
 */
static uint32_t time_initialized;
static uint64_t time_multiplier;
static uint64_t time_ticks_multiplier;
static uint64_t time_boot_realtime_ns;
static uint64_t time_coarse_monotonic_ns;
static int32_t time_timezone[2];
static uint32_t time_sequence, time_disciplined;
static struct kernel_clock_epoch time_epoch;
/* Control fields are serialized by the clock-control mutex. */
#define TIME_ERROR_LIMIT INT64_C(16000000)
#define TIME_FREQ_LIMIT INT64_C(32768000)
static int64_t time_freq, time_tick = 10000, time_constant = 2;
static int32_t time_status = KSTA_UNSYNC;
static int64_t time_maxerror = TIME_ERROR_LIMIT, time_esterror = TIME_ERROR_LIMIT;
static uint64_t time_error_second, time_reference_second;
/* Full signed 64-bit adjtime microseconds, including a sub-us residual. */
static int64_t time_slew_us, time_slew_fraction;

static void store_epoch(struct kernel_clock_epoch *to,const struct kernel_clock_epoch *from)
{
#define STORE_CLOCK_FIELD(field) __atomic_store_n(&to->field,from->field,__ATOMIC_RELAXED)
    STORE_CLOCK_FIELD(raw_ns);
    STORE_CLOCK_FIELD(mono_ns);
    STORE_CLOCK_FIELD(rate_q32);
    STORE_CLOCK_FIELD(phase_ns);
    STORE_CLOCK_FIELD(slew_ns);
    STORE_CLOCK_FIELD(phase_shift);
#undef STORE_CLOCK_FIELD
}

#if KERNEL_HAS_VDSO_ELF
#include <kernel/page.h>
#include <kernel/physical_page.h>
#include <kernel/raw_lock.h>
#include <string.h>

static struct kernel_raw_lock vvar_lock = {
    .rank = KERNEL_RAW_RANK_WAIT_QUEUE, .initialized = KERNEL_RAW_INITIALIZED
};
static struct kernel_vdso_time_data *vvar;
static struct physical_page_allocator *vvar_allocator;
static uint64_t vvar_page;
static uint32_t vvar_owners;

static void finish_vvar(void)
{
    __atomic_store_n(&vvar->boot_realtime_ns, time_boot_realtime_ns, __ATOMIC_RELAXED);
    __atomic_store_n(&vvar->minuteswest, time_timezone[0], __ATOMIC_RELAXED);
    __atomic_store_n(&vvar->dsttime, time_timezone[1], __ATOMIC_RELAXED);
    store_epoch(&vvar->epoch,&time_epoch);
    __atomic_store_n(&vvar->fast_multiplier,time_disciplined?0:time_multiplier,__ATOMIC_RELAXED);
    vvar->coarse_resolution_ns = kernel_time_coarse_resolution_ns();
    __atomic_store_n(&vvar->coarse_monotonic_ns,
        __atomic_load_n(&time_coarse_monotonic_ns, __ATOMIC_RELAXED), __ATOMIC_RELAXED);
    __atomic_store_n(&vvar->multiplier, time_multiplier, __ATOMIC_RELEASE);
    __atomic_add_fetch(&vvar->sequence, 1, __ATOMIC_RELEASE);
}

static void publish_vvar(void)
{
    __atomic_add_fetch(&vvar->sequence,1,__ATOMIC_ACQ_REL);
    finish_vvar();
}

int kernel_time_vvar_acquire(struct physical_page_allocator *allocator, uint64_t *page)
{
    uint64_t candidate;
    void *pointer;
    {
        KERNEL_RAW_SCOPE(guard, &vvar_lock);
        if (vvar) {
            if (vvar_allocator != allocator || vvar_owners == UINT32_MAX) __builtin_trap();
            vvar_owners++;
            *page = vvar_page;
            return 0;
        }
    }
    if (physical_page_allocate(allocator, &candidate) != PHYSICAL_PAGE_STATUS_OK) return -1;
    if (physical_page_map_owned(allocator, candidate, &pointer) != PHYSICAL_PAGE_STATUS_OK)
        __builtin_trap();
    /* The whole page is exposed, so only publish a cleared allocation. */
    memset(pointer, 0, BOAROS_PAGE_SIZE);
    {
        KERNEL_RAW_SCOPE(guard, &vvar_lock);
        if (!vvar) {
            vvar = pointer;
            vvar_allocator = allocator;
            vvar_page = candidate;
            vvar_owners = 1;
            if (time_initialized) publish_vvar();
            *page = candidate;
            return 0;
        }
        if (vvar_allocator != allocator || vvar_owners == UINT32_MAX) __builtin_trap();
        vvar_owners++;
        *page = vvar_page;
    }
    if (physical_page_release(allocator, candidate) != PHYSICAL_PAGE_STATUS_OK) __builtin_trap();
    return 0;
}

void kernel_time_vvar_retain(uint64_t page)
{
    KERNEL_RAW_SCOPE(guard, &vvar_lock);
    if (!vvar || vvar_page != page || !vvar_owners || vvar_owners == UINT32_MAX) __builtin_trap();
    vvar_owners++;
}

void kernel_time_vvar_release(struct physical_page_allocator *allocator, uint64_t page)
{
    {
        KERNEL_RAW_SCOPE(guard, &vvar_lock);
        if (!vvar || vvar_allocator != allocator || vvar_page != page || !vvar_owners)
            __builtin_trap();
        if (--vvar_owners) return;
        /* Stop the tick writer before returning the last kernel reference. */
        vvar = 0;
        vvar_allocator = 0;
        vvar_page = 0;
    }
    if (physical_page_release(allocator, page) != PHYSICAL_PAGE_STATUS_OK) __builtin_trap();
}
#endif

enum kernel_time_status kernel_time_init(uint32_t timebase_frequency,
                                         uint64_t boot_realtime_ns)
{
    if (timebase_frequency == 0U) {
        return KERNEL_TIME_STATUS_INVALID_ARGUMENT;
    }
    if (time_initialized != 0U) {
        return KERNEL_TIME_STATUS_ALREADY_INITIALIZED;
    }

    time_multiplier =
        ((KERNEL_TIME_NS_PER_SECOND << KERNEL_TIME_MULTIPLIER_SHIFT) +
         timebase_frequency - 1U) /
        timebase_frequency;
    time_ticks_multiplier =
        (((uint64_t)timebase_frequency << KERNEL_TIME_MULTIPLIER_SHIFT) +
         KERNEL_TIME_NS_PER_SECOND - 1U) /
        KERNEL_TIME_NS_PER_SECOND;
    time_boot_realtime_ns = boot_realtime_ns;
    __atomic_store_n(&time_coarse_monotonic_ns,
        kernel_time_ticks_to_ns(arch_time_read()), __ATOMIC_RELAXED);
#if KERNEL_HAS_VDSO_ELF
    {
        KERNEL_RAW_SCOPE(guard, &vvar_lock);
        time_initialized = 1U;
        if (vvar) publish_vvar();
    }
#else
    time_initialized = 1U;
#endif
#if BOAROS_COST_DIAGNOSTICS
    kernel_cost_set_timebase(timebase_frequency);
#endif
    return KERNEL_TIME_STATUS_OK;
}

uint32_t kernel_time_is_initialized(void)
{
    return time_initialized;
}

uint64_t kernel_time_ticks_to_ns(uint64_t ticks)
{
    return (uint64_t)(((unsigned __int128)ticks * time_multiplier) >>
                      KERNEL_TIME_MULTIPLIER_SHIFT);
}

uint64_t kernel_time_raw_ns(void)
{
    return kernel_time_ticks_to_ns(arch_time_read());
}

/* TIME is an I/O input on RISC-V. Bracket the CSR read as well as
 * normal loads when sampling a mutable clock epoch. */
static uint64_t clock_counter_ordered(void)
{
#if defined(__riscv) || defined(__loongarch__)
    return arch_time_read_ordered();
#else
    return arch_time_read();
#endif
}

static __attribute__((noinline)) uint64_t apply_published_epoch(uint64_t raw)
{
    struct kernel_clock_epoch epoch;
    kernel_clock_load_epoch(&epoch,&time_epoch);
    return kernel_clock_apply(&epoch,raw);
}

static uint64_t read_time(int realtime)
{
    for (;;) {
        uint32_t seq=__atomic_load_n(&time_sequence,__ATOMIC_ACQUIRE);
        if (seq & 1) continue;
        uint64_t now=kernel_time_ticks_to_ns(clock_counter_ordered());
        if (__builtin_expect(__atomic_load_n(&time_disciplined,__ATOMIC_RELAXED),0))
            now=apply_published_epoch(now);
        if (realtime) now+=__atomic_load_n(&time_boot_realtime_ns,__ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (seq==__atomic_load_n(&time_sequence,__ATOMIC_RELAXED)) return now;
    }
}

uint64_t kernel_time_monotonic_ns(void)
{
    return read_time(0);
}

uint64_t kernel_time_realtime_ns(void)
{
    return read_time(1);
}

void kernel_time_update_coarse(void)
{
    if (!time_initialized) return;
    /* 合并 tick 只采样一次真实 counter，不按补发次数虚构时间。 */
    uint64_t now = kernel_time_monotonic_ns();
#if KERNEL_HAS_VDSO_ELF
    KERNEL_RAW_SCOPE(guard, &vvar_lock);
    if (vvar) __atomic_store_n(&vvar->coarse_monotonic_ns, now, __ATOMIC_RELAXED);
#endif
    __atomic_store_n(&time_coarse_monotonic_ns, now, __ATOMIC_RELAXED);
}

uint64_t kernel_time_coarse_monotonic_ns(void)
{
#if KERNEL_HAS_VDSO_ELF
    /* Use the same sample as user space. A caller on another CPU cannot
     * observe publication half-way through the tick update. */
    KERNEL_RAW_SCOPE(guard, &vvar_lock);
    if (vvar) return __atomic_load_n(&vvar->coarse_monotonic_ns, __ATOMIC_RELAXED);
#endif
    return __atomic_load_n(&time_coarse_monotonic_ns, __ATOMIC_RELAXED);
}

uint64_t kernel_time_coarse_realtime_ns(void)
{
    for (;;) {
        uint32_t seq=__atomic_load_n(&time_sequence,__ATOMIC_ACQUIRE);
        if (seq&1) continue;
        uint64_t offset=__atomic_load_n(&time_boot_realtime_ns,__ATOMIC_RELAXED);
        uint64_t value=offset+kernel_time_coarse_monotonic_ns();
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (seq==__atomic_load_n(&time_sequence,__ATOMIC_RELAXED)) return value;
    }
}

uint64_t kernel_time_coarse_resolution_ns(void)
{
    return KERNEL_TIME_NS_PER_SECOND / KERNEL_TICKS_PER_SECOND;
}

uint64_t kernel_time_boot_realtime_offset(void)
{
    return __atomic_load_n(&time_boot_realtime_ns, __ATOMIC_ACQUIRE);
}

static enum kernel_time_status raw_deadline(uint64_t target_monotonic_ns,uint64_t *deadline)
{
    uint64_t now_ticks,now_ns,delta_ns;
    unsigned __int128 delta_ticks;
    now_ticks = arch_time_read();
    now_ns = kernel_time_ticks_to_ns(now_ticks);
    if (target_monotonic_ns <= now_ns) return KERNEL_TIME_STATUS_DEADLINE_PASSED;
    delta_ns = target_monotonic_ns - now_ns;
    delta_ticks = ((unsigned __int128)delta_ns * time_ticks_multiplier +
                   UINT32_MAX) >> KERNEL_TIME_MULTIPLIER_SHIFT;
    /* The scheduler compares wrapping tick deadlines with signed deltas.
     * Keep the future distance within that half-range; zero means no timer. */
    if (delta_ticks > INT64_MAX) {
        delta_ticks = INT64_MAX;
    }
    *deadline = now_ticks + (uint64_t)delta_ticks;
    if (*deadline == 0U) {
        *deadline = 1U;
    }
    return KERNEL_TIME_STATUS_OK;
}

enum kernel_time_status kernel_time_deadline_from_raw(uint64_t target,uint64_t *deadline)
{
    if (!deadline) return KERNEL_TIME_STATUS_INVALID_ARGUMENT;
    if (!time_initialized) return KERNEL_TIME_STATUS_NOT_INITIALIZED;
    return raw_deadline(target,deadline);
}

enum kernel_time_status kernel_time_deadline_from_monotonic(
    uint64_t target_monotonic_ns,
    uint64_t *deadline)
{
    uint64_t now_ticks;
    uint64_t now_ns;
    uint64_t delta_ns;

    if (deadline == 0) {
        return KERNEL_TIME_STATUS_INVALID_ARGUMENT;
    }
    if (time_initialized == 0U) {
        return KERNEL_TIME_STATUS_NOT_INITIALIZED;
    }

    if (__atomic_load_n(&time_disciplined,__ATOMIC_ACQUIRE)) {
        /* Invert the same monotone function the reader evaluates. Its
         * slowest supported slope exceeds 0.77, including both slews and
         * the -10% tick limit. Twice the nominal distance brackets expiry.
         * The search runs at arm/reconfiguration, never on each tick. */
        for (;;) {
            uint32_t seq=__atomic_load_n(&time_sequence,__ATOMIC_ACQUIRE);
            if (seq&1) continue;
            struct kernel_clock_epoch epoch;
            kernel_clock_load_epoch(&epoch,&time_epoch);
            now_ticks=clock_counter_ordered();
            now_ns=kernel_clock_apply(&epoch,kernel_time_ticks_to_ns(now_ticks));
            uint64_t high=0;
            if (target_monotonic_ns>now_ns) {
                delta_ns=target_monotonic_ns-now_ns;
                unsigned __int128 span=(((unsigned __int128)delta_ns*time_ticks_multiplier) >> 31)+4;
                high=span>INT64_MAX?INT64_MAX:(uint64_t)span;
                if (high>UINT64_MAX-now_ticks) high=UINT64_MAX-now_ticks;
                uint64_t low=0;
                while (high-low>1) {
                    uint64_t mid=low+(high-low)/2;
                    uint64_t raw=kernel_time_ticks_to_ns(now_ticks+mid);
                    if (kernel_clock_apply(&epoch,raw)>=target_monotonic_ns) high=mid;
                    else low=mid;
                }
            }
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            if (seq!=__atomic_load_n(&time_sequence,__ATOMIC_RELAXED)) continue;
            if (!high) return KERNEL_TIME_STATUS_DEADLINE_PASSED;
            *deadline=now_ticks+high;
            return KERNEL_TIME_STATUS_OK;
        }
    }
    return raw_deadline(target_monotonic_ns,deadline);
}

static int64_t clamp64(int64_t n,int64_t low,int64_t high)
{
    return n<low ? low : n>high ? high : n;
}

static void age_errors(uint64_t second)
{
    uint64_t elapsed=second-time_error_second;
    if (elapsed>(uint64_t)(TIME_ERROR_LIMIT-time_maxerror)/500) {
        time_maxerror=TIME_ERROR_LIMIT;
        time_status|=KSTA_UNSYNC;
    } else time_maxerror+=(int64_t)elapsed*500;
    time_error_second=second;
}

static void reset_discipline(uint64_t second)
{
    time_status|=KSTA_UNSYNC;
    time_maxerror=time_esterror=TIME_ERROR_LIMIT;
    time_error_second=second;
    time_slew_us=time_slew_fraction=0;
}

/* Keep the remainder in the same direction as the whole microseconds.
 * Splitting the subtraction first supports the entire signed-long ABI. */
static int64_t remaining_slew(int64_t used,int64_t *fraction)
{
    int64_t whole=time_slew_us-used/1000;
    int64_t rem=time_slew_fraction-used%1000;
    whole+=rem/1000;
    rem%=1000;
    if (whole>0 && rem<0) {
        whole--; rem+=1000;
    } else if (whole<0 && rem>0) {
        whole++; rem-=1000;
    }
    *fraction=rem;
    return whole;
}

static void publish_epoch(struct kernel_clock_epoch *epoch)
{
    __int128 slew=(__int128)time_slew_us*1000+time_slew_fraction;
    epoch->slew_ns=slew>INT64_MAX?INT64_MAX:slew<INT64_MIN?INT64_MIN:(int64_t)slew;
    epoch->rate_q32=(time_tick-10000)*INT64_C(4294967296)/10000 +
                    time_freq*65536/1000000;
    epoch->phase_shift=(uint64_t)(2+time_constant);
    store_epoch(&time_epoch,epoch);
    __atomic_store_n(&time_disciplined,epoch->raw_ns!=epoch->mono_ns ||
        epoch->rate_q32 || epoch->phase_ns || epoch->slew_ns,__ATOMIC_RELEASE);
}

static struct kernel_clock_epoch advance_epoch(uint64_t raw)
{
    struct kernel_clock_epoch next=time_epoch;
    uint64_t elapsed=raw-next.raw_ns;
    next.mono_ns=kernel_clock_apply(&time_epoch,raw);
    next.phase_ns=kernel_clock_phase_left(next.phase_ns,next.phase_shift,elapsed);
    time_slew_us=remaining_slew(kernel_clock_slew_used(next.slew_ns,elapsed),&time_slew_fraction);
    next.raw_ns=raw;
    return next;
}

/* Mark both reader interfaces busy before sampling the old clock at the
 * new epoch. A concurrent reader cannot publish a later old-rate sample
 * and then observe a smaller new-rate reading after a negative slew. */
static void begin_time_change(void)
{
    __atomic_add_fetch(&time_sequence,1,__ATOMIC_ACQ_REL);
#if KERNEL_HAS_VDSO_ELF
    if (vvar) __atomic_add_fetch(&vvar->sequence,1,__ATOMIC_ACQ_REL);
#endif
}

static void end_time_change(void)
{
#if KERNEL_HAS_VDSO_ELF
    if (vvar) finish_vvar();
#endif
    __atomic_add_fetch(&time_sequence,1,__ATOMIC_RELEASE);
}

enum kernel_time_status kernel_time_set_realtime(uint64_t ns)
{
#if KERNEL_HAS_VDSO_ELF
    KERNEL_RAW_SCOPE(guard,&vvar_lock);
#endif
    begin_time_change();
    uint64_t raw=kernel_time_ticks_to_ns(clock_counter_ordered());
    uint64_t now=kernel_clock_apply(&time_epoch,raw);
    if (ns < now || ns > INT64_MAX) {
        end_time_change();
        return KERNEL_TIME_STATUS_INVALID_ARGUMENT;
    }
    struct kernel_clock_epoch epoch=advance_epoch(raw);
    reset_discipline(raw/1000000000);
    epoch.phase_ns=0;
    publish_epoch(&epoch);
    __atomic_store_n(&time_boot_realtime_ns, ns-now, __ATOMIC_RELAXED);
    end_time_change();
    return KERNEL_TIME_STATUS_OK;
}

static void apply_phase(struct kernel_clock_epoch *epoch,int64_t offset,uint64_t second)
{
    if (!(time_status&KSTA_PLL)) return;
    int64_t phase=time_status&KSTA_NANO ? clamp64(offset,-500000000,500000000) :
                                         clamp64(offset,-500000,500000)*1000;
    uint64_t interval=second-time_reference_second;
    time_reference_second=second;
    if (time_status&KSTA_FREQHOLD) interval=0;
    int64_t feedback=0;
    time_status&=~KSTA_MODE;
    if (interval>=256 && ((time_status&KSTA_FLL) || interval>2048)) {
        feedback=phase*65536/(int64_t)interval/4000;
        time_status|=KSTA_MODE;
    }
    int64_t limit=INT64_C(1)<<(3+time_constant);
    int64_t secs=interval>(uint64_t)limit?limit:(int64_t)interval;
    feedback+=phase*secs*65536/(INT64_C(1000)<<(2*(4+time_constant)));
    time_freq=clamp64(time_freq+feedback,-TIME_FREQ_LIMIT,TIME_FREQ_LIMIT);
    epoch->phase_ns=phase;
}

int kernel_time_adjust(struct kernel_timex *tx,enum kernel_clock_change *change)
{
    uint32_t mode=tx->modes;
    *change=KERNEL_CLOCK_UNCHANGED;
    int adjtime=(mode&KADJ_ADJTIME)!=0;
    int single_set=adjtime && !(mode&KADJ_NANO);
    if (adjtime && !(mode&KADJ_OFFSET)) return -KERNEL_EINVAL;
    if (!adjtime && (mode&KADJ_TICK) && (tx->tick<9000 || tx->tick>11000))
        return -KERNEL_EINVAL;
    if ((mode&KADJ_FREQUENCY) && (tx->freq<INT64_MIN/65536000 ||
                                    tx->freq>INT64_MAX/65536000)) return -KERNEL_EINVAL;
    if ((mode&KADJ_SETOFFSET) && (uint64_t)tx->time.subsec >=
            ((mode&KADJ_NANO)?1000000000U:1000000U)) return -KERNEL_EINVAL;
    if (!adjtime && ((mode&KADJ_TAI) || ((mode&KADJ_STATUS) && (tx->status&(KSTA_INS|KSTA_DEL)))))
        return -KERNEL_EOPNOTSUPP;
#if KERNEL_HAS_VDSO_ELF
    KERNEL_RAW_SCOPE(guard,&vvar_lock);
#endif
    int changing=!kernel_timex_read_only(mode);
    if (changing) begin_time_change();
    uint64_t raw=kernel_time_ticks_to_ns(clock_counter_ordered()), second=raw/1000000000;
    uint64_t elapsed=raw-time_epoch.raw_ns;
    uint64_t mono=kernel_clock_apply(&time_epoch,raw);
    uint64_t wall=mono+time_boot_realtime_ns;
    uint64_t sampled_wall=wall;
    if (mode&KADJ_SETOFFSET) {
        __int128 next=(__int128)wall+(__int128)tx->time.sec*1000000000+
                      tx->time.subsec*((mode&KADJ_NANO)?1:1000);
        if (next<mono || next>INT64_MAX) {
            end_time_change();
            return -KERNEL_EINVAL;
        }
        wall=(uint64_t)next;
        *change=KERNEL_CLOCK_STEP;
    }
    age_errors(second);
    int64_t phase=kernel_clock_phase_left(time_epoch.phase_ns,time_epoch.phase_shift,elapsed);
    int64_t used=kernel_clock_slew_used(time_epoch.slew_ns,elapsed);
    int64_t fraction;
    int64_t saved_slew=remaining_slew(used,&fraction);

    if (changing) {
        struct kernel_clock_epoch epoch;
        int mapping=single_set || (mode&KADJ_SETOFFSET) ||
            (!adjtime && (mode&(KADJ_TICK|KADJ_FREQUENCY|KADJ_OFFSET|KADJ_TIMECONST)));
        if (mapping) epoch=advance_epoch(raw);
        if (mode&KADJ_SETOFFSET) {
            reset_discipline(second);
            epoch.phase_ns=0;
            saved_slew=0;
            __atomic_store_n(&time_boot_realtime_ns,wall-mono,__ATOMIC_RELAXED);
        }
        if (single_set) {
            time_slew_us=tx->offset;
            time_slew_fraction=0;
        } else if (!adjtime) {
            if (mode&KADJ_STATUS) {
                if (!(time_status&KSTA_PLL) && (tx->status&KSTA_PLL)) time_reference_second=second;
                time_status=(time_status&(KSTA_NANO|KSTA_MODE)) | (tx->status&255);
            }
            if (mode&KADJ_NANO) time_status|=KSTA_NANO;
            if (mode&KADJ_MICRO) time_status&=~KSTA_NANO;
            if (mode&KADJ_FREQUENCY) time_freq=clamp64(tx->freq,-TIME_FREQ_LIMIT,TIME_FREQ_LIMIT);
            if (mode&KADJ_TICK) time_tick=tx->tick;
            if (mode&KADJ_MAXERROR) time_maxerror=clamp64(tx->maxerror,0,TIME_ERROR_LIMIT);
            if (mode&KADJ_ESTERROR) time_esterror=clamp64(tx->esterror,0,TIME_ERROR_LIMIT);
            if (mode&KADJ_TIMECONST) {
                time_constant=clamp64(tx->constant,0,10);
                if (!(time_status&KSTA_NANO)) time_constant=clamp64(time_constant+4,0,10);
            }
            if (mode&KADJ_OFFSET) apply_phase(&epoch,tx->offset,second);
        }
        if (mapping) {
            phase=epoch.phase_ns;
            publish_epoch(&epoch);
            if (*change==KERNEL_CLOCK_UNCHANGED) *change=KERNEL_CLOCK_RATE;
        }
        end_time_change();
    }
    int64_t unit=(time_status&KSTA_NANO)?1:1000;
    tx->offset=(mode&KADJ_ADJTIME)?saved_slew:phase/unit;
    tx->freq=time_freq;
    tx->maxerror=time_maxerror;
    tx->esterror=time_esterror;
    tx->status=time_status;
    tx->constant=time_constant;
    tx->precision=1;
    tx->tolerance=TIME_FREQ_LIMIT;
    tx->tick=time_tick;
    tx->time.sec=(int64_t)(sampled_wall/1000000000);
    tx->time.subsec=(int64_t)(sampled_wall%1000000000)/unit;
    tx->ppsfreq=tx->jitter=tx->stabil=tx->jitcnt=tx->calcnt=tx->errcnt=tx->stbcnt=0;
    tx->shift=tx->tai=0;
    return (time_status&(KSTA_UNSYNC|KSTA_PPSFREQ|KSTA_PPSTIME))?5:0;
}
void kernel_time_get_timezone(int32_t timezone[2])
{
#if KERNEL_HAS_VDSO_ELF
    KERNEL_RAW_SCOPE(guard, &vvar_lock);
#endif
    timezone[0]=time_timezone[0];timezone[1]=time_timezone[1];
}
void kernel_time_set_timezone(const int32_t timezone[2])
{
#if KERNEL_HAS_VDSO_ELF
    KERNEL_RAW_SCOPE(guard, &vvar_lock);
#endif
    __atomic_add_fetch(&time_sequence,1,__ATOMIC_ACQ_REL);
    time_timezone[0]=timezone[0];time_timezone[1]=timezone[1];
#if KERNEL_HAS_VDSO_ELF
    if (vvar) publish_vvar();
#endif
    __atomic_add_fetch(&time_sequence,1,__ATOMIC_RELEASE);
}
enum kernel_time_status kernel_time_deadline_from_realtime(uint64_t ns,uint64_t *deadline)
{
    uint64_t offset=kernel_time_boot_realtime_offset();
    enum kernel_time_status status=kernel_time_deadline_from_monotonic(ns>offset?ns-offset:0,deadline);
    if (status==KERNEL_TIME_STATUS_DEADLINE_PASSED) *deadline=arch_time_read()+1;
    return status;
}
