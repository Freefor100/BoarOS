#ifndef BOAROS_KERNEL_NETWORK_H
#define BOAROS_KERNEL_NETWORK_H
#include <kernel/virtio_net.h>
#include <kernel/heap.h>
struct kernel_network;
struct kernel_igb_rx;
struct netif;
struct pbuf;
/* One pass through the hardware receive queue under a protocol scope. */
#define KERNEL_NETWORK_RECEIVE_BATCH 8U
int kernel_network_send_frame(struct netif *, const void *, uint16_t size);
/* Device/transport are borrowed from platform; stop joins worker and returns
 * all loans/TX references, then platform may destroy hardware. NULL is timer-only. */
int kernel_network_start(struct kernel_network **owner, struct kernel_heap *heap,
    struct virtio_net_device *device, uint64_t frequency);
int kernel_network_igb_start(struct kernel_network **owner, struct kernel_heap *heap,
    struct kernel_igb_rx *device, uint64_t frequency);
int kernel_network_stop(struct kernel_network **owner);

struct kernel_network_igb_zc_buffer {
    uint64_t physical;
    void *address, *cookie;
};

struct kernel_network_igb_zc_ops {
    /* A successful take reserves writable DMA storage until put. Both
     * callbacks may run under protocol ownership and must not block. */
    int (*take)(void *, struct kernel_network_igb_zc_buffer *);
    void (*put)(void *, void *);
    void (*set_attached)(void *, int attached);
};

int kernel_network_igb_zc_attach(uint32_t ifindex, unsigned queue,
    const struct kernel_network_igb_zc_ops *, void *provider);
int kernel_network_igb_zc_detach(uint32_t ifindex, void *provider);
void kernel_network_igb_zc_kick(uint32_t ifindex, void *provider);
/* Caller holds a pbuf reference. Offset is relative to the provider’s DMA
 * address for the returned cookie. */
int kernel_network_igb_zc_payload(const struct pbuf *, const void *provider,
    void **cookie, unsigned *offset);
#endif
