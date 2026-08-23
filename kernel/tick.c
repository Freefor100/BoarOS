#include <kernel/tick.h>

static uint64_t tick_count;

void kernel_tick_advance(uint64_t elapsed_ticks)
{
    tick_count += elapsed_ticks;
}

uint64_t kernel_tick_count(void)
{
    return tick_count;
}
