#include <kernel/block.h>
#include <assert.h>
#include <stdio.h>

static enum kernel_block_status completion;
static unsigned calls;

static enum kernel_block_status flush(void *context)
{
    assert(context == &calls);
    calls++;
    return completion;
}

int main(void)
{
    struct kernel_block_device device = {0};
    assert(kernel_block_flush(0) == KERNEL_BLOCK_STATUS_INVALID);
    assert(kernel_block_flush(&device) == KERNEL_BLOCK_STATUS_INVALID);
    device.logical_block_size = 512;
    assert(kernel_block_flush(&device) == KERNEL_BLOCK_STATUS_UNSUPPORTED);
    device.cache_mode = KERNEL_BLOCK_CACHE_WRITETHROUGH;
    assert(kernel_block_flush(&device) == KERNEL_BLOCK_STATUS_OK);
    device.cache_mode = KERNEL_BLOCK_CACHE_WRITEBACK;
    assert(kernel_block_flush(&device) == KERNEL_BLOCK_STATUS_UNSUPPORTED);
    device.flush = flush;
    device.context = &calls;
    const enum kernel_block_status results[] = {
        KERNEL_BLOCK_STATUS_OK, KERNEL_BLOCK_STATUS_IO,
        KERNEL_BLOCK_STATUS_TIMEOUT, KERNEL_BLOCK_STATUS_UNSUPPORTED,
        KERNEL_BLOCK_STATUS_STATE,
    };
    for (unsigned i = 0; i < sizeof(results) / sizeof(results[0]); i++) {
        completion = results[i];
        assert(kernel_block_flush(&device) == completion);
        assert(calls == i + 1);
    }
    puts("block flush capability and error tests passed");
}
