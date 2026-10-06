#ifndef BOAROS_ARCH_RISCV_VIRTIO_MMIO_BLOCK_H
#define BOAROS_ARCH_RISCV_VIRTIO_MMIO_BLOCK_H
#include <kernel/virtio_block.h>
#define riscv_virtio_mmio_block_statistics virtio_block_statistics
#define riscv_virtio_mmio_block_status virtio_block_status
#define riscv_virtio_mmio_block virtio_block_device
#define riscv_virtio_dma_address_fn virtio_dma_address_fn
#define RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK VIRTIO_BLOCK_DRIVER_STATUS_OK
#define RISCV_VIRTIO_MMIO_BLOCK_STATUS_INVALID VIRTIO_BLOCK_DRIVER_STATUS_INVALID
#define RISCV_VIRTIO_MMIO_BLOCK_STATUS_NOT_BLOCK VIRTIO_BLOCK_DRIVER_STATUS_NOT_BLOCK
#define RISCV_VIRTIO_MMIO_BLOCK_STATUS_UNSUPPORTED VIRTIO_BLOCK_DRIVER_STATUS_UNSUPPORTED
#define RISCV_VIRTIO_MMIO_BLOCK_STATUS_NO_MEMORY VIRTIO_BLOCK_DRIVER_STATUS_NO_MEMORY
#define RISCV_VIRTIO_MMIO_BLOCK_STATUS_DEVICE VIRTIO_BLOCK_DRIVER_STATUS_DEVICE
#define RISCV_VIRTIO_MMIO_BLOCK_STATUS_STATE VIRTIO_BLOCK_DRIVER_STATUS_STATE
volatile void *riscv_virtio_mmio_block_base(const struct riscv_virtio_mmio_block *);
enum riscv_virtio_mmio_block_status riscv_virtio_mmio_block_init(
    struct riscv_virtio_mmio_block *device,
    volatile void *mmio,
    uint64_t mmio_size,
    struct physical_page_allocator *page_allocator,
    riscv_virtio_dma_address_fn dma_address,
    uint32_t timebase_frequency);

int riscv_virtio_mmio_block_enable_irq(struct riscv_virtio_mmio_block *device, uint32_t source);

enum riscv_virtio_mmio_block_status riscv_virtio_mmio_block_destroy(
    struct riscv_virtio_mmio_block *device);

void riscv_virtio_mmio_block_get_statistics(
    const struct riscv_virtio_mmio_block *device,
    struct riscv_virtio_mmio_block_statistics *statistics);

#endif
