#include <arch/riscv/virtio_mmio_net.h>
#include <arch/riscv/plic.h>
#include <kernel/virtio_mmio.h>
#include <kernel/errno.h>
static int register_irq(void *context,uint32_t source,void (*handler)(void *),void *owner)
{ (void)context;return riscv_plic_register(source,handler,owner); }
static void unregister_irq(void *context,uint32_t source,void *owner)
{ (void)context;riscv_plic_unregister(source,owner); }
int riscv_virtio_mmio_net_init(struct virtio_net_device *device,volatile void *mmio,uint64_t size,
    struct physical_page_allocator *allocator,uint64_t frequency,uint32_t source)
{
    if(!device || device->transport.context || !mmio || ((uintptr_t)mmio&3) || size<0x108 ||
        !allocator || !frequency || !source)return -KERNEL_EINVAL;
    struct virtio_transport transport={0};
    if(virtio_mmio_transport_initialize(&transport,mmio,size,1,register_irq,unregister_irq)!=VIRTIO_OK)
        return -KERNEL_ENODEV;
    return virtio_net_init(device,&transport,allocator,frequency,source);
}
