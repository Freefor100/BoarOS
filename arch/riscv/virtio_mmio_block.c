#include <arch/riscv/virtio_mmio_block.h>
#include <arch/riscv/plic.h>
#include <kernel/virtio_mmio.h>
#include <stddef.h>
static int register_irq(void *context,uint32_t source,void (*handler)(void *),void *owner)
{ (void)context;return riscv_plic_register(source,handler,owner); }
static void unregister_irq(void *context,uint32_t source,void *owner)
{ (void)context;riscv_plic_unregister(source,owner); }
volatile void *riscv_virtio_mmio_block_base(const struct virtio_block_device *device)
{ return device && device->transport.kind==VIRTIO_TRANSPORT_MMIO ? device->transport.context : NULL; }
enum riscv_virtio_mmio_block_status riscv_virtio_mmio_block_init(
    struct virtio_block_device *device,volatile void *mmio,uint64_t size,
    struct physical_page_allocator *allocator,virtio_dma_address_fn dma,uint32_t frequency)
{
    if(!device || device->state || !mmio || ((uintptr_t)mmio&3) || size<0x108 || !allocator || !dma || !frequency)
        return RISCV_VIRTIO_MMIO_BLOCK_STATUS_INVALID;
    if(((volatile uint32_t *)mmio)[0]!=0x74726976)return RISCV_VIRTIO_MMIO_BLOCK_STATUS_INVALID;
    if(((volatile uint32_t *)mmio)[2]!=2)return RISCV_VIRTIO_MMIO_BLOCK_STATUS_NOT_BLOCK;
    struct virtio_transport transport={0};
    enum virtio_status status=virtio_mmio_transport_initialize(&transport,mmio,size,2,register_irq,unregister_irq);
    if(status!=VIRTIO_OK)return status==VIRTIO_INVALID ? RISCV_VIRTIO_MMIO_BLOCK_STATUS_INVALID :
        RISCV_VIRTIO_MMIO_BLOCK_STATUS_UNSUPPORTED;
    return virtio_block_init(device,&transport,allocator,dma,frequency);
}
int riscv_virtio_mmio_block_enable_irq(struct virtio_block_device *device,uint32_t source)
{ return virtio_block_enable_irq(device,source); }
enum riscv_virtio_mmio_block_status riscv_virtio_mmio_block_destroy(struct virtio_block_device *device)
{ return virtio_block_destroy(device); }
void riscv_virtio_mmio_block_get_statistics(const struct virtio_block_device *device,struct virtio_block_statistics *statistics)
{ virtio_block_get_statistics(device,statistics); }
