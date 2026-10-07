#ifndef BOAROS_KERNEL_NETWORK_H
#define BOAROS_KERNEL_NETWORK_H
#include <kernel/virtio_net.h>
#include <kernel/heap.h>
struct kernel_network;
/* Device/transport are borrowed from platform; stop joins worker and returns
 * all loans/TX references, then platform may destroy hardware. NULL is timer-only. */
int kernel_network_start(struct kernel_network **owner, struct kernel_heap *heap,
    struct virtio_net_device *device, uint64_t frequency);
int kernel_network_stop(struct kernel_network **owner);
#endif
