#include <kernel/virtio_transport.h>
#include <platform/loongarch_virt.h>
enum virtio_status __real_virtio_transport_reset(struct virtio_transport *);
enum virtio_status __wrap_virtio_transport_reset(struct virtio_transport *transport)
{
    static int injected;
    if(!injected && transport->kind==VIRTIO_TRANSPORT_PCI) {
        injected=1;transport->quiescent=0;
        la_virt_puts("LA root PCI initial reset confirmation denied\n");
        return VIRTIO_DEVICE;
    }
    return __real_virtio_transport_reset(transport);
}
