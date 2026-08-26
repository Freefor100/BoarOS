#ifndef BOAROS_KERNEL_UACCESS_H
#define BOAROS_KERNEL_UACCESS_H

#include <stddef.h>
#include <stdint.h>

struct kernel_mm;

enum kernel_uaccess_status {
    KERNEL_UACCESS_STATUS_OK = 0,
    KERNEL_UACCESS_STATUS_INVALID_ARGUMENT,
    KERNEL_UACCESS_STATUS_FAULT,
    KERNEL_UACCESS_STATUS_STATE,
};

enum kernel_uaccess_status kernel_copy_to_user(
    const struct kernel_mm *mm,
    uint64_t user_destination,
    const void *kernel_source,
    size_t size,
    size_t *bytes_copied);

#endif
