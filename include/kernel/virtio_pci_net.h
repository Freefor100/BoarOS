#ifndef BOAROS_KERNEL_VIRTIO_PCI_NET_H
#define BOAROS_KERNEL_VIRTIO_PCI_NET_H
#include <kernel/virtio_pci.h>
#include <kernel/virtio_net.h>
struct virtio_pci_net { struct virtio_net_device device; struct virtio_pci_transport pci; };
int virtio_pci_net_start(struct virtio_pci_net *,struct pci_host *,uint16_t,
    struct physical_page_allocator *,uint64_t);
int virtio_pci_net_stop(struct virtio_pci_net *);
#endif
