#include <kernel/page.h>
#include <kernel/physical_page.h>

#include <stdint.h>

#define PHYSICAL_PAGE_ALLOCATOR_INITIALIZED UINT32_C(0x50414745)
#define PHYSICAL_PAGE_NONE UINT64_MAX

static int range_end(uint64_t base, uint64_t size, uint64_t *end)
{
    if (size > UINT64_MAX - base) {
        return 0;
    }

    *end = base + size;
    return 1;
}

static int address_was_allocated(
    const struct physical_page_allocator *allocator,
    uint64_t address)
{
    uint32_t index;

    if ((address & BOAROS_PAGE_MASK) != 0U) {
        return 0;
    }

    for (index = 0U; index < allocator->range_count; index++) {
        const struct physical_page_range *range = &allocator->ranges[index];

        if (address >= range->base && address < range->next) {
            return 1;
        }
    }

    return 0;
}

static int read_recycled_node(
    const struct physical_page_allocator *allocator,
    uint64_t address,
    uint64_t *value)
{
    const unsigned char *bytes = allocator->access(address);
    uint64_t result = 0U;
    uint32_t index;

    if (bytes == 0) {
        return 0;
    }
    for (index = 0U; index < sizeof(result); index++) {
        result |= (uint64_t)bytes[index] << (index * 8U);
    }

    *value = result;
    return 1;
}

static int write_recycled_node(
    const struct physical_page_allocator *allocator,
    uint64_t address,
    uint64_t value)
{
    unsigned char *bytes = allocator->access(address);
    uint32_t index;

    if (bytes == 0) {
        return 0;
    }
    for (index = 0U; index < sizeof(value); index++) {
        bytes[index] = (unsigned char)(value >> (index * 8U));
    }

    return 1;
}

enum physical_page_status physical_page_allocator_init(
    struct physical_page_allocator *allocator,
    const struct boot_memory_layout *layout)
{
    struct physical_page_allocator result;
    uint64_t previous_end = 0U;
    uint32_t index;

    if (allocator == 0 || layout == 0 || layout->usable_count == 0U ||
        layout->usable_count > BOOT_MEMORY_MAX_USABLE_RANGES) {
        return PHYSICAL_PAGE_STATUS_INVALID;
    }

    result.total_pages = 0U;
    result.available_pages = 0U;
    result.recycled_head = PHYSICAL_PAGE_NONE;
    result.range_count = 0U;
    result.initialized = 0U;
    result.access = 0;
    for (index = 0U; index < BOOT_MEMORY_MAX_USABLE_RANGES; index++) {
        result.ranges[index].base = 0U;
        result.ranges[index].next = 0U;
        result.ranges[index].end = 0U;
    }

    for (index = 0U; index < layout->usable_count; index++) {
        uint64_t base = layout->usable[index].base;
        uint64_t size = layout->usable[index].size;
        uint64_t end;
        uint64_t aligned_base;
        uint64_t aligned_end;
        uint64_t page_count;

        if (size == 0U || !range_end(base, size, &end) ||
            (index > 0U && base < previous_end)) {
            return PHYSICAL_PAGE_STATUS_INVALID;
        }
        previous_end = end;

        if (base > UINT64_MAX - BOAROS_PAGE_MASK) {
            continue;
        }
        aligned_base = (base + BOAROS_PAGE_MASK) & ~BOAROS_PAGE_MASK;
        aligned_end = end & ~BOAROS_PAGE_MASK;
        if (aligned_base >= aligned_end) {
            continue;
        }

        page_count = (aligned_end - aligned_base) >> BOAROS_PAGE_SHIFT;
        if (page_count > UINT64_MAX - result.total_pages) {
            return PHYSICAL_PAGE_STATUS_INVALID;
        }

        result.ranges[result.range_count].base = aligned_base;
        result.ranges[result.range_count].next = aligned_base;
        result.ranges[result.range_count].end = aligned_end;
        result.range_count++;
        result.total_pages += page_count;
    }

    if (result.total_pages == 0U) {
        return PHYSICAL_PAGE_STATUS_EMPTY;
    }

    result.available_pages = result.total_pages;
    result.initialized = PHYSICAL_PAGE_ALLOCATOR_INITIALIZED;
    *allocator = result;
    return PHYSICAL_PAGE_STATUS_OK;
}

enum physical_page_status physical_page_allocator_bind_access(
    struct physical_page_allocator *allocator,
    physical_page_access_fn access)
{
    if (allocator == 0 || access == 0 ||
        allocator->initialized != PHYSICAL_PAGE_ALLOCATOR_INITIALIZED) {
        return PHYSICAL_PAGE_STATUS_INVALID;
    }
    if (allocator->access != 0) {
        return PHYSICAL_PAGE_STATUS_STATE;
    }

    allocator->access = access;
    return PHYSICAL_PAGE_STATUS_OK;
}

enum physical_page_status physical_page_allocate(
    struct physical_page_allocator *allocator,
    uint64_t *address)
{
    uint32_t index;

    if (allocator == 0 || address == 0 ||
        allocator->initialized != PHYSICAL_PAGE_ALLOCATOR_INITIALIZED) {
        return PHYSICAL_PAGE_STATUS_INVALID;
    }
    if (allocator->available_pages == 0U) {
        return PHYSICAL_PAGE_STATUS_EMPTY;
    }

    if (allocator->recycled_head != PHYSICAL_PAGE_NONE) {
        uint64_t result = allocator->recycled_head;
        uint64_t next;

        if (allocator->access == 0) {
            return PHYSICAL_PAGE_STATUS_STATE;
        }
        if (!address_was_allocated(allocator, result)) {
            return PHYSICAL_PAGE_STATUS_INVALID;
        }
        if (!read_recycled_node(allocator, result, &next)) {
            return PHYSICAL_PAGE_STATUS_INVALID;
        }
        if (next != PHYSICAL_PAGE_NONE &&
            !address_was_allocated(allocator, next)) {
            return PHYSICAL_PAGE_STATUS_INVALID;
        }

        allocator->recycled_head = next;
        allocator->available_pages--;
        *address = result;
        return PHYSICAL_PAGE_STATUS_OK;
    }

    for (index = 0U; index < allocator->range_count; index++) {
        struct physical_page_range *range = &allocator->ranges[index];

        if (range->next < range->end) {
            uint64_t result = range->next;

            range->next += BOAROS_PAGE_SIZE;
            allocator->available_pages--;
            *address = result;
            return PHYSICAL_PAGE_STATUS_OK;
        }
    }

    return PHYSICAL_PAGE_STATUS_INVALID;
}

enum physical_page_status physical_page_release(
    struct physical_page_allocator *allocator,
    uint64_t address)
{
    uint64_t current;
    uint64_t scanned = 0U;

    if (allocator == 0 ||
        allocator->initialized != PHYSICAL_PAGE_ALLOCATOR_INITIALIZED ||
        !address_was_allocated(allocator, address)) {
        return PHYSICAL_PAGE_STATUS_INVALID;
    }
    if (allocator->access == 0) {
        return PHYSICAL_PAGE_STATUS_STATE;
    }

    current = allocator->recycled_head;
    while (current != PHYSICAL_PAGE_NONE &&
           scanned < allocator->total_pages) {
        if (current == address) {
            return PHYSICAL_PAGE_STATUS_DOUBLE_FREE;
        }
        if (!address_was_allocated(allocator, current)) {
            return PHYSICAL_PAGE_STATUS_INVALID;
        }
        if (!read_recycled_node(allocator, current, &current)) {
            return PHYSICAL_PAGE_STATUS_INVALID;
        }
        scanned++;
    }
    if (current != PHYSICAL_PAGE_NONE ||
        allocator->available_pages >= allocator->total_pages) {
        return PHYSICAL_PAGE_STATUS_INVALID;
    }

    if (!write_recycled_node(allocator, address, allocator->recycled_head)) {
        return PHYSICAL_PAGE_STATUS_INVALID;
    }
    allocator->recycled_head = address;
    allocator->available_pages++;
    return PHYSICAL_PAGE_STATUS_OK;
}

uint64_t physical_page_total(
    const struct physical_page_allocator *allocator)
{
    if (allocator == 0 ||
        allocator->initialized != PHYSICAL_PAGE_ALLOCATOR_INITIALIZED) {
        return 0U;
    }

    return allocator->total_pages;
}

uint64_t physical_page_available(
    const struct physical_page_allocator *allocator)
{
    if (allocator == 0 ||
        allocator->initialized != PHYSICAL_PAGE_ALLOCATOR_INITIALIZED) {
        return 0U;
    }

    return allocator->available_pages;
}
