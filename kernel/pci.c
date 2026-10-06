#include <kernel/pci.h>
#include <string.h>

static uint32_t read(const struct pci_function *f,uint16_t offset,unsigned width)
{ return f->host->read(f->host->context,f->bdf,offset,width); }
static void write(const struct pci_function *f,uint16_t offset,unsigned width,uint32_t value)
{ f->host->write(f->host->context,f->bdf,offset,width,value); }
enum pci_status pci_function_probe(struct pci_host *host,uint16_t bdf,struct pci_function *out)
{
    if (!host || !host->read || !host->write || !out || out->host) return PCI_INVALID;
    struct pci_function f={.host=host,.bdf=bdf};
    f.identity=read(&f,0,4);
    if ((uint16_t)f.identity==0xffff || !(uint16_t)f.identity) return PCI_NOT_PRESENT;
    f.header_type=read(&f,0xe,1)&0x7f;
    if (f.header_type!=0) return PCI_UNSUPPORTED;
    f.interrupt_pin=read(&f,0x3d,1);
    if (f.interrupt_pin>4) return PCI_MALFORMED;
    f.command=read(&f,4,2);
    for (unsigned i=0;i<6;i++) f.saved_bars[i]=read(&f,0x10+i*4,4);
    *out=f;return PCI_OK;
}
enum pci_status pci_virtio_capabilities(const struct pci_function *f,struct pci_virtio_caps *out)
{
    if (!f || !f->host || !out) return PCI_INVALID;
    if (!(read(f,6,2)&0x10)) return PCI_UNSUPPORTED;
    struct pci_virtio_caps caps={0};
    uint64_t seen=0;unsigned present=0,pos=read(f,0x34,1);
    while (pos) {
        if (pos<0x40 || pos>0xfc || (pos&3) || (seen&(UINT64_C(1)<<(pos/4)))) return PCI_MALFORMED;
        seen|=UINT64_C(1)<<(pos/4);
        unsigned next=read(f,pos+1,1);
        if (read(f,pos,1)==9) {
            unsigned length=read(f,pos+2,1),type=read(f,pos+3,1);
            if (length<4 || length>256-pos) return PCI_MALFORMED;
            if (type>=1 && type<=4) {
                if (length<16 || (type==2 && length<20)) return PCI_MALFORMED;
                struct pci_cap_region region={.bar=read(f,pos+4,1),.offset=read(f,pos+8,4),.length=read(f,pos+12,4)};
                unsigned minimum=type==1 ? 56 : type==2 ? 2 : type==3 ? 1 : 8;
                if (region.bar>=6 || region.length<minimum || region.offset>UINT32_MAX-region.length)
                    return PCI_MALFORMED;
                /* 同类 capability 可有不同 id；选第一个完整、可用的标准区域。 */
                if (!(present&(1U<<type))) {
                    if (type==1) caps.common=region;
                    if (type==2) {caps.notify=region;caps.notify_multiplier=read(f,pos+16,4);}
                    if (type==3) caps.isr=region;
                    if (type==4) caps.device=region;
                    present|=1U<<type;
                }
            }
        }
        pos=next;
    }
    if (present!=0x1e) return PCI_UNSUPPORTED;
    *out=caps;return PCI_OK;
}
unsigned pci_host_claimed(const struct pci_host *host)
{ unsigned count=0;for(unsigned i=0;i<PCI_MAX_CLAIMS;i++) count+=host->claims[i].owner!=NULL;return count; }
static void release_claims(struct pci_function *f)
{ for(unsigned i=0;i<PCI_MAX_CLAIMS;i++) if(f->host->claims[i].owner==f) memset(&f->host->claims[i],0,sizeof(f->host->claims[i])); }
static enum pci_status claim(struct pci_function *f,uint64_t size,uint64_t *address)
{
    struct pci_host *h=f->host;
    if (!size || (size&(size-1)) || h->memory_base>UINT64_MAX-h->memory_size) return PCI_INVALID;
    unsigned slot=PCI_MAX_CLAIMS;
    for (unsigned i=0;i<PCI_MAX_CLAIMS;i++) if(!h->claims[i].owner) {slot=i;break;}
    if(slot==PCI_MAX_CLAIMS) return PCI_NO_RESOURCE;
    uint64_t cursor=h->memory_base,end=cursor+h->memory_size;
    for (unsigned attempt=0;attempt<=PCI_MAX_CLAIMS;attempt++) {
        if(cursor>UINT64_MAX-(size-1)) return PCI_NO_RESOURCE;
        cursor=(cursor+size-1)&~(size-1);
        if(cursor>=end || size>end-cursor || cursor+size>UINT64_C(0x100000000)) return PCI_NO_RESOURCE;
        int overlap=0;
        for(unsigned i=0;i<PCI_MAX_CLAIMS;i++) if(h->claims[i].owner &&
            cursor<h->claims[i].address+h->claims[i].size && cursor+size>h->claims[i].address) {
            cursor=h->claims[i].address+h->claims[i].size;overlap=1;break;
        }
        if(!overlap) {
            h->claims[slot].owner=f;h->claims[slot].address=cursor;h->claims[slot].size=size;
            *address=cursor;return PCI_OK;
        }
    }
    return PCI_NO_RESOURCE;
}
static void restore_config(struct pci_function *f)
{
    write(f,4,2,f->command&~7U);
    for(unsigned i=0;i<6;i++) write(f,0x10+i*4,4,f->saved_bars[i]);
    write(f,4,2,f->command);
}
enum pci_status pci_function_assign(struct pci_function *f)
{
    if(!f || !f->host || f->assigned) return PCI_STATE;
    if(f->command&4) return PCI_STATE; /* 不重定位仍可 DMA 的外部 owner。 */
    enum pci_status result=PCI_OK;
    write(f,4,2,f->command&~7U);
    for(unsigned i=0;i<6;i++) {
        uint32_t original=f->saved_bars[i];
        if(original&1) {result=PCI_UNSUPPORTED;break;}
        unsigned kind=(original>>1)&3;
        if(kind!=0 && kind!=2) {result=PCI_UNSUPPORTED;break;}
        if(kind==2 && i==5) {result=PCI_MALFORMED;break;}
        write(f,0x10+i*4,4,UINT32_MAX);
        if(kind==2) write(f,0x14+i*4,4,UINT32_MAX);
        uint64_t mask=read(f,0x10+i*4,4)&~UINT32_C(15);
        if(kind==2) mask|=(uint64_t)read(f,0x14+i*4,4)<<32;
        else mask|=UINT64_C(0xffffffff00000000);
        write(f,0x10+i*4,4,original);
        if(kind==2) write(f,0x14+i*4,4,f->saved_bars[i+1]);
        uint64_t size=~mask+1,address;
        if(!size || (kind==0 && size==UINT64_C(0x100000000))) continue;
        if(size&(size-1)) {result=PCI_MALFORMED;break;}
        result=claim(f,size,&address);
        if(result!=PCI_OK) break;
        f->bars[i]=(struct pci_bar){address,size};
        write(f,0x10+i*4,4,(uint32_t)address|(original&15));
        if(kind==2) {write(f,0x14+i*4,4,address>>32);i++;}
    }
    if(result!=PCI_OK) {restore_config(f);release_claims(f);memset(f->bars,0,sizeof(f->bars));return result;}
    write(f,4,2,(f->command|2)&~4U);f->assigned=1;return PCI_OK;
}
enum pci_status pci_function_restore(struct pci_function *f)
{
    if(!f || !f->host || !f->assigned) return PCI_STATE;
    restore_config(f);release_claims(f);memset(f->bars,0,sizeof(f->bars));f->assigned=0;return PCI_OK;
}
