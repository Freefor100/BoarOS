#include <kernel/block.h>
#include <kernel/errno.h>
#ifdef __riscv
#include <arch/riscv/context.h>
static uintptr_t registry_lock(void) { return riscv_interrupt_save(); }
static void registry_unlock(uintptr_t state) { riscv_interrupt_restore(state); }
#else
/* Host block models share the same registry contract without RISC-V CSRs. */
static unsigned char registry_busy;
static uintptr_t registry_lock(void)
{
    while (__atomic_test_and_set(&registry_busy, __ATOMIC_ACQUIRE)) {}
    return 0;
}
static void registry_unlock(uintptr_t state)
{
    (void)state;
    __atomic_clear(&registry_busy, __ATOMIC_RELEASE);
}
#endif

#include <stddef.h>
#include <stdint.h>

enum kernel_block_status kernel_block_flush(struct kernel_block_device *device)
{
    if (device == 0 || device->logical_block_size == 0U) {
        return KERNEL_BLOCK_STATUS_INVALID;
    }
    if (device->flush != 0) {
        return device->flush(device->context);
    }
    return device->cache_mode == KERNEL_BLOCK_CACHE_WRITETHROUGH
               ? KERNEL_BLOCK_STATUS_OK : KERNEL_BLOCK_STATUS_UNSUPPORTED;
}

enum kernel_block_status kernel_block_read_at(
    struct kernel_block_device *device,
    uint64_t offset,
    void *buffer,
    size_t size)
{
    if (device == 0 || device->read == 0 ||
        device->logical_block_size == 0U) {
        return KERNEL_BLOCK_STATUS_INVALID;
    }
    if (offset > device->capacity_bytes ||
        (uint64_t)size > device->capacity_bytes - offset) {
        return KERNEL_BLOCK_STATUS_OUT_OF_RANGE;
    }
    if (size == 0U) {
        return KERNEL_BLOCK_STATUS_OK;
    }
    if (buffer == 0) {
        return KERNEL_BLOCK_STATUS_INVALID;
    }

    return device->read(device->context, offset, buffer, size);
}

enum kernel_block_status kernel_block_write_at(
    struct kernel_block_device *device,
    uint64_t offset,
    const void *buffer,
    size_t size)
{
    if (device == 0 || device->write == 0 ||
        device->logical_block_size == 0U) {
        return KERNEL_BLOCK_STATUS_UNSUPPORTED;
    }
    if (offset > device->capacity_bytes ||
        (uint64_t)size > device->capacity_bytes - offset) {
        return KERNEL_BLOCK_STATUS_OUT_OF_RANGE;
    }
    if (size == 0U) {
        return KERNEL_BLOCK_STATUS_OK;
    }
    if (buffer == 0) {
        return KERNEL_BLOCK_STATUS_INVALID;
    }

    return device->write(device->context, offset, buffer, size);
}

static struct kernel_block_device *registered_devices;

int kernel_block_register(struct kernel_block_device *device, uint64_t number)
{
    if (!device || !device->read || !device->logical_block_size)
        return -KERNEL_EINVAL;
    uintptr_t irq = registry_lock();
    int error = 0;
    if (device->registered || device->claim_owner) error = -KERNEL_EBUSY;
    for (struct kernel_block_device *it = registered_devices; it; it = it->registry_next)
        if (it->device_number == number) error = -KERNEL_EBUSY;
    if (!error) {
        device->device_number = number;
        device->claim_owner = 0;
        device->registry_next = registered_devices;
        device->registered = 1U;
        registered_devices = device;
    }
    registry_unlock(irq);
    return error;
}

int kernel_block_unregister(struct kernel_block_device *device)
{
    if (!device) return -KERNEL_EINVAL;
    uintptr_t irq = registry_lock();
    int error = 0;
    if (!device->registered) error = -KERNEL_ENODEV;
    else if (device->claim_owner) error = -KERNEL_EBUSY;
    else {
        struct kernel_block_device **link = &registered_devices;
        while (*link && *link != device) link = &(*link)->registry_next;
        if (!*link) __builtin_trap();
        *link = device->registry_next;
        device->registered = 0U;
        device->registry_next = 0;
    }
    registry_unlock(irq);
    return error;
}

struct kernel_block_device *kernel_block_lookup(uint64_t number)
{
    uintptr_t irq = registry_lock();
    struct kernel_block_device *device = registered_devices;
    while (device && device->device_number != number) device = device->registry_next;
    registry_unlock(irq);
    return device;
}

int kernel_block_claim(struct kernel_block_device *device, const void *owner)
{
    if (!device || !owner) return -KERNEL_EINVAL;
    uintptr_t irq = registry_lock();
    int error = device->claim_owner ? -KERNEL_EBUSY : 0;
    if (!error) device->claim_owner = owner;
    registry_unlock(irq);
    return error;
}

void kernel_block_release_claim(struct kernel_block_device *device,
                                const void *owner)
{
    uintptr_t irq = registry_lock();
    if (!device || !owner || device->claim_owner != owner) __builtin_trap();
    device->claim_owner = 0;
    registry_unlock(irq);
}
