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

static unsigned opens, closes, notifications;
static int refuse_open;
static int device_open(struct kernel_block_device *device, uint32_t flags)
{ (void)device; (void)flags; opens++; return refuse_open ? -KERNEL_EBUSY : 0; }
static void device_close(struct kernel_block_device *device, uint32_t flags)
{ (void)device; (void)flags; closes++; }
static void changed(struct kernel_block_device *device)
{
    uint32_t references, count;
    kernel_block_usage(device, &references, &count);
    /* Last-owner callbacks still retain device storage until callback return. */
    assert(kernel_block_unregister(device) == -KERNEL_EBUSY);
    notifications++;
}
static void live_references(void)
{
    struct kernel_block_device device = {.read = read_data, .logical_block_size = 512,
        .open = device_open, .close = device_close, .owners_changed = changed};
    int a, b, mount;
    assert(!kernel_block_register(&device, 0x700));
    struct kernel_block_device *owner = kernel_block_get(0x700);
    assert(owner == &device && device.references == 1);
    assert(kernel_block_unregister(&device) == -KERNEL_EBUSY);
    assert(!kernel_block_open(&device, &a, 0));
    kernel_block_put(owner);
    assert(device.references == 1 && device.open_count == 1);
    assert(!kernel_block_claim(&device, &mount));
    assert(!kernel_block_open(&device, &b, 0));
    assert(kernel_block_open(&device, &b, 0200) == -KERNEL_EBUSY);
    kernel_block_release_claim(&device, &mount);
    kernel_block_close(&device, &b, 0);
    kernel_block_close(&device, &a, 0);
    assert(opens == 2 && closes == 2 && !device.references && !device.open_count);
    refuse_open = 1;
    assert(kernel_block_open(&device, &a, 0200) == -KERNEL_EBUSY);
    assert(opens == 3 && closes == 2 && !device.references && !device.open_count && !kernel_block_claimed(&device));
    assert(notifications >= 4 && !device.notifications);
    assert(!kernel_block_unregister(&device));
    assert(!kernel_block_get(0x700));
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
    live_references();
    puts("block registry identity, exclusive claim and owner release tests passed");
    return 0;
}
