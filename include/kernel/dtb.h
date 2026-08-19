#ifndef BOAROS_KERNEL_DTB_H
#define BOAROS_KERNEL_DTB_H

#include <stdint.h>

enum dtb_status {
    DTB_STATUS_OK = 0,
    DTB_STATUS_INVALID,
    DTB_STATUS_NOT_FOUND,
    DTB_STATUS_UNSUPPORTED,
};

struct dtb_memory_range {
    uint64_t base;
    uint64_t size;
};

enum dtb_status dtb_read_first_memory_range(
    const void *dtb,
    struct dtb_memory_range *range);

#endif
