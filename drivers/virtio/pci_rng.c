#include <kernel/virtio_pci_rng.h>
#include <kernel/errno.h>
static int error(enum virtio_status status)
{
    switch(status) {
    case VIRTIO_OK:return 0;
    case VIRTIO_EMPTY:return -KERNEL_ENODEV;
    case VIRTIO_UNSUPPORTED:return -KERNEL_ENOTSUP;
    case VIRTIO_NO_MEMORY:return -KERNEL_ENOMEM;
    case VIRTIO_INVALID:return -KERNEL_EINVAL;
    default:return -KERNEL_EIO;
    }
}
int virtio_pci_rng_stop(struct virtio_pci_rng *rng)
{
    if(!rng)return -KERNEL_EINVAL;
    int status=virtio_rng_stop(&rng->device);
    if(status)return status;
    if(rng->pci.function.host) {
        status=error(virtio_pci_transport_destroy(&rng->pci));
        if(status)return status;
    }
    return 0;
}
int virtio_pci_rng_start(struct virtio_pci_rng *rng,struct pci_host *host,uint16_t bdf,
    struct physical_page_allocator *allocator,uint64_t frequency)
{
    if(!rng || rng->device.transport.context || rng->pci.function.host || !allocator || !frequency)
        return -KERNEL_EINVAL;
    int status=error(virtio_pci_transport_initialize(&rng->pci,host,bdf,4,0));
    if(status)return status;
    status=virtio_rng_start(&rng->device,&rng->pci.transport,allocator,frequency,rng->pci.irq);
    if(status && virtio_pci_rng_stop(rng))return -KERNEL_EIO;
    return status;
}
