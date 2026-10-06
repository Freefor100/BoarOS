#include <kernel/virtio_pci_block.h>
#include <arch/bus.h>

static unsigned reg_offset(enum virtio_block_register reg,unsigned *width)
{
    static const uint8_t offsets[]={20,0,4,8,12,22,24,24,28,0,0,0,0,32,36,40,44,48,52,21,0,4};
    if((unsigned)reg>=sizeof(offsets)) __builtin_trap();
    *width=reg==VIRTIO_BLOCK_REG_STATUS || reg==VIRTIO_BLOCK_REG_CONFIG_GENERATION ? 1 :
        reg==VIRTIO_BLOCK_REG_QUEUE_SEL || reg==VIRTIO_BLOCK_REG_QUEUE_NUM_MAX ||
        reg==VIRTIO_BLOCK_REG_QUEUE_NUM || reg==VIRTIO_BLOCK_REG_QUEUE_READY ? 2 : 4;
    return offsets[reg];
}
static uint32_t pci_read(void *context,enum virtio_block_register reg)
{
    struct virtio_pci_block *block=context;unsigned width;
    if(reg==VIRTIO_BLOCK_REG_QUEUE_NOTIFY || reg==VIRTIO_BLOCK_REG_GUEST_PAGE_SIZE ||
        reg==VIRTIO_BLOCK_REG_QUEUE_ALIGN || reg==VIRTIO_BLOCK_REG_QUEUE_PFN) __builtin_trap();
    unsigned offset=reg_offset(reg,&width);
    volatile unsigned char *base=(reg==VIRTIO_BLOCK_REG_CONFIG || reg==VIRTIO_BLOCK_REG_CAPACITY_HIGH) ? block->config : block->common;
    if(width==1) return *(volatile uint8_t *)(base+offset);
    if(width==2) return *(volatile uint16_t *)(base+offset);
    return *(volatile uint32_t *)(base+offset);
}
static void pci_write(void *context,enum virtio_block_register reg,uint32_t value)
{
    struct virtio_pci_block *block=context;unsigned width;
    if(reg==VIRTIO_BLOCK_REG_QUEUE_NOTIFY) {*(volatile uint16_t *)block->notify=value;return;}
    if(reg==VIRTIO_BLOCK_REG_GUEST_PAGE_SIZE || reg==VIRTIO_BLOCK_REG_QUEUE_ALIGN || reg==VIRTIO_BLOCK_REG_QUEUE_PFN)
        __builtin_trap();
    unsigned offset=reg_offset(reg,&width);
    if(width==1) *(volatile uint8_t *)(block->common+offset)=value;
    else if(width==2) *(volatile uint16_t *)(block->common+offset)=value;
    else *(volatile uint32_t *)(block->common+offset)=value;
}
static uint32_t pci_ack(void *context)
{ struct virtio_pci_block *block=context;return *block->isr; }
static int pci_irq_register(void *context,uint32_t source,void (*handler)(void *),void *owner)
{ struct virtio_pci_block *b=context;return b->function.host->register_irq(b->function.host->context,source,handler,owner); }
static void pci_irq_unregister(void *context,uint32_t source,void *owner)
{ struct virtio_pci_block *b=context;b->function.host->unregister_irq(b->function.host->context,source,owner); }
static const struct virtio_block_transport_ops pci_ops={pci_read,pci_write,pci_ack,pci_irq_register,pci_irq_unregister};
static volatile unsigned char *region(struct virtio_pci_block *block,const struct pci_cap_region *cap,unsigned alignment)
{
    struct pci_bar bar=block->function.bars[cap->bar];
    if(!bar.size || cap->offset>bar.size || cap->length>bar.size-cap->offset || (cap->offset&(alignment-1))) return 0;
    return block->function.host->map(block->function.host->context,bar.address+cap->offset,cap->length);
}
enum virtio_block_status virtio_pci_block_init(struct virtio_pci_block *block,struct pci_host *host,uint16_t bdf,
    struct physical_page_allocator *allocator,virtio_dma_address_fn dma,uint32_t frequency)
{
    if(!block || block->function.host || !host || !host->map || !host->interrupt_source ||
        !host->register_irq || !host->unregister_irq || !allocator || !dma || !frequency) return VIRTIO_BLOCK_DRIVER_STATUS_INVALID;
    enum pci_status status=pci_function_probe(host,bdf,&block->function);
    if(status!=PCI_OK) return status==PCI_NOT_PRESENT ? VIRTIO_BLOCK_DRIVER_STATUS_NOT_BLOCK : VIRTIO_BLOCK_DRIVER_STATUS_UNSUPPORTED;
    if(block->function.identity!=0x10421af4) {block->function=(struct pci_function){0};return VIRTIO_BLOCK_DRIVER_STATUS_NOT_BLOCK;}
    status=pci_virtio_capabilities(&block->function,&block->caps);
    if(status==PCI_OK) status=pci_function_assign(&block->function);
    if(status!=PCI_OK) {block->function=(struct pci_function){0};return status==PCI_NO_RESOURCE ? VIRTIO_BLOCK_DRIVER_STATUS_NO_MEMORY : VIRTIO_BLOCK_DRIVER_STATUS_UNSUPPORTED;}
    block->common=region(block,&block->caps.common,4);block->notify=region(block,&block->caps.notify,2);
    block->isr=region(block,&block->caps.isr,1);block->config=region(block,&block->caps.device,4);
    enum virtio_block_status result=VIRTIO_BLOCK_DRIVER_STATUS_UNSUPPORTED;
    if(!block->common || !block->notify || !block->isr || !block->config || !block->function.interrupt_pin) goto fail;
    *(volatile uint16_t *)(block->common+22)=0;
    uint64_t notify=(uint64_t)*(volatile uint16_t *)(block->common+30)*block->caps.notify_multiplier;
    if(notify>block->caps.notify.length-2) goto fail;
    block->notify+=notify;
    block->irq=host->interrupt_source(host->context,bdf,block->function.interrupt_pin);
    if(!block->irq) goto fail;
    /* 先确认旧 queue 已 reset，再开 bus-master，不能短暂恢复外部的旧 DMA 地址。 */
    *(volatile uint8_t *)(block->common+20)=0;arch_io_barrier();
    if(*(volatile uint8_t *)(block->common+20)) {result=VIRTIO_BLOCK_DRIVER_STATUS_DEVICE;goto fail;}
    host->write(host->context,bdf,4,2,(block->function.command|6)&~0x400U);
    struct virtio_block_transport transport={block,&pci_ops,2};
    result=virtio_block_init(&block->device,&transport,allocator,dma,frequency);
    if(result==VIRTIO_BLOCK_DRIVER_STATUS_OK) return result;
fail:
    if(pci_function_restore(&block->function)!=PCI_OK) __builtin_trap();
    *block=(struct virtio_pci_block){0};return result;
}
enum virtio_block_status virtio_pci_block_destroy(struct virtio_pci_block *block)
{
    enum virtio_block_status status=virtio_block_destroy(&block->device);
    if(status!=VIRTIO_BLOCK_DRIVER_STATUS_OK) return status;
    if(pci_function_restore(&block->function)!=PCI_OK) __builtin_trap();
    *block=(struct virtio_pci_block){0};return VIRTIO_BLOCK_DRIVER_STATUS_OK;
}
