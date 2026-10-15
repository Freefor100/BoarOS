#include <kernel/tick.h>
#include <kernel/cpu_config.h>

static uint64_t tick_count;

void kernel_tick_advance(uint64_t elapsed_ticks)
{
#if BOAROS_MAX_CPUS > 1
    __atomic_store_n(&tick_count, __atomic_load_n(&tick_count, __ATOMIC_RELAXED) +
                     elapsed_ticks, __ATOMIC_RELEASE);
#else
    tick_count += elapsed_ticks;
#endif
}

uint64_t kernel_tick_count(void)
{
#if BOAROS_MAX_CPUS > 1
    return __atomic_load_n(&tick_count, __ATOMIC_ACQUIRE);
#else
    return tick_count;
#endif
}
