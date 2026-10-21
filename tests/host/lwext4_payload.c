/* Cache descriptors own immutable payloads across publication and device I/O. */
#include <ext4_bcache.h>
#include <ext4_blockdev.h>
#include <ext4_fs.h>
#include <ext4_journal.h>
#include <ext4_errno.h>
#include <kernel/cpu.h>
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned allocations, releases, read_calls;
static uint8_t disk[16*4096];
static bool fail_read;
static struct ext4_buf *publish_during_write;
static struct ext4_block_payload *replacement;
static pthread_barrier_t publication_barrier;

void *ext4_user_malloc(size_t n) { assert(!kernel_cpu_current()->raw_locks);void *p=malloc(n);if(p)allocations++;return p; }
void *ext4_user_calloc(size_t n,size_t s) { assert(!kernel_cpu_current()->raw_locks);void *p=calloc(n,s);if(p)allocations++;return p; }
void *ext4_user_realloc(void *p,size_t n) { assert(!kernel_cpu_current()->raw_locks);void *next=realloc(p,n);if(next&&!p)allocations++;return next; }
void ext4_user_free(void *p) { assert(!kernel_cpu_current()->raw_locks);if(p){assert(allocations);allocations--;}free(p); }

int jbd_trans_borrow_private(struct jbd_trans *trans,struct ext4_block *block,
    ext4_fsblk_t lba,uint32_t offset,uint32_t length,enum ext4_block_access access,bool *found)
{ (void)trans;(void)block;(void)lba;(void)offset;(void)length;(void)access;(void)found;abort(); }
int jbd_trans_get_write_access_range(struct jbd_trans *trans,struct ext4_block *block,
    uint32_t offset,uint32_t length)
{
    (void)trans;(void)block;(void)offset;(void)length;abort();
}
static void release_payload(struct ext4_block_payload *payload)
{
    assert(!kernel_cpu_current()->raw_locks);
    releases++;free(payload->data);free(payload);
}
static struct ext4_block_payload *image(unsigned char byte,unsigned size)
{
    struct ext4_block_payload *payload=malloc(sizeof(*payload));assert(payload);
    uint8_t *data=malloc(size);assert(data);memset(data,byte,size);
    ext4_block_payload_init(payload,data,release_payload);return payload;
}
static int read_device(struct ext4_blockdev *dev,void *bytes,uint64_t lba,uint32_t count)
{
    (void)dev;read_calls++;
    if(fail_read)return EIO;
    assert(lba*512+(uint64_t)count*512<=sizeof(disk));
    memcpy(bytes,disk+lba*512,(size_t)count*512);return EOK;
}
static int write_device(struct ext4_blockdev *dev,const void *bytes,uint64_t lba,uint32_t count)
{
    (void)dev;assert(lba*512+(uint64_t)count*512<=sizeof(disk));
    if(publish_during_write) {
        struct ext4_buf *buf=publish_during_write;publish_during_write=NULL;
        assert(((const unsigned char *)bytes)[0]==0x61);
        ext4_bcache_publish(buf,replacement);
        assert(((const unsigned char *)bytes)[0]==0x61);
    }
    memcpy(disk+lba*512,bytes,(size_t)count*512);return EOK;
}
static void check_bytes(const uint8_t *bytes,unsigned size,unsigned char byte)
{ for(unsigned i=0;i<size;i++)assert(bytes[i]==byte); }

static void publication(struct ext4_blockdev *dev)
{
    unsigned size=dev->lg_bsize,start=releases;
    struct ext4_block old=EXT4_BLOCK_ZERO(),second=EXT4_BLOCK_ZERO(),latest=EXT4_BLOCK_ZERO(),borrowed=EXT4_BLOCK_ZERO();
    assert(ext4_block_get(dev,&old,1)==EOK);check_bytes(old.data,size,0x31);
    struct ext4_block_payload *new=image(0x42,size);
    ext4_bcache_publish(old.buf,new);ext4_block_payload_put(new);
    assert(ext4_block_get(dev,&second,1)==EOK);
    assert(old.payload!=second.payload);check_bytes(old.data,size,0x31);check_bytes(second.data,size,0x42);
    new=image(0x53,size);ext4_bcache_publish(old.buf,new);ext4_block_payload_put(new);
    assert(ext4_block_get(dev,&latest,1)==EOK);check_bytes(latest.data,size,0x53);
    struct ext4_block_payload *private=image(0x84,size);
    uint32_t refs=second.buf->refctr,accepted_refs=latest.payload->references;
    ext4_bcache_prepare_private(&second,&borrowed,private);
    assert(second.buf->refctr==refs+1 && latest.payload->references==accepted_refs+1);
    assert(private->references==2 && releases==start+1);
    ext4_block_payload_put(private);
    assert(borrowed.buf==second.buf && borrowed.payload==latest.payload);
    check_bytes(old.data,size,0x31);check_bytes(second.data,size,0x84);check_bytes(borrowed.data,size,0x53);
    ext4_block_rebind(&old,latest.payload);check_bytes(old.data,size,0x53);
    assert(ext4_block_set(dev,&second)==EOK && releases==start+2);
    assert(ext4_block_set(dev,&latest)==EOK && ext4_block_set(dev,&old)==EOK);
    assert(releases==start+2 && borrowed.buf->refctr==1);
    /* The accepted descriptor owns the final buffer pin independently of
     * the older descriptor used to obtain it, including during reclamation. */
    for (unsigned lba=2;lba<4;lba++) {
        struct ext4_block pressure=EXT4_BLOCK_ZERO();
        assert(ext4_block_get(dev,&pressure,lba)==EOK && ext4_block_set(dev,&pressure)==EOK);
    }
    assert(ext4_bcache_shake(dev->bc)==EOK);check_bytes(borrowed.data,size,0x53);
    assert(releases==start+2 && ext4_block_set(dev,&borrowed)==EOK);
    ext4_bcache_cleanup(dev->bc);assert(releases==start+3 && !allocations);
}
static void current_private_loan(struct ext4_blockdev *dev)
{
    unsigned size=dev->lg_bsize,start=releases;
    struct ext4_block client=EXT4_BLOCK_ZERO(),reader=EXT4_BLOCK_ZERO(),accepted=EXT4_BLOCK_ZERO();
    assert(ext4_block_get(dev,&client,1)==EOK && ext4_block_get(dev,&reader,1)==EOK);
    struct ext4_block_payload *original=client.payload,*private=image(0x64,size);
    uint32_t refs=client.buf->refctr,payload_refs=original->references;
    ext4_bcache_prepare_private(&client,&accepted,private);
    assert(client.buf==accepted.buf && client.buf->refctr==refs+1);
    assert(accepted.payload==original && original->references==payload_refs && private->references==2);
    assert(client.payload==private && client.buf->current_payload==original);
    check_bytes(client.data,size,0x64);check_bytes(accepted.data,size,0x31);check_bytes(reader.data,size,0x31);
    ext4_block_payload_put(private);
    assert(ext4_block_set(dev,&client)==EOK && releases==start+1);
    assert(ext4_block_set(dev,&reader)==EOK && accepted.buf->refctr==1);
    for(unsigned lba=2;lba<4;lba++) {
        struct ext4_block pressure=EXT4_BLOCK_ZERO();
        assert(ext4_block_get(dev,&pressure,lba)==EOK && ext4_block_set(dev,&pressure)==EOK);
    }
    assert(ext4_bcache_shake(dev->bc)==EOK);check_bytes(accepted.data,size,0x31);
    assert(ext4_block_set(dev,&accepted)==EOK);
    ext4_bcache_cleanup(dev->bc);assert(releases==start+1 && !allocations);
}
static void publication_meet(void)
{ int r=pthread_barrier_wait(&publication_barrier);assert(!r || r==PTHREAD_BARRIER_SERIAL_THREAD); }
static void *read_old_publication(void *argument)
{
    struct ext4_blockdev *dev=argument;struct ext4_block held=EXT4_BLOCK_ZERO();
    assert(ext4_block_get(dev,&held,12)==EOK);publication_meet();publication_meet();
    check_bytes(held.data,dev->lg_bsize,0x31);
    assert(ext4_block_set(dev,&held)==EOK);return NULL;
}
static void parallel_publication(struct ext4_blockdev *dev)
{
    unsigned start=releases;struct ext4_block held=EXT4_BLOCK_ZERO();
    assert(ext4_block_get(dev,&held,12)==EOK && ext4_block_set(dev,&held)==EOK);
    assert(!pthread_barrier_init(&publication_barrier,NULL,2));
    pthread_t reader;assert(!pthread_create(&reader,NULL,read_old_publication,dev));publication_meet();
    assert(ext4_block_get(dev,&held,12)==EOK);
    struct ext4_block_payload *new=image(0xa6,dev->lg_bsize);
    ext4_bcache_publish(held.buf,new);ext4_block_payload_put(new);
    struct ext4_block accepted=EXT4_BLOCK_ZERO();new=image(0xb8,dev->lg_bsize);
    ext4_bcache_prepare_private(&held,&accepted,new);ext4_block_payload_put(new);
    check_bytes(held.data,dev->lg_bsize,0xb8);check_bytes(accepted.data,dev->lg_bsize,0xa6);
    assert(ext4_block_set(dev,&held)==EOK && ext4_block_set(dev,&accepted)==EOK);
    assert(ext4_block_get(dev,&held,12)==EOK);
    check_bytes(held.data,dev->lg_bsize,0xa6);assert(ext4_block_set(dev,&held)==EOK);
    publication_meet();assert(!pthread_join(reader,NULL) && !pthread_barrier_destroy(&publication_barrier));
    ext4_bcache_cleanup(dev->bc);assert(releases==start+2 && !allocations);
}
static void loaders(struct ext4_blockdev *dev)
{
    struct ext4_block blocks[3];int errors[3];uint64_t ids[]={2,2,4};
    unsigned before=read_calls;
    assert(ext4_block_get_batch(dev,blocks,ids,errors,3)==EOK && read_calls==before+2);
    assert(blocks[0].payload==blocks[1].payload);
    for(unsigned i=0;i<3;i++){check_bytes(blocks[i].data,dev->lg_bsize,0x31);assert(ext4_block_set(dev,&blocks[i])==EOK);assert(!blocks[i].payload);}
    fail_read=true;
    struct ext4_block failed=EXT4_BLOCK_ZERO();
    assert(ext4_block_get(dev,&failed,7)==EIO && !failed.payload && !failed.data);
    ids[0]=8;ids[1]=9;
    assert(ext4_block_get_batch(dev,blocks,ids,errors,2)==EIO);
    for(unsigned i=0;i<2;i++)assert(errors[i]==EIO && !blocks[i].payload && !blocks[i].data);
    fail_read=false;ext4_bcache_cleanup(dev->bc);assert(!allocations);
}
static void flush_snapshot(struct ext4_blockdev *dev)
{
    unsigned size=dev->lg_bsize,start=releases;
    struct ext4_block block=EXT4_BLOCK_ZERO();assert(ext4_block_get(dev,&block,5)==EOK);
    struct ext4_block_payload *old=image(0x61,size);replacement=image(0x72,size);
    ext4_bcache_publish(block.buf,old);ext4_block_payload_put(old);
    ext4_bcache_set_dirty(block.buf);publish_during_write=block.buf;
    assert(ext4_block_flush_buf(dev,block.buf)==EOK && !publish_during_write);
    check_bytes(disk+5*size,size,0x61);check_bytes(block.buf->data,size,0x72);
    assert(releases==start+1 && ext4_bcache_test_flag(block.buf,BC_DIRTY));
    ext4_block_payload_put(replacement);replacement=NULL;
    assert(ext4_block_flush_buf(dev,block.buf)==EOK);
    check_bytes(disk+5*size,size,0x72);assert(!ext4_bcache_test_flag(block.buf,BC_DIRTY));
    assert(ext4_block_set(dev,&block)==EOK);ext4_bcache_cleanup(dev->bc);
    assert(releases==start+2 && !allocations);
}
static void scope_and_eviction(struct ext4_blockdev *dev)
{
    unsigned start=releases;struct ext4_writeback_scope scope;
    ext4_bcache_scope_begin(dev->bc,&scope);
    struct ext4_block block=EXT4_BLOCK_ZERO();assert(ext4_block_get(dev,&block,10)==EOK);
    ext4_bcache_set_dirty(block.buf);
    struct ext4_block_payload *new=image(0xb7,dev->lg_bsize);
    ext4_bcache_publish(block.buf,new);ext4_block_payload_put(new);
    assert(ext4_block_set(dev,&block)==EOK && ext4_bcache_scope_end(dev->bc,&scope)==EOK);
    ext4_bcache_cleanup(dev->bc);assert(releases==start+1 && !allocations);
    assert(ext4_block_get(dev,&block,10)==EOK);
    new=image(0xc8,dev->lg_bsize);ext4_bcache_publish(block.buf,new);ext4_block_payload_put(new);
    assert(ext4_block_set(dev,&block)==EOK);
    assert(ext4_block_get(dev,&block,11)==EOK && ext4_block_set(dev,&block)==EOK);
    assert(ext4_bcache_shake(dev->bc)==EOK && releases==start+2);
    ext4_bcache_cleanup(dev->bc);assert(!allocations);
}
static void owned_payload_loan(struct ext4_blockdev *dev)
{
    unsigned size=dev->lg_bsize,start=releases;
    struct ext4_block owner=EXT4_BLOCK_ZERO(),loan=EXT4_BLOCK_ZERO();
    /* The private view already supplies initialized bytes; noread owners
     * keep the buffer alive without promising a loaded accepted payload. */
    assert(ext4_block_get_noread(dev,&owner,2)==EOK);
    struct ext4_block_payload *private=image(0x64,size);
    uint32_t baseline_refs=owner.payload->references;
    assert(!ext4_bcache_borrow_owned(&owner,private,true,&loan));
    assert(owner.buf->refctr==2 && private->references==2 && owner.payload->references==baseline_refs);
    check_bytes(loan.data,size,0x64);
    assert(ext4_block_set(dev,&loan)==EOK && owner.buf->refctr==1 && private->references==1);
    struct ext4_block_payload *current=image(0x75,size);
    ext4_bcache_publish(owner.buf,current);ext4_block_payload_put(current);
    struct ext4_block_payload *snapshot=ext4_bcache_borrow_owned(&owner,private,true,&loan);
    assert(snapshot==current && snapshot->references==2 && owner.buf->refctr==2);
    current=image(0x86,size);ext4_bcache_publish(owner.buf,current);ext4_block_payload_put(current);
    assert(ext4_block_set(dev,&owner)==EOK);
    ext4_block_payload_put(private);
    assert(ext4_bcache_shake(dev->bc)==EOK);
    check_bytes(snapshot->data,size,0x75);check_bytes(loan.data,size,0x64);
    ext4_block_payload_put(snapshot);
    /* The final noread loan also drops the uninitialized home buffer and
     * its current payload; the snapshot and private view were independent. */
    assert(releases==start+1 && ext4_block_set(dev,&loan)==EOK && releases==start+3);
    ext4_bcache_cleanup(dev->bc);assert(releases==start+3 && !allocations);
}
static void invalidation_keeps_readers(struct ext4_blockdev *dev)
{
    unsigned size=dev->lg_bsize,start=releases;
    struct ext4_block old=EXT4_BLOCK_ZERO(),fresh=EXT4_BLOCK_ZERO();
    assert(ext4_block_get(dev,&old,13)==EOK);
    struct ext4_block_payload *accepted=image(0xd9,size);
    ext4_bcache_publish(old.buf,accepted);ext4_block_payload_put(accepted);
    assert(ext4_block_set(dev,&old)==EOK && ext4_block_get(dev,&old,13)==EOK);
    ext4_bcache_set_dirty(old.buf);
    ext4_bcache_invalidate_lba(dev->bc,13,1);
    assert(ext4_block_get(dev,&fresh,13)==EOK && fresh.buf!=old.buf);
    check_bytes(old.data,size,0xd9);check_bytes(fresh.data,size,0x31);
    assert(ext4_bcache_test_flag(old.buf,BC_UPTODATE));
    assert(!ext4_bcache_test_flag(old.buf,BC_DIRTY));
    assert(ext4_block_set(dev,&fresh)==EOK && releases==start);
    assert(ext4_block_set(dev,&old)==EOK && releases==start+1);
    /* Unreferenced buffers are retired immediately, without an extra owner. */
    assert(ext4_block_get(dev,&fresh,14)==EOK && ext4_block_set(dev,&fresh)==EOK);
    assert(ext4_block_get(dev,&fresh,15)==EOK && ext4_block_set(dev,&fresh)==EOK);
    ext4_bcache_invalidate_lba(dev->bc,13,3);
    assert(!dev->bc->ref_blocks && !dev->bc->idle_blocks && !allocations);
    check_bytes(disk+13*size,size,0x31);
}
int main(int argc,char **argv)
{
    assert(argc==2);unsigned size=(unsigned)strtoul(argv[1],NULL,10);assert(size==1024 || size==4096);
    memset(disk,0x31,sizeof(disk));
    struct ext4_blockdev_iface iface={.bread=read_device,.bwrite=write_device,.ph_bsize=512,.ph_refctr=1};
    struct ext4_blockdev dev={.bdif=&iface,.lg_bsize=size,.lg_bcnt=sizeof(disk)/size,.part_size=sizeof(disk),.cache_write_back=1};
    struct ext4_bcache cache;assert(ext4_bcache_init_dynamic(&cache,2,size)==EOK);
    assert(ext4_block_bind_bcache(&dev,&cache)==EOK);
    publication(&dev);current_private_loan(&dev);parallel_publication(&dev);loaders(&dev);
    flush_snapshot(&dev);owned_payload_loan(&dev);scope_and_eviction(&dev);invalidation_keeps_readers(&dev);
    assert(ext4_bcache_fini_dynamic(&cache)==EOK);
    printf("PASS cache payload publication/read/batch/error/flush/private-view/scope/eviction block=%u releases=%u\n",size,releases);
    return 0;
}
