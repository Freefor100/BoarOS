/* Actual cache/blockdev sources, independent memory and device owners. */
#include <ext4_blockdev.h>
#include <ext4_errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"cache:%d: %s\n",__LINE__,#x); exit(1); } } while (0)
static unsigned live, allocations, fail_allocation, reads, writes, waits;
static int read_error, write_error, shared_read;
static struct ext4_block *loading_owner;
static struct ext4_block interleaved;
static int insert_during_write;
static struct ext4_blockdev *allocate_peer;
static struct ext4_block allocation_peer;
static unsigned parallel_load,load_entered,load_release,load_waiter;
void *ext4_user_malloc(size_t n)
{
    CHECK(!kernel_cpu_current()->raw_locks);
    if (__atomic_add_fetch(&allocations,1,__ATOMIC_RELAXED)==fail_allocation) return NULL;
    void *p=malloc(n); if(p)__atomic_add_fetch(&live,1,__ATOMIC_RELAXED);
    if (p && allocate_peer) {
        struct ext4_blockdev *peer=allocate_peer; allocate_peer=NULL;
        CHECK(ext4_block_get(peer,&allocation_peer,7)==EOK);
    }
    return p;
}
void *ext4_user_calloc(size_t n,size_t s) { CHECK(!kernel_cpu_current()->raw_locks); if (__atomic_add_fetch(&allocations,1,__ATOMIC_RELAXED)==fail_allocation) return NULL; void *p=calloc(n,s); if(p)__atomic_add_fetch(&live,1,__ATOMIC_RELAXED); return p; }
void *ext4_user_realloc(void *p,size_t n) { (void)p;(void)n;abort(); }
void ext4_user_free(void *p) { CHECK(!kernel_cpu_current()->raw_locks); if(p){CHECK(__atomic_fetch_sub(&live,1,__ATOMIC_RELAXED));free(p);} }
static int dev_read(struct ext4_blockdev *b,void *out,uint64_t block,uint32_t count)
{
    (void)b;__atomic_add_fetch(&reads,1,__ATOMIC_RELAXED);
    if (parallel_load && block==56) {
        __atomic_store_n(&load_entered,1,__ATOMIC_RELEASE);
        while (!__atomic_load_n(&load_release,__ATOMIC_ACQUIRE)) sched_yield();
    }
    if(read_error)return read_error;
    memset(out,(unsigned char)(block/8),count*512);return EOK;
}
static int dev_write(struct ext4_blockdev *b,const void *p,uint64_t block,uint32_t count)
{
    (void)p;(void)block;(void)count;writes++;
    if(insert_during_write){
        bool fresh;insert_during_write=0;interleaved.lb_id=1;
        CHECK(ext4_bcache_alloc(b->bc,&interleaved,&fresh)==EOK && fresh);
        memset(interleaved.data,1,4096);ext4_bcache_set_flag(interleaved.buf,BC_UPTODATE);
    }
    return write_error;
}
static int read_context(void) { return shared_read; }
static void wait_read(void *key)
{
    struct ext4_buf *buf=key;
    __atomic_add_fetch(&waits,1,__ATOMIC_RELAXED);
    if (parallel_load) {
        __atomic_store_n(&load_waiter,1,__ATOMIC_RELEASE);
        for (;;) {
            bool loading;
            { KERNEL_RAW_SCOPE(guard,&buf->bc->metadata);loading=buf->loading; }
            if (!loading) return;
            sched_yield();
        }
    }
    CHECK(loading_owner && loading_owner->buf==buf && buf->refctr==2);
    memset(buf->data,(unsigned char)buf->lba,4096);
    ext4_bcache_set_flag(buf,BC_UPTODATE);buf->loading=false;
}
static void init(struct ext4_bcache *cache,struct ext4_blockdev *dev,
                 struct ext4_blockdev_iface *iface,unsigned target)
{
    CHECK(!live);allocations=fail_allocation=reads=writes=waits=0;
    read_error=write_error=shared_read=insert_during_write=0;loading_owner=NULL;
    *iface=(struct ext4_blockdev_iface){.bread=dev_read,.bwrite=dev_write,
        .ph_bsize=512,.ph_bcnt=8192,.ph_refctr=1,.wait_read=wait_read,.read_context=read_context};
    *dev=(struct ext4_blockdev){.bdif=iface,.bc=cache,.lg_bsize=4096,
        .lg_bcnt=1024,.part_size=4096*1024,.cache_write_back=1};
    CHECK(ext4_bcache_init_dynamic(cache,target,4096)==EOK);cache->bdev=dev;
}
static void finish(struct ext4_bcache *cache)
{
    CHECK(!read_error && !write_error && !shared_read);
    ext4_bcache_cleanup(cache);CHECK(!cache->ref_blocks && !live);
    CHECK(ext4_bcache_fini_dynamic(cache)==EOK);
}
static void get(struct ext4_blockdev *dev,struct ext4_block *block,unsigned lba)
{
    CHECK(ext4_block_get(dev,block,lba)==EOK);
    for(unsigned i=0;i<4096;i++)CHECK(((unsigned char *)block->data)[i]==(unsigned char)lba);
}
static void warm(struct ext4_blockdev *dev,unsigned lba)
{ struct ext4_block b={0};get(dev,&b,lba);CHECK(ext4_bcache_free(dev->bc,&b)==EOK); }
static void cycle(unsigned target,unsigned pinned,unsigned hot,unsigned loops)
{
    struct ext4_bcache cache;struct ext4_blockdev dev;struct ext4_blockdev_iface iface;
    struct ext4_block held[8]={0};init(&cache,&dev,&iface,target);
    for(unsigned i=0;i<pinned;i++)get(&dev,&held[i],128+i);
    for(unsigned i=0;i<hot;i++)warm(&dev,i);
    unsigned before=reads;
    for(unsigned i=0;i<loops;i++)for(unsigned j=0;j<hot;j++)warm(&dev,j);
    printf("cache target=%u pinned=%u hot=%u accesses=%u extra_reads=%u resident_peak=%u\n",
           target,pinned,hot,hot*loops,reads-before,cache.max_ref_blocks);
    CHECK(reads==before);
    for(unsigned i=0;i<pinned;i++)CHECK(ext4_bcache_free(&cache,&held[i])==EOK);
    finish(&cache);
}
static void contracts(void)
{
    struct ext4_bcache cache;struct ext4_blockdev dev;struct ext4_blockdev_iface iface;
    struct ext4_block a={0},b={0};init(&cache,&dev,&iface,2);
    get(&dev,&a,0);ext4_bcache_set_flag(a.buf,BC_DIRTY);CHECK(ext4_bcache_free(&cache,&a)==EOK);
    warm(&dev,1);write_error=EIO;unsigned before=writes;
    /* 命中不为无关脏块写回失败背锅；真实miss仍保留错误owner。 */
    get(&dev,&b,1);CHECK(writes==before);CHECK(ext4_bcache_free(&cache,&b)==EOK);
    CHECK(ext4_block_get(&dev,&b,2)==EIO && cache.ref_blocks==2);
    write_error=0;get(&dev,&a,0);a.buf->journal_pending=1;CHECK(ext4_bcache_free(&cache,&a)==EOK);
    warm(&dev,1);CHECK(ext4_block_get(&dev,&b,2)==EBUSY);
    get(&dev,&a,0);a.buf->journal_pending=0;CHECK(ext4_bcache_free(&cache,&a)==EOK);
    finish(&cache);

    init(&cache,&dev,&iface,1);warm(&dev,0);before=allocations;
    fail_allocation=allocations+1;warm(&dev,0);CHECK(allocations==before);
    CHECK(ext4_block_get(&dev,&b,1)==ENOMEM && !live);fail_allocation=0;
    fail_allocation=allocations+2;CHECK(ext4_block_get(&dev,&b,1)==ENOMEM && !live);
    fail_allocation=0;warm(&dev,1);finish(&cache);

    init(&cache,&dev,&iface,1);CHECK(ext4_block_get_noread(&dev,&a,7)==EOK);
    a.buf->loading=true;loading_owner=&a;get(&dev,&b,7);
    CHECK(waits==1 && reads==0 && a.buf==b.buf && a.buf->refctr==2);
    CHECK(ext4_bcache_free(&cache,&b)==EOK && a.buf->refctr==1);
    a.buf->load_error=EIO;CHECK(ext4_block_get(&dev,&b,7)==EIO && a.buf->refctr==1);
    a.buf->load_error=0;CHECK(ext4_bcache_free(&cache,&a)==EOK);finish(&cache);

    init(&cache,&dev,&iface,1);read_error=EIO;
    CHECK(ext4_block_get(&dev,&b,3)==EIO && !live && !cache.ref_blocks);
    read_error=0;warm(&dev,3);iface.ph_refctr=0;CHECK(ext4_block_get(&dev,&b,3)==EIO);
    iface.ph_refctr=1;CHECK(ext4_block_get(&dev,&b,1024)==ENXIO);finish(&cache);

    init(&cache,&dev,&iface,1);get(&dev,&a,0);ext4_bcache_set_flag(a.buf,BC_DIRTY);
    CHECK(ext4_bcache_free(&cache,&a)==EOK);shared_read=1;warm(&dev,0);warm(&dev,1);
    CHECK(!writes && cache.ref_blocks==2);shared_read=0;finish(&cache);

    init(&cache,&dev,&iface,1);get(&dev,&a,0);ext4_bcache_set_flag(a.buf,BC_DIRTY);
    CHECK(ext4_bcache_free(&cache,&a)==EOK);insert_during_write=1;get(&dev,&b,1);
    CHECK(b.buf==interleaved.buf && b.buf->refctr==2 && cache.ref_blocks==1);
    CHECK(ext4_bcache_free(&cache,&b)==EOK);CHECK(ext4_bcache_free(&cache,&interleaved)==EOK);finish(&cache);
    puts("cache contracts: dirty/journal owner, loading, shared reads, OOM, errors and reclaim interleave balanced");
}
#ifndef EXT4_CACHED_READ_BATCH
static int cache_batch(struct ext4_blockdev *d, struct ext4_block *b, const uint64_t *lba, int *errors, unsigned n)
{
    int first=EOK;
    for(unsigned i=0;i<n;i++){errors[i]=ext4_block_get(d,&b[i],lba[i]);if(!first)first=errors[i];}
    return first;
}
#else
#define cache_batch ext4_block_get_batch
#endif
static unsigned batch_width, batch_calls;
static int batch_failure;
#ifdef EXT4_CACHED_READ_BATCH
static int dev_read_batch(struct ext4_blockdev *dev, struct block_read_span *s, unsigned n, const struct block_read_completion *completion)
{
    (void)completion;
    batch_calls++;if(n>batch_width)batch_width=n;
    int first=EOK;
    for(unsigned i=n;i>0;i--){unsigned j=i-1;
        s[j].status = batch_failure && j==2 ? EIO : dev_read(dev,s[j].buffer,s[j].offset/512,s[j].size/512);
        if(!s[j].status)s[j].completed=s[j].size;
        if(s[j].status)first=s[j].status;
    }
    return first;
}
#endif
static void batch_wait(void *key)
{
    /* 本批的新读已发布并完成，才允许等待外部批次，避免加载环。 */
    CHECK(batch_calls==1 && batch_width==7);
    wait_read(key);
}
static void batch_contracts(void)
{
    struct ext4_bcache cache;struct ext4_blockdev dev;struct ext4_blockdev_iface iface;
    struct ext4_block out[8]={0};uint64_t lba[8];int errors[8];
    init(&cache,&dev,&iface,16);batch_calls=batch_width=0;batch_failure=0;
#ifdef EXT4_CACHED_READ_BATCH
    iface.bread_batch=dev_read_batch;
#endif
    for(unsigned i=0;i<8;i++)lba[i]=i+16;
    CHECK(cache_batch(&dev,out,lba,errors,8)==EOK);
    printf("cached read batch width=%u calls=%u reads=%u\n",batch_width,batch_calls,reads);
    CHECK(batch_width==8 && batch_calls==1 && reads==8);
    for(unsigned i=0;i<8;i++){CHECK(errors[i]==EOK);CHECK(out[i].data[0]==lba[i]);CHECK(ext4_block_set(&dev,&out[i])==EOK);}
    /* journal_pending 的当前版本不能被磁盘旧内容替换。 */
    CHECK(ext4_block_get(&dev,&out[0],16)==EOK);memset(out[0].data,0xa5,4096);
    ext4_bcache_set_flag(out[0].buf,BC_DIRTY);out[0].buf->journal_pending=1;
    CHECK(cache_batch(&dev,out+1,lba,errors,1)==EOK && out[1].data[0]==0xa5 && reads==8);
    out[0].buf->journal_pending=0;CHECK(ext4_block_set(&dev,&out[1])==EOK);CHECK(ext4_block_set(&dev,&out[0])==EOK);
    finish(&cache);

    init(&cache,&dev,&iface,16);batch_calls=batch_width=0;batch_failure=0;
#ifdef EXT4_CACHED_READ_BATCH
    iface.bread_batch=dev_read_batch;
#endif
    struct ext4_block held={0};CHECK(ext4_block_get_noread(&dev,&held,16)==EOK);
    held.buf->loading=true;loading_owner=&held;iface.wait_read=batch_wait;
    CHECK(cache_batch(&dev,out,lba,errors,8)==EOK && waits==1 && reads==7);
    for(unsigned i=0;i<8;i++)CHECK(ext4_block_set(&dev,&out[i])==EOK);
    CHECK(ext4_block_set(&dev,&held)==EOK);finish(&cache);

    init(&cache,&dev,&iface,16);batch_calls=batch_width=0;batch_failure=1;
#ifdef EXT4_CACHED_READ_BATCH
    iface.bread_batch=dev_read_batch;
#endif
    CHECK(cache_batch(&dev,out,lba,errors,8)==EIO);
    for(unsigned i=0;i<8;i++)if(i==2)CHECK(errors[i]==EIO && !out[i].data);
        else CHECK(!errors[i] && ext4_block_set(&dev,&out[i])==EOK);
    batch_failure=0;get(&dev,&out[0],18);CHECK(ext4_block_set(&dev,&out[0])==EOK);
    finish(&cache);
    /* 无批量设备的回退仍经过缓存，每个失败块都可独立重试。 */
    init(&cache,&dev,&iface,16);
    lba[1]=lba[0];CHECK(cache_batch(&dev,out,lba,errors,8)==EOK && reads==7 && out[0].buf==out[1].buf);
    for(unsigned i=0;i<8;i++)CHECK(ext4_block_set(&dev,&out[i])==EOK);
    finish(&cache);
    for(unsigned i=0;i<8;i++)lba[i]=16+i;
    for(unsigned fail=1;fail<=16;fail++) {
        init(&cache,&dev,&iface,16);fail_allocation=fail;
#ifdef EXT4_CACHED_READ_BATCH
        iface.bread_batch=dev_read_batch;
#endif
        CHECK(cache_batch(&dev,out,lba,errors,8)==ENOMEM);
        for(unsigned i=0;i<8;i++)if(!errors[i])CHECK(ext4_block_set(&dev,&out[i])==EOK);
        fail_allocation=0;finish(&cache);
    }
    puts("cached read batches: dirty version, overlapping loads, duplicate blocks, per-block error and fallback balanced");
}
static void invalid_physical_batch(void)
{
    struct ext4_bcache cache; struct ext4_blockdev dev; struct ext4_blockdev_iface iface;
    unsigned char data[1024];
    init(&cache,&dev,&iface,2);
    struct block_read_span spans[2] = {
        {.offset=1,.size=512,.buffer=data,.completed=512},
        {.offset=512,.size=512,.buffer=data+512,.completed=512},
    };
    CHECK(ext4_blockdev_read_batch(&dev,spans,2, NULL)==EINVAL && !reads);
    for (unsigned i=0;i<2;i++) CHECK(spans[i].status==EINVAL && !spans[i].completed);
    finish(&cache);
}
static void competing_allocation(void)
{
    struct ext4_bcache cache;struct ext4_blockdev dev;struct ext4_blockdev_iface iface;
    struct ext4_block block={0};init(&cache,&dev,&iface,8);
    allocate_peer=&dev;
    CHECK(ext4_block_get(&dev,&block,7)==EOK);
    CHECK(block.buf==allocation_peer.buf && block.buf->refctr==2 && cache.ref_blocks==1 && reads==1);
    CHECK(ext4_block_set(&dev,&block)==EOK && ext4_block_set(&dev,&allocation_peer)==EOK);
    finish(&cache);
    puts("cache competing allocation: one indexed buffer, one load, both owners returned");
}
struct parallel_read { struct ext4_blockdev *dev;struct ext4_block block; };
static void *parallel_reader(void *arg)
{
    struct parallel_read *reader=arg;
    get(reader->dev,&reader->block,7);
    return NULL;
}
static void concurrent_load(void)
{
    struct ext4_bcache cache;struct ext4_blockdev dev;struct ext4_blockdev_iface iface;
    init(&cache,&dev,&iface,8);parallel_load=1;load_entered=load_release=load_waiter=0;
    struct parallel_read readers[2]={{.dev=&dev},{.dev=&dev}};
    pthread_t threads[2];CHECK(!pthread_create(&threads[0],NULL,parallel_reader,&readers[0]));
    while (!__atomic_load_n(&load_entered,__ATOMIC_ACQUIRE)) sched_yield();
    CHECK(!pthread_create(&threads[1],NULL,parallel_reader,&readers[1]));
    while (!__atomic_load_n(&load_waiter,__ATOMIC_ACQUIRE)) sched_yield();
    __atomic_store_n(&load_release,1,__ATOMIC_RELEASE);
    for (unsigned i=0;i<2;i++) CHECK(!pthread_join(threads[i],NULL));
    CHECK(reads==1 && readers[0].block.buf==readers[1].block.buf && readers[0].block.buf->refctr==2);
    for (unsigned i=0;i<2;i++) CHECK(ext4_block_set(&dev,&readers[i].block)==EOK);
    parallel_load=0;finish(&cache);
    puts("cache concurrent cold readers: one loader, stable bytes and balanced references");
}
int main(void) { competing_allocation();concurrent_load();cycle(8,0,7,100);cycle(8,0,8,100);cycle(8,8,1,100);cycle(64,8,1,100);contracts();batch_contracts();invalid_physical_batch();return 0; }
