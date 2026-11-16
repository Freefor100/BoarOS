#ifndef BOAROS_TEST_BLOCK_PARTITIONS_H
#define BOAROS_TEST_BLOCK_PARTITIONS_H
#include <kernel/block.h>
#include <kernel/errno.h>

/* The vectors and DMA targets stay outside the task stack on both test
 * architectures. Each call keeps this workspace until the real I/O returns. */
static struct {
    unsigned char input[KERNEL_BLOCK_BATCH_MAX][512];
    unsigned char output[KERNEL_BLOCK_BATCH_MAX][512];
    struct kernel_block_span writes[KERNEL_BLOCK_BATCH_MAX];
    struct block_read_span reads[KERNEL_BLOCK_BATCH_MAX];
    size_t count;
    uint32_t notified;
    unsigned failed;
} partition_batch_workspace;
static void test_partition_completed(void *context,unsigned index,const struct block_read_span *span)
{
    (void)context;
    if(index>=partition_batch_workspace.count) {partition_batch_workspace.failed=1;return;}
    uint32_t bit=UINT32_C(1)<<index;
    if((partition_batch_workspace.notified&bit) || span!=&partition_batch_workspace.reads[index] ||
       span->offset!=index*512U || span->buffer!=partition_batch_workspace.output[index] ||
       span->size!=512 || span->completed!=512 || span->status!=KERNEL_BLOCK_STATUS_OK)
        partition_batch_workspace.failed=1;
    partition_batch_workspace.notified|=bit;
}
static int test_partition_batches(struct kernel_block_device *partition)
{
    const unsigned counts[]={4,5,KERNEL_BLOCK_BATCH_MAX};
    for(unsigned test=0;test<sizeof(counts)/sizeof(counts[0]);test++) {
        unsigned count=counts[test];partition_batch_workspace.count=count;
        partition_batch_workspace.notified=partition_batch_workspace.failed=0;
        for(unsigned i=0;i<count;i++) {
            for(unsigned j=0;j<512;j++) {
                partition_batch_workspace.input[i][j]=(unsigned char)(j*7+19+i*13+count);
                partition_batch_workspace.output[i][j]=0;
            }
            partition_batch_workspace.writes[i]=(struct kernel_block_span){i*512U,partition_batch_workspace.input[i],512};
            partition_batch_workspace.reads[i]=(struct block_read_span){.offset=i*512U,
                .buffer=partition_batch_workspace.output[i],.size=512};
        }
        if(kernel_block_write_batch(partition,partition_batch_workspace.writes,count) || kernel_block_flush(partition))return 1;
        const struct block_read_completion complete={test_partition_completed,NULL};
        if(kernel_block_read_batch(partition,partition_batch_workspace.reads,count,&complete) || partition_batch_workspace.failed)return 2;
        for(unsigned i=0;i<count;i++) {
            if(partition_batch_workspace.writes[i].offset!=i*512U ||
               partition_batch_workspace.reads[i].offset!=i*512U ||
               partition_batch_workspace.reads[i].status!=KERNEL_BLOCK_STATUS_OK ||
               partition_batch_workspace.reads[i].completed!=512)return 3;
            for(unsigned j=0;j<512;j++)
                if(partition_batch_workspace.output[i][j]!=partition_batch_workspace.input[i][j])return 4;
        }
    }
    return 0;
}
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
    int batch_error=test_partition_batches(&children[2]);
    if(batch_error)return 10+batch_error;
    if(kernel_block_read_at(&children[0],32*512,sector,1)!=KERNEL_BLOCK_STATUS_OUT_OF_RANGE ||
        kernel_block_unregister(disk)!=-KERNEL_EBUSY) return 7;
    if(kernel_block_unregister(&children[0]) || kernel_block_unregister(&children[2])) return 8;
    if(registered_here && kernel_block_unregister(disk)) return 9;
    return 0;
}
#endif
