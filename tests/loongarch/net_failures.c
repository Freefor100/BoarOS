#include <kernel/virtio_net.h>
#include <kernel/network.h>
#include <kernel/errno.h>
#include <platform/loongarch_virt.h>
void la_fault_injection_set(int);
int __real_virtio_net_init(struct virtio_net_device *,const struct virtio_transport *,
    struct physical_page_allocator *,uint64_t,uint32_t);
int __real_kernel_network_start(struct kernel_network **,struct kernel_heap *,struct virtio_net_device *,uint64_t);
enum kernel_heap_status __real_kernel_heap_allocate_zeroed(struct kernel_heap *,size_t,size_t,void **);
enum kernel_scheduler_status __real_kernel_thread_create_joinable(void (*)(void *),void *,struct kernel_thread_join *);
enum virtio_status __real_virtio_transport_reset(struct virtio_transport *);
static int upper;
static unsigned resets;
static int reject_irq(void *context,uint32_t source,void (*handler)(void *),void *owner)
{ (void)context;(void)source;(void)handler;(void)owner;return 0; }
int __wrap_virtio_net_init(struct virtio_net_device *device,const struct virtio_transport *transport,
    struct physical_page_allocator *allocator,uint64_t frequency,uint32_t source)
{
    uint64_t pages=physical_page_available(allocator);
    struct virtio_transport copy=*transport;
    if(NET_FAIL_CASE==3)copy.ops.register_irq=reject_irq;
    else if(NET_FAIL_CASE<3)la_fault_injection_set(NET_FAIL_CASE);
    int error=__real_virtio_net_init(device,&copy,allocator,frequency,source);
    la_fault_injection_set(-1);
    if(NET_FAIL_CASE<=3) {
        if(error!=(NET_FAIL_CASE==3 ? -KERNEL_EIO : -KERNEL_ENOMEM) || device->transport.context ||
            device->queues || device->rx_memory || device->tx_memory || device->irq_registered ||
            physical_page_available(allocator)!=pages)la_virt_fatal("net construction rollback");
        la_virt_puts("LA net DMA/IRQ construction rollback passed\n");
    }
    return error;
}
int __wrap_kernel_network_start(struct kernel_network **owner,struct kernel_heap *heap,
    struct virtio_net_device *device,uint64_t frequency)
{
    upper=1;int error=__real_kernel_network_start(owner,heap,device,frequency);upper=0;
    if(NET_FAIL_CASE>=4 && NET_FAIL_CASE<=6) {
        if(error!=-KERNEL_ENOMEM || *owner || (NET_FAIL_CASE==4 ?
            (!device->configured || !device->irq_registered || device->transport.quiescent) :
            (device->irq_registered || !device->transport.quiescent)))
            la_virt_fatal("network worker construction rollback");
        la_virt_puts("LA network heap/task/stack construction rollback passed\n");
    }
    return error;
}
enum kernel_heap_status __wrap_kernel_heap_allocate_zeroed(struct kernel_heap *heap,size_t count,size_t size,void **out)
{
    if(upper && NET_FAIL_CASE==4) { *out=0;return KERNEL_HEAP_STATUS_EMPTY; }
    return __real_kernel_heap_allocate_zeroed(heap,count,size,out);
}
enum kernel_scheduler_status __wrap_kernel_thread_create_joinable(void (*entry)(void *),void *argument,struct kernel_thread_join *join)
{
    if(upper && (NET_FAIL_CASE==5 || NET_FAIL_CASE==6))la_fault_injection_set(NET_FAIL_CASE-5);
    enum kernel_scheduler_status status=__real_kernel_thread_create_joinable(entry,argument,join);
    la_fault_injection_set(-1);return status;
}
enum virtio_status __wrap_virtio_transport_reset(struct virtio_transport *transport)
{
    if(transport->device_id==1) {
        resets++;
        if((NET_FAIL_CASE==7 && resets==1) || (NET_FAIL_CASE==8 && resets==3)) {
            transport->quiescent=0;transport->started=0;
            la_virt_puts("LA net reset refused with owner retained\n");return VIRTIO_DEVICE;
        }
    }
    return __real_virtio_transport_reset(transport);
}
