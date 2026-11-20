#include <kernel/block.h>
#include <kernel/errno.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../common/gpt_fixture.h"
static unsigned char image[512*512];
static uint64_t failure=UINT64_MAX;
static unsigned reads;
void block_model_before_lock(struct kernel_rwlock *lock) { (void)lock; }
static enum kernel_block_status read_image(void *context,uint64_t offset,void *buffer,size_t size)
{
    (void)context; assert(offset<=sizeof(image) && size<=sizeof(image)-offset);
    if(offset==failure)return KERNEL_BLOCK_STATUS_IO;
    reads++;memcpy(buffer,image+offset,size);return 0;
}
static enum kernel_block_status write_image(void *context,uint64_t offset,const void *buffer,size_t size)
{
    (void)context;assert(offset<=sizeof(image) && size<=sizeof(image)-offset);
    memcpy(image+offset,buffer,size);return 0;
}
static void headers32(unsigned field,uint32_t value)
{
    unsigned char *a=image+512,*b=image+sizeof(image)-512;
    gpt_test_put32(a+field,value);gpt_test_put32(b+field,value);
    gpt_test_header_crc(a);gpt_test_header_crc(b);
}
static void headers64(unsigned field,uint64_t value)
{
    unsigned char *a=image+512,*b=image+sizeof(image)-512;
    gpt_test_put64(a+field,value);gpt_test_put64(b+field,value);
    gpt_test_header_crc(a);gpt_test_header_crc(b);
}
static void rejected(struct kernel_block_device *disk,int error)
{
    assert(kernel_block_scan_gpt(disk,gpt_test_children,15)==error);
    assert(!disk->references);
    for(unsigned i=0;i<15;i++)assert(!gpt_test_children[i].registered && !kernel_block_lookup(disk->device_number+i+1));
}
static int fail_publication;
static unsigned batches, removals;

static int publish_partitions(struct kernel_block_device *const *devices,
    size_t count, int create, kernel_block_activate_fn activate)
{
    if (create) {
        assert(count == 2);
        batches++;
    } else {
        assert(count == 1);
        removals++;
    }
    for (size_t i = 0; i < count; i++) {
        assert(devices[i]->parent);
        assert(!kernel_block_get(devices[i]->device_number));
    }
    if (fail_publication) return -KERNEL_ENOMEM;
    if (create) {
        activate(devices, count);
        /* Observer woken by the first IN_CREATE, while the publisher is still active. */
        for (size_t i = 0; i < count; i++) {
            struct kernel_block_device *held = kernel_block_get(devices[i]->device_number);
            assert(held == devices[i]);
            kernel_block_put(held);
            assert(kernel_block_unregister(devices[i]) == -KERNEL_EBUSY);
        }
    }
    return 0;
}

static void test_partition_publication(struct kernel_block_device *disk)
{
    assert(!gpt_test_fixture(disk, 128));
    kernel_block_set_devnode_publisher(publish_partitions);
    fail_publication = 1;
    rejected(disk, -KERNEL_ENOMEM);
    fail_publication = 0;
    assert(!kernel_block_scan_gpt(disk, gpt_test_children, 15));
    assert(!gpt_test_release() && !disk->references);
    assert(batches == 2 && removals == 2);
    kernel_block_set_devnode_publisher(NULL);
}
int main(int argc,char **argv)
{
    struct kernel_block_device disk={.read=read_image,.write=write_image,.logical_block_size=512,.capacity_bytes=sizeof(image)};
    assert(!test_gpt_partitions(&disk));
    assert(!kernel_block_register(&disk,KERNEL_BLOCK_DEVICE_NUMBER(0)));
    test_partition_publication(&disk);
    for(unsigned size=128;size<=512;size*=2) {
        assert(!gpt_test_fixture(&disk,size));reads=0;
        assert(!kernel_block_scan_gpt(&disk,gpt_test_children,15));
        assert(reads==3+2*(128*size/512)); /* No per-entry duplicate reads. */
        assert(disk.references==2 && !gpt_test_release() && !disk.references);
    }
    /* Last encodable minor remains usable; entry gaps retain their numbering. */
    assert(!gpt_test_fixture(&disk,128));gpt_test_entry(14,128,200,215);
    assert(!gpt_test_write_copies(&disk,128));
    assert(!kernel_block_scan_partitions(&disk,gpt_test_children));
    assert(gpt_test_children[14].registered && gpt_test_children[14].device_number==disk.device_number+15);
    assert(!gpt_test_release());
    for(unsigned copy=0;copy<2;copy++) {
        assert(!gpt_test_fixture(&disk,128));
        image[(copy?sizeof(image)-33*512:1024)+90]^=1;
        assert(!kernel_block_scan_gpt(&disk,gpt_test_children,15));assert(!gpt_test_release());
    }
    /* A bit flip in slot 16 must not mask a usable copy as unsupported. */
    for (unsigned copy = 0; copy < 2; copy++) {
        assert(!gpt_test_fixture(&disk, 128));
        size_t entries = copy ? sizeof(image) - 33 * 512 : 1024;
        image[entries + 15 * 128] ^= 1;
        assert(!kernel_block_scan_gpt(&disk, gpt_test_children, 15));
        assert(gpt_test_children[0].registered && gpt_test_children[2].registered);
        assert(!gpt_test_release());
    }
    assert(!gpt_test_fixture(&disk,128));
    if(argc==2) { FILE *out=fopen(argv[1],"wb");assert(out);assert(fwrite(image,1,sizeof(image),out)==sizeof(image));assert(!fclose(out)); }
    image[510]=0;rejected(&disk,-KERNEL_EINVAL);
    assert(!gpt_test_fixture(&disk,128));image[466]=0x83;rejected(&disk,-KERNEL_ENOTSUP);
    assert(!gpt_test_fixture(&disk,128));gpt_test_put32(image+458,510);rejected(&disk,-KERNEL_EINVAL);
    assert(!gpt_test_fixture(&disk,128));headers32(12,91);rejected(&disk,-KERNEL_EINVAL);
    assert(!gpt_test_fixture(&disk,128));headers32(80,0);rejected(&disk,-KERNEL_EINVAL);
    assert(!gpt_test_fixture(&disk,128));headers32(80,UINT32_MAX);rejected(&disk,-KERNEL_ENOTSUP);
    assert(!gpt_test_fixture(&disk,128));headers32(84,192);rejected(&disk,-KERNEL_ENOTSUP);
    assert(!gpt_test_fixture(&disk,128));headers64(72,UINT64_MAX);rejected(&disk,-KERNEL_EINVAL);
    assert(!gpt_test_fixture(&disk,128));headers64(40,1);rejected(&disk,-KERNEL_EINVAL);
    assert(!gpt_test_fixture(&disk,128));headers64(48,511);rejected(&disk,-KERNEL_EINVAL);
    assert(!gpt_test_fixture(&disk,128));image[512+16]^=1;image[sizeof(image)-512+16]^=1;rejected(&disk,-KERNEL_EINVAL);
    assert(!gpt_test_fixture(&disk,128));image[1024+90]^=1;image[sizeof(image)-33*512+90]^=1;rejected(&disk,-KERNEL_EINVAL);
    assert(!gpt_test_fixture(&disk,128));gpt_test_entry(2,128,140,170);assert(!gpt_test_write_copies(&disk,128));rejected(&disk,-KERNEL_EINVAL);
    assert(!gpt_test_fixture(&disk,128));gpt_test_entry(2,128,180,179);assert(!gpt_test_write_copies(&disk,128));rejected(&disk,-KERNEL_EINVAL);
    assert(!gpt_test_fixture(&disk,128));gpt_test_entry(2,128,1,4);assert(!gpt_test_write_copies(&disk,128));rejected(&disk,-KERNEL_EINVAL);
    assert(!gpt_test_fixture(&disk,128));memcpy(gpt_test_table+2*128+16,gpt_test_table+16,16);assert(!gpt_test_write_copies(&disk,128));rejected(&disk,-KERNEL_EINVAL);
    assert(!gpt_test_fixture(&disk,128));gpt_test_entry(15,128,190,200);assert(!gpt_test_write_copies(&disk,128));rejected(&disk,-KERNEL_ENOSPC);
    assert(!gpt_test_fixture(&disk,128));gpt_test_put64(image+sizeof(image)-512+48,470);gpt_test_header_crc(image+sizeof(image)-512);rejected(&disk,-KERNEL_EINVAL);
    /* Either CRC-valid copy is sufficient; no writes/repair by discovery. */
    for(unsigned copy=0;copy<2;copy++) {
        assert(!gpt_test_fixture(&disk,128));failure=copy?sizeof(image)-512:512;
        assert(!kernel_block_scan_gpt(&disk,gpt_test_children,15));assert(!gpt_test_release());failure=UINT64_MAX;
    }
    assert(!gpt_test_fixture(&disk,128));failure=0;rejected(&disk,-KERNEL_EIO);failure=UINT64_MAX;
    /* Collision on the later used number cannot publish the earlier child. */
    struct kernel_block_device collision={.read=read_image,.logical_block_size=512};
    assert(!kernel_block_register(&collision,disk.device_number+3));
    assert(kernel_block_scan_gpt(&disk,gpt_test_children,15)==-KERNEL_EBUSY);
    assert(!disk.references && !gpt_test_children[0].registered && !kernel_block_lookup(disk.device_number+1));
    assert(!kernel_block_unregister(&collision));
    gpt_test_children[1].references=1;
    rejected(&disk,-KERNEL_EBUSY);gpt_test_children[1].references=0;
    disk.logical_block_size=4096;rejected(&disk,-KERNEL_EINVAL);disk.logical_block_size=512;
    assert(!kernel_block_unregister(&disk));
    puts("GPT: CRC, geometry, recovery, entry sizes, overlap, capacity, atomic publication and owners passed");
    return 0;
}
