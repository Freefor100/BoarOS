#ifndef BOAROS_KERNEL_VIRTIO_PCI_H
#define BOAROS_KERNEL_VIRTIO_PCI_H
#include <kernel/pci.h>
#include <kernel/virtio_transport.h>
#define VIRTIO_PCI_MAX_QUEUES 32U
struct virtio_pci_transport {
    struct virtio_transport transport;
    struct pci_function function;
    struct pci_virtio_caps caps;
    volatile unsigned char *common,*notify,*isr,*config;
    volatile uint16_t *queue_notify[VIRTIO_PCI_MAX_QUEUES];
    uint32_t irq;
};
/* Persistent platform owner. A failed reset retains function/BAR ownership. */
enum virtio_status virtio_pci_transport_initialize(struct virtio_pci_transport *,struct pci_host *,
    uint16_t,uint32_t,uint32_t);
enum virtio_status virtio_pci_transport_destroy(struct virtio_pci_transport *);
#endif
