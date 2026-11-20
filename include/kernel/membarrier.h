#ifndef BOAROS_KERNEL_MEMBARRIER_H
#define BOAROS_KERNEL_MEMBARRIER_H

#define KERNEL_IPI_MEMBARRIER (1U << 5)
void kernel_membarrier_receive(void);

#endif
