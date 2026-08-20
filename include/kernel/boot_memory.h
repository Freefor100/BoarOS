#ifndef BOAROS_KERNEL_BOOT_MEMORY_H
#define BOAROS_KERNEL_BOOT_MEMORY_H

#include <kernel/dtb.h>

#include <stdint.h>

#define BOOT_MEMORY_MAX_RESERVED_RANGES (DTB_MAX_RESERVED_RANGES + 2U)
#define BOOT_MEMORY_MAX_USABLE_RANGES (BOOT_MEMORY_MAX_RESERVED_RANGES + 1U)

enum boot_memory_status {
    BOOT_MEMORY_STATUS_OK = 0,
    BOOT_MEMORY_STATUS_INVALID,
    BOOT_MEMORY_STATUS_EMPTY,
};

struct boot_memory_layout {
    uint32_t reserved_count;
    uint32_t usable_count;
    struct dtb_memory_range reserved[BOOT_MEMORY_MAX_RESERVED_RANGES];
    struct dtb_memory_range usable[BOOT_MEMORY_MAX_USABLE_RANGES];
};

enum boot_memory_status boot_memory_build(
    const struct dtb_boot_info *info,
    uint64_t kernel_start,
    uint64_t kernel_end,
    uint64_t dtb_address,
    struct boot_memory_layout *layout);

#endif
