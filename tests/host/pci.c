#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <kernel/pci.h>

static unsigned char config[4096];
static uint32_t masks[6];
static uint32_t read_config(void *context,uint16_t bdf,uint16_t off,unsigned width)
{
    assert(context==config && bdf==8 && off+width<=sizeof(config));
    uint32_t value=0;memcpy(&value,config+off,width);
    if (width==4 && off>=0x10 && off<0x28 && value==UINT32_MAX) return masks[(off-0x10)/4];
    return value;
}
static void write_config(void *context,uint16_t bdf,uint16_t off,unsigned width,uint32_t value)
{ assert(context==config && bdf==8 && off+width<=sizeof(config));memcpy(config+off,&value,width); }
static void put(unsigned off,uint32_t value,unsigned width) { memcpy(config+off,&value,width); }
static void capability(unsigned pos,unsigned next,unsigned type,unsigned bar,unsigned offset,unsigned length)
{
    config[pos]=9;config[pos+1]=next;config[pos+2]=type==2 ? 20 : 16;
    config[pos+3]=type;config[pos+4]=bar;put(pos+8,offset,4);put(pos+12,length,4);
}
int main(void)
{
    put(0,0x10421af4,4);put(6,0x10,2);config[0x34]=0x40;
    capability(0x40,0x50,1,4,0,56);
    capability(0x50,0x64,2,4,4096,4096);put(0x60,4,4);
    capability(0x64,0x74,3,4,8192,1);
    capability(0x74,0,4,4,12288,8);
    put(0x20,12,4);masks[4]=0xffffc00c;masks[5]=UINT32_MAX;
    struct pci_host host={.context=config,.read=read_config,.write=write_config,
        .memory_base=0x40000000,.memory_size=0x100000};
    struct pci_function function={0};struct pci_virtio_caps caps={0};
    assert(pci_function_probe(&host,8,&function)==PCI_OK);
    assert(pci_virtio_capabilities(&function,&caps)==PCI_OK && caps.common.bar==4 &&
        caps.common.length==56 && caps.notify_multiplier==4);
    assert(pci_function_assign(&function)==PCI_OK && function.bars[4].size==16384 &&
        function.bars[4].address==0x40000000 && pci_host_claimed(&host)==1);
    assert(pci_function_restore(&function)==PCI_OK && pci_host_claimed(&host)==0 &&
        read_config(config,8,0x20,4)==12 && read_config(config,8,4,2)==0);
    /* A capability cycle or invalid extent is rejected without partial output. */
    struct pci_virtio_caps untouched=caps;
    config[0x75]=0x40;
    assert(pci_virtio_capabilities(&function,&caps)==PCI_MALFORMED && !memcmp(&caps,&untouched,sizeof(caps)));
    config[0x75]=0;config[0x52]=16;
    assert(pci_virtio_capabilities(&function,&caps)==PCI_MALFORMED);
    config[0x52]=20;put(0x48,UINT32_MAX,4);
    assert(pci_virtio_capabilities(&function,&caps)==PCI_MALFORMED);
    put(0x48,0,4);host.memory_size=4096;
    assert(pci_function_assign(&function)==PCI_NO_RESOURCE && pci_host_claimed(&host)==0 &&
        read_config(config,8,0x20,4)==12 && read_config(config,8,4,2)==0);
    puts("PCI capability bounds, BAR ownership and rollback passed");
    return 0;
}
