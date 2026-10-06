#include <kernel/virtio_split_queue.h>
#include <arch/bus.h>
#include <string.h>
_Static_assert(sizeof(struct virtio_descriptor)==16,"split descriptor ABI");
_Static_assert(offsetof(struct virtio_available_ring,index)==2 &&
    _Alignof(struct virtio_available_ring)>=2,"atomic aligned available index");
_Static_assert(offsetof(struct virtio_used_ring,ring)==4 &&
    sizeof(struct virtio_used_element)==8 && _Alignof(struct virtio_used_ring)>=4,"used ABI");

enum virtio_status virtio_split_initialize(struct virtio_split_queue *q,void *memory,
    size_t capacity,uint16_t size,uint32_t available,uint32_t used)
{
    if(!q || !memory || ((uintptr_t)memory&15) || !size || size>VIRTIO_SPLIT_MAX_SIZE ||
        (size&(size-1)) || available<16U*size || (available&1) || (used&3) ||
        available>capacity || 6U+2U*size>capacity-available ||
        (uint64_t)used<(uint64_t)available+6U+2U*size ||
        used>capacity || 6U+8U*size>capacity-used)return VIRTIO_INVALID;
    if(q->inflight || q->transport)return VIRTIO_STATE;
    *q=(struct virtio_split_queue){.descriptors=memory,
        .available=(void *)((unsigned char *)memory+available),
        .used=(void *)((unsigned char *)memory+used),.size=size};
    memset(q->descriptors,0,16U*size);
    memset((void *)q->available,0,6U+2U*size);
    memset((void *)q->used,0,6U+8U*size);
    return VIRTIO_OK;
}
enum virtio_status virtio_split_publish(struct virtio_split_queue *q,struct virtio_transport *t,
    uint16_t queue,uint16_t head,void *token,uint32_t min_length,uint32_t max_length)
{
    if(!q || !q->size || !t || !t->ops.write || !token || head>=q->size || min_length>max_length)return VIRTIO_INVALID;
    if(q->fault.reason || !t->started || t->quiescent ||
        (q->transport && q->transport!=t))return VIRTIO_STATE;
    if(q->inflight==q->size)return VIRTIO_FULL;
    uint32_t mask=0;uint16_t index=head;
    for(;;) {
        if(index>=q->size || (mask&(UINT32_C(1)<<index)))return VIRTIO_STATE;
        struct virtio_descriptor d=q->descriptors[index];
        if(d.address>UINT64_MAX-d.length)return VIRTIO_STATE;
        if(d.flags&~(VIRTIO_DESC_NEXT|VIRTIO_DESC_WRITE|VIRTIO_DESC_INDIRECT))return VIRTIO_STATE;
        mask|=UINT32_C(1)<<index;
        if(d.flags&VIRTIO_DESC_INDIRECT) {
            if(!(t->features&(UINT64_C(1)<<28)))return VIRTIO_UNSUPPORTED;
            if(index!=head || d.flags!=VIRTIO_DESC_INDIRECT || (d.address&15) || !d.length || (d.length&15))return VIRTIO_STATE;
            break;
        }
        if(!(d.flags&VIRTIO_DESC_NEXT))break;
        index=d.next;
    }
    if((mask&q->claimed) || q->owners[head].token || q->available->index!=q->next_available)return VIRTIO_STATE;
    q->transport=t;q->owners[head].token=token;q->owners[head].mask=mask;
    q->owners[head].min_length=min_length;q->owners[head].max_length=max_length;
    q->claimed|=mask;q->inflight++;
    q->available->ring[q->next_available%q->size]=head;
    /* 先发布owner与完整链，最后一次半字写推进设备可见idx。 */
    arch_dma_barrier();q->available->index=++q->next_available;
    arch_dma_barrier();t->ops.write(t->context,VIRTIO_REG_QUEUE_NOTIFY,queue);
    return VIRTIO_OK;
}
static enum virtio_status fault(struct virtio_split_queue *q,enum virtio_queue_fault_reason reason,
    uint16_t observed,uint16_t count,uint32_t head,uint32_t length)
{
    q->fault=(struct virtio_queue_fault){reason,observed,count,q->last_used,head,length};
    return VIRTIO_DEVICE;
}
enum virtio_status virtio_split_take(struct virtio_split_queue *q,struct virtio_completion *done)
{
    if(!q || !q->size || !done)return VIRTIO_INVALID;
    if(q->fault.reason)return VIRTIO_STATE;
    uint16_t observed=q->used->index,count=(uint16_t)(observed-q->last_used);
    arch_dma_barrier();
    if(count>q->size)return fault(q,VIRTIO_QUEUE_OVERFLOW,observed,count,UINT32_MAX,0);
    if(!count)return VIRTIO_EMPTY;
    struct virtio_used_element item=q->used->ring[q->last_used++%q->size];
    if(item.id>=q->size || !q->owners[item.id].token)return fault(q,VIRTIO_QUEUE_BAD_HEAD,observed,count,item.id,item.length);
    if(item.length<q->owners[item.id].min_length || item.length>q->owners[item.id].max_length)
        return fault(q,VIRTIO_QUEUE_BAD_LENGTH,observed,count,item.id,item.length);
    *done=(struct virtio_completion){q->owners[item.id].token,item.id,item.length};
    q->claimed&=~q->owners[item.id].mask;q->inflight--;
    q->owners[item.id].token=0;q->owners[item.id].mask=0;
    return VIRTIO_OK;
}
enum virtio_status virtio_split_reset(struct virtio_split_queue *q,const struct virtio_transport *t)
{
    if(!q || !q->size || !t)return VIRTIO_INVALID;
    if(!t->quiescent || t->started || (q->transport && q->transport!=t))return VIRTIO_STATE;
    memset(q->owners,0,sizeof(q->owners));q->inflight=0;q->claimed=0;
    q->last_used=0;q->next_available=0;q->transport=0;q->fault=(struct virtio_queue_fault){0};
    q->available->index=0;q->used->index=0;arch_dma_barrier();return VIRTIO_OK;
}
