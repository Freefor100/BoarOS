#ifndef BOAROS_KERNEL_VIRTIO_BLOCK_H
#define BOAROS_KERNEL_VIRTIO_BLOCK_H

#include <kernel/block.h>
#include <kernel/virtio_split_queue.h>
#include <kernel/physical_page.h>
#include <kernel/scheduler.h>

#include <stdint.h>

enum virtio_block_status {
    VIRTIO_BLOCK_DRIVER_STATUS_OK = 0,
    VIRTIO_BLOCK_DRIVER_STATUS_INVALID,
    VIRTIO_BLOCK_DRIVER_STATUS_NOT_BLOCK,
    VIRTIO_BLOCK_DRIVER_STATUS_UNSUPPORTED,
    VIRTIO_BLOCK_DRIVER_STATUS_NO_MEMORY,
    VIRTIO_BLOCK_DRIVER_STATUS_DEVICE,
    VIRTIO_BLOCK_DRIVER_STATUS_STATE,
};

typedef int (*virtio_dma_address_fn)(
    const void *pointer,
    uint64_t size,
    uint64_t *physical_address);

struct virtio_block_statistics {
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

struct virtio_block_device {
    struct kernel_block_device block;
    struct physical_page_allocator *page_allocator;
    virtio_dma_address_fn dma_address;
    struct virtio_transport transport;
    void *queue_memory;
    uint64_t queue_physical_address;
    uint64_t timeout_ticks;
    uint16_t queue_size;
    struct virtio_split_queue queue;
    uint32_t transport_version;
    uint32_t queue_allocation_order;
    uint32_t state;
    uint32_t read_only;
    uint32_t irq_source, active, inflight, barrier_waiters, barrier;
    uint64_t statistics_start, statistics_last;
    struct kernel_wait_queue available;
    struct virtio_block_statistics statistics;
};

enum virtio_block_status virtio_block_init(struct virtio_block_device *,
    const struct virtio_transport *, struct physical_page_allocator *,
    virtio_dma_address_fn, uint32_t);
int virtio_block_enable_irq(struct virtio_block_device *,uint32_t);
enum virtio_block_status virtio_block_destroy(struct virtio_block_device *);
void virtio_block_get_statistics(const struct virtio_block_device *,struct virtio_block_statistics *);
#endif
