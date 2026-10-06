#ifndef BOAROS_KERNEL_VIRTIO_MMIO_H
#define BOAROS_KERNEL_VIRTIO_MMIO_H
#include <kernel/virtio_transport.h>
/* Host device models override the register-access boundary, not queue policy. */
uint32_t virtio_mmio_read32(volatile void *,unsigned);
void virtio_mmio_write32(volatile void *,unsigned,uint32_t);
enum virtio_status virtio_mmio_transport_initialize(struct virtio_transport *,volatile void *,uint64_t,
    uint32_t,int (*)(void *,uint32_t,void (*)(void *),void *),void (*)(void *,uint32_t,void *));
#endif
