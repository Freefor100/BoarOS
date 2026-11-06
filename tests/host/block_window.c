#include <kernel/block.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
static unsigned char bytes[512*1024],disk[512*1024];
static unsigned calls,fail_call;
static struct kernel_block_device *active_device;
static enum kernel_block_status read_at(void *context,uint64_t offset,void *buffer,size_t size)
{ (void)context;memcpy(buffer,disk+offset,size);return KERNEL_BLOCK_STATUS_OK; }
static void *read_during_phase(void *unused)
{
    (void)unused;unsigned char result[512];
    assert(!kernel_block_read_at(active_device,0,result,sizeof(result)));
    assert(!memcmp(result,bytes,sizeof(result)));return NULL;
}
static int next(const void *context,const void **cursor,struct kernel_block_span *span)
{
    (void)context;if (!*cursor)return 0;*span=*(const struct kernel_block_span *)*cursor;*cursor=0;return 1;
}
static enum kernel_block_status write_phase(void *context,kernel_block_phase_next_fn produce,
    const void *owner,const void *first)
{
    (void)context;const void *cursor=first;struct kernel_block_span span;size_t total=0;
    while (produce(owner,&cursor,&span))total+=span.size;
    assert(total==sizeof(bytes));
    ++calls;
    /* Revisit exactly the same immutable descriptors before consuming payload. */
    cursor=first;size_t repeated=0;
    while (produce(owner,&cursor,&span)) {
        assert(span.offset<=sizeof(disk) && span.size<=sizeof(disk)-span.offset);
        size_t amount = calls==fail_call ? 128*1024 : span.size;
        memcpy(disk+span.offset,span.buffer,amount);repeated+=amount;
        if (calls==fail_call) return KERNEL_BLOCK_STATUS_IO;
    }
    assert(repeated==total);
    if(calls==1) {
        pthread_t reader;assert(!pthread_create(&reader,NULL,read_during_phase,NULL));
        assert(!pthread_join(reader,NULL));
    }
    return KERNEL_BLOCK_STATUS_OK;
}
int main(void)
{
    struct kernel_block_device device={.read=read_at,.write_phase=write_phase,.logical_block_size=512,.capacity_bytes=sizeof(disk)};
    active_device=&device;
    for (unsigned i=0;i<sizeof(bytes);i++)bytes[i]=(unsigned char)(i/512+i*17);
    struct kernel_block_span span={0,bytes,sizeof(bytes)};
    assert(!kernel_block_write_phase(&device,next,0,&span));
    assert(calls==1 && !memcmp(bytes,disk,sizeof(bytes)) && !device.references);
    memset(disk,0,sizeof(disk));calls=0;fail_call=1;
    assert(kernel_block_write_phase(&device,next,0,&span)==KERNEL_BLOCK_STATUS_IO);
    assert(calls==1 && !device.references && !memcmp(bytes,disk,128*1024));
    for (size_t i=128*1024;i<sizeof(disk);i++)assert(!disk[i]);
    calls=0;
    struct kernel_block_device child={.parent=&device,.start_bytes=1,
        .logical_block_size=512,.capacity_bytes=512};
    struct kernel_block_span unaligned={0,bytes,512};
    assert(kernel_block_write_phase(&child,next,0,&unaligned)==KERNEL_BLOCK_STATUS_INVALID && !calls);
    child.start_bytes=0;device.logical_block_size=1024;
    assert(kernel_block_write_phase(&child,next,0,&unaligned)==KERNEL_BLOCK_STATUS_INVALID && !calls);
    device.logical_block_size=512;
    span.size+=512;
    assert(kernel_block_write_phase(&device,next,0,&span)==KERNEL_BLOCK_STATUS_OUT_OF_RANGE && !calls);
    span.size=0;
    assert(kernel_block_write_phase(&device,next,0,&span)==KERNEL_BLOCK_STATUS_INVALID && !calls);
    puts("PASS block phase: continuous producer submission, immutable producer restart, preflight, error prefix and request owners");
}
