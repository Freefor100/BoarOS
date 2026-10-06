#ifndef BOAROS_HOST_ARCH_BUS_H
#define BOAROS_HOST_ARCH_BUS_H
static inline void arch_dma_barrier(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
static inline void arch_io_barrier(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
#endif
