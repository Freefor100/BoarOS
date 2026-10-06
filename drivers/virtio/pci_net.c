#include <kernel/virtio_pci_net.h>
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
int virtio_pci_net_stop(struct virtio_pci_net *net)
{
    if(!net)return -KERNEL_EINVAL;
    int status=virtio_net_stop(&net->device);
    if(status)return status;
    if(net->pci.function.host) {
        status=error(virtio_pci_transport_destroy(&net->pci));
        if(status)return status;
    }
    return 0;
}
int virtio_pci_net_start(struct virtio_pci_net *net,struct pci_host *host,uint16_t bdf,
    struct physical_page_allocator *allocator,uint64_t frequency)
{
    if(!net || net->device.transport.context || net->pci.function.host || !allocator || !frequency)
        return -KERNEL_EINVAL;
    int status=error(virtio_pci_transport_initialize(&net->pci,host,bdf,1,6));
    if(status)return status;
    status=virtio_net_init(&net->device,&net->pci.transport,allocator,frequency,net->pci.irq);
    if(status && virtio_pci_net_stop(net))return -KERNEL_EIO;
    return status;
}
