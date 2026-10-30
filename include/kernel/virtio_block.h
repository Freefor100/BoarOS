#ifndef BOAROS_KERNEL_VIRTIO_BLOCK_H
#define BOAROS_KERNEL_VIRTIO_BLOCK_H

#include <kernel/block.h>
#include <kernel/page.h>
#include <kernel/cost.h>
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
    uint64_t interrupts, sleeps, wakes, queue_waits, max_inflight;
    /* BOAROS_COST_DIAGNOSTICS下累计时间；普通构建保持零。 */
    uint64_t inflight_ticks, busy_ticks, total_ticks, queue_wait_ticks, service_ticks;
};

#define VIRTIO_BLOCK_REQUEST_SLOTS 8U
#define VIRTIO_BLOCK_BATCH_WORKSPACE_BYTES 2048U
#define VIRTIO_BLOCK_BATCH_POOL_PAGES \
    ((VIRTIO_BLOCK_REQUEST_SLOTS * VIRTIO_BLOCK_BATCH_WORKSPACE_BYTES + BOAROS_PAGE_SIZE - 1U) / BOAROS_PAGE_SIZE)

struct virtio_block_device {
    struct kernel_block_device block;
    struct physical_page_allocator *page_allocator;
    virtio_dma_address_fn dma_address;
    struct virtio_transport transport;
    void *queue_memory;
    uint64_t queue_physical_address;
    uint64_t timeout_ticks;
    uint16_t queue_size;
    uint64_t batch_physical[VIRTIO_BLOCK_BATCH_POOL_PAGES];
    void *batch_memory[VIRTIO_BLOCK_BATCH_POOL_PAGES];
    unsigned batch_claimed;
    struct kernel_raw_lock metadata;
    struct virtio_split_queue queue;
    uint32_t transport_version;
    uint32_t queue_allocation_order;
    uint32_t state;
    uint32_t read_only;
    uint32_t max_segment_size, max_segments;
    uint32_t irq_source, active, barrier_waiters, barrier;
#if BOAROS_COST_DIAGNOSTICS
    uint64_t statistics_start, statistics_last;
#endif
    struct kernel_wait_queue admission, idle, slots, slots_low, descriptors, workspaces;
    struct virtio_block_statistics statistics;
};

enum virtio_block_status virtio_block_init(struct virtio_block_device *,
    const struct virtio_transport *, struct physical_page_allocator *,
    virtio_dma_address_fn, uint32_t);
int virtio_block_enable_irq(struct virtio_block_device *,uint32_t);
enum virtio_block_status virtio_block_destroy(struct virtio_block_device *);
void virtio_block_get_statistics(const struct virtio_block_device *,struct virtio_block_statistics *);
#endif
