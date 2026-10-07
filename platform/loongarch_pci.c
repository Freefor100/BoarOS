#include <platform/loongarch_pci.h>
#include <platform/loongarch_virt.h>
#include <arch/context.h>
#include <arch/bus.h>

/* Fixed QEMU v11.1 virt.h: ECAM 128 buses, low PCI memory aperture. */
#define ECAM_BASE UINT64_C(0x20000000)
#define ECAM_SIZE UINT64_C(0x08000000)
static uint32_t config_read(void *context,uint16_t bdf,uint16_t offset,unsigned width)
{
    (void)context;
    uint64_t address=((uint64_t)bdf<<12)+offset;
    if(offset>=4096 || width>4096U-offset || address+width>ECAM_SIZE) return UINT32_MAX;
    volatile void *p=(void *)(uintptr_t)(LA_UNCACHED_BASE+ECAM_BASE+address);
    if(width==1) return *(volatile uint8_t *)p;
    if(width==2 && !(offset&1)) return *(volatile uint16_t *)p;
    if(width==4 && !(offset&3)) return *(volatile uint32_t *)p;
    la_virt_fatal("PCI config width");
}
static void config_write(void *context,uint16_t bdf,uint16_t offset,unsigned width,uint32_t value)
{
    (void)context;
    uint64_t address=((uint64_t)bdf<<12)+offset;
    if(offset>=4096 || width>4096U-offset || address+width>ECAM_SIZE) la_virt_fatal("PCI config range");
    volatile void *p=(void *)(uintptr_t)(LA_UNCACHED_BASE+ECAM_BASE+address);
    if(width==1) *(volatile uint8_t *)p=value;
    else if(width==2 && !(offset&1)) *(volatile uint16_t *)p=value;
    else if(width==4 && !(offset&3)) *(volatile uint32_t *)p=value;
    else la_virt_fatal("PCI config width");
    arch_io_barrier();
}
static volatile void *map_resource(void *context,uint64_t address,uint64_t size)
{
    (void)context;
    if(!size || address<0x40000000 || address>=0x80000000 || size>0x80000000-address) return 0;
    return (void *)(uintptr_t)(LA_UNCACHED_BASE+address);
}
static uint32_t interrupt_source(void *context,uint16_t bdf,uint8_t pin)
{ (void)context;return !(bdf>>8) && pin>=1 && pin<=4 ? 16+(((bdf>>3)+pin-1)&3) : 0; }
static int register_interrupt(void *context,uint32_t source,void (*handler)(void *),void *owner)
{ (void)context;return la_virt_irq_register(source,handler,owner); }
static void unregister_interrupt(void *context,uint32_t source,void *owner)
{ (void)context;la_virt_irq_unregister(source,owner); }
struct pci_host *la_virt_pci_host(void)
{
    static struct pci_host host;
    if(!host.read) {
        host.read=config_read;host.write=config_write;
        host.map=map_resource;host.interrupt_source=interrupt_source;
        host.register_irq=register_interrupt;host.unregister_irq=unregister_interrupt;
        host.memory_base=0x40000000;host.memory_size=0x40000000;
    }
    return &host;
}
static uint32_t iocsr_read(uint32_t address)
{ uint32_t value;__asm__ volatile("iocsrrd.w %0, %1":"=r"(value):"r"(address):"memory");return value; }
static void iocsr_write(uint32_t address,uint32_t value)
{ __asm__ volatile("iocsrwr.w %0, %1"::"r"(value),"r"(address):"memory"); }
static volatile uint32_t *pic(unsigned offset)
{ return (void *)(uintptr_t)(LA_UNCACHED_BASE+0x10000000+offset); }
static int initialized,dispatching;
static struct { uint32_t source;void (*handle)(void *);void *owner; } handlers[32];
int la_virt_irq_active(void) { return dispatching; }
int la_virt_irq_initialize(void)
{
    if(initialized) return 0;
    /* 所有外设先 mask；映射完成、handler owner 发布后才单独打开。 */
    *pic(0x20)=UINT32_MAX;*pic(0x24)=UINT32_MAX;
    *pic(0x60)=0;*pic(0x64)=0;*pic(0x3e0)=0;*pic(0x3e4)=0;
    *pic(0x40)=UINT32_MAX;*pic(0x44)=UINT32_MAX;
    uint64_t misc,address=0x420;
    __asm__ volatile("iocsrrd.d %0, %1":"=r"(misc):"r"(address):"memory");
    misc=(misc|(UINT64_C(1)<<48))&~(UINT64_C(1)<<49);
    __asm__ volatile("iocsrwr.d %0, %1"::"r"(misc),"r"(address):"memory");
    for(unsigned i=0;i<8;i++) {iocsr_write(0x1600+i*4,0);iocsr_write(0x1800+i*4,UINT32_MAX);}
    for(unsigned i=0;i<2;i++) iocsr_write(0x14c0+i*4,0x01010101);
    for(unsigned i=0;i<64;i++) iocsr_write(0x1c00+i*4,0x01010101);
    uint64_t mask=4,value=4;
    __asm__ volatile("csrxchg %0, %1, 4":"+r"(value):"r"(mask):"memory");
    initialized=1;return 1;
}
int la_virt_irq_register(uint32_t source,void (*handle)(void *),void *owner)
{
    if(!initialized || source>=64 || !source || !handle || !owner) return 0;
    uintptr_t irq=arch_interrupt_save();unsigned slot=32;
    for(unsigned i=0;i<32;i++) {
        if(handlers[i].owner==owner) {arch_interrupt_restore(irq);return 0;}
        if(!handlers[i].owner) slot=i;
    }
    if(slot==32) {arch_interrupt_restore(irq);return 0;}
    handlers[slot].source=source;handlers[slot].handle=handle;handlers[slot].owner=owner;
    *(volatile uint8_t *)((uintptr_t)pic(0x200)+source)=source;
    iocsr_write(0x1600+(source/32)*4,iocsr_read(0x1600+(source/32)*4)|(1U<<(source%32)));
    *pic(0x20+(source/32)*4)&=~(1U<<(source%32));
    arch_io_barrier();arch_interrupt_restore(irq);return 1;
}
void la_virt_irq_unregister(uint32_t source,void *owner)
{
    uintptr_t irq=arch_interrupt_save();unsigned found=32,remaining=0;
    for(unsigned i=0;i<32;i++) if(handlers[i].source==source && handlers[i].owner) {
        if(handlers[i].owner==owner) found=i;else remaining++;
    }
    if(found==32) la_virt_fatal("PCI interrupt owner");
    if(!remaining) {
        *pic(0x20+(source/32)*4)|=1U<<(source%32);
        iocsr_write(0x1600+(source/32)*4,iocsr_read(0x1600+(source/32)*4)&~(1U<<(source%32)));
    }
    handlers[found].owner=0;handlers[found].handle=0;arch_io_barrier();arch_interrupt_restore(irq);
}
void la_virt_irq_dispatch(void)
{
    if(!initialized || dispatching) la_virt_fatal("external interrupt state");
    dispatching=1;
    for(unsigned round=0;round<64;round++) {
        uint32_t pending=iocsr_read(0x1800)|iocsr_read(0x1804);
        if(!pending) break;
        for(unsigned group=0;group<2;group++) {
            uint32_t bits=iocsr_read(0x1800+group*4);
            while(bits) {
                unsigned bit=(unsigned)__builtin_ctz(bits),source=group*32+bit;bits&=bits-1;
                /* 共享 level pin 在逐设备 ISR 之间可能一直为高；mask/unmask 重新采样 intirr。 */
                *pic(0x20+group*4)|=1U<<bit;
                arch_io_barrier();
                iocsr_write(0x1800+group*4,1U<<bit);
                unsigned handled=0;
                for(unsigned i=0;i<32;i++) if(handlers[i].owner && handlers[i].source==source) {
                    handled++;handlers[i].handle(handlers[i].owner);
                }
                if(handled) *pic(0x20+group*4)&=~(1U<<bit);
                arch_io_barrier();
            }
        }
    }
    dispatching=0;
}
