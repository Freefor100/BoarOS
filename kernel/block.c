#include <kernel/block.h>
#include <kernel/errno.h>
#ifdef __riscv
#include <arch/context.h>
static uintptr_t registry_lock(void) { return arch_interrupt_save(); }
static void registry_unlock(uintptr_t state) { arch_interrupt_restore(state); }
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

enum kernel_block_status kernel_block_write_batch(
    struct kernel_block_device *device,
    const struct kernel_block_span *spans,
    size_t count)
{
    if (!device || !device->logical_block_size)
        return KERNEL_BLOCK_STATUS_INVALID;
    if (!device->write && !device->write_batch)
        return KERNEL_BLOCK_STATUS_UNSUPPORTED;
    if (count > KERNEL_BLOCK_BATCH_MAX || (count && !spans))
        return KERNEL_BLOCK_STATUS_INVALID;
    int nonempty = 0;
    for (size_t i = 0; i < count; i++) {
        const struct kernel_block_span *span = &spans[i];
        if (span->offset > device->capacity_bytes ||
            (uint64_t)span->size > device->capacity_bytes - span->offset)
            return KERNEL_BLOCK_STATUS_OUT_OF_RANGE;
        if (!span->size) continue;
        if (!span->buffer || span->size - 1 > UINTPTR_MAX - (uintptr_t)span->buffer)
            return KERNEL_BLOCK_STATUS_INVALID;
        nonempty = 1;
        for (size_t j = 0; j < i; j++) {
            if (spans[j].size && span->offset < spans[j].offset + spans[j].size &&
                spans[j].offset < span->offset + span->size)
                return KERNEL_BLOCK_STATUS_INVALID;
        }
    }
    if (!nonempty) return KERNEL_BLOCK_STATUS_OK;
    if (device->write_batch)
        return device->write_batch(device->context, spans, count);
    for (size_t i = 0; i < count; i++) {
        if (!spans[i].size) continue;
        enum kernel_block_status result = device->write(device->context,
            spans[i].offset, spans[i].buffer, spans[i].size);
        if (result != KERNEL_BLOCK_STATUS_OK) return result;
    }
    return KERNEL_BLOCK_STATUS_OK;
}

static int memory_overlaps(uintptr_t a, size_t an, uintptr_t b, size_t bn)
{ return an && bn && (a < b ? b - a < an : a - b < bn); }

enum kernel_block_status kernel_block_read_batch(struct kernel_block_device *device,
    struct kernel_block_read_span *spans, size_t count)
{
    if (!device || !device->logical_block_size || count > KERNEL_BLOCK_BATCH_MAX || (count && !spans))
        return KERNEL_BLOCK_STATUS_INVALID;
    if (!device->read && !device->read_batch) return KERNEL_BLOCK_STATUS_UNSUPPORTED;
    for (size_t i = 0; i < count; i++) {
        spans[i].completed = 0; spans[i].status = KERNEL_BLOCK_STATUS_NOT_SUBMITTED;
    }
    int nonempty = 0;
    for (size_t i = 0; i < count; i++) {
        struct kernel_block_read_span *span = &spans[i];
        if (span->offset > device->capacity_bytes || span->size > device->capacity_bytes - span->offset)
            return span->status = KERNEL_BLOCK_STATUS_OUT_OF_RANGE;
        if (!span->size) { span->status = KERNEL_BLOCK_STATUS_OK; continue; }
        uintptr_t buffer = (uintptr_t)span->buffer;
        if (!buffer || span->size - 1 > UINTPTR_MAX - buffer ||
            memory_overlaps(buffer, span->size, (uintptr_t)spans, count * sizeof(*spans)))
            return span->status = KERNEL_BLOCK_STATUS_INVALID;
        for (size_t j = 0; j < i; j++)
            if (memory_overlaps(buffer, span->size, (uintptr_t)spans[j].buffer, spans[j].size))
                return span->status = KERNEL_BLOCK_STATUS_INVALID;
        nonempty = 1;
    }
    if (!nonempty) return KERNEL_BLOCK_STATUS_OK;
    if (device->read_batch) return device->read_batch(device->context, spans, count);
    enum kernel_block_status result = KERNEL_BLOCK_STATUS_OK;
    for (size_t i = 0; i < count; i++) {
        if (!spans[i].size) continue;
        spans[i].status = device->read(device->context, spans[i].offset, spans[i].buffer, spans[i].size);
        if (spans[i].status == KERNEL_BLOCK_STATUS_OK) spans[i].completed = spans[i].size;
        else if (result == KERNEL_BLOCK_STATUS_OK) result = spans[i].status;
    }
    return result;
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
