#ifndef BOAROS_KERNEL_VIRTIO_PCI_BLOCK_H
#define BOAROS_KERNEL_VIRTIO_PCI_BLOCK_H
#include <kernel/pci.h>
#include <kernel/virtio_block.h>
struct virtio_pci_block {
    struct virtio_block_device device;
    struct pci_function function;
    struct pci_virtio_caps caps;
    volatile unsigned char *common,*notify,*isr,*config;
    uint32_t irq;
};
enum virtio_block_status virtio_pci_block_init(struct virtio_pci_block *,struct pci_host *,uint16_t,
    struct physical_page_allocator *,virtio_dma_address_fn,uint32_t);
enum virtio_block_status virtio_pci_block_destroy(struct virtio_pci_block *);
#endif
