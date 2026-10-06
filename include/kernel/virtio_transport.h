#ifndef BOAROS_KERNEL_VIRTIO_TRANSPORT_H
#define BOAROS_KERNEL_VIRTIO_TRANSPORT_H
#include <stdint.h>

enum virtio_status {
    VIRTIO_OK, VIRTIO_INVALID, VIRTIO_UNSUPPORTED, VIRTIO_NO_MEMORY,
    VIRTIO_DEVICE, VIRTIO_TIMEOUT, VIRTIO_STATE, VIRTIO_EMPTY, VIRTIO_FULL
};
enum virtio_register {
    VIRTIO_REG_STATUS, VIRTIO_REG_DEVICE_FEATURES_SEL, VIRTIO_REG_DEVICE_FEATURES,
    VIRTIO_REG_DRIVER_FEATURES_SEL, VIRTIO_REG_DRIVER_FEATURES,
    VIRTIO_REG_QUEUE_SEL, VIRTIO_REG_QUEUE_NUM_MAX, VIRTIO_REG_QUEUE_NUM,
    VIRTIO_REG_QUEUE_READY, VIRTIO_REG_GUEST_PAGE_SIZE, VIRTIO_REG_QUEUE_ALIGN,
    VIRTIO_REG_QUEUE_PFN, VIRTIO_REG_QUEUE_NOTIFY, VIRTIO_REG_QUEUE_DESC_LOW,
    VIRTIO_REG_QUEUE_DESC_HIGH, VIRTIO_REG_QUEUE_DRIVER_LOW, VIRTIO_REG_QUEUE_DRIVER_HIGH,
    VIRTIO_REG_QUEUE_DEVICE_LOW, VIRTIO_REG_QUEUE_DEVICE_HIGH, VIRTIO_REG_CONFIG_GENERATION
};
enum virtio_transport_kind { VIRTIO_TRANSPORT_MMIO=1, VIRTIO_TRANSPORT_PCI=2 };
struct virtio_transport_ops {
    uint32_t (*read)(void *,enum virtio_register);
    void (*write)(void *,enum virtio_register,uint32_t);
    uint32_t (*config_read)(void *,uint32_t,unsigned);
    /* PCI ISR read-to-clear and MMIO W1C belong to the adapter. */
    uint32_t (*ack_interrupt)(void *);
    int (*register_irq)(void *,uint32_t,void (*)(void *),void *);
    void (*unregister_irq)(void *,uint32_t,void *);
    enum virtio_status (*prepare_queue)(void *,uint16_t);
};
struct virtio_transport {
    void *context;
    /* Value copy prevents temporary callback-table lifetime and boot VA aliases. */
    struct virtio_transport_ops ops;
    uint32_t version, device_id, kind, config_size;
    uint64_t features;
    uint8_t quiescent, negotiated, started;
};
enum virtio_status virtio_transport_reset(struct virtio_transport *);
enum virtio_status virtio_transport_begin(struct virtio_transport *,uint64_t,uint64_t,uint64_t *);
enum virtio_status virtio_transport_start(struct virtio_transport *);
enum virtio_status virtio_transport_queue(struct virtio_transport *,uint16_t,uint16_t,
    uint64_t,uint32_t,uint32_t,uint32_t);
enum virtio_status virtio_transport_config_read(const struct virtio_transport *,uint32_t,unsigned,uint32_t *);
#endif
