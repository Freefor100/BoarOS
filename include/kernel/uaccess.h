#ifndef BOAROS_KERNEL_UACCESS_H
#define BOAROS_KERNEL_UACCESS_H

#include <stddef.h>
#include <stdint.h>

struct kernel_mm;

enum kernel_uaccess_status {
    KERNEL_UACCESS_STATUS_OK = 0,
    KERNEL_UACCESS_STATUS_INVALID_ARGUMENT,
    KERNEL_UACCESS_STATUS_FAULT,
    KERNEL_UACCESS_STATUS_TOO_LONG,
    KERNEL_UACCESS_STATUS_STATE,
};

enum kernel_uaccess_status kernel_user_range_check(
    uint64_t user_address,
    size_t size);

enum kernel_uaccess_status kernel_copy_to_user(
    struct kernel_mm *mm,
    uint64_t user_destination,
    const void *kernel_source,
    size_t size,
    size_t *bytes_copied);

enum kernel_uaccess_status kernel_copy_from_user(
    struct kernel_mm *mm,
    void *kernel_destination,
    uint64_t user_source,
    size_t size,
    size_t *bytes_copied);

/* On success, string_length excludes the terminating NUL byte. */
enum kernel_uaccess_status kernel_copy_string_from_user(
    struct kernel_mm *mm,
    char *kernel_destination,
    uint64_t user_source,
    size_t capacity,
    size_t *string_length);

#endif
