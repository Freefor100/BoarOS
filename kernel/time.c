#include <kernel/cost.h>
#include <arch/timer.h>
#include <kernel/time.h>
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

#if KERNEL_HAS_VDSO_ELF
#include <kernel/page.h>
#include <kernel/physical_page.h>
#include <kernel/raw_lock.h>
#include <string.h>

static struct kernel_raw_lock vvar_lock = {
    .rank = KERNEL_RAW_RANK_OBJECT, .initialized = KERNEL_RAW_INITIALIZED
};
static struct kernel_vdso_time_data *vvar;
static struct physical_page_allocator *vvar_allocator;
static uint64_t vvar_page;
static uint32_t vvar_owners;

static void publish_vvar(void)
{
    vvar->boot_realtime_ns = time_boot_realtime_ns;
    vvar->coarse_resolution_ns = kernel_time_coarse_resolution_ns();
    __atomic_store_n(&vvar->coarse_monotonic_ns,
        __atomic_load_n(&time_coarse_monotonic_ns, __ATOMIC_RELAXED), __ATOMIC_RELAXED);
    __atomic_store_n(&vvar->multiplier, time_multiplier, __ATOMIC_RELEASE);
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

uint64_t kernel_time_monotonic_ns(void)
{
    return kernel_time_ticks_to_ns(arch_time_read());
}

uint64_t kernel_time_realtime_ns(void)
{
    return time_boot_realtime_ns + kernel_time_monotonic_ns();
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
    /* RTC offset 启动后不可变；单个原子样本避免两套快照撕裂。 */
    return time_boot_realtime_ns + kernel_time_coarse_monotonic_ns();
}

uint64_t kernel_time_coarse_resolution_ns(void)
{
    return KERNEL_TIME_NS_PER_SECOND / KERNEL_TICKS_PER_SECOND;
}

uint64_t kernel_time_boot_realtime_offset(void)
{
    return time_boot_realtime_ns;
}

enum kernel_time_status kernel_time_deadline_from_monotonic(
    uint64_t target_monotonic_ns,
    uint64_t *deadline)
{
    uint64_t now_ticks;
    uint64_t now_ns;
    uint64_t delta_ns;
    unsigned __int128 delta_ticks;

    if (deadline == 0) {
        return KERNEL_TIME_STATUS_INVALID_ARGUMENT;
    }
    if (time_initialized == 0U) {
        return KERNEL_TIME_STATUS_NOT_INITIALIZED;
    }

    now_ticks = arch_time_read();
    now_ns = kernel_time_ticks_to_ns(now_ticks);
    if (target_monotonic_ns <= now_ns) {
        return KERNEL_TIME_STATUS_DEADLINE_PASSED;
    }
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
