#include <kernel/virtio_mmio.h>
#include <stddef.h>
static unsigned offset(enum virtio_register reg)
{
    static const uint16_t offsets[]={0x70,0x14,0x10,0x24,0x20,0x30,0x34,0x38,
        0x44,0x28,0x3c,0x40,0x50,0x80,0x84,0x90,0x94,0xa0,0xa4,0xfc};
    if((unsigned)reg>=sizeof(offsets)/sizeof(offsets[0]))__builtin_trap();
    return offsets[reg];
}
static uint32_t mmio_read(void *context,enum virtio_register reg)
{ return *(volatile uint32_t *)((unsigned char *)context+offset(reg)); }
static void mmio_write(void *context,enum virtio_register reg,uint32_t value)
{ *(volatile uint32_t *)((unsigned char *)context+offset(reg))=value; }
static uint32_t mmio_config(void *context,uint32_t offset,unsigned width)
{
    volatile unsigned char *p=(volatile unsigned char *)context+0x100+offset;
    if(width==1)return *p;
    if(width==2)return *(volatile uint16_t *)p;
    return *(volatile uint32_t *)p;
}
static uint32_t mmio_ack(void *context)
{
    uint32_t pending=*(volatile uint32_t *)((unsigned char *)context+0x60);
    if(pending)*(volatile uint32_t *)((unsigned char *)context+0x64)=pending;
    return pending;
}
enum virtio_status virtio_mmio_transport_initialize(struct virtio_transport *out,volatile void *mmio,
    uint64_t size,uint32_t device_id,int (*register_irq)(void *,uint32_t,void (*)(void *),void *),
    void (*unregister_irq)(void *,uint32_t,void *))
{
    if(!out || !mmio || ((uintptr_t)mmio&3) || size<0x100 || size-0x100>UINT32_MAX)return VIRTIO_INVALID;
    volatile uint32_t *registers=mmio;
    if(registers[0]!=0x74726976)return VIRTIO_INVALID;
    if(registers[2]!=device_id || (registers[1]!=1 && registers[1]!=2))return VIRTIO_UNSUPPORTED;
    /* RV启动期可能仍在物理别名：逐项从实际PC取得函数地址，不能加载链接VA表。 */
    volatile struct virtio_transport *t=out;
    t->context=(void *)mmio;t->version=registers[1];t->device_id=device_id;
    t->kind=VIRTIO_TRANSPORT_MMIO;t->config_size=size-0x100;
    t->quiescent=0;t->negotiated=0;t->started=0;t->features=0;
    t->ops.read=mmio_read;t->ops.write=mmio_write;t->ops.config_read=mmio_config;
    t->ops.ack_interrupt=mmio_ack;t->ops.register_irq=register_irq;
    t->ops.unregister_irq=unregister_irq;t->ops.prepare_queue=NULL;
    return VIRTIO_OK;
}
