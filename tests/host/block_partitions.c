#include <kernel/block.h>
#include <kernel/errno.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>

extern _Thread_local uint64_t block_test_read_bytes, block_test_write_bytes;

static unsigned char disk[128 * 512];
static unsigned reads, writes, flushes, batches, phases, notifications;
static uint64_t fail_offset = UINT64_MAX;
static pthread_mutex_t handshake = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static unsigned pause_read, read_paused, writer_waiting, resume_read;
static struct kernel_rwlock *expected_lock;
static _Thread_local unsigned peer;
void block_model_before_lock(struct kernel_rwlock *lock)
{
    if (!peer) return;
    assert(lock == expected_lock);
    assert(!pthread_mutex_lock(&handshake));
    writer_waiting = 1; pthread_cond_broadcast(&changed);
    assert(!pthread_mutex_unlock(&handshake));
}
static void pause_in_read(void)
{
    assert(!pthread_mutex_lock(&handshake));
    if (pause_read) {
        pause_read = 0; read_paused = 1; pthread_cond_broadcast(&changed);
        while (!resume_read) assert(!pthread_cond_wait(&changed, &handshake));
    }
    assert(!pthread_mutex_unlock(&handshake));
}
static enum kernel_block_status rd(void *c, uint64_t o, void *b, size_t n)
{ (void)c; assert(o + n <= sizeof(disk)); if (o == fail_offset) return KERNEL_BLOCK_STATUS_IO; memcpy(b, disk + o, n); reads++; pause_in_read(); return 0; }
static enum kernel_block_status wr(void *c, uint64_t o, const void *b, size_t n)
{ (void)c; assert(o + n <= sizeof(disk)); if (o == fail_offset) return KERNEL_BLOCK_STATUS_IO; memcpy(disk + o, b, n); writes++; return 0; }
static enum kernel_block_status flush(void *c) { (void)c; flushes++; return 0; }
static void le32(unsigned char *p, unsigned n)
{ for(unsigned i=0;i<4;i++) p[i]=(unsigned char)(n>>(i*8)); }
static void entry(unsigned slot, unsigned start, unsigned size)
{ unsigned char *p=disk+446+16*slot; p[4]=0x83; le32(p+8,start); le32(p+12,size); }
static enum kernel_block_status batch_write(void *c, const struct kernel_block_span *spans, size_t count)
{
    batches++;
    for (size_t i=0;i<count;i++) {
        enum kernel_block_status status=wr(c,spans[i].offset,spans[i].buffer,spans[i].size);
        if(status) return status;
    }
    return 0;
}
static enum kernel_block_status batch_read(void *c, struct block_read_span *spans, size_t count,
    const struct block_read_completion *completion)
{
    batches++;
    enum kernel_block_status result=0;
    for(size_t i=0;i<count;i++) {
        spans[i].status=rd(c,spans[i].offset,spans[i].buffer,spans[i].size);
        spans[i].completed=spans[i].status ? 0 : spans[i].size;
        if(spans[i].status && !result) result=spans[i].status;
        if(completion && i+1<count) completion->complete(completion->context,i,&spans[i]);
    }
    return result;
}
static void completed_read(void *context, unsigned i, const struct block_read_span *span)
{
    struct block_read_span *original=context;
    assert(span==&original[i] && span->offset==i*512);
    assert(span->status==0 && span->completed==512); notifications++;
}
static int produce(const void *context, const void **cursor, struct kernel_block_span *span)
{
    const struct kernel_block_span *current=*cursor, *end=context;
    if(current==end) return 0;
    *span=*current; *cursor=current+1; return 1;
}
static enum kernel_block_status phase_write(void *c, kernel_block_phase_next_fn next,
    const void *context, const void *first)
{
    phases++;
    /* Drivers may restart traversal: translation must not mutate producer nodes. */
    for(unsigned pass=0;pass<2;pass++) {
        const void *cursor=first; struct kernel_block_span span;
        while(next(context,&cursor,&span)) {
            assert(span.offset>=8*512);
            if(pass) { enum kernel_block_status status=wr(c,span.offset,span.buffer,span.size); if(status) return status; }
        }
    }
    return 0;
}
static void io_batches(struct kernel_block_device *d, struct kernel_block_device *p)
{
    unsigned char data[1024], out[1024]; memset(data,0x71,sizeof(data));
    struct kernel_block_span spans[2]={{0,data,512},{512,data+512,512}};
    struct block_read_span reads[2]={{.offset=0,.buffer=out,.size=512},{.offset=512,.buffer=out+512,.size=512}};
    struct block_read_completion complete={completed_read,reads};
    d->write_batch=batch_write; d->read_batch=batch_read; d->write_phase=phase_write;
    assert(!kernel_block_write_batch(p,spans,2) && batches==1);
    assert(!memcmp(disk+8*512,data,sizeof(data)) && spans[0].offset==0);
    fail_offset=9*512;
    uint64_t submitted = block_test_read_bytes;
    assert(kernel_block_read_batch(p,reads,2,&complete)==KERNEL_BLOCK_STATUS_IO);
    assert(block_test_read_bytes-submitted==1024);
    assert(batches==2 && notifications==1 && reads[0].completed==512 && reads[1].completed==0 && reads[1].status==KERNEL_BLOCK_STATUS_IO);
    fail_offset=UINT64_MAX;
    assert(!kernel_block_write_phase(p,produce,spans+2,spans) && phases==1);
    spans[1].offset=p->capacity_bytes;
    assert(kernel_block_write_phase(p,produce,spans+2,spans)==KERNEL_BLOCK_STATUS_OUT_OF_RANGE && phases==1);
    assert(kernel_block_write_batch(p,spans,2)==KERNEL_BLOCK_STATUS_OUT_OF_RANGE && batches==2);
    d->write_batch=0; d->read_batch=0; d->write_phase=0;
    spans[1].offset=512;
    assert(!kernel_block_write_phase(p,produce,spans+2,spans));
    fail_offset=9*512;
    unsigned char scratch[512]; size_t done;
    uint64_t previous_read=block_test_read_bytes,previous_write=block_test_write_bytes;
    assert(kernel_block_write_bytes(p,3,data,700,scratch,sizeof(scratch),&done)==KERNEL_BLOCK_STATUS_IO && done==509);
    assert(block_test_read_bytes-previous_read==1024 && block_test_write_bytes-previous_write==512);
    fail_offset=UINT64_MAX;
}
static void *rmw_thread(void *arg)
{
    unsigned char data=0x23,scratch[512]; size_t done;
    assert(!kernel_block_write_bytes(arg,1,&data,1,scratch,512,&done) && done==1); return 0;
}
static void *whole_thread(void *arg)
{
    peer=1; unsigned char data[512]; memset(data,0x42,sizeof(data));
    assert(!kernel_block_write_at(arg,8*512,data,sizeof(data))); return 0;
}
static void serialized_io(struct kernel_block_device *d, struct kernel_block_device *p)
{
    pause_read=1; expected_lock=&d->io_lock.lock;
    pthread_t rmw,whole; assert(!pthread_create(&rmw,0,rmw_thread,p));
    assert(!pthread_mutex_lock(&handshake));
    while(!read_paused) assert(!pthread_cond_wait(&changed,&handshake));
    assert(!pthread_mutex_unlock(&handshake));
    assert(kernel_block_unregister(p)==-KERNEL_EBUSY);
    assert(kernel_block_unregister(d)==-KERNEL_EBUSY);
    assert(!pthread_create(&whole,0,whole_thread,d));
    assert(!pthread_mutex_lock(&handshake));
    while(!writer_waiting) assert(!pthread_cond_wait(&changed,&handshake));
    resume_read=1; pthread_cond_broadcast(&changed); assert(!pthread_mutex_unlock(&handshake));
    assert(!pthread_join(rmw,0) && !pthread_join(whole,0));
    for(unsigned i=0;i<512;i++) assert(disk[8*512+i]==0x42);
    assert(p->references==0 && d->references==2);
}
static void invalid_tables(void)
{
    struct kernel_block_device d={.read=rd,.write=wr,.logical_block_size=512,.capacity_bytes=sizeof(disk)},p[4]={0};
    assert(!kernel_block_register(&d,0xfc20));
    memset(disk,0,sizeof(disk)); assert(!kernel_block_scan_mbr(&d,p) && !d.references);
    disk[510]=0x55; disk[511]=0xaa;
    disk[446]=0x7f;
    assert(kernel_block_scan_mbr(&d,p)==-KERNEL_EINVAL && !d.references && !kernel_block_lookup(0xfc21));
    disk[446]=0;
    entry(0,8,16); entry(1,16,16);
    assert(kernel_block_scan_mbr(&d,p)==-KERNEL_EINVAL && !d.references && !kernel_block_lookup(0xfc21));
    entry(1,120,16); assert(kernel_block_scan_mbr(&d,p)==-KERNEL_EINVAL && !d.references);
    entry(1,UINT32_MAX,UINT32_MAX); assert(kernel_block_scan_mbr(&d,p)==-KERNEL_EINVAL && !d.references);
    entry(1,32,0); assert(kernel_block_scan_mbr(&d,p)==-KERNEL_EINVAL);
    entry(1,0,16); assert(kernel_block_scan_mbr(&d,p)==-KERNEL_EINVAL);
    entry(1,32,16); disk[446+16+4]=0x0f; assert(kernel_block_scan_mbr(&d,p)==-KERNEL_ENOTSUP);
    entry(1,32,16);
    struct kernel_block_device collision={.read=rd,.logical_block_size=512};
    assert(!kernel_block_register(&collision,0xfc22));
    assert(kernel_block_scan_mbr(&d,p)==-KERNEL_EBUSY && !d.references && !kernel_block_lookup(0xfc21));
    assert(!kernel_block_unregister(&collision));
    assert(!kernel_block_scan_mbr(&d,p));
    assert(p[0].capacity_bytes==8192 && p[1].start_bytes==16384);
    assert(!kernel_block_unregister(&p[0]) && !kernel_block_unregister(&p[1]));
    assert(!kernel_block_unregister(&d));
}
int main(void)
{
    struct kernel_block_device d={.read=rd,.write=wr,.flush=flush,
        .logical_block_size=512,.capacity_bytes=sizeof(disk)}, p[4]={0};
    assert(!kernel_block_register(&d,KERNEL_BLOCK_DEVICE_NUMBER(1)));
    disk[510]=0x55; disk[511]=0xaa; entry(0,8,16); entry(2,32,32);
    assert(!kernel_block_scan_mbr(&d,p));
    assert(kernel_block_lookup(0xfc11)==&p[0]);
    assert(kernel_block_lookup(0xfc13)==&p[2]);
    assert(!kernel_block_lookup(0xfc12));
    assert(d.references==2 && p[0].capacity_bytes==8192);
    int a,b;
    assert(!kernel_block_claim(&p[0],&a));
    assert(!kernel_block_open(&p[2],&b,0200));
    assert(kernel_block_open(&d,&b,0200)==-KERNEL_EBUSY);
    assert(kernel_block_claim(&d,&b)==-KERNEL_EBUSY);
    kernel_block_close(&p[2],&b,0200); kernel_block_release_claim(&p[0],&a);
    assert(!kernel_block_claim(&d,&a));
    assert(kernel_block_open(&p[2],&b,0200)==-KERNEL_EBUSY);
    kernel_block_release_claim(&d,&a);
    unsigned char data[1024], scratch[512]; memset(data,0x6a,sizeof(data));
    assert(!kernel_block_write_at(&p[0],512,data,512));
    assert(!memcmp(disk+9*512,data,512) && disk[8*512]==0);
    assert(kernel_block_write_at(&p[0],8192,data,1)==KERNEL_BLOCK_STATUS_OUT_OF_RANGE);
    size_t completed;
    assert(!kernel_block_write_bytes(&p[0],3,data,700,scratch,sizeof(scratch),&completed));
    assert(completed==700 && disk[8*512+2]==0 && disk[8*512+3]==0x6a && disk[8*512+703]==0x6a);
    io_batches(&d,&p[0]);
    serialized_io(&d,&p[0]);
    assert(!kernel_block_set_read_only(&p[0],1));
    assert(kernel_block_read_only(&p[0]) && !kernel_block_read_only(&d) && !kernel_block_read_only(&p[2]));
    assert(!kernel_block_set_read_only(&p[0],0));
    d.write=0; assert(kernel_block_read_only(&p[0])); d.write=wr;
    assert(kernel_block_read_at(&p[0],UINT64_MAX,data,1)==KERNEL_BLOCK_STATUS_OUT_OF_RANGE);
    assert(!kernel_block_set_read_only(&d,1));
    assert(kernel_block_read_only(&p[0]));
    assert(kernel_block_write_at(&p[0],0,data,1)==KERNEL_BLOCK_STATUS_READ_ONLY);
    assert(!kernel_block_set_read_only(&p[0],0) && kernel_block_read_only(&p[0]));
    assert(!kernel_block_flush(&p[0]) && flushes==1);
    assert(kernel_block_unregister(&d)==-KERNEL_EBUSY);
    assert(!kernel_block_unregister(&p[0]) && !kernel_block_unregister(&p[2]));
    assert(!d.references && !kernel_block_unregister(&d));
    invalid_tables();
    puts("block partitions: discovery, translated I/O, claims, read-only and lifetime passed");
}
