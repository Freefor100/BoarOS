#ifndef BOAROS_KERNEL_DTB_H
#define BOAROS_KERNEL_DTB_H

#include <stdint.h>

#define DTB_MAX_RESERVED_RANGES 16U

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

struct dtb_boot_info {
    struct dtb_memory_range memory;
    uint32_t dtb_size;
    uint32_t timebase_frequency;
    uint32_t reserved_count;
    struct dtb_memory_range reserved[DTB_MAX_RESERVED_RANGES];
};

enum dtb_status dtb_read_boot_info(const void *dtb,
                                   struct dtb_boot_info *info);

#endif
