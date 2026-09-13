#include <kernel/block.h>

#include <stddef.h>
#include <stdint.h>

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
