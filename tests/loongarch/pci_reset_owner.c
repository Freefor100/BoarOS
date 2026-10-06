#include <kernel/virtio_pci_block.h>
#include <platform/loongarch_virt.h>
static struct virtio_transport *denied;
static unsigned tested_init,tested_destroy;
enum virtio_status __real_virtio_transport_reset(struct virtio_transport *);
enum virtio_block_status __real_virtio_pci_block_init(struct virtio_pci_block *,struct pci_host *,
    uint16_t,struct physical_page_allocator *,virtio_dma_address_fn,uint32_t);
enum virtio_block_status __real_virtio_pci_block_destroy(struct virtio_pci_block *);
enum virtio_status __wrap_virtio_transport_reset(struct virtio_transport *transport)
{
    if(transport==denied) {
        denied=0;transport->quiescent=0;
        return VIRTIO_DEVICE;
    }
    return __real_virtio_transport_reset(transport);
}
enum virtio_block_status __wrap_virtio_pci_block_init(struct virtio_pci_block *block,struct pci_host *host,
    uint16_t bdf,struct physical_page_allocator *allocator,virtio_dma_address_fn dma,uint32_t frequency)
{
    if(!tested_init++) {
        unsigned claims=pci_host_claimed(host);
        uint64_t pages=physical_page_available(allocator);
        denied=&block->pci.transport;
        enum virtio_block_status status=__real_virtio_pci_block_init(block,host,bdf,allocator,dma,frequency);
        if(denied || status!=VIRTIO_BLOCK_DRIVER_STATUS_DEVICE || !block->pci.function.host ||
            pci_host_claimed(host)<=claims || block->core_owned || block->device.queue_memory ||
            physical_page_available(allocator)!=pages)la_virt_fatal("PCI init failed reset owner");
        if(virtio_pci_block_destroy(block)!=VIRTIO_BLOCK_DRIVER_STATUS_OK ||
            block->pci.function.host || pci_host_claimed(host)!=claims ||
            physical_page_available(allocator)!=pages)la_virt_fatal("PCI partial init cleanup");
        la_virt_puts("LA PCI initial reset failure retains/reclaims BAR owner passed\n");
    }
    return __real_virtio_pci_block_init(block,host,bdf,allocator,dma,frequency);
}
enum virtio_block_status __wrap_virtio_pci_block_destroy(struct virtio_pci_block *block)
{
    if(block->core_owned && !tested_destroy++) {
        struct pci_host *host=block->pci.function.host;
        struct physical_page_allocator *allocator=block->device.page_allocator;
        uint64_t pages=physical_page_available(allocator);
        uint64_t queue_pages=UINT64_C(1)<<block->device.queue_allocation_order;
        unsigned claims=pci_host_claimed(host);
        denied=&block->pci.transport;
        enum virtio_block_status status=__real_virtio_pci_block_destroy(block);
        if(denied || status!=VIRTIO_BLOCK_DRIVER_STATUS_DEVICE || block->core_owned ||
            !block->pci.function.host || block->device.queue_memory || pci_host_claimed(host)!=claims ||
            physical_page_available(allocator)!=pages+queue_pages)la_virt_fatal("PCI final reset owner");
        status=__real_virtio_pci_block_destroy(block);
        if(status!=VIRTIO_BLOCK_DRIVER_STATUS_OK || block->pci.function.host ||
            pci_host_claimed(host)>=claims || physical_page_available(allocator)!=pages+queue_pages)
            la_virt_fatal("PCI final reset retry");
        la_virt_puts("LA PCI final reset failure retries BAR without double queue free passed\n");
        return status;
    }
    return __real_virtio_pci_block_destroy(block);
}
