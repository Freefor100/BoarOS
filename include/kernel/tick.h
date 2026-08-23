#ifndef BOAROS_KERNEL_TICK_H
#define BOAROS_KERNEL_TICK_H

#include <stdint.h>

#define KERNEL_TICKS_PER_SECOND 100U

void kernel_tick_advance(uint64_t elapsed_ticks);
uint64_t kernel_tick_count(void);

#endif
