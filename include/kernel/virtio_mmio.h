#ifndef BOAROS_KERNEL_VIRTIO_MMIO_H
#define BOAROS_KERNEL_VIRTIO_MMIO_H
#include <kernel/virtio_transport.h>
enum virtio_status virtio_mmio_transport_initialize(struct virtio_transport *,volatile void *,uint64_t,
    uint32_t,int (*)(void *,uint32_t,void (*)(void *),void *),void (*)(void *,uint32_t,void *));
#endif
