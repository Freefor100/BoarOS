#include "bpf_fs.h"
#include <kernel/bpf.h>
#include <kernel/cpu.h>
#include <arch/smp.h>
#include <kernel/errno.h>
#include <string.h>
static char *number(char *out,unsigned n)
{char digits[16];unsigned count=0;do{digits[count++]=(char)('0'+n%10);n/=10;}while(n);while(count)*out++=digits[--count];return out;}
int kernel_sysfs_mount(struct kernel_heap *heap,uint64_t flags,struct kernel_vfs_mount **out)
{
    int result=kernel_bpf_initialize(heap);if(result)return result;
    char possible[32],online[BOAROS_MAX_CPUS*12+2];char *p=number(possible,0),*o=online;
    unsigned count=kernel_cpu_get_boot_count();
    if(count>1){*p++='-';p=number(p,count-1);}*p++='\n';
    for(unsigned j=0;j<count;j++) {
#if BOAROS_MAX_CPUS > 1
        struct kernel_cpu *cpu=arch_smp_cpu(j);if(!cpu || !kernel_cpu_online(cpu))continue;
#endif
        if(o!=online)*o++=',';
        o=number(o,j);
    }
    *o++='\n';uint32_t size;const void *btf=kernel_bpf_kernel_btf(&size);
    if(!btf)return -KERNEL_ENOENT;
#define DIR(p,n) {p,n,0,0,KERNEL_VFS_S_IFDIR|0555}
#define FILE(p,n,d,s) {p,n,d,s,KERNEL_VFS_S_IFREG|0444}
    const struct bpf_fs_entry entries[]={
        DIR(1,"devices"),DIR(2,"system"),DIR(3,"cpu"),
        FILE(4,"possible",possible,(size_t)(p-possible)),FILE(4,"online",online,(size_t)(o-online)),
        DIR(1,"kernel"),DIR(7,"btf"),{8,"vmlinux",btf,size,KERNEL_VFS_S_IFREG|0444},
        DIR(1,"fs"),DIR(10,"bpf"),DIR(7,"tracing"),
    };
#undef DIR
#undef FILE
    return bpf_metadata_mount(heap,flags,0x62656572,entries,sizeof(entries)/sizeof(entries[0]),out);
}
