#include <arch/riscv/virtio_mmio_rng.h>
#include <arch/riscv/plic.h>
#include <kernel/virtio_mmio.h>
#include <kernel/errno.h>
static int register_irq(void *context,uint32_t source,void (*handler)(void *),void *owner)
{ (void)context;return riscv_plic_register(source,handler,owner); }
static void unregister_irq(void *context,uint32_t source,void *owner)
{ (void)context;riscv_plic_unregister(source,owner); }
int riscv_virtio_mmio_rng_start(struct virtio_rng_device *device,volatile void *mmio,uint64_t size,
    struct physical_page_allocator *allocator,uint64_t frequency,uint32_t source)
{
    if(!device || device->transport.context || !mmio || ((uintptr_t)mmio&3) || size<0x100 ||
        !allocator || !frequency || !source)return -KERNEL_EINVAL;
    struct virtio_transport transport={0};
    enum virtio_status status=virtio_mmio_transport_initialize(&transport,mmio,size,4,register_irq,unregister_irq);
    if(status!=VIRTIO_OK)return -KERNEL_ENODEV;
    return virtio_rng_start(device,&transport,allocator,frequency,source);
}
int riscv_virtio_mmio_rng_stop(struct virtio_rng_device *device)
{ return virtio_rng_stop(device); }
