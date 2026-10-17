/* Public reads through actual inode mapping, cache and blockdev code. */
#include "block_fault.h"
#include <ext4.h>
#include <ext4_fs.h>
#include <ext4_errno.h>
#include <ext4_blockdev.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#define CHECK(x) do { if(!(x)){fprintf(stderr,"batch-read:%d: %s\n",__LINE__,#x);exit(1);} } while(0)
static struct fault_block disk;
static unsigned width, batches;
static uint64_t fail_sector=UINT64_MAX;
static uint64_t short_sector=UINT64_MAX;
static unsigned track_allocations, allocation_attempts;
static pthread_mutex_t hold_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t hold_changed=PTHREAD_COND_INITIALIZER;
static unsigned read_held, release_read;
static _Thread_local unsigned hold_current;
static struct block_read_span *held_spans;
static unsigned held_count;
static const struct block_read_span *active_reads;
static unsigned active_count;
static uint32_t active_completed, file_completed;
static const struct file_read_completion *file_completion;
static int suppress_progress;
static void observe_file_read(void *unused, unsigned index, const struct file_read_span *span)
{
    (void)unused;
    CHECK(!(file_completed & (UINT32_C(1) << index)));
    file_completed |= UINT32_C(1) << index;
    for (unsigned i = 0; i < active_count; i++) {
        uintptr_t a = (uintptr_t)span->buffer, b = (uintptr_t)active_reads[i].buffer;
        if (a < b ? b - a < span->size : a - b < active_reads[i].size)
            CHECK(active_completed & (UINT32_C(1) << i));
    }
    if (index == 7) CHECK(active_completed != (UINT32_C(1) << active_count) - 1);
}
void *__real_malloc(size_t);
void *__wrap_malloc(size_t size)
{
    if (track_allocations) {
        allocation_attempts++;
        if (track_allocations==2) return NULL;
    }
    return __real_malloc(size);
}
static int dev_open(struct ext4_blockdev *d){(void)d;return EOK;}
static int dev_read(struct ext4_blockdev *d,void *p,uint64_t b,uint32_t n)
{(void)d;return kernel_block_read_at(&disk.device,b*512,p,(size_t)n*512)==0?EOK:EIO;}
static int dev_write(struct ext4_blockdev *d,const void *p,uint64_t b,uint32_t n)
{(void)d;(void)p;(void)b;(void)n;return EROFS;}
static int dev_batch(struct ext4_blockdev *d,struct block_read_span *s,unsigned n, const struct block_read_completion *completion)
{
    batches++;if(n>width)width=n;int first=EOK;
    if(hold_current){
        CHECK(pthread_mutex_lock(&hold_lock)==0);
        held_spans=s;held_count=n;read_held=1;
        CHECK(pthread_cond_broadcast(&hold_changed)==0);
        while(!release_read)CHECK(pthread_cond_wait(&hold_changed,&hold_lock)==0);
        CHECK(pthread_mutex_unlock(&hold_lock)==0);
    }
    if(file_completion){active_reads=s;active_count=n;active_completed=0;}
    for(unsigned k=n;k>0;k--){unsigned i=k-1;s[i].status= s[i].offset/512<=fail_sector && fail_sector-s[i].offset/512<s[i].size/512 ? EIO : dev_read(d,s[i].buffer,s[i].offset/512,s[i].size/512);if(!s[i].status)s[i].completed=s[i].offset/512==short_sector?s[i].size-512:s[i].size;if(!first)first=s[i].status;if(file_completion)active_completed|=UINT32_C(1)<<i;if(completion && !suppress_progress)completion->complete(completion->context,i,&s[i]);}
    if(file_completion){active_reads=NULL;active_count=0;}
    return first;
}
static struct file_read_workspace read_workspace;
static int read_batch(const ext4_file *f,struct file_read_span *s,unsigned n)
{return ext4_fpread_batch(f,s,n,n>1?&read_workspace:NULL,UINT64_MAX, file_completion);}
struct held_read {
    const ext4_file *file;
    struct file_read_span spans[2];
    struct file_read_workspace workspace;
    unsigned char bytes[2][4096];
    int result;
};
static void *read_while_held(void *arg)
{
    struct held_read *read=arg;hold_current=1;
    read->result=ext4_fpread_batch(read->file,read->spans,2,&read->workspace,UINT64_MAX, NULL);
    return NULL;
}
static unsigned char expected(size_t off){return off<8*4096 || off>=12*4096 ? 1+off/4096:0;}
static void clear(struct ext4_blockdev *d){ext4_bcache_cleanup(d->bc);width=batches=0;}
struct prefix_observation { unsigned char (*bytes)[4096]; uint32_t seen; uint64_t offset; };
static void observe_prefix(void *context, unsigned index, const struct file_read_span *span)
{
    struct prefix_observation *observation = context;
    CHECK(index < 31 && !(observation->seen & (UINT32_C(1) << index)));
    CHECK(!span->error && span->completed == 4096);
    if (!observation->seen) CHECK(observation->bytes[31][0] == 0xcd);
    observation->seen |= UINT32_C(1) << index;
    for (unsigned j = 0; j < 4096; j++) CHECK(((unsigned char *)span->buffer)[j] == expected(observation->offset+j));
}
int main(int argc,char **argv)
{
    CHECK(argc==2 && fault_block_open(&disk,argv[1])==0);
    unsigned char physical[512];
    struct ext4_blockdev_iface iface={.open=dev_open,.close=dev_open,.bread=dev_read,.bread_batch=dev_batch,.bwrite=dev_write,
        .ph_bsize=512,.ph_bcnt=disk.device.capacity_bytes/512,.ph_bbuf=physical};
    struct ext4_blockdev dev={.bdif=&iface,.part_size=disk.device.capacity_bytes};
    CHECK(ext4_device_register(&dev,"batch")==EOK && ext4_mount("batch","/",true)==EOK);
    ext4_file file;CHECK(ext4_fopen(&file,"/data","r")==EOK);
    unsigned char out[8][4096];struct file_read_span spans[8];
    for(unsigned i=0;i<8;i++)spans[i]=(struct file_read_span){i*4096,out[i],4096,0,0};
    const struct file_read_completion completion={observe_file_read,NULL};
    file_completion=&completion;file_completed=0;
    clear(&dev);CHECK(read_batch(&file,spans,8)==EOK && file_completed==255);
    suppress_progress=1;file_completed=0;
    CHECK(read_batch(&file,spans,8)==EOK && !file_completed);
    suppress_progress=0;
    file_completion=NULL;
    printf("file batch blocksize=%u width=%u batches=%u\n",dev.lg_bsize,width,batches);
    CHECK(width==8);
    for(unsigned i=0;i<8;i++){CHECK(!spans[i].error && spans[i].completed==4096);for(unsigned j=0;j<4096;j++)CHECK(out[i][j]==expected(i*4096+j));}
    /* 部分块使规划跨过工作区容量：前窗完整输出先交付，最后一窗直接返回。 */
    unsigned char (*many_bytes)[4096]=malloc(32*4096);CHECK(many_bytes);
    memset(many_bytes,0xcd,32*4096);
    struct file_read_span many[32];
    struct prefix_observation observation={many_bytes,0,dev.lg_bsize-17};
    const struct file_read_completion prefix_completion={observe_prefix,&observation};
    for(unsigned i=0;i<32;i++)many[i]=(struct file_read_span){observation.offset,many_bytes[i],4096,0,0};
    file_completion=&prefix_completion;suppress_progress=1;
    CHECK(read_batch(&file,many,32)==EOK && observation.seen);
    file_completion=NULL;suppress_progress=0;
    for(unsigned i=0;i<32;i++){
        CHECK(!many[i].error && many[i].completed==4096);
        for(unsigned j=0;j<4096;j++)CHECK(many_bytes[i][j]==expected(observation.offset+j));
    }
    free(many_bytes);
    /* Metadata is already resident; request-control work must be reusable. */
    allocation_attempts=0;track_allocations=2;
    for(unsigned i=0;i<3;i++)CHECK(read_batch(&file,spans,8)==EOK);
    for(unsigned i=0;i<3;i++)CHECK(read_batch(&file,spans,1)==EOK);
    track_allocations=0;
    printf("steady multi-page read allocation attempts=%u\n",allocation_attempts);
    CHECK(allocation_attempts==0);
    CHECK(ext4_fpread_batch(&file,spans,8,NULL,UINT64_MAX, NULL)==ENOMEM);
    for(unsigned i=0;i<8;i++)CHECK(spans[i].error==ENOMEM && !spans[i].completed);
    struct file_read_workspace aliased_workspace;
    struct file_read_span overlap={0,&aliased_workspace,sizeof(aliased_workspace),0,0};
    CHECK(ext4_fpread_batch(&file,&overlap,1,&aliased_workspace,UINT64_MAX, NULL)==EINVAL);
    union {struct file_read_workspace workspace;struct file_read_span spans[2];} aliased;
    aliased.spans[0]=(struct file_read_span){0,out[0],4096,0,0};
    CHECK(ext4_fpread_batch(&file,aliased.spans,1,&aliased.workspace,UINT64_MAX, NULL)==EINVAL);
    memset(out,0xcd,sizeof(out));
    spans[0]=(struct file_read_span){4090,out[0],4096,0,0};
    spans[1]=(struct file_read_span){8192,out[1],4096,0,0};
    CHECK(ext4_fpread_batch(&file,spans,2,&read_workspace,4096, NULL)==EOK);
    CHECK(spans[0].size==4096 && spans[0].completed==6 && spans[1].size==4096 && !spans[1].completed);
    for(unsigned i=0;i<4096;i++)CHECK(out[0][i]==(i<6?1:0xcd) && out[1][i]==0xcd);
    for(unsigned i=0;i<8;i++)spans[i]=(struct file_read_span){i*4096,out[i],4096,0,0};
    CHECK(read_batch(&file,spans,8)==EOK);
    struct held_read held={.file=&file};
    memset(held.bytes,0xcd,sizeof(held.bytes));
    for(unsigned i=0;i<2;i++)held.spans[i]=(struct file_read_span){i*4096,held.bytes[i],4096,0,0};
    pthread_t reader;CHECK(pthread_create(&reader,NULL,read_while_held,&held)==0);
    CHECK(pthread_mutex_lock(&hold_lock)==0);
    while(!read_held)CHECK(pthread_cond_wait(&hold_changed,&hold_lock)==0);
    CHECK(held_count==2 && held_spans==held.workspace.reads);
    struct block_read_span submitted[2];memcpy(submitted,held_spans,sizeof(submitted));
    CHECK(pthread_mutex_unlock(&hold_lock)==0);
    /* The first request owns its mapped descriptors while device I/O waits;
     * a second complete read must neither borrow nor overwrite that storage. */
    CHECK(read_batch(&file,spans,8)==EOK && !memcmp(submitted,held_spans,sizeof(submitted)));
    for(unsigned i=0;i<sizeof(held.bytes);i++)CHECK(((unsigned char *)held.bytes)[i]==0xcd);
    CHECK(pthread_mutex_lock(&hold_lock)==0);release_read=1;
    CHECK(pthread_cond_broadcast(&hold_changed)==0 && pthread_mutex_unlock(&hold_lock)==0);
    CHECK(pthread_join(reader,NULL)==0 && held.result==EOK);
    for(unsigned i=0;i<2;i++){
        CHECK(!held.spans[i].error && held.spans[i].completed==4096);
        for(unsigned j=0;j<4096;j++)CHECK(held.bytes[i][j]==expected(i*4096+j));
    }
    puts("held request descriptors and independent concurrent workspace passed");
    spans[0]=(struct file_read_span){1,out[0],17,0,0};
    spans[1]=(struct file_read_span){4096,out[1],4096,0,0};
    allocation_attempts=0;track_allocations=2;
    CHECK(read_batch(&file,spans,2)==ENOMEM);track_allocations=0;
    CHECK(allocation_attempts==1 && spans[0].error==ENOMEM && !spans[0].completed);
    CHECK(!spans[1].error && spans[1].completed==4096 && out[1][0]==2);
    /* Mixed holes, EOF and unaligned fragments preserve independent results. */
    const uint64_t offsets[8]={32761,32768,40000,49150,65530,65536+123,4093,131072};
    clear(&dev);memset(out,0xcd,sizeof(out));
    for(unsigned i=0;i<8;i++)spans[i]=(struct file_read_span){offsets[i],out[i],4096,0,0};
    CHECK(read_batch(&file,spans,8)==EOK);
    for(unsigned i=0;i<8;i++){
        size_t n=offsets[i]>=65536+123?0:65536+123-offsets[i];if(n>4096)n=4096;
        CHECK(!spans[i].error && spans[i].completed==n);
        for(unsigned j=0;j<n;j++)CHECK(out[i][j]==expected(offsets[i]+j));
        for(unsigned j=n;j<4096;j++)CHECK(out[i][j]==0xcd);
    }
    /* One output buffer still batches the disk fragments on both sides of a
     * sparse range; output count does not limit device request width. */
    clear(&dev);memset(out,0xcd,sizeof(out));
    spans[0]=(struct file_read_span){7*4096,out,6*4096,0,0};
    CHECK(read_batch(&file,spans,1)==EOK);
    CHECK(!spans[0].error && spans[0].completed==6*4096);
    printf("scalar sparse read blocksize=%u width=%u batches=%u\n",dev.lg_bsize,width,batches);
    CHECK(width==2 && batches==1);
    for(unsigned i=0;i<6*4096;i++)CHECK(((unsigned char *)out)[i]==expected(7*4096+i));
    /* One physical failure leaves other spans valid, including a successful prefix. */
    clear(&dev);struct ext4_inode_ref ref;ext4_fsblk_t home,tail;
    CHECK(ext4_fs_get_inode_ref(dev.fs,file.inode,&ref)==EOK);
    CHECK(ext4_fs_get_inode_dblk_idx(&ref,1,&home,true)==EOK && home);
    CHECK(ext4_fs_get_inode_dblk_idx(&ref,12*4096/dev.lg_bsize,&tail,true)==EOK && tail);
    CHECK(ext4_fs_put_inode_ref(&ref)==EOK);
    short_sector=home*(dev.lg_bsize/512);
    spans[0]=(struct file_read_span){dev.lg_bsize,out[0],4096,0,0};
    spans[1]=(struct file_read_span){16384,out[1],4096,0,0};
    CHECK(read_batch(&file,spans,2)==EIO && spans[0].error==EIO && !spans[0].completed);
    CHECK(!spans[1].error && spans[1].completed==4096);
    short_sector=UINT64_MAX;
    width=batches=0;
    fail_sector=tail*(dev.lg_bsize/512);
    spans[0]=(struct file_read_span){7*4096,out,6*4096,0,0};
    CHECK(read_batch(&file,spans,1)==EIO && spans[0].error==EIO && spans[0].completed==5*4096);
    CHECK(width==2 && batches==1);
    for(unsigned i=0;i<5*4096;i++)CHECK(((unsigned char *)out)[i]==expected(7*4096+i));
    fail_sector=UINT64_MAX;CHECK(read_batch(&file,spans,1)==EOK && spans[0].completed==6*4096);
    clear(&dev);fail_sector=home*(dev.lg_bsize/512);
    spans[0]=(struct file_read_span){dev.lg_bsize-17,out[0],4096,0,0};
    spans[1]=(struct file_read_span){16384,out[1],4096,0,0};
    CHECK(read_batch(&file,spans,2)==EIO);
    CHECK(spans[0].error==EIO && spans[0].completed==17);
    CHECK(!spans[1].error && spans[1].completed==4096 && out[1][0]==5);
    fail_sector=UINT64_MAX;CHECK(read_batch(&file,spans,2)==EOK && spans[0].completed==4096);
    clear(&dev);spans[0]=(struct file_read_span){0,out[0],4096,0,0};
    spans[1]=(struct file_read_span){0,out[0]+1,17,0,0};
    CHECK(read_batch(&file,spans,2)==EINVAL && batches==0);
    spans[1]=(struct file_read_span){UINT64_MAX,out[1],17,0,0};
    CHECK(read_batch(&file,spans,2)==EINVAL && batches==0);
    CHECK(read_batch(&file,NULL,0)==EOK && batches==0);
    /* Cache's newer content wins over the disk even before checkpoint. */
    struct ext4_block cached={0};CHECK(ext4_block_get(&dev,&cached,home)==EOK);
    memset(cached.data,0xa5,dev.lg_bsize);cached.buf->journal_pending=1;
    spans[0]=(struct file_read_span){dev.lg_bsize,out[0],4096,0,0};
    CHECK(read_batch(&file,spans,1)==EOK && out[0][0]==0xa5);
    cached.buf->journal_pending=0;CHECK(ext4_block_set(&dev,&cached)==EOK);
    CHECK(ext4_fclose(&file)==EOK && ext4_umount("/")==EOK && ext4_device_unregister("batch")==EOK);
    fault_block_close(&disk);
    puts("file batch holes/EOF/fragments/current cache/OOM/short/error/retry passed");return 0;
}
