#include <kernel/block.h>
#include <kernel/errno.h>
#include <assert.h>
#include <stdio.h>

static enum kernel_block_status read_data(void *context, uint64_t offset,
                                         void *buffer, size_t size)
{
    (void)context; (void)offset; (void)buffer; (void)size;
    return KERNEL_BLOCK_STATUS_OK;
}

int main(void)
{
    struct kernel_block_device first = {.read = read_data, .logical_block_size = 512};
    struct kernel_block_device second = {.read = read_data, .logical_block_size = 512};
    struct kernel_block_device fixture = {0};
    int owner_a, owner_b;
    assert(KERNEL_BLOCK_DEVICE_NUMBER(0) == 0xfc00);
    assert(KERNEL_BLOCK_DEVICE_NUMBER(1) == 0xfc10);
    assert(KERNEL_BLOCK_DEVICE_NUMBER(16) == 0x10fc00);
    assert(kernel_block_lookup(0xfc00) == 0);
    assert(kernel_block_register(&first, 0xfc00) == 0);
    assert(kernel_block_register(&second, 0xfc00) == -KERNEL_EBUSY);
    assert(kernel_block_register(&second, 0xfc10) == 0);
    assert(kernel_block_lookup(0xfc00) == &first);
    assert(kernel_block_lookup(0xfc10) == &second);
    assert(kernel_block_claim(&first, &owner_a) == 0);
    assert(kernel_block_claim(&first, &owner_b) == -KERNEL_EBUSY);
    assert(kernel_block_claim(&first, &owner_a) == -KERNEL_EBUSY);
    assert(kernel_block_claim(&second, &owner_b) == 0);
    assert(kernel_block_unregister(&first) == -KERNEL_EBUSY);
    kernel_block_release_claim(&first, &owner_a);
    assert(kernel_block_unregister(&first) == 0);
    assert(kernel_block_lookup(0xfc00) == 0);
    assert(kernel_block_lookup(0xfc10) == &second);
    assert(kernel_block_claim(&first, &owner_b) == 0);
    assert(kernel_block_register(&first, 0xfc00) == -KERNEL_EBUSY);
    kernel_block_release_claim(&first, &owner_b);
    kernel_block_release_claim(&second, &owner_b);
    assert(kernel_block_unregister(&second) == 0);
    assert(kernel_block_claim(&fixture, &owner_a) == 0);
    kernel_block_release_claim(&fixture, &owner_a);
    puts("block registry identity, exclusive claim and owner release tests passed");
    return 0;
}
