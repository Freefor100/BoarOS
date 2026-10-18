#ifndef BOAROS_TEST_BLOCK_PARTITIONS_H
#define BOAROS_TEST_BLOCK_PARTITIONS_H
#include <kernel/block.h>
#include <kernel/errno.h>
/* Use unused ranges of the disposable 1 MiB block fixtures on both transports. */
static int test_primary_partitions(struct kernel_block_device *disk)
{
    struct kernel_block_device children[4] = {0};
    unsigned char sector[512] = {0}, data[512];
    int registered_here = !disk->registered, mount_owner, exclusive_owner;
    if (registered_here && kernel_block_register(disk, KERNEL_BLOCK_DEVICE_NUMBER(0))) return 1;
    sector[510]=0x55; sector[511]=0xaa;
    sector[446+4]=0x83; sector[446+9]=4; sector[446+12]=32;
    sector[478+4]=0x83; sector[478+8]=64; sector[478+9]=4; sector[478+12]=64;
    if (kernel_block_write_at(disk,0,sector,sizeof(sector)) || kernel_block_scan_mbr(disk,children)) return 2;
    if (kernel_block_lookup(disk->device_number+1)!=&children[0] ||
        kernel_block_lookup(disk->device_number+3)!=&children[2] ||
        children[0].capacity_bytes!=32*512 || children[2].capacity_bytes!=64*512) return 3;
    if (kernel_block_claim(&children[0],&mount_owner) ||
        kernel_block_open(&children[2],&exclusive_owner,0200) ||
        kernel_block_claim(disk,&exclusive_owner)!=-KERNEL_EBUSY) return 4;
    kernel_block_close(&children[2],&exclusive_owner,0200);
    kernel_block_release_claim(&children[0],&mount_owner);
    for(unsigned i=0;i<512;i++) data[i]=(unsigned char)(i*7+19);
    struct kernel_block_span writes[2]={{0,data,256},{256,data+256,256}};
    if(kernel_block_write_batch(&children[2],writes,2) || kernel_block_flush(&children[2]) ||
        kernel_block_read_at(disk,1088*512,sector,512)) return 5;
    for(unsigned i=0;i<512;i++) if(data[i]!=sector[i]) return 6;
    if(kernel_block_read_at(&children[0],32*512,sector,1)!=KERNEL_BLOCK_STATUS_OUT_OF_RANGE ||
        kernel_block_unregister(disk)!=-KERNEL_EBUSY) return 7;
    if(kernel_block_unregister(&children[0]) || kernel_block_unregister(&children[2])) return 8;
    if(registered_here && kernel_block_unregister(disk)) return 9;
    return 0;
}
#endif
