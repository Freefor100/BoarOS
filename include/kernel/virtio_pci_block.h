#ifndef BOAROS_KERNEL_VIRTIO_PCI_BLOCK_H
#define BOAROS_KERNEL_VIRTIO_PCI_BLOCK_H
#include <kernel/virtio_pci.h>
#include <kernel/virtio_block.h>
struct virtio_pci_block {
    struct virtio_block_device device;
    struct virtio_pci_transport pci;
    uint8_t core_owned;
};
enum virtio_block_status virtio_pci_block_init(struct virtio_pci_block *,struct pci_host *,uint16_t,
    struct physical_page_allocator *,virtio_dma_address_fn,uint32_t);
enum virtio_block_status virtio_pci_block_destroy(struct virtio_pci_block *);
#endif
