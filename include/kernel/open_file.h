#ifndef BOAROS_KERNEL_OPEN_FILE_H
#define BOAROS_KERNEL_OPEN_FILE_H

#include <kernel/page_cache.h>

#include <stddef.h>
#include <stdint.h>

struct kernel_heap;
struct kernel_open_file_description;
struct kernel_vfs_mount;

enum kernel_open_file_status {
    KERNEL_OPEN_FILE_STATUS_OK = 0,
    KERNEL_OPEN_FILE_STATUS_INVALID_ARGUMENT,
    KERNEL_OPEN_FILE_STATUS_NO_MEMORY,
    KERNEL_OPEN_FILE_STATUS_CLEANUP_REQUIRED,
    KERNEL_OPEN_FILE_STATUS_STATE,
};

/* VFS errors are returned through linux_result when status is OK. */
enum kernel_open_file_status kernel_open_file_create(
    struct kernel_heap *heap,
    struct kernel_vfs_mount *mount,
    const char *path,
    struct kernel_open_file_description **owner,
    int *linux_result);

enum kernel_open_file_status kernel_open_file_acquire(
    struct kernel_open_file_description *file);

/* Success consumes owner; cleanup failure leaves it retryable. */
enum kernel_open_file_status kernel_open_file_release(
    struct kernel_open_file_description **owner);

uint64_t kernel_open_file_size(
    const struct kernel_open_file_description *file);
uint32_t kernel_open_file_mode(
    const struct kernel_open_file_description *file);
uint64_t kernel_open_file_offset(
    const struct kernel_open_file_description *file);

enum kernel_open_file_status kernel_open_file_advance(
    struct kernel_open_file_description *file,
    uint64_t bytes);

enum kernel_page_cache_status kernel_open_file_get_page(
    struct kernel_open_file_description *file,
    uint64_t page_index,
    uint64_t *physical_address,
    size_t *valid_bytes);

enum kernel_page_cache_status kernel_open_file_lookup_page(
    struct kernel_open_file_description *file,
    uint64_t page_index,
    uint64_t *physical_address,
    size_t *valid_bytes);

int kernel_open_file_pread(struct kernel_open_file_description *file,
                           uint64_t offset,
                           void *buffer,
                           size_t size,
                           size_t *bytes_read);

#endif
