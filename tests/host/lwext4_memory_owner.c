/* Real budget/pool allocation and payload references, without journal replay. */
#include <ext4_fs.h>
#include <ext4_journal.h>
#include <ext4_errno.h>
#include <kernel/cpu.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned live, calls;
static bool fail_calloc, fail_malloc, block_calloc;
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed=PTHREAD_COND_INITIALIZER;
static unsigned gate_state;

void *ext4_user_malloc(size_t bytes)
{
    assert(!kernel_cpu_current()->raw_locks);
    __atomic_fetch_add(&calls,1,__ATOMIC_RELAXED);
    if(fail_malloc){fail_malloc=false;return NULL;}
    void *p=malloc(bytes);if(p)__atomic_fetch_add(&live,1,__ATOMIC_RELAXED);return p;
}
void *ext4_user_calloc(size_t count,size_t bytes)
{
    assert(!kernel_cpu_current()->raw_locks);
    __atomic_fetch_add(&calls,1,__ATOMIC_RELAXED);
    if(fail_calloc){fail_calloc=false;return NULL;}
    if(block_calloc) {
        assert(!pthread_mutex_lock(&gate));block_calloc=false;gate_state=1;
        assert(!pthread_cond_broadcast(&changed));
        while(gate_state!=2)assert(!pthread_cond_wait(&changed,&gate));
        assert(!pthread_mutex_unlock(&gate));
    }
    void *p=calloc(count,bytes);if(p)__atomic_fetch_add(&live,1,__ATOMIC_RELAXED);return p;
}
void *ext4_user_realloc(void *pointer,size_t bytes) { (void)pointer;(void)bytes;abort(); }
void ext4_user_free(void *pointer)
{
    assert(!kernel_cpu_current()->raw_locks);
    if(pointer)assert(__atomic_fetch_sub(&live,1,__ATOMIC_RELAXED));
    free(pointer);
}
void ext4_operation_memory_denied(struct ext4_fs *fs) { (void)fs;abort(); }
int jbd_trans_borrow_private(struct jbd_trans *trans,struct ext4_block *block,
    ext4_fsblk_t lba,uint32_t offset,uint32_t length,enum ext4_block_access access,bool *found)
{ (void)trans;(void)block;(void)lba;(void)offset;(void)length;(void)access;(void)found;abort(); }
int jbd_trans_get_write_access_range(struct jbd_trans *trans,struct ext4_block *block,
    uint32_t offset,uint32_t length)
{ (void)trans;(void)block;(void)offset;(void)length;abort(); }

static void start(struct jbd_journal *journal,unsigned size,size_t limit)
{
    memset(journal,0,sizeof(*journal));journal->block_size=size;
    assert(jbd_memory_start(journal,size)==EOK);
    jbd_memory_set_limit(journal,limit);
}
static void payload_outlives_record_and_stop(unsigned size)
{
    struct jbd_journal *journal=malloc(sizeof(*journal));assert(journal);
    start(journal,size,0);size_t charge=jbd_image_charge(journal);
    jbd_memory_set_limit(journal,charge);jbd_memory_set_grouped(journal,true);
    struct jbd_memory *owner=journal->memory;jbd_memory_get(owner);
    struct jbd_image *image=jbd_image_alloc(journal);assert(image);
    memset(image->data,0x69,size);ext4_block_payload_get(&image->payload);
    struct ext4_block_payload *reader=&image->payload;
    jbd_image_free(image); /* Retire the record; the real reader is still live. */
    assert(jbd_memory_used(journal)==charge && !jbd_memory_cached(journal));
    assert(!jbd_image_alloc(journal) && jbd_memory_budget_failures(journal)==1);
    struct jbd_memory_stats stats;jbd_memory_snapshot(owner,&stats);
    assert(stats.used==charge && !stats.reserved && stats.peak==charge);
    jbd_memory_stop(journal);free(journal);
    jbd_memory_snapshot(owner,&stats);assert(stats.used==charge && !stats.cached && !stats.reserved);
    for(unsigned i=0;i<size;i++)assert(reader->data[i]==0x69);
    ext4_block_payload_put(reader);jbd_memory_snapshot(owner,&stats);
    assert(!stats.used && !stats.cached && !stats.reserved);jbd_memory_put(owner);assert(!live);
}
static void pools_and_failed_allocations(unsigned size)
{
    struct jbd_journal journal;start(&journal,size,1024*1024);jbd_memory_set_grouped(&journal,true);
    struct jbd_image *image=jbd_image_alloc(&journal);assert(image);unsigned before=calls;
    jbd_image_free(image);assert(jbd_memory_cached(&journal)==jbd_image_charge(&journal));
    assert(jbd_memory_cached_images(&journal)==1);
    struct jbd_image *reused=jbd_image_alloc(&journal);assert(reused==image && calls==before);
    jbd_image_free(reused);jbd_trim_cache(&journal,0);assert(!jbd_memory_used(&journal));
    for(size_t bytes=0;bytes<=384;bytes=bytes?bytes*2:1) {
        void *object=jbd_alloc(&journal,bytes);assert(object);jbd_free(object);
    }
    jbd_trim_cache(&journal,0);assert(!jbd_memory_used(&journal));
    jbd_memory_set_cache_disabled(&journal,true);
    fail_calloc=true;assert(!jbd_image_alloc(&journal));
    fail_malloc=true;assert(!jbd_image_alloc(&journal));
    struct jbd_memory_stats stats;jbd_memory_snapshot(journal.memory,&stats);
    assert(!stats.used && !stats.reserved && !stats.cached);
    jbd_memory_stop(&journal);assert(!live);
}
static void transaction_controls_reuse(unsigned size)
{
    struct jbd_journal journal;start(&journal,size,1024*1024);jbd_memory_set_grouped(&journal,true);
    size_t charge=jbd_allocation_charge(sizeof(struct jbd_trans));
    struct jbd_trans *trans=jbd_alloc(&journal,sizeof(*trans));assert(trans);
    unsigned warmed=calls;
    for(unsigned i=0;i<16;i++) {
        memset(trans,0xa7,sizeof(*trans));jbd_free(trans);
        trans=jbd_alloc(&journal,sizeof(*trans));assert(trans && calls==warmed);
        for(size_t j=0;j<sizeof(*trans);j++)assert(!((unsigned char *)trans)[j]);
    }
    jbd_free(trans);
    struct jbd_memory_stats stats;jbd_memory_snapshot(journal.memory,&stats);
    assert(stats.used==charge && stats.cached==charge && !stats.reserved);
    jbd_trim_cache(&journal,0);jbd_memory_snapshot(journal.memory,&stats);
    assert(!stats.used && !stats.cached && !stats.reserved);
    trans=jbd_alloc(&journal,sizeof(*trans));assert(trans);jbd_free(trans);
    jbd_memory_stop(&journal);assert(!live);
}
struct allocating_task { struct jbd_journal *journal;struct jbd_image *image; };
static void *allocate_one(void *argument)
{
    struct allocating_task *task=argument;task->image=jbd_image_alloc(task->journal);return NULL;
}
static void concurrent_reservation(unsigned size)
{
    struct jbd_journal journal;start(&journal,size,0);
    size_t charge=jbd_image_charge(&journal);jbd_memory_set_limit(&journal,charge);
    struct allocating_task task={.journal=&journal};pthread_t worker;
    gate_state=0;block_calloc=true;assert(!pthread_create(&worker,NULL,allocate_one,&task));
    assert(!pthread_mutex_lock(&gate));
    while(gate_state!=1)assert(!pthread_cond_wait(&changed,&gate));
    assert(!pthread_mutex_unlock(&gate));
    struct jbd_memory_stats stats;jbd_memory_snapshot(journal.memory,&stats);
    assert(!stats.used && stats.reserved==charge);
    assert(!jbd_image_alloc(&journal));
    jbd_memory_snapshot(journal.memory,&stats);assert(!stats.used && stats.reserved==charge);
    assert(!pthread_mutex_lock(&gate));gate_state=2;assert(!pthread_cond_broadcast(&changed));
    assert(!pthread_mutex_unlock(&gate));assert(!pthread_join(worker,NULL) && task.image);
    jbd_memory_snapshot(journal.memory,&stats);
    assert(stats.used==charge && !stats.reserved && stats.peak==charge && stats.budget_failures==1);
    jbd_image_free(task.image);jbd_memory_stop(&journal);assert(!live);
}
struct external_account { size_t active; unsigned references; };
static void account_retain(void *context) { ((struct external_account *)context)->references++; }
static void account_release(void *context)
{ struct external_account *a=context;assert(a->references>1);a->references--; }
static void account_change(void *context,size_t bytes,bool acquiring)
{
    struct external_account *a=context;
    if (acquiring) {assert(a->active<=SIZE_MAX-bytes);a->active+=bytes;}
    else {assert(a->active>=bytes);a->active-=bytes;}
}
static void borrowed_source_outlives_stop(unsigned size)
{
    struct jbd_journal journal;start(&journal,size,1024*1024);
    struct external_account account={.references=1};
    struct jbd_memory_account methods={&account,account_retain,account_release,account_change};
    jbd_memory_set_account(journal.memory,&methods);
    jbd_memory_set_grouped(&journal,true);
    struct jbd_image *image=jbd_image_alloc(&journal);assert(image);
    size_t image_bytes=jbd_image_charge(&journal);assert(account.active==image_bytes);
    jbd_image_free(image);assert(!account.active && jbd_memory_cached(&journal)==image_bytes);
    struct jbd_image *reused=jbd_image_alloc(&journal);assert(reused==image && account.active==image_bytes);
    struct ext4_block_payload *reader=&reused->payload;ext4_block_payload_get(reader);
    jbd_image_free(reused);assert(account.active==image_bytes && !jbd_memory_cached(&journal));
    ext4_block_payload_put(reader);assert(!account.active && jbd_memory_cached(&journal)==image_bytes);
    jbd_trim_cache(&journal,0);assert(!account.active && !jbd_memory_used(&journal));
    struct jbd_memory *owner=journal.memory;jbd_memory_get(owner);
    void *control=jbd_alloc(&journal,32),*source=ext4_user_malloc(size);assert(control && source);
    /* The external payload already has an allocation owner. Journal retention
     * counts against its quota but must not charge that allocation twice. */
    account_change(&account,size,true);size_t held=account.active;
    assert(jbd_memory_borrow(&journal,size));
    assert(account.active==held && account.references==2);
    jbd_memory_stop(&journal);struct jbd_memory_stats stats;jbd_memory_snapshot(owner,&stats);
    assert(stats.used==held && stats.borrowed==size && held>size && account.active==held);
    ext4_user_free(source);account_change(&account,size,false);jbd_memory_release_borrow(owner,size);
    assert(account.active==held-size);
    jbd_free(control);jbd_memory_snapshot(owner,&stats);
    assert(!stats.used && !stats.reserved && !stats.borrowed && !account.active);
    jbd_memory_put(owner);assert(!live && account.references==1);
}
struct pooled_task { struct jbd_journal *journal;pthread_barrier_t *start; };
static void meet(pthread_barrier_t *barrier)
{ int r=pthread_barrier_wait(barrier);assert(!r || r==PTHREAD_BARRIER_SERIAL_THREAD); }
static void *concurrent_pools_worker(void *argument)
{
    struct pooled_task *task=argument;meet(task->start);
    for(unsigned i=0;i<128;i++) {
        void *control=jbd_alloc(task->journal,16*(i%6+1));assert(control);
        struct jbd_image *image=jbd_image_alloc(task->journal);assert(image);
        memset(image->data,(unsigned char)i,task->journal->block_size);
        ext4_block_payload_get(&image->payload);jbd_image_free(image);
        for(unsigned j=0;j<task->journal->block_size;j++)assert(((unsigned char *)image->data)[j]==(unsigned char)i);
        ext4_block_payload_put(&image->payload);jbd_free(control);
    }
    return NULL;
}
static void concurrent_pools(unsigned size)
{
    struct jbd_journal journal;start(&journal,size,512*1024);jbd_memory_set_grouped(&journal,true);
    pthread_t workers[8];pthread_barrier_t barrier;assert(!pthread_barrier_init(&barrier,NULL,9));
    struct pooled_task task={&journal,&barrier};
    for(unsigned i=0;i<8;i++)assert(!pthread_create(&workers[i],NULL,concurrent_pools_worker,&task));
    meet(&barrier);for(unsigned i=0;i<8;i++)assert(!pthread_join(workers[i],NULL));
    assert(!pthread_barrier_destroy(&barrier));
    struct jbd_memory_stats stats;jbd_memory_snapshot(journal.memory,&stats);
    assert(!stats.reserved && stats.used==stats.cached && stats.peak<=stats.limit && !stats.budget_failures);
    jbd_trim_cache(&journal,0);assert(!jbd_memory_used(&journal));jbd_memory_stop(&journal);assert(!live);
}
struct home_task { struct jbd_journal *journal;struct ext4_buf *buf;pthread_barrier_t *barrier; };
static void *pin_same_home(void *argument)
{
    struct home_task *task=argument;assert(jbd_memory_pin_home(task->journal,task->buf));
    meet(task->barrier);meet(task->barrier);jbd_memory_unpin_home(task->journal,task->buf);return NULL;
}
static int read_home(struct ext4_blockdev *dev,void *bytes,uint64_t lba,uint32_t count)
{ (void)dev;(void)lba;memset(bytes,0x37,count*512);return EOK; }
static int write_home(struct ext4_blockdev *dev,const void *bytes,uint64_t lba,uint32_t count)
{ (void)dev;(void)bytes;(void)lba;(void)count;return EOK; }
static void shared_home_charge(unsigned size)
{
    struct jbd_journal journal;start(&journal,size,1024*1024);
    struct ext4_bcache cache;assert(ext4_bcache_init_dynamic(&cache,2,size)==EOK);
    struct ext4_blockdev_iface iface={.bread=read_home,.bwrite=write_home,.ph_bsize=512,.ph_refctr=1};
    struct ext4_blockdev dev={.bdif=&iface,.lg_bsize=size,.lg_bcnt=8,.part_size=8*size,.cache_write_back=1};
    assert(ext4_block_bind_bcache(&dev,&cache)==EOK);
    struct ext4_block block=EXT4_BLOCK_ZERO();assert(ext4_block_get(&dev,&block,1)==EOK);
    size_t header=16;while(header<sizeof(struct ext4_buf))header*=2;
    jbd_memory_set_limit(&journal,size+header);jbd_memory_set_grouped(&journal,true);
    struct jbd_image *cached=jbd_image_alloc(&journal);assert(cached);jbd_image_free(cached);
    assert(jbd_memory_cached(&journal));
    /* A first home pin must reclaim its image pool only after dropping both
     * short locks; allocator wrappers check that this actually happens. */
    assert(jbd_memory_pin_home(&journal,block.buf));
    assert(!jbd_memory_cached(&journal) && jbd_memory_used(&journal)==size+header);
    jbd_memory_unpin_home(&journal,block.buf);
    pthread_barrier_t barrier;assert(!pthread_barrier_init(&barrier,NULL,3));
    struct home_task task={&journal,block.buf,&barrier};pthread_t a,b;
    assert(!pthread_create(&a,NULL,pin_same_home,&task) && !pthread_create(&b,NULL,pin_same_home,&task));
    meet(&barrier);
    assert(block.buf->journal_pending==2 && jbd_memory_used(&journal)==size+header);
    meet(&barrier);assert(!pthread_join(a,NULL) && !pthread_join(b,NULL));
    assert(!block.buf->journal_pending && !jbd_memory_used(&journal));
    assert(!pthread_barrier_destroy(&barrier));
    assert(ext4_block_set(&dev,&block)==EOK);ext4_bcache_cleanup(&cache);
    assert(ext4_bcache_fini_dynamic(&cache)==EOK);jbd_memory_stop(&journal);assert(!live);
}
int main(int argc,char **argv)
{
    assert(argc==2);unsigned size=(unsigned)strtoul(argv[1],NULL,10);assert(size==1024 || size==4096);
    payload_outlives_record_and_stop(size);pools_and_failed_allocations(size);transaction_controls_reuse(size);
    concurrent_reservation(size);borrowed_source_outlives_stop(size);
    concurrent_pools(size);shared_home_charge(size);
    printf("PASS journal budget payload/stop/pool/transaction-control/OOM/concurrent-reservation/source/shared-home block=%u\n",size);
    return 0;
}
