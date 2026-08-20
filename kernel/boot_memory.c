#include <kernel/boot_memory.h>

#include <stdint.h>

static int range_end(uint64_t base, uint64_t size, uint64_t *end)
{
    if (size > UINT64_MAX - base) {
        return 0;
    }

    *end = base + size;
    return 1;
}

static void sort_ranges(struct dtb_memory_range *ranges, uint32_t count)
{
    uint32_t index;

    for (index = 1U; index < count; index++) {
        struct dtb_memory_range current = ranges[index];
        uint32_t position = index;

        while (position > 0U &&
               ranges[position - 1U].base > current.base) {
            ranges[position] = ranges[position - 1U];
            position--;
        }
        ranges[position] = current;
    }
}

static enum boot_memory_status append_clipped_range(
    struct dtb_memory_range *ranges,
    uint32_t *count,
    uint64_t memory_base,
    uint64_t memory_end,
    uint64_t base,
    uint64_t size)
{
    uint64_t end;

    if (size == 0U) {
        return BOOT_MEMORY_STATUS_OK;
    }
    if (!range_end(base, size, &end)) {
        return BOOT_MEMORY_STATUS_INVALID;
    }
    if (end <= memory_base || base >= memory_end) {
        return BOOT_MEMORY_STATUS_OK;
    }

    if (base < memory_base) {
        base = memory_base;
    }
    if (end > memory_end) {
        end = memory_end;
    }

    ranges[*count].base = base;
    ranges[*count].size = end - base;
    (*count)++;
    return BOOT_MEMORY_STATUS_OK;
}

enum boot_memory_status boot_memory_build(
    const struct dtb_boot_info *info,
    uint64_t kernel_start,
    uint64_t kernel_end,
    uint64_t dtb_address,
    struct boot_memory_layout *layout)
{
    struct boot_memory_layout result;
    struct dtb_memory_range candidates[BOOT_MEMORY_MAX_RESERVED_RANGES];
    uint64_t memory_end;
    uint64_t dtb_end;
    uint64_t cursor;
    uint32_t candidate_count = 0U;
    uint32_t index;
    enum boot_memory_status status;

    if (info == 0 || layout == 0 || info->memory.size == 0U ||
        info->reserved_count > DTB_MAX_RESERVED_RANGES ||
        !range_end(info->memory.base, info->memory.size, &memory_end) ||
        kernel_start >= kernel_end || kernel_start < info->memory.base ||
        kernel_end > memory_end || info->dtb_size == 0U ||
        !range_end(dtb_address, info->dtb_size, &dtb_end)) {
        return BOOT_MEMORY_STATUS_INVALID;
    }

    for (index = 0U; index < info->reserved_count; index++) {
        status = append_clipped_range(candidates,
                                      &candidate_count,
                                      info->memory.base,
                                      memory_end,
                                      info->reserved[index].base,
                                      info->reserved[index].size);
        if (status != BOOT_MEMORY_STATUS_OK) {
            return status;
        }
    }

    status = append_clipped_range(candidates,
                                  &candidate_count,
                                  info->memory.base,
                                  memory_end,
                                  kernel_start,
                                  kernel_end - kernel_start);
    if (status != BOOT_MEMORY_STATUS_OK) {
        return status;
    }
    status = append_clipped_range(candidates,
                                  &candidate_count,
                                  info->memory.base,
                                  memory_end,
                                  dtb_address,
                                  dtb_end - dtb_address);
    if (status != BOOT_MEMORY_STATUS_OK) {
        return status;
    }

    sort_ranges(candidates, candidate_count);
    result.reserved_count = 0U;
    for (index = 0U; index < candidate_count; index++) {
        if (result.reserved_count == 0U) {
            result.reserved[0] = candidates[index];
            result.reserved_count = 1U;
        } else {
            struct dtb_memory_range *previous =
                &result.reserved[result.reserved_count - 1U];
            uint64_t previous_end = previous->base + previous->size;
            uint64_t current_end =
                candidates[index].base + candidates[index].size;

            if (candidates[index].base <= previous_end) {
                if (current_end > previous_end) {
                    previous->size = current_end - previous->base;
                }
            } else {
                result.reserved[result.reserved_count] = candidates[index];
                result.reserved_count++;
            }
        }
    }

    result.usable_count = 0U;
    cursor = info->memory.base;
    for (index = 0U; index < result.reserved_count; index++) {
        uint64_t reserved_end = result.reserved[index].base +
                                result.reserved[index].size;

        if (cursor < result.reserved[index].base) {
            result.usable[result.usable_count].base = cursor;
            result.usable[result.usable_count].size =
                result.reserved[index].base - cursor;
            result.usable_count++;
        }
        cursor = reserved_end;
    }
    if (cursor < memory_end) {
        result.usable[result.usable_count].base = cursor;
        result.usable[result.usable_count].size = memory_end - cursor;
        result.usable_count++;
    }

    if (result.usable_count == 0U) {
        return BOOT_MEMORY_STATUS_EMPTY;
    }

    *layout = result;
    return BOOT_MEMORY_STATUS_OK;
}
