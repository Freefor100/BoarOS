#ifndef BOAROS_ARCH_RISCV_VIRTIO_MMIO_BLOCK_H
#define BOAROS_ARCH_RISCV_VIRTIO_MMIO_BLOCK_H

#include <kernel/block.h>
#include <kernel/physical_page.h>

#include <stdint.h>

enum riscv_virtio_mmio_block_status {
    RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK = 0,
    RISCV_VIRTIO_MMIO_BLOCK_STATUS_INVALID,
    RISCV_VIRTIO_MMIO_BLOCK_STATUS_NOT_BLOCK,
    RISCV_VIRTIO_MMIO_BLOCK_STATUS_UNSUPPORTED,
    RISCV_VIRTIO_MMIO_BLOCK_STATUS_NO_MEMORY,
    RISCV_VIRTIO_MMIO_BLOCK_STATUS_DEVICE,
    RISCV_VIRTIO_MMIO_BLOCK_STATUS_STATE,
};

typedef int (*riscv_virtio_dma_address_fn)(
    const void *pointer,
    uint64_t size,
    uint64_t *physical_address);

struct riscv_virtio_mmio_block_statistics {
    uint64_t requests;
    uint64_t direct_requests;
    uint64_t bounce_requests;
    uint64_t sectors_read;
    uint64_t sectors_written;
    uint64_t timeouts;
    uint64_t io_errors;
};

struct riscv_virtio_mmio_block {
    struct kernel_block_device block;
    struct physical_page_allocator *page_allocator;
    riscv_virtio_dma_address_fn dma_address;
    volatile unsigned char *mmio;
    void *queue_memory;
    uint64_t queue_physical_address;
    uint64_t mmio_size;
    uint64_t timeout_ticks;
    uint16_t queue_size;
    uint16_t last_used_index;
    uint32_t transport_version;
    uint32_t queue_allocation_order;
    uint32_t state;
    uint32_t read_only;
    struct riscv_virtio_mmio_block_statistics statistics;
};

enum riscv_virtio_mmio_block_status riscv_virtio_mmio_block_init(
    struct riscv_virtio_mmio_block *device,
    volatile void *mmio,
    uint64_t mmio_size,
    struct physical_page_allocator *page_allocator,
    riscv_virtio_dma_address_fn dma_address,
    uint32_t timebase_frequency);

enum riscv_virtio_mmio_block_status riscv_virtio_mmio_block_destroy(
    struct riscv_virtio_mmio_block *device);

void riscv_virtio_mmio_block_get_statistics(
    const struct riscv_virtio_mmio_block *device,
    struct riscv_virtio_mmio_block_statistics *statistics);

#endif
