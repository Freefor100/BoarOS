#include <kernel/virtio_rng.h>
#include <kernel/errno.h>
#include <platform/loongarch_virt.h>
void la_fault_injection_set(int);
int __real_virtio_rng_start(struct virtio_rng_device *,const struct virtio_transport *,
    struct physical_page_allocator *,uint64_t,uint32_t);
static int reject_irq(void *context,uint32_t source,void (*handler)(void *),void *owner)
{ (void)context;(void)source;(void)handler;(void)owner;return 0; }
int __wrap_virtio_rng_start(struct virtio_rng_device *device,const struct virtio_transport *transport,
    struct physical_page_allocator *allocator,uint64_t frequency,uint32_t source)
{
    uint64_t pages=physical_page_available(allocator);
    struct virtio_transport copy=*transport;
    if(RNG_FAIL_AFTER==3)copy.ops.register_irq=reject_irq;
    else la_fault_injection_set(RNG_FAIL_AFTER);
    int error=__real_virtio_rng_start(device,&copy,allocator,frequency,source);
    la_fault_injection_set(-1);
    if(error!=(RNG_FAIL_AFTER==3 ? -KERNEL_EIO : -KERNEL_ENOMEM) ||
        device->transport.context || device->queue_memory || device->queue_phys ||
        device->started || device->irq_registered || physical_page_available(allocator)!=pages)
        la_virt_fatal("RNG construction rollback");
    la_virt_puts("LA RNG construction DMA/task/stack/IRQ rollback passed\n");
    return error;
}
