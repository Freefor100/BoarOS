#include <kernel/virtio_mmio.h>
#include <kernel/virtio_pci.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
static _Alignas(16) unsigned char config[256],bar[16384];
static uint32_t cfg_read(void *context,uint16_t bdf,uint16_t offset,unsigned width)
{
    assert(context==config && bdf==8 && offset+width<=sizeof(config));
    uint32_t value=0;memcpy(&value,config+offset,width);
    if(width==4 && offset>=0x10 && offset<0x28 && value==UINT32_MAX)
        return offset==0x20 ? 0xffffc00c : offset==0x24 ? UINT32_MAX : 0;
    return value;
}
static void cfg_write(void *context,uint16_t bdf,uint16_t offset,unsigned width,uint32_t value)
{ assert(context==config && bdf==8 && offset+width<=sizeof(config));memcpy(config+offset,&value,width); }
static volatile void *map(void *context,uint64_t address,uint64_t size)
{ assert(context==config && address>=0x40000000 && address-0x40000000+size<=sizeof(bar));return bar+(address-0x40000000); }
static uint32_t irq_source(void *context,uint16_t bdf,uint8_t pin)
{ assert(context==config && bdf==8 && pin==1);return 17; }
static int irq_register(void *context,uint32_t source,void (*handler)(void *),void *owner)
{ (void)context;(void)handler;(void)owner;assert(source==17);return 1; }
static void irq_unregister(void *context,uint32_t source,void *owner)
{ (void)context;(void)owner;assert(source==17); }
static void put(unsigned offset,uint32_t value,unsigned width)
{ memcpy(config+offset,&value,width); }
static void cap(unsigned pos,unsigned next,unsigned type,unsigned offset,unsigned size)
{
    config[pos]=9;config[pos+1]=next;config[pos+2]=type==2 ? 20 : 16;
    config[pos+3]=type;config[pos+4]=4;
    put(pos+8,offset,4);put(pos+12,size,4);
}
int main(void)
{
    _Alignas(16) uint32_t mmio[128]={0x74726976,2,4};
    struct virtio_transport t={0};uint64_t features;
    mmio[0x10/4]=1;mmio[0x34/4]=32;mmio[0x100/4]=0x87654321;
    assert(virtio_mmio_transport_initialize(&t,mmio,sizeof(mmio),4,irq_register,irq_unregister)==VIRTIO_OK);
    assert(t.kind==VIRTIO_TRANSPORT_MMIO && t.device_id==4);
    assert(virtio_transport_begin(&t,0,0,&features)==VIRTIO_OK && features==0x100000000ULL);
    assert(virtio_transport_queue(&t,0,1,0x100000,16,32,4096)==VIRTIO_OK);
    assert(mmio[0x80/4]==0x100000 && mmio[0x90/4]==0x100010 && mmio[0xa0/4]==0x100020);
    uint32_t value=0;
    assert(virtio_transport_config_read(&t,0,4,&value)==VIRTIO_OK && value==0x87654321);
    assert(virtio_transport_config_read(&t,1,2,&value)==VIRTIO_INVALID && value==0x87654321);
    assert(virtio_transport_config_read(&t,256,4,&value)==VIRTIO_INVALID);
    mmio[0x60/4]=3;assert(t.ops.ack_interrupt(t.context)==3 && mmio[0x64/4]==3);
    assert(virtio_transport_start(&t)==VIRTIO_OK && virtio_transport_reset(&t)==VIRTIO_OK);
    mmio[1]=1;
    assert(virtio_mmio_transport_initialize(&t,mmio,sizeof(mmio),4,irq_register,irq_unregister)==VIRTIO_OK);
    assert(virtio_transport_begin(&t,0,0,0)==VIRTIO_OK);
    assert(virtio_transport_queue(&t,0,1,0x100000,16,4096,4096)==VIRTIO_OK);
    assert(mmio[0x28/4]==4096 && mmio[0x3c/4]==4096 && mmio[0x40/4]==0x100);
    put(0,0x10441af4,4);put(6,0x10,2);put(0x20,12,4);config[0x3d]=1;config[0x34]=0x40;
    cap(0x40,0x50,1,0,56);cap(0x50,0x64,2,4096,4096);put(0x60,4,4);
    cap(0x64,0,3,8192,1);
    struct pci_host host={.context=config,.read=cfg_read,.write=cfg_write,.map=map,
        .interrupt_source=irq_source,.register_irq=irq_register,.unregister_irq=irq_unregister,
        .memory_base=0x40000000,.memory_size=sizeof(bar)};
    struct virtio_pci_transport p={0};
    assert(virtio_pci_transport_initialize(&p,&host,8,4,0)==VIRTIO_OK && pci_host_claimed(&host)==1);
    assert(p.irq==17 && !p.config && !p.transport.config_size);
    *(uint32_t *)(bar+4)=1;*(uint16_t *)(bar+24)=32;*(uint16_t *)(bar+30)=3;
    assert(virtio_transport_begin(&p.transport,0,0,0)==VIRTIO_OK);
    assert(virtio_transport_queue(&p.transport,0,1,0x100000,16,32,16384)==VIRTIO_OK);
    /* This memory model explicitly supplies the newly selected queue register bank. */
    *(uint16_t *)(bar+28)=0;*(uint16_t *)(bar+30)=7;
    assert(virtio_transport_queue(&p.transport,1,1,0x200000,16,32,16384)==VIRTIO_OK);
    p.transport.ops.write(p.transport.context,VIRTIO_REG_QUEUE_NOTIFY,1);
    assert(*(uint16_t *)(bar+4096+28)==1 && !*(uint16_t *)(bar+4096+12));
    *(uint16_t *)(bar+28)=0;*(uint16_t *)(bar+30)=65535;
    assert(virtio_transport_queue(&p.transport,2,1,0x300000,16,32,16384)==VIRTIO_UNSUPPORTED);
    assert(virtio_transport_config_read(&p.transport,0,1,&value)==VIRTIO_INVALID);
    assert(virtio_pci_transport_destroy(&p)==VIRTIO_OK && !pci_host_claimed(&host));
    assert(cfg_read(config,8,0x20,4)==12 && !cfg_read(config,8,4,2));
    assert(virtio_pci_transport_initialize(&p,&host,8,4,8)==VIRTIO_UNSUPPORTED && !pci_host_claimed(&host));
    /* Transitional identity still exposes the modern caps; unused PIO stays disabled. */
    put(0,0x10051af4,4);put(0x2c,0x00041af4,4);put(4,1,2);put(0x10,0x101,4);
    assert(virtio_pci_transport_initialize(&p,&host,8,4,0)==VIRTIO_OK);
    assert(pci_host_claimed(&host)==1 && !(cfg_read(config,8,4,2)&1));
    assert(cfg_read(config,8,0x10,4)==0x101 && !p.function.bars[0].size);
    assert(virtio_pci_transport_destroy(&p)==VIRTIO_OK && !pci_host_claimed(&host));
    assert(cfg_read(config,8,4,2)==1 && cfg_read(config,8,0x10,4)==0x101);
    put(0x2c,0x00021af4,4);
    assert(virtio_pci_transport_initialize(&p,&host,8,4,0)==VIRTIO_EMPTY && !pci_host_claimed(&host));
    put(0x2c,0x00041af4,4);config[0x34]=0;
    assert(virtio_pci_transport_initialize(&p,&host,8,4,0)==VIRTIO_UNSUPPORTED && !pci_host_claimed(&host));
    puts("VirtIO legacy/modern MMIO and PCI widths, per-queue notification, optional config and BAR owner passed");
}
