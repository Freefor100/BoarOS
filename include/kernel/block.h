#ifndef BOAROS_KERNEL_BLOCK_H
#define BOAROS_KERNEL_BLOCK_H

#include <stddef.h>
#include <stdint.h>

enum kernel_block_status {
    KERNEL_BLOCK_STATUS_OK = 0,
    KERNEL_BLOCK_STATUS_INVALID,
    KERNEL_BLOCK_STATUS_OUT_OF_RANGE,
    KERNEL_BLOCK_STATUS_IO,
    KERNEL_BLOCK_STATUS_TIMEOUT,
    KERNEL_BLOCK_STATUS_UNSUPPORTED,
    KERNEL_BLOCK_STATUS_NO_MEMORY,
    KERNEL_BLOCK_STATUS_STATE,
};

struct kernel_block_device;

typedef enum kernel_block_status (*kernel_block_read_fn)(
    void *context,
    uint64_t offset,
    void *buffer,
    size_t size);

typedef enum kernel_block_status (*kernel_block_write_fn)(
    void *context,
    uint64_t offset,
    const void *buffer,
    size_t size);

struct kernel_block_device {
    void *context;
    kernel_block_read_fn read;
    kernel_block_write_fn write;
    uint64_t capacity_bytes;
    uint32_t logical_block_size;
};

enum kernel_block_status kernel_block_read_at(
    struct kernel_block_device *device,
    uint64_t offset,
    void *buffer,
    size_t size);

enum kernel_block_status kernel_block_write_at(
    struct kernel_block_device *device,
    uint64_t offset,
    const void *buffer,
    size_t size);

#endif
