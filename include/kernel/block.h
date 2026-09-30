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

enum kernel_block_cache_mode {
    KERNEL_BLOCK_CACHE_UNKNOWN = 0,
    KERNEL_BLOCK_CACHE_WRITETHROUGH,
    KERNEL_BLOCK_CACHE_WRITEBACK,
};

typedef enum kernel_block_status (*kernel_block_flush_fn)(void *context);

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
    kernel_block_flush_fn flush;
    enum kernel_block_cache_mode cache_mode;
    /* Registry membership is owned by the device lifetime, claims by mounts. */
    struct kernel_block_device *registry_next;
    const void *claim_owner;
    uint64_t device_number;
    uint32_t registered;
};

/* Linux new_encode_dev(252, disk_index * 16); whole disks only. */
#define KERNEL_BLOCK_DEVICE_NUMBER(index) \
    (UINT64_C(0xfc00) | (((uint64_t)(index) * 16U) & 0xffU) | \
     ((((uint64_t)(index) * 16U) & ~UINT64_C(0xff)) << 12))

/* Return zero or negative errno. Lookup borrows the boot-owned lifetime;
 * successful claims prevent unregister until the exact owner releases them. */
int kernel_block_register(struct kernel_block_device *device, uint64_t number);
int kernel_block_unregister(struct kernel_block_device *device);
struct kernel_block_device *kernel_block_lookup(uint64_t number);
int kernel_block_claim(struct kernel_block_device *device, const void *owner);
void kernel_block_release_claim(struct kernel_block_device *device,
                                const void *owner);

/* Complete all prior writes at the device's persistence boundary. A missing
 * callback is sufficient only for a declared write-through device. */
enum kernel_block_status kernel_block_flush(struct kernel_block_device *device);

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
