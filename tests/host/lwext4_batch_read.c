/* Public reads through actual inode mapping, cache and blockdev code. */
#include "block_fault.h"
#include <ext4.h>
#include <ext4_fs.h>
#include <ext4_errno.h>
#include <ext4_blockdev.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if(!(x)){fprintf(stderr,"batch-read:%d: %s\n",__LINE__,#x);exit(1);} } while(0)
static struct fault_block disk;
static unsigned width, batches;
static uint64_t fail_sector=UINT64_MAX;
static int dev_open(struct ext4_blockdev *d){(void)d;return EOK;}
static int dev_read(struct ext4_blockdev *d,void *p,uint64_t b,uint32_t n)
{(void)d;return kernel_block_read_at(&disk.device,b*512,p,(size_t)n*512)==0?EOK:EIO;}
static int dev_write(struct ext4_blockdev *d,const void *p,uint64_t b,uint32_t n)
{(void)d;(void)p;(void)b;(void)n;return EROFS;}
static int dev_batch(struct ext4_blockdev *d,struct ext4_block_read_span *s,unsigned n)
{
    batches++;if(n>width)width=n;int first=EOK;
    for(unsigned k=n;k>0;k--){unsigned i=k-1;s[i].error= s[i].block<=fail_sector && fail_sector-s[i].block<s[i].count ? EIO : dev_read(d,s[i].data,s[i].block,s[i].count);if(!first)first=s[i].error;}
    return first;
}
#ifndef EXT4_FILE_READ_BATCH
struct ext4_file_read_span {uint64_t offset;void *buffer;size_t size,completed;int error;};
#endif
#if !defined(EXT4_FILE_READ_BATCH) || defined(TEST_SCALAR_FILE_READ)
static int read_batch(const ext4_file *f,struct ext4_file_read_span *s,unsigned n)
{int first=0;for(unsigned i=0;i<n;i++){s[i].error=ext4_fpread(f,s[i].offset,s[i].buffer,s[i].size,&s[i].completed);if(!first)first=s[i].error;}return first;}
#else
#define read_batch ext4_fpread_batch
#endif
static unsigned char expected(size_t off){return off<8*4096 || off>=12*4096 ? 1+off/4096:0;}
static void clear(struct ext4_blockdev *d){ext4_bcache_cleanup(d->bc);width=batches=0;}
int main(int argc,char **argv)
{
    CHECK(argc==2 && fault_block_open(&disk,argv[1])==0);
    unsigned char physical[512];
    struct ext4_blockdev_iface iface={.open=dev_open,.close=dev_open,.bread=dev_read,.bread_batch=dev_batch,.bwrite=dev_write,
        .ph_bsize=512,.ph_bcnt=disk.device.capacity_bytes/512,.ph_bbuf=physical};
    struct ext4_blockdev dev={.bdif=&iface,.part_size=disk.device.capacity_bytes};
    CHECK(ext4_device_register(&dev,"batch")==EOK && ext4_mount("batch","/",true)==EOK);
    ext4_file file;CHECK(ext4_fopen(&file,"/data","r")==EOK);
    unsigned char out[8][4096];struct ext4_file_read_span spans[8];
    for(unsigned i=0;i<8;i++)spans[i]=(struct ext4_file_read_span){i*4096,out[i],4096,0,0};
    clear(&dev);CHECK(read_batch(&file,spans,8)==EOK);
    printf("file batch blocksize=%u width=%u batches=%u\n",dev.lg_bsize,width,batches);
    CHECK(width== (dev.lg_bsize>4096 ? 32768/dev.lg_bsize:8));
    for(unsigned i=0;i<8;i++){CHECK(!spans[i].error && spans[i].completed==4096);for(unsigned j=0;j<4096;j++)CHECK(out[i][j]==expected(i*4096+j));}
    /* Mixed holes, EOF and unaligned fragments preserve independent results. */
    const uint64_t offsets[8]={32761,32768,40000,49150,65530,65536+123,4093,131072};
    clear(&dev);memset(out,0xcd,sizeof(out));
    for(unsigned i=0;i<8;i++)spans[i]=(struct ext4_file_read_span){offsets[i],out[i],4096,0,0};
    CHECK(read_batch(&file,spans,8)==EOK);
    for(unsigned i=0;i<8;i++){
        size_t n=offsets[i]>=65536+123?0:65536+123-offsets[i];if(n>4096)n=4096;
        CHECK(!spans[i].error && spans[i].completed==n);
        for(unsigned j=0;j<n;j++)CHECK(out[i][j]==expected(offsets[i]+j));
        for(unsigned j=n;j<4096;j++)CHECK(out[i][j]==0xcd);
    }
    /* One physical failure leaves other spans valid, including a successful prefix. */
    clear(&dev);struct ext4_inode_ref ref;ext4_fsblk_t home;
    CHECK(ext4_fs_get_inode_ref(dev.fs,file.inode,&ref)==EOK);
    CHECK(ext4_fs_get_inode_dblk_idx(&ref,1,&home,true)==EOK && home);
    CHECK(ext4_fs_put_inode_ref(&ref)==EOK);fail_sector=home*(dev.lg_bsize/512);
    spans[0]=(struct ext4_file_read_span){dev.lg_bsize-17,out[0],4096,0,0};
    spans[1]=(struct ext4_file_read_span){16384,out[1],4096,0,0};
    CHECK(read_batch(&file,spans,2)==EIO);
    CHECK(spans[0].error==EIO && spans[0].completed==17);
    CHECK(!spans[1].error && spans[1].completed==4096 && out[1][0]==5);
    fail_sector=UINT64_MAX;CHECK(read_batch(&file,spans,2)==EOK && spans[0].completed==4096);
    clear(&dev);spans[0]=(struct ext4_file_read_span){0,out[0],4096,0,0};
    spans[1]=(struct ext4_file_read_span){0,out[0]+1,17,0,0};
    CHECK(read_batch(&file,spans,2)==EINVAL && batches==0);
    spans[1]=(struct ext4_file_read_span){UINT64_MAX,out[1],17,0,0};
    CHECK(read_batch(&file,spans,2)==EINVAL && batches==0);
    CHECK(read_batch(&file,NULL,0)==EOK && batches==0);
    /* Cache's newer content wins over the disk even before checkpoint. */
    struct ext4_block held={0};CHECK(ext4_block_get(&dev,&held,home)==EOK);
    memset(held.data,0xa5,dev.lg_bsize);held.buf->journal_pending=1;
    spans[0]=(struct ext4_file_read_span){dev.lg_bsize,out[0],4096,0,0};
    CHECK(read_batch(&file,spans,1)==EOK && out[0][0]==0xa5);
    held.buf->journal_pending=0;CHECK(ext4_block_set(&dev,&held)==EOK);
    CHECK(ext4_fclose(&file)==EOK && ext4_umount("/")==EOK && ext4_device_unregister("batch")==EOK);
    fault_block_close(&disk);
    puts("file batch holes/EOF/fragments/current cache/independent error/retry passed");return 0;
}
