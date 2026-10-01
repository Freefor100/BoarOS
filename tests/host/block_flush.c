#include <kernel/block.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct batch_disk {
    unsigned char bytes[64];
    unsigned writes, batches;
    unsigned fail_at;
    enum kernel_block_status result;
    uint64_t order[8];
};

static enum kernel_block_status write_bytes(void *context, uint64_t offset,
                                           const void *buffer, size_t size)
{
    struct batch_disk *disk = context;
    disk->order[disk->writes++] = offset;
    if (disk->writes == disk->fail_at) return disk->result;
    memcpy(disk->bytes + offset, buffer, size);
    return KERNEL_BLOCK_STATUS_OK;
}

static enum kernel_block_status write_batch(void *context,
    const struct kernel_block_span *spans, size_t count)
{
    struct batch_disk *disk = context;
    disk->batches++;
    if (disk->result != KERNEL_BLOCK_STATUS_OK) return disk->result;
    for (size_t i = 0; i < count; i++) {
        if (!spans[i].size) continue;
        memcpy(disk->bytes + spans[i].offset, spans[i].buffer, spans[i].size);
    }
    return KERNEL_BLOCK_STATUS_OK;
}

static void test_write_batch(void)
{
    struct batch_disk disk = {0};
    struct kernel_block_device device = {
        .context = &disk, .write = write_bytes,
        .capacity_bytes = sizeof(disk.bytes), .logical_block_size = 512,
    };
    const char values[] = "abcdefgh";
    struct kernel_block_span spans[9] = {
        {16, values, 3}, {0, values + 3, 2}, {32, values + 5, 3},
    };
    assert(kernel_block_write_batch(0, spans, 3) == KERNEL_BLOCK_STATUS_INVALID);
    device.logical_block_size = 0;
    assert(kernel_block_write_batch(&device, spans, 3) == KERNEL_BLOCK_STATUS_INVALID);
    device.logical_block_size = 512;
    assert(kernel_block_write_batch(&device, 0, 1) == KERNEL_BLOCK_STATUS_INVALID);
    assert(kernel_block_write_batch(&device, spans, 9) == KERNEL_BLOCK_STATUS_INVALID);
    assert(kernel_block_write_batch(&device, 0, 0) == KERNEL_BLOCK_STATUS_OK);
    spans[3] = (struct kernel_block_span){64, 0, 0};
    assert(kernel_block_write_batch(&device, spans + 3, 1) == KERNEL_BLOCK_STATUS_OK);
    spans[3].offset = 65;
    assert(kernel_block_write_batch(&device, spans, 4) == KERNEL_BLOCK_STATUS_OUT_OF_RANGE);
    spans[3] = (struct kernel_block_span){63, values, 2};
    assert(kernel_block_write_batch(&device, spans, 4) == KERNEL_BLOCK_STATUS_OUT_OF_RANGE);
    spans[3] = (struct kernel_block_span){UINT64_MAX, values, 2};
    assert(kernel_block_write_batch(&device, spans, 4) == KERNEL_BLOCK_STATUS_OUT_OF_RANGE);
    spans[3] = (struct kernel_block_span){48, 0, 1};
    assert(kernel_block_write_batch(&device, spans, 4) == KERNEL_BLOCK_STATUS_INVALID);
    spans[3] = (struct kernel_block_span){18, values, 2};
    assert(kernel_block_write_batch(&device, spans, 4) == KERNEL_BLOCK_STATUS_INVALID);
    spans[3] = (struct kernel_block_span){15, values, 20};
    assert(kernel_block_write_batch(&device, spans, 4) == KERNEL_BLOCK_STATUS_INVALID);
    unsigned char empty[64] = {0};
    assert(!memcmp(disk.bytes, empty, sizeof(empty)) && !disk.writes && !disk.batches);
    assert(kernel_block_write_batch(&device, spans, 3) == KERNEL_BLOCK_STATUS_OK);
    assert(!memcmp(disk.bytes, "de", 2) && !memcmp(disk.bytes + 16, "abc", 3) &&
           !memcmp(disk.bytes + 32, "fgh", 3));
    assert(disk.writes == 3 && disk.order[0] == 16 && disk.order[1] == 0 && disk.order[2] == 32);
    const enum kernel_block_status failures[] = {
        KERNEL_BLOCK_STATUS_IO, KERNEL_BLOCK_STATUS_TIMEOUT,
        KERNEL_BLOCK_STATUS_UNSUPPORTED, KERNEL_BLOCK_STATUS_NO_MEMORY,
        KERNEL_BLOCK_STATUS_STATE,
    };
    for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); i++) {
        disk = (struct batch_disk){.fail_at = 2, .result = failures[i]};
        assert(kernel_block_write_batch(&device, spans, 3) == failures[i]);
        assert(disk.writes == 2 && !memcmp(disk.bytes + 16, "abc", 3) &&
               !memcmp(disk.bytes + 32, empty, 3));
    }
    disk = (struct batch_disk){0};
    device.write_batch = write_batch;
    unsigned char before_bytes[64];
    memcpy(before_bytes, disk.bytes, sizeof(before_bytes));
    spans[3] = (struct kernel_block_span){18, values, 2};
    assert(kernel_block_write_batch(&device, spans, 4) == KERNEL_BLOCK_STATUS_INVALID);
    spans[3] = (struct kernel_block_span){63, values, 2};
    assert(kernel_block_write_batch(&device, spans, 4) == KERNEL_BLOCK_STATUS_OUT_OF_RANGE);
    spans[3] = (struct kernel_block_span){48, (void *)UINTPTR_MAX, 2};
    assert(kernel_block_write_batch(&device, spans, 4) == KERNEL_BLOCK_STATUS_INVALID);
    assert(!disk.batches && !memcmp(before_bytes, disk.bytes, sizeof(before_bytes)));
    spans[3] = (struct kernel_block_span){19, values + 3, 1};
    assert(kernel_block_write_batch(&device, spans, 4) == KERNEL_BLOCK_STATUS_OK);
    assert(disk.batches == 1 && !disk.writes && !memcmp(disk.bytes + 16, "abcd", 4));
    for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); i++) {
        disk.result = failures[i];
        assert(kernel_block_write_batch(&device, spans, 4) == failures[i]);
    }
    spans[0] = (struct kernel_block_span){0, 0, 0};
    unsigned before = disk.batches;
    assert(kernel_block_write_batch(&device, spans, 1) == KERNEL_BLOCK_STATUS_OK);
    assert(disk.batches == before);
    disk = (struct batch_disk){0};
    for (size_t i = 0; i < 8; i++) spans[i] = (struct kernel_block_span){i * 8, values + i, 1};
    device.write = 0;
    assert(kernel_block_write_batch(&device, spans, 8) == KERNEL_BLOCK_STATUS_OK);
    for (size_t i = 0; i < 8; i++) assert(disk.bytes[i * 8] == (unsigned char)values[i]);
    device.write = 0; device.write_batch = 0;
    assert(kernel_block_write_batch(&device, spans, 1) == KERNEL_BLOCK_STATUS_UNSUPPORTED);
}

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
    test_write_batch();
    puts("block batch preflight, ordered fallback and callback error tests passed");
}
