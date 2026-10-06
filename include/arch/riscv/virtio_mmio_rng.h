#ifndef BOAROS_ARCH_RISCV_VIRTIO_MMIO_RNG_H
#define BOAROS_ARCH_RISCV_VIRTIO_MMIO_RNG_H
#include <kernel/virtio_rng.h>
#define riscv_virtio_mmio_rng virtio_rng_device
int riscv_virtio_mmio_rng_start(struct virtio_rng_device *,volatile void *,uint64_t,
    struct physical_page_allocator *,uint64_t,uint32_t);
int riscv_virtio_mmio_rng_stop(struct virtio_rng_device *);
#endif
