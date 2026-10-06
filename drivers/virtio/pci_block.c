#include <kernel/virtio_pci_block.h>
static enum virtio_block_status block_status(enum virtio_status status)
{
    switch(status) {
    case VIRTIO_OK:return VIRTIO_BLOCK_DRIVER_STATUS_OK;
    case VIRTIO_EMPTY:return VIRTIO_BLOCK_DRIVER_STATUS_NOT_BLOCK;
    case VIRTIO_INVALID:return VIRTIO_BLOCK_DRIVER_STATUS_INVALID;
    case VIRTIO_UNSUPPORTED:return VIRTIO_BLOCK_DRIVER_STATUS_UNSUPPORTED;
    case VIRTIO_NO_MEMORY:return VIRTIO_BLOCK_DRIVER_STATUS_NO_MEMORY;
    case VIRTIO_DEVICE:case VIRTIO_TIMEOUT:return VIRTIO_BLOCK_DRIVER_STATUS_DEVICE;
    default:return VIRTIO_BLOCK_DRIVER_STATUS_STATE;
    }
}
enum virtio_block_status virtio_pci_block_init(struct virtio_pci_block *block,struct pci_host *host,uint16_t bdf,
    struct physical_page_allocator *allocator,virtio_dma_address_fn dma,uint32_t frequency)
{
    if(!block || block->core_owned || block->device.state || !allocator || !dma || !frequency)return VIRTIO_BLOCK_DRIVER_STATUS_INVALID;
    enum virtio_status status=virtio_pci_transport_initialize(&block->pci,host,bdf,2,8);
    if(status!=VIRTIO_OK)return block_status(status);
    enum virtio_block_status result=virtio_block_init(&block->device,&block->pci.transport,allocator,dma,frequency);
    block->core_owned=result==VIRTIO_BLOCK_DRIVER_STATUS_OK || block->device.queue_memory!=0;
    if(result!=VIRTIO_BLOCK_DRIVER_STATUS_OK) {
        if(block->core_owned)return result;
        if(virtio_pci_transport_destroy(&block->pci)!=VIRTIO_OK)return VIRTIO_BLOCK_DRIVER_STATUS_DEVICE;
    }
    return result;
}
enum virtio_block_status virtio_pci_block_destroy(struct virtio_pci_block *block)
{
    if(!block)return VIRTIO_BLOCK_DRIVER_STATUS_INVALID;
    if(!block->pci.function.host)return VIRTIO_BLOCK_DRIVER_STATUS_STATE;
    if(block->core_owned) {
        enum virtio_block_status status=virtio_block_destroy(&block->device);
        if(status!=VIRTIO_BLOCK_DRIVER_STATUS_OK)return status;
        block->core_owned=0;
    } else if(block->device.queue_memory || block->device.queue_physical_address || block->device.active) {
        __builtin_trap();
    }
    enum virtio_status result=virtio_pci_transport_destroy(&block->pci);
    if(result!=VIRTIO_OK)return block_status(result);
    *block=(struct virtio_pci_block){0};return VIRTIO_BLOCK_DRIVER_STATUS_OK;
}
