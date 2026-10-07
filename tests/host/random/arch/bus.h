#ifndef BOAROS_HOST_ARCH_BUS_H
#define BOAROS_HOST_ARCH_BUS_H
#include <stdint.h>
int arch_dma_image_address(uint64_t,uint64_t,uint64_t *);
static inline void arch_dma_barrier(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
static inline void arch_io_barrier(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
#endif
