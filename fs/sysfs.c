#include "bpf_fs.h"
#include <kernel/block.h>
#include <kernel/bpf.h>
#include <kernel/cpu.h>
#include <arch/smp.h>
#include <kernel/errno.h>
#include <kernel/heap.h>
#include <string.h>
static char *number(char *out,uint64_t n)
{char digits[20];unsigned count=0;do{digits[count++]=(char)('0'+n%10);n/=10;}while(n);while(count)*out++=digits[--count];return out;}

struct sysfs_block_data {
    char name[32];
    char logical_size[12], dma_alignment[12];
    uint64_t inode;
};

static size_t number_line(char *out, uint32_t value)
{
    char *end=number(out,value);
    *end++='\n';
    return end-out;
}

static int block_stat_snapshot(uint64_t device, uint64_t generation, struct kernel_heap *heap,
    char **buffer, size_t *length)
{
    struct kernel_block_usage usage;
    int error = kernel_block_stat(device, generation, &usage);
    if (error) return error;
    uint64_t fields[KERNEL_BLOCK_STAT_FIELDS];
    kernel_block_stat_values(&usage, fields);
    char *data;
    if (kernel_heap_allocate(heap, sizeof(fields) / sizeof(fields[0]) * 21U, (void **)&data))
        return -KERNEL_ENOMEM;
    char *end = data;
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        if (i) *end++ = ' ';
        end = number(end, fields[i]);
    }
    *end++ = '\n';
    *buffer = data;
    *length = (size_t)(end - data);
    return 0;
}

static int mount_block_snapshot(struct kernel_heap *heap,uint64_t flags,
    const struct bpf_fs_entry *base,size_t base_count,uint64_t block_inode,
    struct kernel_vfs_mount **out)
{
    struct kernel_block_device **devices=NULL;
    size_t capacity=0,count;
    int result;
    while ((result=kernel_block_snapshot(devices,capacity,&count))==-KERNEL_ENOSPC) {
        if (devices) kernel_heap_release(heap,devices);
        if (kernel_heap_allocate_zeroed(heap,count,sizeof(*devices),(void **)&devices))
            return -KERNEL_ENOMEM;
        capacity=count;
    }
    if (result || !count) {
        if (devices) kernel_heap_release(heap,devices);
        return result ? result : bpf_metadata_mount(heap,flags,0x62656572,base,base_count,out);
    }

    struct bpf_fs_entry *entries=NULL;
    struct bpf_fs_attribute *attributes=NULL;
    struct sysfs_block_data *data=NULL;
    size_t used=base_count, attribute_count=0;
    if (count>(SIZE_MAX-base_count)/4 ||
        kernel_heap_allocate_zeroed(heap,base_count+count*4,sizeof(*entries),(void **)&entries) ||
        kernel_heap_allocate_zeroed(heap,count,sizeof(*data),(void **)&data) ||
        kernel_heap_allocate_zeroed(heap,count,sizeof(*attributes),(void **)&attributes))
        result=-KERNEL_ENOMEM;
    if (!result) {
        memcpy(entries,base,base_count*sizeof(*entries));
        for (size_t i=0;i<count;i++) {
            const struct kernel_block_device *device=devices[i];
            struct sysfs_block_data *disk=&data[i];
            /* A partition shares its whole disk's queue. */
            if (device->parent || !kernel_block_device_name(device->device_number,disk->name)) continue;
            uint64_t disk_inode=used+2, queue_inode=disk_inode+1;
            disk->inode=disk_inode;
            uint32_t sector=kernel_block_logical_size(device);
            uint32_t alignment=kernel_block_user_dio_alignment(sector);
            entries[used++]=(struct bpf_fs_entry){block_inode,disk->name,0,0,KERNEL_VFS_S_IFDIR|0555};
            entries[used++]=(struct bpf_fs_entry){disk_inode,"queue",0,0,KERNEL_VFS_S_IFDIR|0555};
            entries[used++]=(struct bpf_fs_entry){queue_inode,"logical_block_size",disk->logical_size,
                number_line(disk->logical_size,sector),KERNEL_VFS_S_IFREG|0444};
            if (alignment)
                entries[used++]=(struct bpf_fs_entry){queue_inode,"dma_alignment",disk->dma_alignment,
                    number_line(disk->dma_alignment,alignment-1),KERNEL_VFS_S_IFREG|0444};
        }
        for (size_t i=0;i<count;i++) {
            const struct kernel_block_device *device=devices[i];
            if (device->parent) {
                size_t parent=0;
                while (parent<count && devices[parent]!=device->parent) parent++;
                if (parent==count || !data[parent].inode ||
                    !kernel_block_device_name(device->device_number,data[i].name)) continue;
                data[i].inode=used+2;
                entries[used++]=(struct bpf_fs_entry){data[parent].inode,data[i].name,0,0,KERNEL_VFS_S_IFDIR|0555};
            }
            if (data[i].inode)
                attributes[attribute_count++]=(struct bpf_fs_attribute){
                    data[i].inode,"stat",device->device_number,device->registration_generation,block_stat_snapshot
                };
        }
    }
    /* The immutable files copy the values. Open sysfs views must not pin disks. */
    for (size_t i=0;i<count;i++) kernel_block_put(devices[i]);
    kernel_heap_release(heap,devices);
    if (!result) result=bpf_metadata_mount_attributes(heap,flags,0x62656572,
        entries,used,attributes,attribute_count,out);
    if (attributes) kernel_heap_release(heap,attributes);
    if (data) kernel_heap_release(heap,data);
    if (entries) kernel_heap_release(heap,entries);
    return result;
}

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
        DIR(1,"fs"),DIR(10,"bpf"),DIR(7,"tracing"),DIR(1,"block"),
    };
#undef DIR
#undef FILE
    size_t base_count=sizeof(entries)/sizeof(entries[0]);
    /* The root is inode 1. The last base entry owns the block directory. */
    return mount_block_snapshot(heap,flags,entries,base_count,base_count+1,out);
}
