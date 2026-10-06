#ifndef BOAROS_KERNEL_VIRTIO_PCI_RNG_H
#define BOAROS_KERNEL_VIRTIO_PCI_RNG_H
#include <kernel/virtio_pci.h>
#include <kernel/virtio_rng.h>
struct virtio_pci_rng {
    struct virtio_rng_device device;
    struct virtio_pci_transport pci;
};
int virtio_pci_rng_start(struct virtio_pci_rng *,struct pci_host *,uint16_t,
    struct physical_page_allocator *,uint64_t);
int virtio_pci_rng_stop(struct virtio_pci_rng *);
#endif
