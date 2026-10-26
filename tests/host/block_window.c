#include <kernel/block.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned char bytes[512*1024],disk[512*1024];
static unsigned calls,fail_call;
static int next(const void *context,const void **cursor,struct kernel_block_span *span)
{
    (void)context;if (!*cursor)return 0;*span=*(const struct kernel_block_span *)*cursor;*cursor=0;return 1;
}
static enum kernel_block_status write_phase(void *context,kernel_block_phase_next_fn produce,
    const void *owner,const void *first)
{
    (void)context;const void *cursor=first;struct kernel_block_span span;size_t total=0;
    while (produce(owner,&cursor,&span))total+=span.size;
    assert(total && total<=128*1024);
    if (++calls==fail_call)return KERNEL_BLOCK_STATUS_IO;
    /* Revisit exactly the same immutable descriptors before consuming payload. */
    cursor=first;size_t repeated=0;
    while (produce(owner,&cursor,&span)) {
        assert(span.offset<=sizeof(disk) && span.size<=sizeof(disk)-span.offset);
        memcpy(disk+span.offset,span.buffer,span.size);repeated+=span.size;
    }
    assert(repeated==total);return KERNEL_BLOCK_STATUS_OK;
}
int main(void)
{
    struct kernel_block_device device={.write_phase=write_phase,.logical_block_size=512,.capacity_bytes=sizeof(disk)};
    for (unsigned i=0;i<sizeof(bytes);i++)bytes[i]=(unsigned char)(i/512+i*17);
    struct kernel_block_span span={0,bytes,sizeof(bytes)};
    assert(!kernel_block_write_phase(&device,next,0,&span));
    assert(calls==4 && !memcmp(bytes,disk,sizeof(bytes)) && !device.references);
    memset(disk,0,sizeof(disk));calls=0;fail_call=2;
    assert(kernel_block_write_phase(&device,next,0,&span)==KERNEL_BLOCK_STATUS_IO);
    assert(calls==2 && !device.references && !memcmp(bytes,disk,128*1024));
    for (size_t i=128*1024;i<sizeof(disk);i++)assert(!disk[i]);
    calls=0;span.size+=512;
    assert(kernel_block_write_phase(&device,next,0,&span)==KERNEL_BLOCK_STATUS_OUT_OF_RANGE && !calls);
    puts("PASS block phase: bounded128KiB transfers, immutable producer restart, preflight, error prefix and request owners");
}
