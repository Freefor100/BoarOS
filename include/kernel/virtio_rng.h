#ifndef BOAROS_KERNEL_VIRTIO_RNG_H
#define BOAROS_KERNEL_VIRTIO_RNG_H
#include <kernel/physical_page.h>
#include <kernel/scheduler.h>
#include <kernel/virtio_split_queue.h>
struct virtio_rng_device {
    struct virtio_transport transport;
    struct virtio_split_queue queue;
    struct physical_page_allocator *allocator;
    void *queue_memory;
    uint64_t queue_phys,frequency;
    struct kernel_thread_join worker;
    struct kernel_wait_queue progress;
    uint32_t version,order,irq_source;
    uint8_t irq_registered,started,stopping,active,configured,completed,invalid;
    uint32_t response_length;
    uint64_t requests,bytes,zero_responses,errors,timeouts;
};
/* A non-null transport context remains a live owner after failed cleanup. */
int virtio_rng_start(struct virtio_rng_device *,const struct virtio_transport *,
    struct physical_page_allocator *,uint64_t,uint32_t);
int virtio_rng_stop(struct virtio_rng_device *);
#endif
