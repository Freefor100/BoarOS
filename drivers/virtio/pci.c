#include <kernel/virtio_pci.h>
#include <arch/bus.h>

int virtio_pci_device_matches(struct pci_host *host,uint16_t bdf,uint32_t device_id)
{
    if(!host || !host->read || !device_id)return 0;
    uint32_t identity=host->read(host->context,bdf,0,4),kind=identity>>16;
    if((identity&0xffff)!=0x1af4 || kind<0x1000 || kind>0x107f)return 0;
    /* transitional 的设备种类由 subsystem 决定，不能从旧 PCI ID 相减。 */
    kind=kind<0x1040 ? host->read(host->context,bdf,0x2c,4)>>16 : kind-0x1040;
    return kind==device_id;
}

static unsigned reg_offset(enum virtio_register reg,unsigned *width)
{
    static const uint8_t offsets[]={20,0,4,8,12,22,24,24,28,0,0,0,0,32,36,40,44,48,52,21};
    if((unsigned)reg>=sizeof(offsets) || reg==VIRTIO_REG_QUEUE_NOTIFY ||
        reg==VIRTIO_REG_GUEST_PAGE_SIZE || reg==VIRTIO_REG_QUEUE_ALIGN || reg==VIRTIO_REG_QUEUE_PFN)__builtin_trap();
    *width=reg==VIRTIO_REG_STATUS || reg==VIRTIO_REG_CONFIG_GENERATION ? 1 :
        reg==VIRTIO_REG_QUEUE_SEL || reg==VIRTIO_REG_QUEUE_NUM_MAX ||
        reg==VIRTIO_REG_QUEUE_NUM || reg==VIRTIO_REG_QUEUE_READY ? 2 : 4;
    return offsets[reg];
}
static uint32_t pci_read(void *context,enum virtio_register reg)
{
    struct virtio_pci_transport *p=context;unsigned width;
    unsigned offset=reg_offset(reg,&width);
    if(width==1)return *(volatile uint8_t *)(p->common+offset);
    if(width==2)return *(volatile uint16_t *)(p->common+offset);
    return *(volatile uint32_t *)(p->common+offset);
}
static void pci_write(void *context,enum virtio_register reg,uint32_t value)
{
    struct virtio_pci_transport *p=context;unsigned width;
    if(reg==VIRTIO_REG_QUEUE_NOTIFY) {
        if(value>=VIRTIO_PCI_MAX_QUEUES || !p->queue_notify[value])__builtin_trap();
        *p->queue_notify[value]=value;return;
    }
    unsigned offset=reg_offset(reg,&width);
    if(width==1)*(volatile uint8_t *)(p->common+offset)=value;
    else if(width==2)*(volatile uint16_t *)(p->common+offset)=value;
    else *(volatile uint32_t *)(p->common+offset)=value;
}
static uint32_t pci_config(void *context,uint32_t offset,unsigned width)
{
    struct virtio_pci_transport *p=context;
    if(width==1)return *(volatile uint8_t *)(p->config+offset);
    if(width==2)return *(volatile uint16_t *)(p->config+offset);
    return *(volatile uint32_t *)(p->config+offset);
}
static uint32_t pci_ack(void *context)
{ struct virtio_pci_transport *p=context;return *p->isr; }
static int irq_register(void *context,uint32_t source,void (*handler)(void *),void *owner)
{ struct virtio_pci_transport *p=context;return p->function.host->register_irq(p->function.host->context,source,handler,owner); }
static void irq_unregister(void *context,uint32_t source,void *owner)
{ struct virtio_pci_transport *p=context;p->function.host->unregister_irq(p->function.host->context,source,owner); }
static enum virtio_status prepare_queue(void *context,uint16_t queue)
{
    struct virtio_pci_transport *p=context;
    if(queue>=VIRTIO_PCI_MAX_QUEUES)return VIRTIO_UNSUPPORTED;
    uint64_t offset=(uint64_t)*(volatile uint16_t *)(p->common+30)*p->caps.notify_multiplier;
    if((offset&1) || offset>p->caps.notify.length-2)return VIRTIO_UNSUPPORTED;
    p->queue_notify[queue]=(volatile uint16_t *)(p->notify+offset);return VIRTIO_OK;
}
static volatile unsigned char *region(struct virtio_pci_transport *p,const struct pci_cap_region *cap,unsigned alignment)
{
    struct pci_bar bar=p->function.bars[cap->bar];
    if(!cap->length || !bar.size || cap->offset>bar.size || cap->length>bar.size-cap->offset ||
        ((bar.address+cap->offset)&(alignment-1)))return 0;
    return p->function.host->map(p->function.host->context,bar.address+cap->offset,cap->length);
}
enum virtio_status virtio_pci_transport_initialize(struct virtio_pci_transport *p,struct pci_host *host,
    uint16_t bdf,uint32_t device_id,uint32_t config_size)
{
    if(!p || p->function.host || !host || !host->map || !host->interrupt_source ||
        !host->register_irq || !host->unregister_irq || !device_id || device_id>0xefbf)return VIRTIO_INVALID;
    enum pci_status status=pci_function_probe(host,bdf,&p->function);
    if(status!=PCI_OK)return status==PCI_NOT_PRESENT ? VIRTIO_EMPTY : VIRTIO_UNSUPPORTED;
    enum virtio_status result=VIRTIO_UNSUPPORTED;
    if(!virtio_pci_device_matches(host,bdf,device_id)) {result=VIRTIO_EMPTY;goto unassigned;}
    status=pci_virtio_capabilities(&p->function,&p->caps);
    if(status!=PCI_OK || p->caps.device.length<config_size)goto unassigned;
    status=pci_function_assign(&p->function);
    if(status!=PCI_OK) {result=status==PCI_NO_RESOURCE ? VIRTIO_NO_MEMORY : VIRTIO_UNSUPPORTED;goto unassigned;}
    p->common=region(p,&p->caps.common,4);p->notify=region(p,&p->caps.notify,2);
    p->isr=region(p,&p->caps.isr,1);
    if(p->caps.device.length)p->config=region(p,&p->caps.device,4);
    if(!p->common || !p->notify || !p->isr || (p->caps.device.length && !p->config) ||
        !p->function.interrupt_pin)goto assigned;
    p->irq=host->interrupt_source(host->context,bdf,p->function.interrupt_pin);
    if(!p->irq)goto assigned;
    p->transport=(struct virtio_transport){.context=p,.version=2,.device_id=device_id,
        .kind=VIRTIO_TRANSPORT_PCI,.config_size=p->caps.device.length,
        .ops={pci_read,pci_write,pci_config,pci_ack,irq_register,irq_unregister,prepare_queue}};
    /* 先确认旧queue停止，再开启bus-master；失败保留真实BAR owner。 */
    result=virtio_transport_reset(&p->transport);
    if(result!=VIRTIO_OK)return result;
    host->write(host->context,bdf,4,2,(p->function.command|6)&~0x401U);
    return VIRTIO_OK;
assigned:
    if(pci_function_restore(&p->function)!=PCI_OK)__builtin_trap();
unassigned:
    *p=(struct virtio_pci_transport){0};return result;
}
enum virtio_status virtio_pci_transport_destroy(struct virtio_pci_transport *p)
{
    if(!p || !p->function.host || !p->function.assigned)return VIRTIO_STATE;
    enum virtio_status result=virtio_transport_reset(&p->transport);
    if(result!=VIRTIO_OK)return result;
    if(pci_function_restore(&p->function)!=PCI_OK)__builtin_trap();
    *p=(struct virtio_pci_transport){0};return VIRTIO_OK;
}
