#include <kernel/virtio_transport.h>
#include <kernel/virtio_split_queue.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct model {
    uint32_t status, select, features_select, offered[2], accepted[2];
    unsigned reset_stuck, reject_features, notified;
};
static uint32_t read_reg(void *opaque, enum virtio_register reg)
{
    struct model *m=opaque;
    switch(reg) {
    case VIRTIO_REG_STATUS:return m->status;
    case VIRTIO_REG_DEVICE_FEATURES:return m->offered[m->features_select];
    case VIRTIO_REG_QUEUE_NUM_MAX:return 32;
    case VIRTIO_REG_QUEUE_READY:return 0;
    default:return 0;
    }
}
static void write_reg(void *opaque, enum virtio_register reg,uint32_t value)
{
    struct model *m=opaque;
    switch(reg) {
    case VIRTIO_REG_STATUS:
        if(value || !m->reset_stuck)m->status=value;
        if(m->reject_features)m->status&=~8U;
        break;
    case VIRTIO_REG_DEVICE_FEATURES_SEL:m->features_select=value;break;
    case VIRTIO_REG_DRIVER_FEATURES_SEL:m->select=value;break;
    case VIRTIO_REG_DRIVER_FEATURES:m->accepted[m->select]=value;break;
    case VIRTIO_REG_QUEUE_NOTIFY:m->notified++;break;
    default:break;
    }
}
static uint32_t ack(void *opaque) { (void)opaque;return 0; }
static uint32_t config(void *opaque,uint32_t offset,unsigned width)
{ (void)opaque;(void)offset;(void)width;return 0; }
static void complete(struct virtio_split_queue *q,unsigned head,unsigned length)
{
    volatile struct virtio_used_ring *u=q->used;
    u->ring[u->index%q->size]=(struct virtio_used_element){head,length};
    u->index++;
}
int main(void)
{
    _Alignas(4096) unsigned char memory[8192];
    struct model m={.offered={0x123,1}};
    struct virtio_transport t={.context=&m,.version=2,.device_id=2,
        .ops={.read=read_reg,.write=write_reg,.ack_interrupt=ack,.config_read=config}};
    struct virtio_split_queue q={0};
    struct virtio_completion done;
    uint64_t features=0;
    assert(virtio_transport_begin(&t,0x121,0,&features)==VIRTIO_OK);
    assert(features==0x100000121ULL && m.accepted[1]==1 && m.status==11);
    struct virtio_split_queue invalid={0};
    assert(virtio_split_initialize(&invalid,memory,sizeof(memory),32,UINT32_MAX-1,1024)==VIRTIO_INVALID);
    assert(virtio_transport_queue(&t,0,32,UINT64_MAX-1055,512,1024,4096)==VIRTIO_INVALID);
    assert(virtio_split_initialize(&q,memory,sizeof(memory),32,512,1024)==VIRTIO_OK);
    assert(virtio_transport_queue(&t,0,32,0x100000,512,1024,4096)==VIRTIO_OK);
    assert(virtio_transport_start(&t)==VIRTIO_OK && !t.quiescent);
    for(unsigned i=0;i<32;i++)q.descriptors[i]=(struct virtio_descriptor){0x200000+16*i,16,VIRTIO_DESC_WRITE,0};
    /* Completion releases only descriptor ownership; the business token stays live. */
    int owners[32]={0};
    for(unsigned i=0;i<32;i++)assert(virtio_split_publish(&q,&t,0,i,&owners[i],0,16)==VIRTIO_OK);
    assert(q.inflight==32 && m.notified==32);
    assert(virtio_split_publish(&q,&t,0,0,&owners[0],0,16)==VIRTIO_FULL);
    for(int i=31;i>=0;i--)complete(&q,(unsigned)i,16);
    for(int i=31;i>=0;i--) {
        assert(virtio_split_take(&q,&done)==VIRTIO_OK);
        assert(done.head==(unsigned)i && done.token==&owners[i] && done.length==16);
    }
    assert(!q.inflight && virtio_split_take(&q,&done)==VIRTIO_EMPTY);
    for(unsigned i=0;i<65544;i++) {
        assert(virtio_split_publish(&q,&t,0,0,&owners[0],1,16)==VIRTIO_OK);
        complete(&q,0,16);assert(virtio_split_take(&q,&done)==VIRTIO_OK);
    }
    q.descriptors[0].flags=VIRTIO_DESC_NEXT;q.descriptors[0].next=1;
    q.descriptors[1].flags=VIRTIO_DESC_WRITE;
    assert(virtio_split_publish(&q,&t,0,0,&owners[0],0,16)==VIRTIO_OK);
    assert(virtio_split_publish(&q,&t,0,1,&owners[1],0,16)==VIRTIO_STATE);
    struct virtio_transport other=t;other.quiescent=1;other.started=0;
    assert(virtio_split_reset(&q,&other)==VIRTIO_STATE && q.inflight==1);
    assert(virtio_split_reset(&q,&t)==VIRTIO_STATE && q.inflight==1);
    m.reset_stuck=1;assert(virtio_transport_reset(&t)==VIRTIO_DEVICE && !t.quiescent);
    assert(virtio_split_reset(&q,&t)==VIRTIO_STATE && q.inflight==1);
    m.reset_stuck=0;assert(virtio_transport_reset(&t)==VIRTIO_OK);
    assert(virtio_split_reset(&q,&t)==VIRTIO_OK && !q.inflight);
    q.descriptors[0]=(struct virtio_descriptor){0x300000,32,VIRTIO_DESC_INDIRECT,0};
    assert(virtio_transport_begin(&t,0,0,0)==VIRTIO_OK);
    assert(virtio_transport_begin(&t,0,0,0)==VIRTIO_OK);
    assert(virtio_transport_queue(&t,0,32,0x100000,512,1024,4096)==VIRTIO_OK);
    assert(virtio_transport_start(&t)==VIRTIO_OK);
    assert(virtio_split_publish(&q,&t,0,0,&owners[0],0,64)==VIRTIO_UNSUPPORTED);
    m.offered[0]|=1U<<28;
    assert(virtio_transport_begin(&t,1U<<28,1U<<28,0)==VIRTIO_OK);
    assert(virtio_transport_start(&t)==VIRTIO_OK);
    assert(virtio_split_publish(&q,&t,0,0,&owners[0],0,64)==VIRTIO_OK);
    complete(&q,0,65);assert(virtio_split_take(&q,&done)==VIRTIO_DEVICE && q.inflight==1);
    assert(q.fault.reason==VIRTIO_QUEUE_BAD_LENGTH && q.fault.length==65);
    assert(virtio_transport_reset(&t)==VIRTIO_OK && virtio_split_reset(&q,&t)==VIRTIO_OK);
    q.descriptors[0].flags=VIRTIO_DESC_WRITE;
    assert(virtio_transport_begin(&t,0,0,0)==VIRTIO_OK);
    assert(virtio_transport_queue(&t,0,32,0x100000,512,1024,4096)==VIRTIO_OK);
    assert(virtio_transport_start(&t)==VIRTIO_OK);
    assert(virtio_split_publish(&q,&t,0,0,&owners[0],0,16)==VIRTIO_OK);
    complete(&q,1,1);assert(virtio_split_take(&q,&done)==VIRTIO_DEVICE && q.inflight==1);
    assert(q.fault.reason==VIRTIO_QUEUE_BAD_HEAD);
    assert(virtio_transport_reset(&t)==VIRTIO_OK && virtio_split_reset(&q,&t)==VIRTIO_OK);
    assert(virtio_transport_begin(&t,0,0,0)==VIRTIO_OK);
    assert(virtio_transport_queue(&t,0,32,0x100000,512,1024,4096)==VIRTIO_OK);
    assert(virtio_transport_start(&t)==VIRTIO_OK);
    assert(virtio_split_publish(&q,&t,0,0,&owners[0],0,16)==VIRTIO_OK);
    complete(&q,0,1);complete(&q,0,1);
    assert(virtio_split_take(&q,&done)==VIRTIO_OK && done.token==&owners[0]);
    assert(virtio_split_take(&q,&done)==VIRTIO_DEVICE && q.fault.reason==VIRTIO_QUEUE_BAD_HEAD);
    assert(virtio_transport_reset(&t)==VIRTIO_OK && virtio_split_reset(&q,&t)==VIRTIO_OK);
    q.used->index=33;
    assert(virtio_split_take(&q,&done)==VIRTIO_DEVICE && q.fault.reason==VIRTIO_QUEUE_OVERFLOW);
    assert(virtio_transport_reset(&t)==VIRTIO_OK && virtio_split_reset(&q,&t)==VIRTIO_OK);
    m.offered[1]=0;assert(virtio_transport_begin(&t,0,0,0)==VIRTIO_UNSUPPORTED);
    m.offered[1]=1;m.reject_features=1;
    assert(virtio_transport_begin(&t,0,0,0)==VIRTIO_DEVICE);
    m.reject_features=0;t.version=1;
    assert(virtio_transport_begin(&t,0x21,0,&features)==VIRTIO_OK && features==0x21);
    assert(virtio_transport_queue(&t,0,32,0x100000,512,4096,4096)==VIRTIO_OK);
    assert(virtio_split_initialize(&q,memory,sizeof(memory),32,2,1024)==VIRTIO_INVALID);
    puts("VirtIO feature/reset/descriptor ownership, chain, reverse completion, wrap and malformed device tests passed");
}
