#ifndef BOAROS_KERNEL_VIRTIO_SPLIT_QUEUE_H
#define BOAROS_KERNEL_VIRTIO_SPLIT_QUEUE_H
#include <kernel/virtio_transport.h>
#include <stddef.h>
#define VIRTIO_SPLIT_MAX_SIZE 32U
#define VIRTIO_DESC_NEXT 1U
#define VIRTIO_DESC_WRITE 2U
#define VIRTIO_DESC_INDIRECT 4U
struct virtio_descriptor { uint64_t address; uint32_t length; uint16_t flags,next; };
struct virtio_available_ring {
    uint16_t flags,index,ring[VIRTIO_SPLIT_MAX_SIZE],used_event;
};
struct virtio_used_element { uint32_t id,length; };
struct virtio_used_ring {
    uint16_t flags,index;
    struct virtio_used_element ring[VIRTIO_SPLIT_MAX_SIZE];
    uint16_t available_event;
};
enum virtio_queue_fault_reason {
    VIRTIO_QUEUE_NO_FAULT, VIRTIO_QUEUE_OVERFLOW, VIRTIO_QUEUE_BAD_HEAD, VIRTIO_QUEUE_BAD_LENGTH
};
struct virtio_queue_fault {
    enum virtio_queue_fault_reason reason;
    uint16_t used_index,observed_count,consumed;
    uint32_t head,length;
};
struct virtio_completion { void *token; uint32_t head,length; };
struct virtio_split_queue {
    const struct virtio_transport *transport;
    struct virtio_descriptor *descriptors;
    volatile struct virtio_available_ring *available;
    volatile struct virtio_used_ring *used;
    uint16_t size,last_used,next_available,inflight;
    uint32_t claimed;
    struct { void *token; uint32_t mask,min_length,max_length; } owners[VIRTIO_SPLIT_MAX_SIZE];
    struct virtio_queue_fault fault;
};
/* Caller serializes publish/take/reset against IRQ. Ring and business buffers are borrowed.
 * Taking a completion returns descriptor ownership, never frees the business token.
 * Abandoning outstanding tokens requires confirmed transport reset first. */
enum virtio_status virtio_split_initialize(struct virtio_split_queue *,void *,size_t,
    uint16_t,uint32_t,uint32_t);
enum virtio_status virtio_split_publish(struct virtio_split_queue *,struct virtio_transport *,
    uint16_t,uint16_t,void *,uint32_t,uint32_t);
enum virtio_status virtio_split_take(struct virtio_split_queue *,struct virtio_completion *);
enum virtio_status virtio_split_reset(struct virtio_split_queue *,const struct virtio_transport *);
#endif
