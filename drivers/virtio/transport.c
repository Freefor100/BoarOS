#include <kernel/virtio_transport.h>
#include <arch/bus.h>

static int valid(const struct virtio_transport *t)
{ return t && t->context && t->ops.read && t->ops.write && (t->version==1 || t->version==2); }
enum virtio_status virtio_transport_reset(struct virtio_transport *t)
{
    if(!valid(t))return VIRTIO_INVALID;
    t->quiescent=0;t->started=0;t->negotiated=0;t->features=0;
    t->ops.write(t->context,VIRTIO_REG_STATUS,0);
    arch_io_barrier();
    if(t->ops.read(t->context,VIRTIO_REG_STATUS))return VIRTIO_DEVICE;
    arch_dma_barrier();t->quiescent=1;
    return VIRTIO_OK;
}
enum virtio_status virtio_transport_begin(struct virtio_transport *t,uint64_t wanted,
    uint64_t required,uint64_t *accepted)
{
    if(!valid(t) || (required&~wanted))return VIRTIO_INVALID;
    enum virtio_status result=virtio_transport_reset(t);
    if(result!=VIRTIO_OK)return result;
    t->ops.write(t->context,VIRTIO_REG_STATUS,1);
    t->ops.write(t->context,VIRTIO_REG_STATUS,3);
    t->ops.write(t->context,VIRTIO_REG_DEVICE_FEATURES_SEL,0);
    uint64_t offered=t->ops.read(t->context,VIRTIO_REG_DEVICE_FEATURES);
    if(t->version==2) {
        t->ops.write(t->context,VIRTIO_REG_DEVICE_FEATURES_SEL,1);
        offered|=(uint64_t)t->ops.read(t->context,VIRTIO_REG_DEVICE_FEATURES)<<32;
        wanted|=UINT64_C(1)<<32;required|=UINT64_C(1)<<32;
    }
    if(required&~offered)return VIRTIO_UNSUPPORTED;
    uint64_t features=offered&wanted;
    t->ops.write(t->context,VIRTIO_REG_DRIVER_FEATURES_SEL,0);
    t->ops.write(t->context,VIRTIO_REG_DRIVER_FEATURES,(uint32_t)features);
    if(t->version==2) {
        t->ops.write(t->context,VIRTIO_REG_DRIVER_FEATURES_SEL,1);
        t->ops.write(t->context,VIRTIO_REG_DRIVER_FEATURES,(uint32_t)(features>>32));
        t->ops.write(t->context,VIRTIO_REG_STATUS,11);
        arch_io_barrier();
        if(!(t->ops.read(t->context,VIRTIO_REG_STATUS)&8))return VIRTIO_UNSUPPORTED;
    }
    t->features=features;t->negotiated=1;if(accepted)*accepted=features;
    return VIRTIO_OK;
}
enum virtio_status virtio_transport_start(struct virtio_transport *t)
{
    if(!valid(t))return VIRTIO_INVALID;
    if(!t->negotiated || t->started)return VIRTIO_STATE;
    arch_dma_barrier();t->quiescent=0;
    t->ops.write(t->context,VIRTIO_REG_STATUS,t->version==2 ? 15 : 7);
    arch_io_barrier();
    if((t->ops.read(t->context,VIRTIO_REG_STATUS)&(4U|64U|128U))!=4)return VIRTIO_DEVICE;
    t->started=1;return VIRTIO_OK;
}
static void address(struct virtio_transport *t,enum virtio_register low,uint64_t value)
{
    t->ops.write(t->context,low,(uint32_t)value);
    t->ops.write(t->context,(enum virtio_register)(low+1),(uint32_t)(value>>32));
}
enum virtio_status virtio_transport_queue(struct virtio_transport *t,uint16_t queue,
    uint16_t size,uint64_t physical,uint32_t available,uint32_t used,uint32_t page_size)
{
    if(!valid(t) || !size || (size&(size-1)) || !physical || (physical&15) ||
        available<16U*size || (available&1) || (used&3) ||
        (uint64_t)used<(uint64_t)available+6U+2U*size ||
        physical>UINT64_MAX-((uint64_t)used+6U+8U*size))return VIRTIO_INVALID;
    if(!t->negotiated || t->started)return VIRTIO_STATE;
    if(t->version==1 && (!page_size || (page_size&(page_size-1)) ||
        physical%page_size || physical/page_size>UINT32_MAX ||
        available!=16U*size || used%page_size || used<available+6U+2U*size))return VIRTIO_INVALID;
    t->ops.write(t->context,VIRTIO_REG_QUEUE_SEL,queue);
    if(t->ops.read(t->context,VIRTIO_REG_QUEUE_NUM_MAX)<size ||
        (t->version==2 && t->ops.read(t->context,VIRTIO_REG_QUEUE_READY)))return VIRTIO_UNSUPPORTED;
    if(t->ops.prepare_queue) {
        enum virtio_status result=t->ops.prepare_queue(t->context,queue);
        if(result!=VIRTIO_OK)return result;
    }
    t->ops.write(t->context,VIRTIO_REG_QUEUE_NUM,size);
    if(t->version==1) {
        t->ops.write(t->context,VIRTIO_REG_GUEST_PAGE_SIZE,page_size);
        t->ops.write(t->context,VIRTIO_REG_QUEUE_ALIGN,page_size);
        arch_dma_barrier();
        t->ops.write(t->context,VIRTIO_REG_QUEUE_PFN,(uint32_t)(physical/page_size));
    } else {
        address(t,VIRTIO_REG_QUEUE_DESC_LOW,physical);
        address(t,VIRTIO_REG_QUEUE_DRIVER_LOW,physical+available);
        address(t,VIRTIO_REG_QUEUE_DEVICE_LOW,physical+used);
        arch_dma_barrier();t->ops.write(t->context,VIRTIO_REG_QUEUE_READY,1);
    }
    return VIRTIO_OK;
}
enum virtio_status virtio_transport_config_read(const struct virtio_transport *t,
    uint32_t offset,unsigned width,uint32_t *out)
{
    if(!valid(t) || !out || !t->ops.config_read || (width!=1 && width!=2 && width!=4) ||
        (offset&(width-1)) || offset>t->config_size || width>t->config_size-offset)return VIRTIO_INVALID;
    *out=t->ops.config_read(t->context,offset,width);return VIRTIO_OK;
}
