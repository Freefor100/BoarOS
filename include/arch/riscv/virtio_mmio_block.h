#ifndef BOAROS_ARCH_RISCV_VIRTIO_MMIO_BLOCK_H
#define BOAROS_ARCH_RISCV_VIRTIO_MMIO_BLOCK_H

#include <kernel/block.h>
#include <kernel/physical_page.h>
#include <kernel/scheduler.h>

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
    uint64_t flush_requests;
    uint64_t interrupts, sleeps, wakes, queue_waits, runtime_polls, max_inflight;
    /* 时间加权在途深度（Σ深度·tick）、忙时、总span、等待与设备服务时间。 */
    uint64_t inflight_ticks, busy_ticks, total_ticks, queue_wait_ticks, service_ticks;
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
    uint32_t irq_source, active, inflight, barrier_waiters, barrier;
    uint64_t statistics_start, statistics_last;
    struct kernel_wait_queue available;
    struct riscv_virtio_mmio_block_statistics statistics;
};

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
